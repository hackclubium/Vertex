#include "test/fixture.h"

#include "network/fetcher.h"
#include "network/resource_cache.h"
#include "network/text_decode.h"
#include "network/url.h"
#include "platform/downloads.h"
#include "html/parser.h"
#include "html/resources.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

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

    return result;
}
