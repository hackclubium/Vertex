#include "network/websocket.h"
#include "network/fetcher.h"

#include <curl/curl.h>

#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

namespace {

// ── SHA-1 (RFC 3174) ─────────────────────────────────────────────────────────
// Only used to validate the handshake's Sec-WebSocket-Accept value — a
// protocol-correctness check, not a security boundary — so a plain,
// textbook, single-shot implementation is appropriate (no streaming API
// needed: the input is always one short string).
std::array<uint8_t, 20> Sha1Hash(const std::string& msg) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;

    std::vector<uint8_t> data(msg.begin(), msg.end());
    const uint64_t mlBits = (uint64_t)data.size() * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0x00);
    for (int i = 7; i >= 0; --i) data.push_back((uint8_t)(mlBits >> (i * 8)));

    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)data[chunk + i * 4] << 24) |
                   ((uint32_t)data[chunk + i * 4 + 1] << 16) |
                   ((uint32_t)data[chunk + i * 4 + 2] << 8) |
                   (uint32_t)data[chunk + i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::array<uint8_t, 20> out;
    uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(hs[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(hs[i]);
    }
    return out;
}

// ── base64 ───────────────────────────────────────────────────────────────────

std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += tbl[(n >> 6) & 0x3F];
        out += tbl[n & 0x3F];
    }
    const size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += tbl[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

} // namespace

std::string ComputeWebSocketAccept(const std::string& secWebSocketKey) {
    const auto hash = Sha1Hash(secWebSocketKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return Base64Encode(hash.data(), hash.size());
}

namespace {

// ── ws(s):// URL parsing ─────────────────────────────────────────────────────

struct WsUrl {
    bool secure = false;
    std::string host;
    int port = 0;
    std::string path = "/";
};

bool ParseWsUrl(const std::string& url, WsUrl& out) {
    std::string rest;
    if (url.rfind("wss://", 0) == 0)      { out.secure = true;  out.port = 443; rest = url.substr(6); }
    else if (url.rfind("ws://", 0) == 0)  { out.secure = false; out.port = 80;  rest = url.substr(5); }
    else return false;
    if (rest.empty()) return false;

    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (hostport.empty()) return false;

    if (hostport[0] == '[') {
        // IPv6 literal: [::1]:port
        size_t close = hostport.find(']');
        if (close == std::string::npos) return false;
        out.host = hostport.substr(1, close - 1);
        if (close + 1 < hostport.size() && hostport[close + 1] == ':') {
            try { out.port = std::stoi(hostport.substr(close + 2)); } catch (...) { return false; }
        }
        return !out.host.empty();
    }

    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        try { out.port = std::stoi(hostport.substr(colon + 1)); } catch (...) { return false; }
    } else {
        out.host = hostport;
    }
    return !out.host.empty();
}

// ── frame codec (RFC 6455 §5.2) ──────────────────────────────────────────────

constexpr size_t kMaxFramePayload = 64 * 1024 * 1024;

// Client-to-server frames must always be masked (§5.3).
std::string EncodeFrame(uint8_t opcode, const uint8_t* payload, size_t len, bool fin = true) {
    std::string out;
    out.reserve(len + 14);
    out.push_back((char)((fin ? 0x80 : 0x00) | (opcode & 0x0F)));
    const uint8_t maskBit = 0x80;
    if (len <= 125) {
        out.push_back((char)(maskBit | (uint8_t)len));
    } else if (len <= 0xFFFF) {
        out.push_back((char)(maskBit | 126));
        out.push_back((char)(len >> 8));
        out.push_back((char)(len & 0xFF));
    } else {
        out.push_back((char)(maskBit | 127));
        for (int i = 7; i >= 0; --i) out.push_back((char)((uint64_t)len >> (i * 8)));
    }
    uint8_t maskKey[4];
    {
        static thread_local std::mt19937 gen{ std::random_device{}() };
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : maskKey) b = (uint8_t)dist(gen);
    }
    out.append((const char*)maskKey, 4);
    size_t base = out.size();
    out.resize(base + len);
    for (size_t i = 0; i < len; i++) out[base + i] = (char)(payload[i] ^ maskKey[i % 4]);
    return out;
}

std::string EncodeFrame(uint8_t opcode, const std::string& payload, bool fin = true) {
    return EncodeFrame(opcode, (const uint8_t*)payload.data(), payload.size(), fin);
}

struct RawFrame {
    bool fin = false;
    uint8_t opcode = 0;
    std::string payload;
};

enum class ParseResult { NeedMore, Ok, Error };

// Incremental parser: frames can arrive split across multiple recv() calls,
// so incomplete data is buffered until a full frame is available.
class FrameParser {
public:
    void feed(const char* data, size_t len) { buf_.append(data, len); }

    ParseResult next(RawFrame& out) {
        if (buf_.size() < 2) return ParseResult::NeedMore;
        const uint8_t b0 = (uint8_t)buf_[0];
        const uint8_t b1 = (uint8_t)buf_[1];
        const bool fin = (b0 & 0x80) != 0;
        const uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        size_t pos = 2;

        if (len == 126) {
            if (buf_.size() < pos + 2) return ParseResult::NeedMore;
            len = ((uint8_t)buf_[pos] << 8) | (uint8_t)buf_[pos + 1];
            pos += 2;
        } else if (len == 127) {
            if (buf_.size() < pos + 8) return ParseResult::NeedMore;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | (uint8_t)buf_[pos + i];
            pos += 8;
        }
        if (len > kMaxFramePayload) return ParseResult::Error;

        uint8_t maskKey[4] = { 0, 0, 0, 0 };
        if (masked) {
            if (buf_.size() < pos + 4) return ParseResult::NeedMore;
            memcpy(maskKey, buf_.data() + pos, 4);
            pos += 4;
        }
        if (buf_.size() < pos + len) return ParseResult::NeedMore;

        out.fin = fin;
        out.opcode = opcode;
        out.payload.assign(buf_.data() + pos, (size_t)len);
        if (masked) {
            for (size_t i = 0; i < out.payload.size(); i++)
                out.payload[i] = (char)((uint8_t)out.payload[i] ^ maskKey[i % 4]);
        }
        buf_.erase(0, pos + (size_t)len);
        return ParseResult::Ok;
    }

private:
    std::string buf_;
};

// ── raw socket I/O over a curl CONNECT_ONLY handle ──────────────────────────
// curl does only the TCP connect + TLS handshake (if wss://) here; every byte
// sent or received past that point is WebSocket protocol bytes Vertex builds
// and parses itself.

bool CurlSendAll(CURL* curl, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        size_t n = 0;
        CURLcode rc = curl_easy_send(curl, data.data() + sent, data.size() - sent, &n);
        if (rc == CURLE_AGAIN) {
            curl_socket_t sock = CURL_SOCKET_BAD;
            curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);
            if (sock == CURL_SOCKET_BAD) return false;
            fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
            timeval tv{ 1, 0 };
            select((int)sock + 1, nullptr, &wfds, nullptr, &tv);
            continue;
        }
        if (rc != CURLE_OK) return false;
        sent += n;
    }
    return true;
}

// Reads until the blank line terminating the HTTP response headers is seen.
// Any bytes read past it (the start of the first WS frame, if the server
// pipelined it right after the handshake) are returned via `leftover`.
bool ReadHttpResponseHeaders(CURL* curl, curl_socket_t sock, std::string& headers, std::string& leftover) {
    constexpr size_t kMaxHeaderBytes = 64 * 1024;
    char buf[4096];
    for (;;) {
        size_t termPos = headers.find("\r\n\r\n");
        if (termPos != std::string::npos) {
            leftover = headers.substr(termPos + 4);
            headers.resize(termPos);
            return true;
        }
        if (headers.size() > kMaxHeaderBytes) return false;

        fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
        timeval tv{ 10, 0 };
        int sel = select((int)sock + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) return false;

        size_t n = 0;
        CURLcode rc = curl_easy_recv(curl, buf, sizeof(buf), &n);
        if (rc == CURLE_AGAIN) continue;
        if (rc != CURLE_OK || n == 0) return false;
        headers.append(buf, n);
    }
}

std::string HeaderValue(const std::string& headers, const std::string& name) {
    std::string lower = headers;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    std::string needle = name;
    for (auto& c : needle) c = (char)tolower((unsigned char)c);

    // Find the name at the start of a line (position 0, or right after a
    // '\n') so a coincidental substring inside another header's value can't
    // match.
    size_t pos = 0;
    for (;;) {
        pos = lower.find(needle, pos);
        if (pos == std::string::npos) return "";
        if (pos == 0 || lower[pos - 1] == '\n') break;
        pos += needle.size();
    }

    size_t colon = headers.find(':', pos);
    if (colon == std::string::npos) return "";
    size_t lineEnd = headers.find("\r\n", colon);
    if (lineEnd == std::string::npos) lineEnd = headers.size();
    std::string val = headers.substr(colon + 1, lineEnd - colon - 1);
    size_t start = val.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = val.find_last_not_of(" \t");
    return val.substr(start, end - start + 1);
}

// ── connection registry ──────────────────────────────────────────────────────

struct WsOutboxItem {
    uint8_t opcode;
    std::string payload;
};

struct WsConnection {
    std::mutex outboxMutex;
    std::deque<WsOutboxItem> outbox;
    bool closeRequested = false;
    int closeCode = 1000;
    std::string closeReason;
};

std::mutex g_wsMutex;
std::map<int, std::shared_ptr<WsConnection>> g_connections;
std::atomic<int> g_nextHandle{ 1 };
std::atomic<int> g_liveCount{ 0 };

struct PendingWsEvent {
    WsEventCallback callback;
    WsEvent event;
};
std::mutex g_eventMutex;
std::deque<PendingWsEvent> g_wsEvents;

void PushEvent(const WsEventCallback& cb, WsEvent ev) {
    std::lock_guard<std::mutex> lock(g_eventMutex);
    g_wsEvents.push_back({ cb, std::move(ev) });
}

void RunWebSocketConnection(int handle, std::string url, WsEventCallback onEvent,
                            std::shared_ptr<WsConnection> conn) {
    auto abnormalClose = [&](const std::string& errMsg, int code) {
        if (!errMsg.empty()) PushEvent(onEvent, WsEvent{ WsEventKind::Error, errMsg });
        PushEvent(onEvent, WsEvent{ WsEventKind::Close, "", false, code, "", false });
    };

    WsUrl parsed;
    if (!ParseWsUrl(url, parsed)) { abnormalClose("Invalid WebSocket URL", 1006); return; }

    EnsureCurlInit();
    CURL* curl = curl_easy_init();
    if (!curl) { abnormalClose("Failed to initialize connection", 1006); return; }

    const std::string transportUrl =
        (parsed.secure ? "https://" : "http://") + parsed.host + ":" + std::to_string(parsed.port) + "/";
    curl_easy_setopt(curl, CURLOPT_URL, transportUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    if (curl_easy_perform(curl) != CURLE_OK) {
        curl_easy_cleanup(curl);
        abnormalClose("Connection failed", 1006);
        return;
    }

    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);
    if (sock == CURL_SOCKET_BAD) {
        curl_easy_cleanup(curl);
        abnormalClose("No active socket", 1006);
        return;
    }

    // ── handshake ──
    uint8_t keyRaw[16];
    {
        static thread_local std::mt19937 gen{ std::random_device{}() };
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : keyRaw) b = (uint8_t)dist(gen);
    }
    const std::string secKey = Base64Encode(keyRaw, 16);

    const std::string request =
        "GET " + parsed.path + " HTTP/1.1\r\n"
        "Host: " + parsed.host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + secKey + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    if (!CurlSendAll(curl, request)) {
        curl_easy_cleanup(curl);
        abnormalClose("Failed to send handshake", 1006);
        return;
    }

    std::string headers, leftover;
    if (!ReadHttpResponseHeaders(curl, sock, headers, leftover)) {
        curl_easy_cleanup(curl);
        abnormalClose("Handshake response timed out or was too large", 1006);
        return;
    }
    if (headers.find(" 101 ") == std::string::npos) {
        curl_easy_cleanup(curl);
        abnormalClose("Server did not upgrade to WebSocket", 1002);
        return;
    }
    const std::string accept = HeaderValue(headers, "sec-websocket-accept");
    const std::string expectedAccept = ComputeWebSocketAccept(secKey);
    if (accept.empty() || accept != expectedAccept) {
        curl_easy_cleanup(curl);
        abnormalClose("Invalid Sec-WebSocket-Accept", 1002);
        return;
    }

    PushEvent(onEvent, WsEvent{ WsEventKind::Open });

    FrameParser parser;
    parser.feed(leftover.data(), leftover.size());

    std::string fragBuf;
    uint8_t fragOpcode = 0;
    bool fragActive = false;
    bool closeSent = false, closeReceived = false;
    char readBuf[8192];

    for (;;) {
        // Drain any queued outgoing sends / a requested close before waiting
        // on the socket, so sends aren't delayed more than one poll interval.
        std::deque<WsOutboxItem> toSend;
        bool doClose = false;
        int closeCode = 1000;
        std::string closeReason;
        {
            std::lock_guard<std::mutex> lock(conn->outboxMutex);
            toSend.swap(conn->outbox);
            if (conn->closeRequested && !closeSent) {
                doClose = true;
                closeCode = conn->closeCode;
                closeReason = conn->closeReason;
            }
        }
        bool sendFailed = false;
        for (auto& item : toSend) {
            if (!CurlSendAll(curl, EncodeFrame(item.opcode, item.payload))) { sendFailed = true; break; }
        }
        if (sendFailed) break;
        if (doClose) {
            std::string payload;
            payload.push_back((char)((closeCode >> 8) & 0xFF));
            payload.push_back((char)(closeCode & 0xFF));
            payload += closeReason;
            CurlSendAll(curl, EncodeFrame(0x8, payload));
            closeSent = true;
        }
        if (closeSent && closeReceived) break;

        fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
        timeval tv{ 0, 50000 }; // 50ms — bounds how stale outbox/close checks can get.
        int sel = select((int)sock + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        size_t n = 0;
        CURLcode rc = curl_easy_recv(curl, readBuf, sizeof(readBuf), &n);
        if (rc == CURLE_AGAIN) continue;
        if (rc != CURLE_OK || n == 0) break; // peer closed or a real socket error

        parser.feed(readBuf, n);

        RawFrame frame;
        ParseResult pr;
        bool protocolError = false;
        while ((pr = parser.next(frame)) == ParseResult::Ok) {
            switch (frame.opcode) {
            case 0x0: // continuation
                if (fragActive) {
                    fragBuf += frame.payload;
                    if (frame.fin) {
                        PushEvent(onEvent, WsEvent{ WsEventKind::Message, fragBuf, fragOpcode == 0x2 });
                        fragBuf.clear();
                        fragActive = false;
                    }
                }
                break;
            case 0x1: // text
            case 0x2: // binary
                if (!frame.fin) {
                    fragBuf = frame.payload;
                    fragOpcode = frame.opcode;
                    fragActive = true;
                } else {
                    PushEvent(onEvent, WsEvent{ WsEventKind::Message, frame.payload, frame.opcode == 0x2 });
                }
                break;
            case 0x8: { // close
                closeReceived = true;
                int code = 1005;
                std::string reason;
                if (frame.payload.size() >= 2) {
                    code = ((uint8_t)frame.payload[0] << 8) | (uint8_t)frame.payload[1];
                    reason = frame.payload.substr(2);
                }
                if (!closeSent) {
                    CurlSendAll(curl, EncodeFrame(0x8, frame.payload));
                    closeSent = true;
                }
                PushEvent(onEvent, WsEvent{ WsEventKind::Close, "", false, code, reason, true });
                break;
            }
            case 0x9: // ping -> pong
                CurlSendAll(curl, EncodeFrame(0xA, frame.payload));
                break;
            case 0xA: // pong
                break;
            default:
                protocolError = true;
                break;
            }
            if (closeReceived || protocolError) break;
        }
        if (pr == ParseResult::Error || protocolError) {
            PushEvent(onEvent, WsEvent{ WsEventKind::Error, "WebSocket protocol error" });
            break;
        }
        if (closeReceived) break;
    }

    if (!closeReceived) {
        PushEvent(onEvent, WsEvent{ WsEventKind::Close, "", false, closeSent ? 1000 : 1006, "", false });
    }
    curl_easy_cleanup(curl);
}

} // namespace

int OpenWebSocket(const std::string& url, WsEventCallback onEvent) {
    const int handle = g_nextHandle.fetch_add(1);
    auto conn = std::make_shared<WsConnection>();
    {
        std::lock_guard<std::mutex> lock(g_wsMutex);
        g_connections[handle] = conn;
    }
    g_liveCount.fetch_add(1);
    std::thread([handle, url, onEvent, conn]() {
        RunWebSocketConnection(handle, url, onEvent, conn);
        {
            std::lock_guard<std::mutex> lock(g_wsMutex);
            g_connections.erase(handle);
        }
        g_liveCount.fetch_sub(1);
    }).detach();
    return handle;
}

void SendWebSocketText(int handle, const std::string& text) {
    std::shared_ptr<WsConnection> conn;
    {
        std::lock_guard<std::mutex> lock(g_wsMutex);
        auto it = g_connections.find(handle);
        if (it == g_connections.end()) return;
        conn = it->second;
    }
    std::lock_guard<std::mutex> lock(conn->outboxMutex);
    conn->outbox.push_back({ 0x1, text });
}

void CloseWebSocket(int handle, int code, const std::string& reason) {
    std::shared_ptr<WsConnection> conn;
    {
        std::lock_guard<std::mutex> lock(g_wsMutex);
        auto it = g_connections.find(handle);
        if (it == g_connections.end()) return;
        conn = it->second;
    }
    std::lock_guard<std::mutex> lock(conn->outboxMutex);
    if (!conn->closeRequested) {
        conn->closeRequested = true;
        conn->closeCode = code;
        conn->closeReason = reason;
    }
}

size_t DrainWebSocketEvents(size_t maxEvents) {
    size_t drained = 0;
    for (;;) {
        PendingWsEvent next;
        {
            std::lock_guard<std::mutex> lock(g_eventMutex);
            if (g_wsEvents.empty() || drained >= maxEvents) break;
            next = std::move(g_wsEvents.front());
            g_wsEvents.pop_front();
        }
        if (next.callback) next.callback(std::move(next.event));
        ++drained;
    }
    return drained;
}

bool HasOpenWebSockets() {
    if (g_liveCount.load() > 0) return true;
    std::lock_guard<std::mutex> lock(g_eventMutex);
    return !g_wsEvents.empty();
}
