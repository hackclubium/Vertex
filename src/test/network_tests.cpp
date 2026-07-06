#include "test/fixture.h"

#include "network/fetcher.h"
#include "network/resource_cache.h"
#include "network/text_decode.h"
#include "network/url.h"
#include "network/websocket.h"
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
        std::string actual;
        actual += ResolveUrlAgainstBase("data:text/css,.picture%7Bbackground%3Anone%7D",
            "https://www.webstandards.org/files/acid2/test.html") + "\n";
        actual += ResolveUrlAgainstBase("/files/acid2/reference.html",
            "https://www.webstandards.org/files/acid2/test.html") + "\n";
        actual += ResolveUrlAgainstBase("reference.html",
            "https://www.webstandards.org/files/acid2/test.html") + "\n";
        ExpectEqual("network/resolve-url/scheme-and-relative",
            actual,
            "data:text/css,.picture%7Bbackground%3Anone%7D\n"
            "https://www.webstandards.org/files/acid2/reference.html\n"
            "https://www.webstandards.org/files/acid2/reference.html\n",
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
        std::string source = ReadTextFile(root / "src/network/fetcher.cpp");
        const bool hasSession = source.find("EnsureCurlInit") != std::string::npos
            && source.find("curl_easy_init") != std::string::npos;
        const bool enablesDecode = source.find("CURLOPT_ACCEPT_ENCODING") != std::string::npos;
        const bool resolvesRedirects = source.find("CURLINFO_EFFECTIVE_URL") != std::string::npos;
        ExpectEqual("network/http-session-decoding-and-final-url",
            std::string(hasSession ? "session " : "no-session ")
                + (enablesDecode ? "decode " : "no-decode ")
                + (resolvesRedirects ? "url\n" : "no-url\n"),
            "session decode url\n",
            result);
    }

    {
        // TLS verification must stay on — a live bad-cert fetch would be
        // slow/flaky/offline-hostile in CI, so this asserts the curl options
        // directly rather than hitting a real MITM-able endpoint.
        auto root = FindRepoRoot();
        std::string source = ReadTextFile(root / "src/network/fetcher.cpp");
        const bool verifiesPeer = source.find("CURLOPT_SSL_VERIFYPEER, 1L") != std::string::npos;
        const bool verifiesHost = source.find("CURLOPT_SSL_VERIFYHOST, 2L") != std::string::npos;
        const bool disablesPeer = source.find("CURLOPT_SSL_VERIFYPEER, 0L") != std::string::npos;
        const bool disablesHost = source.find("CURLOPT_SSL_VERIFYHOST, 0L") != std::string::npos;
        ExpectEqual("network/https-fetches-verify-peer-and-host",
            std::string(verifiesPeer && !disablesPeer ? "verify-peer " : "no-verify-peer ")
                + (verifiesHost && !disablesHost ? "verify-host\n" : "no-verify-host\n"),
            "verify-peer verify-host\n",
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

    return result;
}
