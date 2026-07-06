#ifdef __APPLE__
#include "network/tls_socket.h"
#include "network/socket.h"

#include <Security/SecureTransport.h>
#include <CoreFoundation/CoreFoundation.h>

namespace {

// SSLSetConnection only carries one opaque pointer through to the read/write
// callbacks, but Recv() needs to pass its own per-call timeout down into the
// callback too — bundle both behind that one pointer instead of hardcoding
// a fixed timeout in the callback.
struct IoContext {
    TcpSocket* sock;
    int timeoutMs;
};

// Blocks internally (looping TcpSocket::Recv) until either the full
// requested byte count is obtained or a real error/close occurs — so this
// never needs to report errSSLWouldBlock (that status is for a caller that
// wants to be re-invoked later with the same request; this callback always
// either fully satisfies the request or reports a terminal failure).
OSStatus TlsReadCallback(SSLConnectionRef connection, void* data, size_t* dataLength) {
    auto* io = (IoContext*)connection;
    size_t requested = *dataLength;
    size_t received = 0;
    char* out = (char*)data;
    while (received < requested) {
        int n = io->sock->Recv(out + received, requested - received, io->timeoutMs);
        if (n == 0) { *dataLength = received; return errSSLClosedGraceful; }
        if (n < 0) { *dataLength = received; return -1; }
        received += (size_t)n;
    }
    *dataLength = received;
    return noErr;
}

OSStatus TlsWriteCallback(SSLConnectionRef connection, const void* data, size_t* dataLength) {
    auto* io = (IoContext*)connection;
    bool ok = io->sock->SendAll((const char*)data, *dataLength);
    if (!ok) { *dataLength = 0; return -1; }
    return noErr;
}

} // namespace

struct TlsConnection::Impl {
    TcpSocket sock;
    SSLContextRef ctx = nullptr;
    IoContext ioCtx{};
    bool handshakeDone = false;

    ~Impl() {
        if (ctx) CFRelease(ctx);
    }
};

TlsConnection::TlsConnection() : impl_(new Impl()) {}
TlsConnection::~TlsConnection() { Close(); delete impl_; }

bool TlsConnection::Connect(const std::string& host, int port, int timeoutMs) {
    Impl& im = *impl_;
    if (!im.sock.Connect(host, port, timeoutMs)) return false;

    im.ctx = SSLCreateContext(kCFAllocatorDefault, kSSLClientSide, kSSLStreamType);
    if (!im.ctx) return false;

    im.ioCtx = { &im.sock, timeoutMs };
    if (SSLSetIOFuncs(im.ctx, TlsReadCallback, TlsWriteCallback) != noErr) return false;
    if (SSLSetConnection(im.ctx, &im.ioCtx) != noErr) return false;
    // SNI + hostname verification against the system trust store (default
    // authentication settings — SSLCreateContext leaves peer-cert
    // validation enabled by default; it's never explicitly disabled here).
    if (SSLSetPeerDomainName(im.ctx, host.c_str(), host.size()) != noErr) return false;

    OSStatus status;
    do {
        status = SSLHandshake(im.ctx);
    } while (status == errSSLWouldBlock);

    if (status != noErr) return false; // includes certificate validation failures

    im.handshakeDone = true;
    return true;
}

bool TlsConnection::SendAll(const char* data, size_t len) {
    Impl& im = *impl_;
    if (!im.handshakeDone) return false;
    size_t sent = 0;
    while (sent < len) {
        size_t processed = 0;
        OSStatus status = SSLWrite(im.ctx, data + sent, len - sent, &processed);
        if (status != noErr && status != errSSLWouldBlock) return false;
        if (processed == 0 && status != noErr) return false;
        sent += processed;
    }
    return true;
}

int TlsConnection::Recv(char* buffer, size_t maxLen, int timeoutMs) {
    Impl& im = *impl_;
    if (!im.handshakeDone) return -1;
    im.ioCtx.timeoutMs = timeoutMs;
    size_t processed = 0;
    OSStatus status = SSLRead(im.ctx, buffer, maxLen, &processed);
    if (status == errSSLClosedGraceful || status == errSSLClosedAbort)
        return processed > 0 ? (int)processed : 0;
    if (status != noErr && status != errSSLWouldBlock)
        return processed > 0 ? (int)processed : -1;
    return (int)processed;
}

void TlsConnection::Close() {
    Impl& im = *impl_;
    if (im.handshakeDone) {
        SSLClose(im.ctx);
        im.handshakeDone = false;
    }
    im.sock.Close();
}

bool TlsConnection::IsValid() const { return impl_->handshakeDone; }

#endif // __APPLE__
