#include "fixture.h"

#include "network/fetcher.h"
#include "network/cookies.h"
#include "network/resource_cache.h"
#include "network/text_decode.h"
#include "network/url.h"
#include "network/websocket.h"
#include "network/http_client.h"
#include "platform/downloads.h"
#include "platform/profile.h"
#include "html/parser.h"
#include "html/resources.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

namespace {

#ifdef _WIN32
using SockFd = SOCKET;
constexpr SockFd kInvalidSock = INVALID_SOCKET;
#else
using SockFd = int;
constexpr SockFd kInvalidSock = -1;
#endif

std::string ExtractHeaderValue(const std::string& headers, const std::string& name) {
    std::string lower = headers;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    std::string needle = name;
    for (auto& c : needle) c = (char)tolower((unsigned char)c);
    size_t pos = lower.find(needle);
    if (pos == std::string::npos) return "";
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

// Encodes an unmasked server-to-client frame. Short payloads only (<=125
// bytes) — this test server only ever talks to Vertex's own WebSocket
// client in a controlled test, never a real browser, so the 126/127
// extended-length cases aren't needed here (they're already covered by
// websocket.cpp's own frame codec, which this integration test exercises
// from the client side regardless).
std::string ServerEncodeFrame(uint8_t opcode, const std::string& payload) {
    std::string out;
    out.push_back((char)(0x80 | opcode));
    out.push_back((char)payload.size());
    out += payload;
    return out;
}

// Minimal single-connection RFC 6455 test server: completes the handshake
// (via the same ComputeWebSocketAccept() Vertex's real client uses to
// validate a server's response), echoes back whatever text it receives, and
// answers "__SERVER_CLOSE__" with a server-initiated close — giving this
// suite a real socket to drive OpenWebSocket()/SendWebSocketText()/
// CloseWebSocket() against without any external server or network access.
struct TestWsServer {
    int port = 0;
    std::thread serverThread;

    void start() {
#ifdef _WIN32
        static bool wsaInited = [] { WSADATA wsa; return WSAStartup(MAKEWORD(2, 2), &wsa) == 0; }();
        (void)wsaInited;
#endif
        SockFd listenSock = (SockFd)socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral — avoids port collisions between test runs
        bind(listenSock, (sockaddr*)&addr, sizeof(addr));
        listen(listenSock, 1);
        socklen_t len = sizeof(addr);
        getsockname(listenSock, (sockaddr*)&addr, &len);
        port = ntohs(addr.sin_port);

        serverThread = std::thread([listenSock]() {
            SockFd clientSock = accept(listenSock, nullptr, nullptr);
            closesocket(listenSock);
            if (clientSock == kInvalidSock) return;

            std::string buf;
            char tmp[2048];
            auto recvMore = [&]() -> bool {
                int n = recv(clientSock, tmp, sizeof(tmp), 0);
                if (n <= 0) return false;
                buf.append(tmp, n);
                return true;
            };

            while (buf.find("\r\n\r\n") == std::string::npos)
                if (!recvMore()) { closesocket(clientSock); return; }
            size_t headerEnd = buf.find("\r\n\r\n");
            std::string headers = buf.substr(0, headerEnd);
            buf.erase(0, headerEnd + 4);

            std::string key = ExtractHeaderValue(headers, "sec-websocket-key");
            std::string accept = ComputeWebSocketAccept(key);
            std::string resp =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
            send(clientSock, resp.data(), (int)resp.size(), 0);

            for (;;) {
                while (buf.size() < 2)
                    if (!recvMore()) { closesocket(clientSock); return; }
                uint8_t b1 = (uint8_t)buf[1];
                uint8_t opcode = (uint8_t)buf[0] & 0x0F;
                uint64_t plen = b1 & 0x7F; // client frames in this test never exceed 125 bytes
                size_t pos = 2;
                while (buf.size() < pos + 4 + plen)
                    if (!recvMore()) { closesocket(clientSock); return; }
                uint8_t maskKey[4];
                memcpy(maskKey, buf.data() + pos, 4);
                pos += 4;
                std::string payload = buf.substr(pos, (size_t)plen);
                for (size_t i = 0; i < payload.size(); i++)
                    payload[i] = (char)((uint8_t)payload[i] ^ maskKey[i % 4]);
                buf.erase(0, pos + (size_t)plen);

                if (opcode == 0x8) {
                    std::string echoClose = ServerEncodeFrame(0x8, payload);
                    send(clientSock, echoClose.data(), (int)echoClose.size(), 0);
                    closesocket(clientSock);
                    return;
                }
                if (payload == "__SERVER_CLOSE__") {
                    std::string closePayload;
                    closePayload.push_back((char)0x03);
                    closePayload.push_back((char)0xE8); // 1000
                    std::string closeResp = ServerEncodeFrame(0x8, closePayload);
                    send(clientSock, closeResp.data(), (int)closeResp.size(), 0);
                    closesocket(clientSock);
                    return;
                }
                std::string echoResp = ServerEncodeFrame(opcode, payload);
                send(clientSock, echoResp.data(), (int)echoResp.size(), 0);
            }
        });
    }

    void join() { if (serverThread.joinable()) serverThread.join(); }
};

std::vector<uint8_t> HexToBytesForHttpTest(const std::string& hex) {
    std::vector<uint8_t> out;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back((uint8_t)((val(hex[i]) << 4) | val(hex[i + 1])));
    return out;
}

void HandleHttpTestConnection(SockFd clientSock, int serverPort) {
    std::string req;
    char tmp[4096];
    while (req.find("\r\n\r\n") == std::string::npos) {
        int n = recv(clientSock, tmp, sizeof(tmp), 0);
        if (n <= 0) { closesocket(clientSock); return; }
        req.append(tmp, n);
    }
    size_t lineEnd = req.find("\r\n");
    std::string requestLine = req.substr(0, lineEnd);
    size_t sp1 = requestLine.find(' ');
    size_t sp2 = requestLine.find(' ', sp1 + 1);
    std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
        ? requestLine.substr(sp1 + 1, sp2 - sp1 - 1) : "";

    std::string resp;
    if (path == "/plain") {
        std::string body = "hello from plain route";
        resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    } else if (path == "/chunked") {
        resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n\r\n";
        for (const char* part : { "chunk-one-", "chunk-two-", "chunk-three" }) {
            char lenBuf[16];
            snprintf(lenBuf, sizeof(lenBuf), "%zx", strlen(part));
            resp += std::string(lenBuf) + "\r\n" + part + "\r\n";
        }
        resp += "0\r\n\r\n";
    } else if (path == "/gzip") {
        // gzip-compresses to "hello gzip from python" — same vector already
        // independently verified in codec/inflate/gzip-wrapper.
        auto gz = HexToBytesForHttpTest(
            "1f8b08000000000002ffcb48cdc9c95748afca2c50482bcacf5528a82cc9c8c"
            "f03009c07368816000000");
        resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Encoding: gzip\r\n"
               "Content-Length: " + std::to_string(gz.size()) + "\r\n\r\n";
        resp.append((const char*)gz.data(), gz.size());
    } else if (path == "/redirect1") {
        resp = "HTTP/1.1 302 Found\r\nLocation: /redirect2\r\nContent-Length: 0\r\n\r\n";
    } else if (path == "/redirect2") {
        resp = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + std::to_string(serverPort) +
               "/plain\r\nContent-Length: 0\r\n\r\n";
    } else if (path == "/setcookie") {
        std::string body = "cookie set";
        resp = "HTTP/1.1 200 OK\r\nSet-Cookie: testcookie=abc123; Path=/\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    } else if (path == "/checkcookie") {
        bool hasCookie = req.find("testcookie=abc123") != std::string::npos;
        std::string body = hasCookie ? "has-cookie" : "no-cookie";
        resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    } else if (path == "/noframing") {
        resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbody with no explicit framing";
    } else if (path == "/?q=1") {
        std::string body = "query-without-slash-and-no-fragment";
        resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    } else if (path == "/host") {
        std::string body = ExtractHeaderValue(req, "host");
        resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    } else {
        std::string body = "not found";
        resp = "HTTP/1.1 404 Not Found\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    send(clientSock, resp.data(), (int)resp.size(), 0);
    closesocket(clientSock);
}

// A minimal multi-connection HTTP/1.1 test server: one thread per accepted
// connection (Vertex's own client always sends Connection: close and opens
// a fresh socket per request, including for each hop of a redirect, so this
// needs to serve more than one connection over the test's lifetime, unlike
// TestWsServer above). Runs for the rest of the test process — it's a
// detached background thread, cleaned up by process exit like the rest of
// vertex-tests' per-suite-per-process model.
struct TestHttpServer {
    int port = 0;

    void start() {
#ifdef _WIN32
        static bool wsaInited = [] { WSADATA wsa; return WSAStartup(MAKEWORD(2, 2), &wsa) == 0; }();
        (void)wsaInited;
#endif
        SockFd listenSock = (SockFd)socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(listenSock, (sockaddr*)&addr, sizeof(addr));
        listen(listenSock, 16);
        socklen_t len = sizeof(addr);
        getsockname(listenSock, (sockaddr*)&addr, &len);
        port = ntohs(addr.sin_port);

        std::thread([listenSock, p = port]() {
            for (;;) {
                SockFd clientSock = accept(listenSock, nullptr, nullptr);
                if (clientSock == kInvalidSock) return;
                std::thread(HandleHttpTestConnection, clientSock, p).detach();
            }
        }).detach();
    }
};

} // namespace

static Node* FindScriptById(Node* root, const std::string& id) {
    if (!root) return nullptr;
    if (root->type == NodeType::Element && root->tagName == "script" && root->attr("id") == id)
        return root;
    for (auto& child : root->children)
        if (auto* found = FindScriptById(child.get(), id)) return found;
    return nullptr;
}

TestResult RunNetworkTests() {
    TestResult result;

    {
        auto res = FetchUrl("data:text/plain,Hello%20World%21");
        std::string actual = (res.success ? "success " : "failure ")
            + res.contentType + " " + res.body + "\n";
        ExpectEqual("network/data-url/plain", actual,
            "success text/plain Hello World!\n", result);
    }

    {
        auto res = FetchUrl("data:image/png;base64,QUJDRA%3D%3D");
        std::string actual = (res.success ? "success " : "failure ")
            + res.contentType + " "
            + std::to_string(res.body.size()) + " " + res.body + "\n";
        ExpectEqual("network/data-url/base64", actual,
            "success image/png 4 ABCD\n", result);
    }

    {
        std::filesystem::path path = std::filesystem::temp_directory_path() / "vertex-file-url-test.html";
        {
            std::ofstream out(path, std::ios::binary);
            out << "<h1>local</h1>";
        }
        std::string url = "file:///" + path.generic_string();
        auto res = FetchUrl(url);
        std::filesystem::remove(path);
        std::string actual = (res.success ? "success " : "failure ")
            + res.contentType + " " + res.body + "\n";
        ExpectEqual("network/file-url/local-html",
            actual,
            "success text/html <h1>local</h1>\n",
            result);
    }

    {
        std::string actual;
        actual += vertex::downloads::SuggestFilename(
            "https://example.test/files/report%2026.pdf",
            "attachment; filename=\"invoice?.pdf\"",
            "") + "\n";
        actual += vertex::downloads::SuggestFilename(
            "https://example.test/files/archive.tar.gz?download=1",
            "",
            "my:name.zip") + "\n";
        actual += vertex::downloads::SuggestFilename(
            "data:text/plain,hello",
            "",
            "") + "\n";
        actual += vertex::downloads::MakeUniquePathForTests(
            "C:/Downloads",
            "file.txt",
            { "C:/Downloads/file.txt", "C:/Downloads/file (1).txt" }) + "\n";
        ExpectEqual("network/downloads/filename-selection-and-safe-paths",
            actual,
            "invoice_.pdf\n"
            "my_name.zip\n"
            "download.txt\n"
            "C:/Downloads/file (2).txt\n",
            result);
    }

    {
        std::filesystem::path root = std::filesystem::temp_directory_path() / "vertex-profile-test-data";
        std::filesystem::path cache = std::filesystem::temp_directory_path() / "vertex-profile-test-cache";
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(cache);

        auto paths = vertex::profile::ResolvePaths(root.generic_string(), cache.generic_string(), "Default");
        bool dirs = vertex::profile::EnsureDirectories(paths);
        vertex::profile::AppendTsvRow(paths.historyFile, { "123", "https://example.test/a", "Example" });
        vertex::profile::AppendTsvRow(paths.historyFile, { "124", "https://example.test/b?x=1\t2", "Two\nLines" });
        auto rows = vertex::profile::ReadTsvRows(paths.historyFile);

        std::string actual;
        actual += dirs ? "dirs\n" : "no-dirs\n";
        actual += std::filesystem::exists(paths.profileRoot) ? "profile\n" : "no-profile\n";
        actual += std::filesystem::exists(paths.localStorageDir) ? "local-storage\n" : "no-local-storage\n";
        actual += std::filesystem::exists(paths.cacheProfileRoot) ? "cache\n" : "no-cache\n";
        actual += std::to_string(rows.size()) + "\n";
        actual += rows.size() > 1 && rows[1].size() > 2 ? rows[1][1] + "|" + rows[1][2] + "\n" : "missing\n";

        std::filesystem::remove_all(root);
        std::filesystem::remove_all(cache);

        ExpectEqual("network/profile/paths-and-tsv-storage",
            actual,
            "dirs\nprofile\nlocal-storage\ncache\n2\nhttps://example.test/b?x=1\t2|Two\nLines\n",
            result);
    }

    {
        const std::string cp1252 = "caf\xE9";
        const std::string metaCp1252 = "<meta charset=\"windows-1252\">caf\xE9";
        std::string actual;
        actual += DecodeTextToUtf8(cp1252, "text/html; charset=windows-1252") + "\n";
        actual += DecodeTextToUtf8(metaCp1252, "text/html", true) + "\n";
        actual += DecodeTextToUtf8("\xEF\xBB\xBFhello", "text/html") + "\n";
        ExpectEqual("network/text-decode/headers-meta-and-bom",
            actual,
            "caf\xC3\xA9\n<meta charset=\"windows-1252\">caf\xC3\xA9\nhello\n",
            result);
    }

    {
        CookieJar::instance().handleSetCookie("vertex_ok=1; Domain=example.test; Path=/app", "https://example.test/app/start");
        CookieJar::instance().handleSetCookie("vertex_bad=1; Domain=evil.test; Path=/", "https://example.test/app/start");
        CookieJar::instance().handleSetCookie("vertex_secure=1; Secure; Path=/", "https://example.test/app/start");
        CookieJar::instance().handleSetCookie("vertex_default=1", "https://example.test/app/start");
        CookieJar::instance().setFromJS("vertex_js_http_only=1; HttpOnly; Path=/", "https://example.test/app/start");
        CookieJar::instance().setFromJS("vertex_js_secure_http=1; Secure; Path=/", "http://example.test/app/start");
        CookieJar::instance().setFromJS("vertex_inject=ok\r\nX-Bad: 1; Path=/", "https://example.test/app/start");
        std::string actual;
        actual += CookieJar::instance().cookieHeader("https://example.test/app/page") + "\n";
        actual += CookieJar::instance().cookieHeader("http://example.test/application/page") + "\n";
        actual += CookieJar::instance().cookieHeader("https://example.test/other/page") + "\n";
        actual += CookieJar::instance().documentCookies("http://example.test/app/page") + "\n";
        ExpectEqual("network/cookies/domain-path-and-secure-boundaries",
            actual,
            "vertex_ok=1; vertex_secure=1; vertex_default=1\n"
            "\n"
            "vertex_secure=1\n"
            "vertex_ok=1; vertex_default=1\n",
            result);
    }

    {
        std::string actual;
        actual += ResolveUrlAgainstBase("data:text/css,.picture%7Bbackground%3Anone%7D",
            "https://www.webstandards.org/files/acid2/test.html") + "\n";
        actual += ResolveUrlAgainstBase("/files/acid2/reference.html",
            "https://www.webstandards.org/files/acid2/test.html") + "\n";
        actual += ResolveUrlAgainstBase("reference.html",
            "https://www.webstandards.org/files/acid2/test.html") + "\n";
        actual += ResolveUrlAgainstBase("../img/logo.png?x=1#top",
            "https://example.test/a/b/page.html?old=1#frag") + "\n";
        actual += ResolveUrlAgainstBase("?next=1",
            "https://example.test/a/b/page.html?old=1#frag") + "\n";
        actual += ResolveUrlAgainstBase("#section",
            "https://example.test/a/b/page.html?old=1#frag") + "\n";
        ExpectEqual("network/resolve-url/scheme-and-relative",
            actual,
            "data:text/css,.picture%7Bbackground%3Anone%7D\n"
            "https://www.webstandards.org/files/acid2/reference.html\n"
            "https://www.webstandards.org/files/acid2/reference.html\n"
            "https://example.test/a/img/logo.png?x=1#top\n"
            "https://example.test/a/b/page.html?next=1\n"
            "https://example.test/a/b/page.html?old=1#section\n",
            result);
    }

    {
        const std::string bing = "https://www.bing.com/ck/a?x=1&amp;u=a1aHR0cHM6Ly9oZWxpdW0uY29tcHV0ZXIv&amp;ntb=1";
        ExpectEqual("network/bing-result-link-opens-and-previews-direct-destination",
            UnwrapBingRedirect(bing) + "\n",
            "https://helium.computer/\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string source = ReadTextFile(root / "src/network/http_client.cpp");
        const bool hasSocketConnect = source.find("TcpSocket sock") != std::string::npos;
        const bool enablesDecode = source.find("Accept-Encoding: gzip") != std::string::npos;
        const bool resolvesRedirects = source.find("r.finalUrl") != std::string::npos;
        const bool handlesCookies = source.find("Cookie:") != std::string::npos
            && source.find("set-cookie") != std::string::npos;
        ExpectEqual("network/http-session-decoding-and-final-url",
            std::string(hasSocketConnect ? "session " : "no-session ")
                + (enablesDecode ? "decode " : "no-decode ")
                + (resolvesRedirects ? "url " : "no-url ")
                + (handlesCookies ? "cookies\n" : "no-cookies\n"),
            "session decode url cookies\n",
            result);
    }

    {
        // TLS verification must stay on. HTTP is now via FetchHttp/http_client.cpp
        // which uses TlsConnection for https:// — assert the hand-rolled TLS
        // backend code directly rather than hitting a real MITM-able endpoint.
        auto root = FindRepoRoot();
        std::string htSource = ReadTextFile(root / "src/network/http_client.cpp");
        const bool hasTlsTransport = htSource.find("TlsConnection") != std::string::npos;
        const bool hasTlsVerify = ReadTextFile(root / "src/network/tls_windows.cpp").find("SCH_CRED_AUTO_CRED_VALIDATION") != std::string::npos
            && ReadTextFile(root / "src/network/tls_linux.cpp").find("MBEDTLS_SSL_VERIFY_REQUIRED") != std::string::npos;
        ExpectEqual("network/https-fetches-use-tls-and-verify-peer",
            std::string(hasTlsTransport ? "tls " : "no-tls ")
                + (hasTlsVerify ? "verify\n" : "no-verify\n"),
            "tls verify\n",
            result);
    }

    {
        // Same reasoning as above, for the hand-rolled TLS backends
        // (tls_windows.cpp/tls_linux.cpp/tls_macos.cpp) — assert the
        // certificate-validation posture directly rather than hitting a
        // real MITM-able endpoint in the automated suite. (Manually
        // verified end-to-end against real HTTPS servers during
        // development — see commit history/memory, not repeatable here.)
        auto root = FindRepoRoot();
        std::string win = ReadTextFile(root / "src/network/tls_windows.cpp");
        std::string linux_ = ReadTextFile(root / "src/network/tls_linux.cpp");
        const bool windowsValidates =
            win.find("SCH_CRED_AUTO_CRED_VALIDATION") != std::string::npos &&
            win.find("SCH_CRED_MANUAL_CRED_VALIDATION") == std::string::npos &&
            win.find("SCH_CRED_IGNORE_NO_REVOCATION_CHECK") == std::string::npos;
        const bool linuxValidates =
            linux_.find("MBEDTLS_SSL_VERIFY_REQUIRED") != std::string::npos &&
            linux_.find("MBEDTLS_SSL_VERIFY_NONE") == std::string::npos &&
            linux_.find("mbedtls_ssl_get_verify_result") != std::string::npos;
        ExpectEqual("network/hand-rolled-tls-verifies-certificates",
            std::string(windowsValidates ? "windows-verifies " : "windows-no-verify ") +
                (linuxValidates ? "linux-verifies\n" : "linux-no-verify\n"),
            "windows-verifies linux-verifies\n",
            result);
    }

    {
        auto document = ParseHtml(
            "<html><head><script id=\"classic\" "
            "src=\"data:text/javascript,window.answer%3D42%3B\"></script></head></html>");
        LoadExternalScriptSources(document, "https://example.test/page.html");
        Node* script = FindScriptById(document.get(), "classic");
        std::string source;
        if (script) {
            for (const auto& child : script->children)
                if (child->type == NodeType::Text) source += child->text;
        }
        ExpectEqual("network/external-classic-script-is-fetched-into-dom-order",
            (script ? script->attr("__vertex_script_filename") : "missing") + "\n" + source + "\n",
            "data:text/javascript,window.answer%3D42%3B\nwindow.answer=42;\n",
            result);
    }

    {
        ResourceCache::instance().clearForTests();
        auto first = FetchResourceCached("data:text/plain,cache-me", 1024, ResourceKind::Script);
        auto second = FetchResourceCached("data:text/plain,cache-me", 1024, ResourceKind::Script);
        const auto stats = ResourceCache::instance().stats();
        std::string actual;
        actual += (first.success ? first.body : "first-fail") + "\n";
        actual += (second.success ? second.body : "second-fail") + "\n";
        actual += "requests=" + std::to_string(stats.requests) + "\n";
        actual += "network=" + std::to_string(stats.networkFetches) + "\n";
        actual += "hits=" + std::to_string(stats.cacheHits) + "\n";
        ExpectEqual("network/resource-cache/reuses-successful-resources",
            actual,
            "cache-me\ncache-me\nrequests=2\nnetwork=1\nhits=1\n",
            result);
    }

    {
        ResourceCache::instance().clearForTests();
        std::string actual = "pending\n";
        FetchResourceAsync("data:text/plain,async-ok", 1024, ResourceKind::Image,
            [&](FetchResult res) {
                actual += res.success ? res.body : res.error;
                actual += "\n";
            });
        actual += HasPendingResourceCompletions() ? "has-pending\n" : "no-pending\n";
        for (int i = 0; i < 200 && actual.find("async-ok\n") == std::string::npos; ++i) {
            DrainResourceCompletions();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        DrainResourceCompletions();
        const auto stats = ResourceCache::instance().stats();
        actual += "requests=" + std::to_string(stats.requests) + "\n";
        actual += "network=" + std::to_string(stats.networkFetches) + "\n";
        ExpectEqual("network/resource-cache/async-completes-on-drain",
            actual,
            "pending\nhas-pending\nasync-ok\nrequests=1\nnetwork=1\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string cache = ReadTextFile(root / "src/network/resource_cache.cpp");
        const bool boundedWorkers =
            cache.find("g_workerThreads") != std::string::npos
            && cache.find("g_asyncJobs") != std::string::npos
            && cache.find(".detach()") == std::string::npos;
        const bool dedupesInflight =
            cache.find("g_inflight") != std::string::npos
            && cache.find("waiters") != std::string::npos;
        ExpectEqual("network/resource-cache/async-uses-worker-pool-and-inflight-dedupe",
            std::string(boundedWorkers ? "pool " : "thread-per-fetch ")
                + (dedupesInflight ? "dedupe\n" : "duplicates\n"),
            "pool dedupe\n",
            result);
    }

    {
        // RFC 6455 §1.3's own worked example — a fixed, well-known input
        // and expected output, so this needs no live server at all.
        ExpectEqual("network/websocket/accept-matches-rfc6455-worked-example",
            ComputeWebSocketAccept("dGhlIHNhbXBsZSBub25jZQ==") + "\n",
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\n",
            result);
    }

    {
        TestWsServer server;
        server.start();
        std::string actual;
        std::atomic<int> events{0};
        std::string lastMessage;
        int lastCloseCode = 0;
        bool lastCloseClean = false;

        int handle = OpenWebSocket("ws://127.0.0.1:" + std::to_string(server.port) + "/",
            [&](WsEvent ev) {
                switch (ev.kind) {
                case WsEventKind::Open:    actual += "open;"; break;
                case WsEventKind::Message: lastMessage = ev.data; actual += "message;"; break;
                case WsEventKind::Close:
                    lastCloseCode = ev.code;
                    lastCloseClean = ev.wasClean;
                    actual += "close;";
                    break;
                case WsEventKind::Error:   actual += "error;"; break;
                }
                ++events;
            });

        auto waitForEvents = [&](int count) {
            for (int i = 0; i < 500 && events.load() < count; ++i) {
                DrainWebSocketEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            DrainWebSocketEvents();
        };

        waitForEvents(1); // open
        SendWebSocketText(handle, "hello vertex");
        waitForEvents(2); // + message
        CloseWebSocket(handle, 1000, "bye");
        waitForEvents(3); // + close
        server.join();

        actual += lastMessage + ";";
        actual += std::to_string(lastCloseCode) + ";";
        actual += (lastCloseClean ? "clean" : "unclean");
        actual += "\n";

        ExpectEqual("network/websocket/open-echo-and-clean-close-round-trip",
            actual,
            "open;message;close;hello vertex;1000;clean\n",
            result);
    }

    {
        TestWsServer server;
        server.start();
        std::atomic<int> events{0};
        int closeCode = 0;

        int handle = OpenWebSocket("ws://127.0.0.1:" + std::to_string(server.port) + "/",
            [&](WsEvent ev) {
                if (ev.kind == WsEventKind::Close) closeCode = ev.code;
                ++events;
            });

        for (int i = 0; i < 500 && events.load() < 1; ++i) {
            DrainWebSocketEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        DrainWebSocketEvents();
        SendWebSocketText(handle, "__SERVER_CLOSE__");
        for (int i = 0; i < 500 && events.load() < 2; ++i) {
            DrainWebSocketEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        DrainWebSocketEvents();
        server.join();

        ExpectEqual("network/websocket/server-initiated-close-is-handled",
            std::to_string(closeCode) + "\n",
            "1000\n",
            result);
    }

    {
        TestHttpServer server;
        server.start();
        std::string base = "http://127.0.0.1:" + std::to_string(server.port);

        auto plain = FetchHttp(base + "/plain");
        ExpectEqual("network/http/plain-content-length-body",
            (plain.success ? "ok:" : "fail:") + plain.body + ":" + plain.contentType + "\n",
            "ok:hello from plain route:text/plain\n",
            result);

        auto chunked = FetchHttp(base + "/chunked");
        ExpectEqual("network/http/chunked-transfer-encoding",
            (chunked.success ? "ok:" : "fail:") + chunked.body + "\n",
            "ok:chunk-one-chunk-two-chunk-three\n",
            result);

        auto gz = FetchHttp(base + "/gzip");
        ExpectEqual("network/http/gzip-content-encoding",
            (gz.success ? "ok:" : "fail:") + gz.body + "\n",
            "ok:hello gzip from python\n",
            result);

        auto redirected = FetchHttp(base + "/redirect1");
        ExpectEqual("network/http/multi-hop-redirect-chain",
            (redirected.success ? "ok:" : "fail:") + redirected.body + ":" + redirected.finalUrl + "\n",
            "ok:hello from plain route:" + base + "/plain\n",
            result);

        FetchHttp(base + "/setcookie");
        auto cookieCheck = FetchHttp(base + "/checkcookie");
        ExpectEqual("network/http/cookie-set-then-sent-on-next-request",
            (cookieCheck.success ? "ok:" : "fail:") + cookieCheck.body + "\n",
            "ok:has-cookie\n",
            result);

        auto noFraming = FetchHttp(base + "/noframing");
        ExpectEqual("network/http/no-explicit-framing-reads-until-close",
            (noFraming.success ? "ok:" : "fail:") + noFraming.body + "\n",
            "ok:body with no explicit framing\n",
            result);

        auto queryOnly = FetchHttp("http://127.0.0.1:" + std::to_string(server.port) + "?q=1#client-fragment");
        ExpectEqual("network/http/query-only-url-strips-client-fragment",
            (queryOnly.success ? "ok:" : "fail:") + queryOnly.body + "\n",
            "ok:query-without-slash-and-no-fragment\n",
            result);

        auto hostHeader = FetchHttp(base + "/host");
        ExpectEqual("network/http/host-header-keeps-non-default-port",
            (hostHeader.success ? "ok:" : "fail:") + hostHeader.body + "\n",
            "ok:127.0.0.1:" + std::to_string(server.port) + "\n",
            result);

        auto notFound = FetchHttp(base + "/does-not-exist");
        ExpectEqual("network/http/404-is-reported-as-failure",
            std::string(notFound.success ? "unexpected-ok " : "failed ") + std::to_string(notFound.status) + "\n",
            "failed 404\n",
            result);
    }

    return result;
}
