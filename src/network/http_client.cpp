#include "network/http_client.h"
#include "network/socket.h"
#include "network/tls_socket.h"
#include "network/cookies.h"
#include "network/url.h"
#include "codec/inflate.h"
#include <algorithm>
#include <cctype>

namespace {

struct HttpUrl {
    bool secure = false;
    std::string host;
    int port = 80;
    std::string path = "/";
};

bool ParseHttpUrl(const std::string& url, HttpUrl& out) {
    std::string rest;
    if (url.rfind("https://", 0) == 0)     { out.secure = true;  out.port = 443; rest = url.substr(8); }
    else if (url.rfind("http://", 0) == 0) { out.secure = false; out.port = 80;  rest = url.substr(7); }
    else return false;
    if (rest.empty()) return false;

    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (hostport.empty()) return false;

    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        try { out.port = std::stoi(hostport.substr(colon + 1)); } catch (...) { return false; }
    } else {
        out.host = hostport;
    }
    return !out.host.empty();
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return start == std::string::npos ? "" : s.substr(start, end - start + 1);
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
                     size_t maxResponseBytes, FetchResult& r,
                     std::string& outBody, std::vector<HeaderLine>& headers) {
    // Validate host and path to prevent header injection via CRLF
    for (char c : parsed.host) {
        if (c == '\r' || c == '\n') { r.error = "Invalid host"; return false; }
    }
    for (char c : parsed.path) {
        if (c == '\r' || c == '\n') { r.error = "Invalid path"; return false; }
    }
    std::string cookieHeader = CookieJar::instance().cookieHeader(currentUrl);
    std::string request =
        "GET " + parsed.path + " HTTP/1.1\r\n"
        "Host: " + parsed.host + "\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Vertex/0.1 (+https://github.com/vertex-browser)\r\n"
        "Accept-Encoding: gzip\r\n"
        "Connection: close\r\n";
    if (!cookieHeader.empty()) request += "Cookie: " + cookieHeader + "\r\n";
    request += "\r\n";

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
            if (n <= 0) break; // peer closed early — treat what we have as final (matches lenient real-world behavior)
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
    FetchResult r;
    std::string currentUrl = url;
    std::set<std::string> visited;

    for (int redirects = 0; redirects <= 10; redirects++) {
        if (visited.count(currentUrl)) {
            r.error = "Redirect cycle detected";
            return r;
        }
        visited.insert(currentUrl);
        
        HttpUrl parsed;
        if (!ParseHttpUrl(currentUrl, parsed)) { r.error = "Unsupported or malformed URL"; return r; }

        std::string outBody;
        std::vector<HeaderLine> headers;
        bool ok;
        if (parsed.secure) {
            TlsConnection tls;
            if (!tls.Connect(parsed.host, parsed.port)) { r.error = "TLS connection failed"; return r; }
            ok = PerformRequest(tls, parsed, currentUrl, maxResponseBytes, r, outBody, headers);
        } else {
            TcpSocket sock;
            if (!sock.Connect(parsed.host, parsed.port)) { r.error = "Connection failed"; return r; }
            ok = PerformRequest(sock, parsed, currentUrl, maxResponseBytes, r, outBody, headers);
        }
        if (!ok) return r;

        std::string contentEncoding = ToLower(FindHeader(headers, "content-encoding"));
        if (contentEncoding == "gzip") {
            std::string decompressed;
            if (GzipInflate((const uint8_t*)outBody.data(), outBody.size(), decompressed))
                outBody = std::move(decompressed);
            // A body that fails to decompress is passed through as-is rather
            // than failing the whole fetch — matches curl's own tolerance
            // for a mislabeled/malformed Content-Encoding.
        } else if (contentEncoding == "deflate") {
            std::string decompressed;
            if (ZlibInflate((const uint8_t*)outBody.data(), outBody.size(), decompressed) ||
                Inflate((const uint8_t*)outBody.data(), outBody.size(), decompressed)) {
                outBody = std::move(decompressed);
            }
        }

        for (auto& h : headers) {
            if (h.name == "set-cookie") CookieJar::instance().handleSetCookie(h.value, currentUrl);
        }

        if (r.status >= 300 && r.status < 400) {
            std::string location = FindHeader(headers, "location");
            if (!location.empty() && redirects < 10) {
                currentUrl = ResolveUrlAgainstBase(location, currentUrl);
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
