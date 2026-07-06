#ifdef _WIN32
#include "network/tls_socket.h"
#include "network/socket.h"

#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <sspi.h>

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "secur32.lib")

namespace {
constexpr size_t kRecvChunkSize = 16384;
}

struct TlsConnection::Impl {
    TcpSocket sock; // raw transport — SChannel only ever sees/produces bytes over this
    CredHandle credHandle{};
    CtxtHandle ctxHandle{};
    bool credAcquired = false;
    bool ctxCreated = false;
    bool handshakeDone = false;
    SecPkgContext_StreamSizes streamSizes{};
    std::vector<uint8_t> recvBuf;      // encrypted bytes read but not yet fully decrypted into a record
    std::vector<uint8_t> plaintextBuf; // decrypted bytes not yet consumed by the caller's Recv()

    ~Impl() {
        if (ctxCreated) DeleteSecurityContext(&ctxHandle);
        if (credAcquired) FreeCredentialsHandle(&credHandle);
    }
};

TlsConnection::TlsConnection() : impl_(new Impl()) {}
TlsConnection::~TlsConnection() { Close(); delete impl_; }

bool TlsConnection::Connect(const std::string& host, int port, int timeoutMs) {
    Impl& im = *impl_;
    if (!im.sock.Connect(host, port, timeoutMs)) return false;

    SCHANNEL_CRED cred = {};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;

    TimeStamp expiry;
    SECURITY_STATUS ss = AcquireCredentialsHandleW(
        nullptr, (SEC_WCHAR*)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, nullptr,
        &cred, nullptr, nullptr, &im.credHandle, &expiry);
    if (ss != SEC_E_OK) return false;
    im.credAcquired = true;

    // ASCII hostnames only (SNI + certificate hostname check) — sufficient
    // scope for now, matches this initiative's other "common case first"
    // simplifications (e.g. JPEG's nearest-neighbor upsampling).
    std::wstring whost(host.begin(), host.end());

    const DWORD sspiFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
        ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    DWORD outFlags = 0;

    SecBuffer outBuf{ 0, SECBUFFER_TOKEN, nullptr };
    SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, &outBuf };

    ss = InitializeSecurityContextW(&im.credHandle, nullptr, (SEC_WCHAR*)whost.c_str(),
        sspiFlags, 0, 0, nullptr, 0, &im.ctxHandle, &outDesc, &outFlags, nullptr);
    if (ss != SEC_I_CONTINUE_NEEDED) return false;
    im.ctxCreated = true;

    if (outBuf.cbBuffer > 0 && outBuf.pvBuffer) {
        bool sent = im.sock.SendAll((const char*)outBuf.pvBuffer, outBuf.cbBuffer);
        FreeContextBuffer(outBuf.pvBuffer);
        if (!sent) return false;
    }

    std::vector<uint8_t> incoming;
    char recvChunk[kRecvChunkSize];

    for (;;) {
        SecBuffer inBuffers[2];
        inBuffers[0] = { (DWORD)incoming.size(), SECBUFFER_TOKEN, incoming.empty() ? nullptr : incoming.data() };
        inBuffers[1] = { 0, SECBUFFER_EMPTY, nullptr };
        SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, inBuffers };

        SecBuffer outBuf2{ 0, SECBUFFER_TOKEN, nullptr };
        SecBufferDesc outDesc2{ SECBUFFER_VERSION, 1, &outBuf2 };

        ss = InitializeSecurityContextW(&im.credHandle, &im.ctxHandle, (SEC_WCHAR*)whost.c_str(),
            sspiFlags, 0, 0, &inDesc, 0, nullptr, &outDesc2, &outFlags, nullptr);

        if (outBuf2.cbBuffer > 0 && outBuf2.pvBuffer) {
            bool sent = im.sock.SendAll((const char*)outBuf2.pvBuffer, outBuf2.cbBuffer);
            FreeContextBuffer(outBuf2.pvBuffer);
            if (!sent) return false;
        }

        if (ss == SEC_E_OK) {
            // Handshake complete. Any bytes SChannel marked SECBUFFER_EXTRA
            // are the start of the first application-data record, already
            // read off the wire — keep them for the first Recv() call.
            if (inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer > 0) {
                size_t extraOffset = incoming.size() - inBuffers[1].cbBuffer;
                im.recvBuf.assign(incoming.begin() + extraOffset, incoming.end());
            }
            break;
        } else if (ss == SEC_I_CONTINUE_NEEDED) {
            if (inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer > 0) {
                size_t extraOffset = incoming.size() - inBuffers[1].cbBuffer;
                incoming.assign(incoming.begin() + extraOffset, incoming.end());
            } else {
                incoming.clear();
            }
            int n = im.sock.Recv(recvChunk, sizeof(recvChunk), timeoutMs);
            if (n <= 0) return false;
            incoming.insert(incoming.end(), recvChunk, recvChunk + n);
        } else if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            // Not enough bytes yet for SChannel to parse this record —
            // read more without discarding what's already buffered.
            int n = im.sock.Recv(recvChunk, sizeof(recvChunk), timeoutMs);
            if (n <= 0) return false;
            incoming.insert(incoming.end(), recvChunk, recvChunk + n);
        } else {
            return false; // handshake failed (incl. certificate validation failure)
        }
    }

    if (QueryContextAttributesW(&im.ctxHandle, SECPKG_ATTR_STREAM_SIZES, &im.streamSizes) != SEC_E_OK)
        return false;

    im.handshakeDone = true;
    return true;
}

bool TlsConnection::SendAll(const char* data, size_t len) {
    Impl& im = *impl_;
    if (!im.handshakeDone) return false;

    size_t sent = 0;
    while (sent < len) {
        size_t chunkLen = std::min(len - sent, (size_t)im.streamSizes.cbMaximumMessage);
        std::vector<uint8_t> msgBuf(im.streamSizes.cbHeader + chunkLen + im.streamSizes.cbTrailer);
        memcpy(msgBuf.data() + im.streamSizes.cbHeader, data + sent, chunkLen);

        SecBuffer buffers[4];
        buffers[0] = { im.streamSizes.cbHeader, SECBUFFER_STREAM_HEADER, msgBuf.data() };
        buffers[1] = { (DWORD)chunkLen, SECBUFFER_DATA, msgBuf.data() + im.streamSizes.cbHeader };
        buffers[2] = { im.streamSizes.cbTrailer, SECBUFFER_STREAM_TRAILER,
                       msgBuf.data() + im.streamSizes.cbHeader + chunkLen };
        buffers[3] = { 0, SECBUFFER_EMPTY, nullptr };
        SecBufferDesc desc{ SECBUFFER_VERSION, 4, buffers };

        if (EncryptMessage(&im.ctxHandle, 0, &desc, 0) != SEC_E_OK) return false;

        // Sent as three separate writes (not one contiguous blob) since
        // EncryptMessage may use less than the allocated header/trailer
        // capacity — each SecBuffer's own pointer+length is authoritative,
        // matching Microsoft's own reference SChannel client sample.
        if (!im.sock.SendAll((const char*)buffers[0].pvBuffer, buffers[0].cbBuffer)) return false;
        if (!im.sock.SendAll((const char*)buffers[1].pvBuffer, buffers[1].cbBuffer)) return false;
        if (!im.sock.SendAll((const char*)buffers[2].pvBuffer, buffers[2].cbBuffer)) return false;

        sent += chunkLen;
    }
    return true;
}

int TlsConnection::Recv(char* buffer, size_t maxLen, int timeoutMs) {
    Impl& im = *impl_;
    if (!im.handshakeDone) return -1;

    if (!im.plaintextBuf.empty()) {
        size_t n = std::min(maxLen, im.plaintextBuf.size());
        memcpy(buffer, im.plaintextBuf.data(), n);
        im.plaintextBuf.erase(im.plaintextBuf.begin(), im.plaintextBuf.begin() + n);
        return (int)n;
    }

    char recvChunk[kRecvChunkSize];
    for (;;) {
        if (im.recvBuf.empty()) {
            int r = im.sock.Recv(recvChunk, sizeof(recvChunk), timeoutMs);
            if (r <= 0) return r;
            im.recvBuf.insert(im.recvBuf.end(), recvChunk, recvChunk + r);
        }

        SecBuffer buffers[4];
        buffers[0] = { (DWORD)im.recvBuf.size(), SECBUFFER_DATA, im.recvBuf.data() };
        buffers[1] = { 0, SECBUFFER_EMPTY, nullptr };
        buffers[2] = { 0, SECBUFFER_EMPTY, nullptr };
        buffers[3] = { 0, SECBUFFER_EMPTY, nullptr };
        SecBufferDesc desc{ SECBUFFER_VERSION, 4, buffers };

        SECURITY_STATUS ss = DecryptMessage(&im.ctxHandle, &desc, 0, nullptr);

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            int r = im.sock.Recv(recvChunk, sizeof(recvChunk), timeoutMs);
            if (r <= 0) return r;
            im.recvBuf.insert(im.recvBuf.end(), recvChunk, recvChunk + r);
            continue;
        }
        if (ss == SEC_I_CONTEXT_EXPIRED) return 0; // peer sent close_notify
        if (ss != SEC_E_OK) return -1;

        uint8_t* dataPtr = nullptr; size_t dataLen = 0;
        uint8_t* extraPtr = nullptr; size_t extraLen = 0;
        for (auto& b : buffers) {
            if (b.BufferType == SECBUFFER_DATA) { dataPtr = (uint8_t*)b.pvBuffer; dataLen = b.cbBuffer; }
            else if (b.BufferType == SECBUFFER_EXTRA) { extraPtr = (uint8_t*)b.pvBuffer; extraLen = b.cbBuffer; }
        }

        std::vector<uint8_t> leftover;
        if (extraPtr && extraLen > 0) leftover.assign(extraPtr, extraPtr + extraLen);
        if (dataLen > 0) im.plaintextBuf.assign(dataPtr, dataPtr + dataLen);
        im.recvBuf = std::move(leftover);

        if (!im.plaintextBuf.empty()) {
            size_t n = std::min(maxLen, im.plaintextBuf.size());
            memcpy(buffer, im.plaintextBuf.data(), n);
            im.plaintextBuf.erase(im.plaintextBuf.begin(), im.plaintextBuf.begin() + n);
            return (int)n;
        }
        // This record carried no application data (e.g. an alert) — loop
        // to decode/read the next one instead of returning a spurious 0.
    }
}

void TlsConnection::Close() {
    Impl& im = *impl_;
    if (im.handshakeDone) {
        DWORD type = SCHANNEL_SHUTDOWN;
        SecBuffer inBuf{ sizeof(type), SECBUFFER_TOKEN, &type };
        SecBufferDesc inDesc{ SECBUFFER_VERSION, 1, &inBuf };
        if (ApplyControlToken(&im.ctxHandle, &inDesc) == SEC_E_OK) {
            SecBuffer outBuf{ 0, SECBUFFER_TOKEN, nullptr };
            SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, &outBuf };
            DWORD outFlags = 0;
            const DWORD sspiFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
            if (InitializeSecurityContextW(&im.credHandle, &im.ctxHandle, nullptr, sspiFlags, 0, 0,
                                            nullptr, 0, nullptr, &outDesc, &outFlags, nullptr) == SEC_E_OK &&
                outBuf.cbBuffer > 0 && outBuf.pvBuffer) {
                im.sock.SendAll((const char*)outBuf.pvBuffer, outBuf.cbBuffer);
                FreeContextBuffer(outBuf.pvBuffer);
            }
        }
        im.handshakeDone = false;
    }
    im.sock.Close();
}

bool TlsConnection::IsValid() const { return impl_->handshakeDone; }

#endif // _WIN32
