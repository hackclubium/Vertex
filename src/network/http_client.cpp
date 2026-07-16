#include "network/http_client.h"
#include "network/arti_client.h"
#include "network/socket.h"
#include "network/tls_socket.h"
#include "network/cookies.h"
#include "network/url.h"
#include "codec/inflate.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace {

struct HttpUrl {
    bool secure = false;
    std::string host;
    std::string hostHeader;
    int port = 80;
    std::string path = "/";
};

bool ParseHttpUrl(const std::string& url, HttpUrl& out) {
    std::string rest;
    if (url.rfind("https://", 0) == 0)     { out.secure = true;  out.port = 443; rest = url.substr(8); }
    else if (url.rfind("http://", 0) == 0) { out.secure = false; out.port = 80;  rest = url.substr(7); }
    else return false;
    if (rest.empty()) return false;

    size_t pathStart = rest.find_first_of("/?#");
    std::string hostport = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);
    if (pathStart == std::string::npos || rest[pathStart] == '#') {
        out.path = "/";
    } else if (rest[pathStart] == '?') {
        out.path = "/" + rest.substr(pathStart);
    } else {
        out.path = rest.substr(pathStart);
    }
    size_t fragment = out.path.find('#');
    if (fragment != std::string::npos) out.path.erase(fragment);
    if (out.path.empty()) out.path = "/";
    if (hostport.empty()) return false;

    if (hostport.front() == '[') {
        size_t close = hostport.find(']');
        if (close == std::string::npos) return false;
        out.host = hostport.substr(1, close - 1);
        if (close + 1 < hostport.size()) {
            if (hostport[close + 1] != ':') return false;
            try { out.port = std::stoi(hostport.substr(close + 2)); } catch (...) { return false; }
        }
    } else {
        size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            out.host = hostport.substr(0, colon);
            try { out.port = std::stoi(hostport.substr(colon + 1)); } catch (...) { return false; }
        } else {
            out.host = hostport;
        }
    }
    if (out.host.empty() || out.port <= 0 || out.port > 65535) return false;
    const bool defaultPort = (!out.secure && out.port == 80) || (out.secure && out.port == 443);
    out.hostHeader = hostport.front() == '[' ? "[" + out.host + "]" : out.host;
    if (!defaultPort) out.hostHeader += ":" + std::to_string(out.port);
    return true;
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string ToUpper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return start == std::string::npos ? "" : s.substr(start, end - start + 1);
}

bool EndsWithOnion(const std::string& host) {
    std::string lower = ToLower(host);
    return lower.size() > 6 && lower.compare(lower.size() - 6, 6, ".onion") == 0;
}

void TorProxy(std::string& host, int& port) {
    host = "127.0.0.1";
    port = 9050;
    const char* env = std::getenv("VERTEX_TOR_SOCKS");
    if (!env || !*env) return;
    std::string value = env;
    size_t colon = value.rfind(':');
    if (colon == std::string::npos) { host = value; return; }
    host = value.substr(0, colon);
    try { port = std::stoi(value.substr(colon + 1)); } catch (...) { port = 9050; }
    if (host.empty()) host = "127.0.0.1";
}

struct HeaderLine { std::string name, value; };

// Splits the raw header block (everything after the status line, up to but
// not including the blank line) into name/value pairs, preserving
// duplicates (a response can legitimately send multiple Set-Cookie lines).
std::vector<HeaderLine> ParseHeaderLines(const std::string& block) {
    std::vector<HeaderLine> out;
    size_t pos = 0;
    while (pos < block.size()) {
        size_t lineEnd = block.find("\r\n", pos);
        if (lineEnd == std::string::npos) lineEnd = block.size();
        std::string line = block.substr(pos, lineEnd - pos);
        pos = lineEnd + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        out.push_back({ ToLower(Trim(line.substr(0, colon))), Trim(line.substr(colon + 1)) });
    }
    return out;
}

std::string FindHeader(const std::vector<HeaderLine>& headers, const char* name) {
    std::string lname = ToLower(name);
    for (auto& h : headers) if (h.name == lname) return h.value;
    return "";
}

constexpr size_t kMaxHeaderBytes = 256 * 1024;

// Reads from the socket, accumulating into `buf`, until "\r\n\r\n" is seen
// (the end of the header block). Returns false on error/timeout/oversized
// headers. Any bytes read past the terminator are left in `buf` past
// `headerEnd` for the caller to treat as the start of the body. Templated
// so the exact same logic drives both the plain-TCP (http://) and TLS
// (https://) transports, which share an identical Recv()/SendAll() shape
// but aren't polymorphic (no virtual base — that overhead isn't needed
// since the transport choice is a one-time decision per request, not a
// runtime-varying one within a single call).
template <typename Sock>
bool ReadHeaders(Sock& sock, std::string& buf, size_t& headerEnd) {
    char chunk[4096];
    for (;;) {
        size_t pos = buf.find("\r\n\r\n");
        if (pos != std::string::npos) { headerEnd = pos; return true; }
        if (buf.size() > kMaxHeaderBytes) return false;
        int n = sock.Recv(chunk, sizeof(chunk));
        if (n <= 0) return false;
        buf.append(chunk, n);
    }
}

// Decodes a chunked-transfer-encoding body. `body` already holds whatever
// bytes were read past the header terminator; more are pulled from the
// socket as needed. Returns false on a malformed chunk or exceeding the cap.
template <typename Sock>
bool ReadChunkedBody(Sock& sock, std::string& body, std::string& out, size_t maxBytes) {
    char chunk[4096];
    auto fillTo = [&](size_t n) -> bool {
        while (body.size() < n) {
            int r = sock.Recv(chunk, sizeof(chunk));
            if (r <= 0) return false;
            body.append(chunk, r);
        }
        return true;
    };
    size_t pos = 0;
    for (;;) {
        size_t lineEnd;
        for (;;) {
            std::string::size_type found = body.find("\r\n", pos);
            if (found != std::string::npos) { lineEnd = found; break; }
            int r = sock.Recv(chunk, sizeof(chunk));
            if (r <= 0) return false;
            body.append(chunk, r);
        }
        std::string sizeLine = body.substr(pos, lineEnd - pos);
        size_t semi = sizeLine.find(';'); // chunk extensions, ignored
        if (semi != std::string::npos) sizeLine = sizeLine.substr(0, semi);
        size_t chunkSize = 0;
        try { chunkSize = std::stoul(Trim(sizeLine), nullptr, 16); } catch (...) { return false; }
        pos = lineEnd + 2;

        if (chunkSize == 0) return true; // final chunk — ignore any trailer headers after it

        if (out.size() + chunkSize > maxBytes) return false;
        if (!fillTo(pos + chunkSize + 2)) return false; // +2 for the chunk's trailing CRLF
        out.append(body, pos, chunkSize);
        pos += chunkSize + 2;

        // Keep the buffer from growing unboundedly across many chunks.
        // Clean up more aggressively to handle many small chunks efficiently.
        if (pos > 8192) { body.erase(0, pos); pos = 0; }
    }
}

// Runs one full request/response over an already-connected socket (plain or
// TLS). Returns false only for hard transport failures (r.error is set);
// HTTP-level outcomes (redirects, 4xx/5xx) are reported via `r` itself.
template <typename Sock>
bool PerformRequest(Sock& sock, const HttpUrl& parsed, const std::string& currentUrl,
                     const FetchRequest& fetchRequest,
                      size_t maxResponseBytes, FetchResult& r,
                      std::string& outBody, std::vector<HeaderLine>& headers) {
    // Validate host and path to prevent header injection via CRLF
    for (char c : parsed.hostHeader) {
        if (c == '\r' || c == '\n') { r.error = "Invalid host"; return false; }
    }
    for (char c : parsed.path) {
        if (c == '\r' || c == '\n') { r.error = "Invalid path"; return false; }
    }
    std::string method = fetchRequest.method.empty() ? "GET" : ToUpper(fetchRequest.method);
    if (method != "GET" && method != "POST") { r.error = "Unsupported HTTP method"; return false; }
    for (char c : method) {
        if (c == '\r' || c == '\n' || c == ' ') { r.error = "Invalid method"; return false; }
    }
    for (char c : fetchRequest.contentType) {
        if (c == '\r' || c == '\n') { r.error = "Invalid content type"; return false; }
    }
    std::string cookieHeader = CookieJar::instance().cookieHeader(currentUrl);
    std::string request =
        method + " " + parsed.path + " HTTP/1.1\r\n"
        "Host: " + parsed.hostHeader + "\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Vertex/0.1 (+https://github.com/vertex-browser)\r\n"
        "Accept-Encoding: gzip\r\n"
        "Connection: close\r\n";
    if (!cookieHeader.empty()) request += "Cookie: " + cookieHeader + "\r\n";
    if (method == "POST") {
        request += "Content-Type: " + (fetchRequest.contentType.empty() ? "application/x-www-form-urlencoded" : fetchRequest.contentType) + "\r\n";
        request += "Content-Length: " + std::to_string(fetchRequest.body.size()) + "\r\n";
    }
    request += "\r\n";
    if (method == "POST") request += fetchRequest.body;

    if (!sock.SendAll(request.data(), request.size())) { r.error = "Failed to send request"; return false; }

    std::string buf;
    size_t headerEnd = 0;
    if (!ReadHeaders(sock, buf, headerEnd)) { r.error = "Failed to read response headers"; return false; }

    std::string headerBlock = buf.substr(0, headerEnd);
    std::string body = buf.substr(headerEnd + 4); // bytes already read past the header terminator

    size_t firstLineEnd = headerBlock.find("\r\n");
    if (firstLineEnd == std::string::npos) { r.error = "Malformed status line"; return false; }
    std::string statusLine = headerBlock.substr(0, firstLineEnd);
    size_t sp1 = statusLine.find(' ');
    if (sp1 == std::string::npos) { r.error = "Malformed status line"; return false; }
    try { r.status = std::stoi(statusLine.substr(sp1 + 1, 3)); }
    catch (...) { r.error = "Malformed status line"; return false; }

    headers = ParseHeaderLines(headerBlock.substr(firstLineEnd + 2));

    std::string transferEncoding = ToLower(FindHeader(headers, "transfer-encoding"));
    std::string contentLengthStr = FindHeader(headers, "content-length");

    if (transferEncoding.find("chunked") != std::string::npos) {
        if (!ReadChunkedBody(sock, body, outBody, maxResponseBytes)) {
            r.error = "Malformed chunked response or size limit exceeded";
            return false;
        }
    } else if (!contentLengthStr.empty()) {
        size_t contentLength = 0;
        try { contentLength = std::stoul(contentLengthStr); }
        catch (...) { r.error = "Malformed Content-Length"; return false; }
        if (contentLength > maxResponseBytes) { r.error = "Response exceeds size limit"; return false; }
        char chunk[8192];
        while (body.size() < contentLength) {
            int n = sock.Recv(chunk, sizeof(chunk));
            if (n <= 0) { r.error = "Truncated response body"; return false; }
            body.append(chunk, n);
        }
        outBody = body.substr(0, std::min(body.size(), contentLength));
    } else {
        // No framing header — read until the connection closes.
        outBody = std::move(body);
        char chunk[8192];
        for (;;) {
            int n = sock.Recv(chunk, sizeof(chunk));
            if (n <= 0) break;
            if (outBody.size() + (size_t)n > maxResponseBytes) { r.error = "Response exceeds size limit"; return false; }
            outBody.append(chunk, n);
        }
    }
    return true;
}

} // namespace

FetchResult FetchHttp(const std::string& url, size_t maxResponseBytes) {
    FetchRequest request;
    request.url = url;
    return FetchHttp(request, maxResponseBytes);
}

FetchResult FetchHttp(const FetchRequest& request, size_t maxResponseBytes) {
    FetchResult r;
    std::string currentUrl = request.url;
    FetchRequest currentRequest = request;
    std::set<std::string> visited;

    for (int redirects = 0; redirects <= 10; redirects++) {
        if (visited.count(currentUrl)) {
            r.error = "Redirect cycle detected";
            return r;
        }
        visited.insert(currentUrl);
        if (IsOnionUrl(currentUrl) && std::getenv("VERTEX_TOR_SOCKS") == nullptr) {
            if (FetchViaEmbeddedArti(currentUrl, maxResponseBytes, r)) return r;
        }
        
        HttpUrl parsed;
        if (!ParseHttpUrl(currentUrl, parsed)) { r.error = "Unsupported or malformed URL"; return r; }

        std::string outBody;
        std::vector<HeaderLine> headers;
        bool ok;
        const bool onion = EndsWithOnion(parsed.host);
        if (parsed.secure) {
            TlsConnection tls;
            if (onion) {
                std::string proxyHost;
                int proxyPort = 0;
                TorProxy(proxyHost, proxyPort);
                if (!tls.ConnectSocks5(proxyHost, proxyPort, parsed.host, parsed.port)) { r.error = "Tor SOCKS5 TLS connection failed"; return r; }
            } else if (!tls.Connect(parsed.host, parsed.port)) { r.error = "TLS connection failed"; return r; }
            ok = PerformRequest(tls, parsed, currentUrl, currentRequest, maxResponseBytes, r, outBody, headers);
        } else {
            TcpSocket sock;
            if (onion) {
                std::string proxyHost;
                int proxyPort = 0;
                TorProxy(proxyHost, proxyPort);
                if (!sock.ConnectSocks5(proxyHost, proxyPort, parsed.host, parsed.port)) { r.error = "Tor SOCKS5 connection failed"; return r; }
            } else if (!sock.Connect(parsed.host, parsed.port)) { r.error = "Connection failed"; return r; }
            ok = PerformRequest(sock, parsed, currentUrl, currentRequest, maxResponseBytes, r, outBody, headers);
        }
        if (!ok) return r;

        std::string contentEncoding = ToLower(FindHeader(headers, "content-encoding"));
        if (contentEncoding == "gzip") {
            std::string decompressed;
            if (GzipInflate((const uint8_t*)outBody.data(), outBody.size(), decompressed, maxResponseBytes))
                outBody = std::move(decompressed);
            else { r.error = "Malformed or oversized gzip response"; return r; }
        } else if (contentEncoding == "deflate") {
            std::string decompressed;
            if (ZlibInflate((const uint8_t*)outBody.data(), outBody.size(), decompressed, maxResponseBytes) ||
                Inflate((const uint8_t*)outBody.data(), outBody.size(), decompressed, maxResponseBytes)) {
                outBody = std::move(decompressed);
            } else { r.error = "Malformed or oversized deflate response"; return r; }
        }

        for (auto& h : headers) {
            if (h.name == "set-cookie") CookieJar::instance().handleSetCookie(h.value, currentUrl);
        }

        if (r.status >= 300 && r.status < 400) {
            std::string location = FindHeader(headers, "location");
            if (!location.empty() && redirects < 10) {
                currentUrl = ResolveUrlAgainstBase(location, currentUrl);
                currentRequest.url = currentUrl;
                if (r.status == 303 || ((r.status == 301 || r.status == 302) && ToUpper(currentRequest.method) == "POST")) {
                    currentRequest.method = "GET";
                    currentRequest.body.clear();
                    currentRequest.contentType.clear();
                }
                continue;
            }
        }

        r.body = std::move(outBody);
        r.contentType = FindHeader(headers, "content-type");
        size_t semi = r.contentType.find(';');
        if (semi != std::string::npos) r.contentType = Trim(r.contentType.substr(0, semi));
        r.contentDisposition = FindHeader(headers, "content-disposition");
        r.finalUrl = currentUrl;
        r.success = r.status < 400;
        if (!r.success) { r.error = "HTTP " + std::to_string(r.status); r.body.clear(); }
        return r;
    }

    r.error = "Too many redirects";
    return r;
}
