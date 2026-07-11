// site_probe.cpp — real-site compatibility harness.
// Fetches a page, parses DOM, loads bounded external scripts, runs JS, lays out,
// and prints enough stats/errors to drive crash-first compatibility work.

#include "css/stylesheet.h"
#include "html/parser.h"
#include "html/resources.h"
#include "js/engine.h"
#include "layout/layout_engine.h"
#include "network/resource_cache.h"
#include "network/text_decode.h"
#include "network/url.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct ProbeMeasure : ITextMeasure {
    float MeasureText(const std::wstring& s, const FontKey& f) override { return (float)s.size() * f.size * 0.5f; }
    float SpaceWidth(const FontKey& f) override { return f.size * 0.3f; }
    bool ImageIntrinsic(const std::string&, float& w, float& h) override { w = 0; h = 0; return false; }
    void RequestImage(const std::string&) override {}
};

static std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string TextContent(const Node* n) {
    if (!n) return {};
    if (n->type == NodeType::Text) return n->text;
    std::string out;
    for (const auto& c : n->children) out += TextContent(c.get());
    return out;
}

static std::string PageTitle(const Node* root) {
    std::vector<const Node*> stack{root};
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        if (n && n->type == NodeType::Element && n->tagName == "title") return TextContent(n);
        if (n) for (const auto& c : n->children) stack.push_back(c.get());
    }
    return {};
}

static Stylesheet CollectCss(const Node* root) {
    Stylesheet sheet;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tagName == "style") {
            Stylesheet part = ParseStylesheet(TextContent(n));
            if (part.rootRemBaseSet) { sheet.rootRemBase = part.rootRemBase; sheet.rootRemBaseSet = true; }
            for (auto& r : part.rules) sheet.rules.push_back(std::move(r));
        }
        for (const auto& c : n->children) walk(c.get());
    };
    walk(root);
    sheet.rebuildRuleBuckets();
    return sheet;
}

static bool IsScriptNode(const Node* n) {
    if (!n || n->type != NodeType::Element || n->tagName != "script") return false;
    std::string type = Lower(n->attr("type"));
    return type.empty() || type == "text/javascript" || type == "application/javascript"
        || type == "application/ecmascript" || type == "text/ecmascript";
}

static std::vector<Node*> Scripts(Node* root) {
    std::vector<Node*> out;
    std::vector<Node*> stack{root};
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        if (IsScriptNode(n)) out.push_back(n);
        if (n) for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.push_back(it->get());
    }
    return out;
}

static std::string NormalizeUrl(std::string url) {
    if (HasUrlScheme(url)) return url;
    return "https://" + url;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: site_probe URL [max_scripts]\n");
        return 2;
    }
    const std::string url = NormalizeUrl(argv[1]);
    const int maxScripts = argc > 2 ? std::max(0, std::atoi(argv[2])) : 64;

    std::printf("url=%s\n", url.c_str());
    FetchResult docRes = FetchResourceCached(url, 12 * 1024 * 1024, ResourceKind::Document);
    std::printf("fetch success=%d status=%d bytes=%zu type=%s final=%s error=%s\n",
        docRes.success ? 1 : 0, docRes.status, docRes.body.size(), docRes.contentType.c_str(),
        docRes.finalUrl.c_str(), docRes.error.c_str());
    if (!docRes.success) return 1;

    const std::string finalUrl = docRes.finalUrl.empty() ? url : docRes.finalUrl;
    std::string html = DecodeTextToUtf8(docRes.body, docRes.contentType);
    auto dom = ParseHtml(html);
    std::printf("title=%s\n", PageTitle(dom.get()).c_str());

    LoadExternalScriptSources(dom, finalUrl);
    auto scripts = Scripts(dom.get());
    std::printf("scripts=%zu max=%d\n", scripts.size(), maxScripts);

    JsEngine js;
    JsScriptBudget budget;
    budget.maxScriptBytes = 256 * 1024;
    js.setScriptBudget(budget);
    js.setDocument(dom, []() {}, finalUrl);

    int attempted = 0;
    int failed = 0;
    for (Node* script : scripts) {
        if (attempted >= maxScripts) break;
        std::string source = TextContent(script);
        if (source.empty()) continue;
        std::string filename = script->attr("__vertex_script_filename");
        if (filename.empty()) filename = "inline:" + std::to_string(attempted);
        std::printf("script[%d] bytes=%zu file=%s\n", attempted, source.size(), filename.c_str());
        bool ok = false;
        try {
            ok = js.runScript(source, filename);
        } catch (const std::exception& e) {
            std::printf("script[%d] native_exception=%s\n", attempted, e.what());
        } catch (...) {
            std::printf("script[%d] native_exception=unknown\n", attempted);
        }
        if (!ok) ++failed;
        ++attempted;
    }
    try { js.runMacrotasks(32); } catch (...) { std::printf("macrotasks_exception=1\n"); }

    Stylesheet sheet = CollectCss(dom.get());
    ProbeMeasure measure;
    LayoutInput in;
    in.document = dom.get();
    in.sheet = &sheet;
    in.measure = &measure;
    in.viewportW = 1366.f;
    in.viewportH = 768.f;
    in.zoom = 1.f;
    in.baseUrl = finalUrl;
    auto layout = LayoutDocument(in);
    std::printf("layout=%s docH=%.0f\n", layout ? "ok" : "null", layout ? layout->contentH : 0.f);

    JsScriptStats stats = js.scriptStats();
    std::printf("summary attempted=%d failed=%d stats_attempted=%zu runtime_failures=%zu parse_failures=%zu skipped=%zu parse_ms=%.2f run_ms=%.2f\n",
        attempted, failed, stats.scriptsAttempted, stats.runtimeFailures, stats.parseFailures,
        stats.scriptsSkippedByBudget, stats.parseMs, stats.compileRunMs);
    return failed == 0 ? 0 : 3;
}
