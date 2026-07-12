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

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI SehHandler(EXCEPTION_POINTERS* p) {
    DWORD code = p->ExceptionRecord->ExceptionCode;
    void* addr = p->ExceptionRecord->ExceptionAddress;
    std::fprintf(stderr, "CRASH: exception=0x%08lX addr=%p\n", code, addr);

    HANDLE hProcess = GetCurrentProcess();
    SymInitialize(hProcess, nullptr, TRUE);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

    void* stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, nullptr);
    for (USHORT i = 0; i < frames; ++i) {
        DWORD64 addr64 = (DWORD64)stack[i];
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
        DWORD lineDisp = 0;
        if (SymFromAddr(hProcess, addr64, &disp, sym))
            std::fprintf(stderr, "  [%u] %s+0x%llX", i, sym->Name, disp);
        else
            std::fprintf(stderr, "  [%u] 0x%llX", i, addr64);
        if (SymGetLineFromAddr64(hProcess, addr64, &lineDisp, &line))
            std::fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
        std::fprintf(stderr, "\n");
    }
    SymCleanup(hProcess);

    std::fflush(stderr);
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

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
        if (n && n->type == NodeType::Element && Lower(n->tagName) == "title") return TextContent(n);
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

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    std::fflush(stdout);
    va_end(ap);
}

static std::string Truncate(const std::string& s, size_t limit) {
    if (s.size() <= limit) return s;
    return s.substr(0, limit) + "...[" + std::to_string(s.size()) + "]";
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(SehHandler);
#endif

    if (argc < 2) {
        std::fprintf(stderr, "usage: site_probe URL [max_scripts] [--verbose]\n");
        return 2;
    }
    const std::string url = NormalizeUrl(argv[1]);
    int maxScripts = 64;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--verbose" || arg == "-v") verbose = true;
        else maxScripts = std::max(0, std::atoi(argv[i]));
    }

    Log("=== %s ===\n", url.c_str());

    // — Fetch —
    FetchResult docRes = FetchResourceCached(url, 12 * 1024 * 1024, ResourceKind::Document);
    Log("fetch ok=%d status=%d bytes=%zu type=%s final=%s\n",
        docRes.success ? 1 : 0, docRes.status, docRes.body.size(),
        docRes.contentType.c_str(), docRes.finalUrl.c_str());
    if (!docRes.success) {
        Log("FAIL fetch error=%s\n", docRes.error.c_str());
        return 1;
    }

    const std::string finalUrl = docRes.finalUrl.empty() ? url : docRes.finalUrl;

    // — Parse HTML —
    std::string html = DecodeTextToUtf8(docRes.body, docRes.contentType);
    auto dom = ParseHtml(html);

    // Count DOM nodes for quick health check
    size_t domNodeCount = 0;
    {
        std::vector<const Node*> s{dom.get()};
        while (!s.empty()) { auto* n = s.back(); s.pop_back(); ++domNodeCount; if (n) for (auto& c : n->children) s.push_back(c.get()); }
    }
    Log("dom nodes=%zu html_bytes=%zu\n", domNodeCount, html.size());
    Log("title=%s\n", Truncate(PageTitle(dom.get()), 120).c_str());

    // — Load external scripts —
    LoadExternalScriptSources(dom, finalUrl);
    auto scripts = Scripts(dom.get());

    // Count inline vs external
    size_t inlineScripts = 0;
    size_t externalScripts = 0;
    size_t totalScriptBytes = 0;
    for (Node* s : scripts) {
        std::string src = TextContent(s);
        totalScriptBytes += src.size();
        if (s->attr("__vertex_script_filename").empty()) ++inlineScripts;
        else ++externalScripts;
    }
    Log("scripts total=%zu inline=%zu external=%zu script_bytes=%zu max=%d\n",
        scripts.size(), inlineScripts, externalScripts, totalScriptBytes, maxScripts);

    // — Run JS —
    JsEngine js;
    JsScriptBudget budget;
    budget.maxScriptBytes = 256 * 1024;
    js.setScriptBudget(budget);
    js.setDocument(dom, []() {}, finalUrl);

    int attempted = 0;
    int failed = 0;
    int skipped = 0;
    for (Node* script : scripts) {
        if (attempted >= maxScripts) break;
        std::string source = TextContent(script);
        if (source.empty()) continue;

        std::string filename = script->attr("__vertex_script_filename");
        if (filename.empty()) filename = "inline:" + std::to_string(attempted);

        if (source.size() > budget.maxScriptBytes) {
            if (verbose) Log("  [%d] SKIP %zu bytes %s\n", attempted, source.size(), filename.c_str());
            ++skipped;
            ++attempted;
            continue;
        }

        if (verbose) Log("  [%d] %zu bytes %s\n", attempted, source.size(), Truncate(filename, 100).c_str());

        bool ok = false;
        try {
            ok = js.runScript(source, filename);
        } catch (const std::exception& e) {
            Log("  [%d] C++ exception: %s\n", attempted, e.what());
        } catch (...) {
            Log("  [%d] C++ exception: unknown\n", attempted);
        }
        if (!ok) ++failed;
        ++attempted;
    }
    try { js.runMacrotasks(32); } catch (...) { Log("macrotasks crashed\n"); }

    // — Layout —
    Stylesheet sheet = CollectCss(dom.get());
    Log("css rules=%zu\n", sheet.rules.size());

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

    if (layout) {
        Log("layout ok docW=%.0f docH=%.0f\n", layout->contentW, layout->contentH);
    } else {
        Log("layout FAIL (null)\n");
    }

    // — Summary —
    JsScriptStats stats = js.scriptStats();
    Log("RESULT url=%s fetch=%d dom=%zu scripts=%d/%zu ok=%d fail=%d skip=%d parse=%d runtime=%d "
        "parse_ms=%.0f run_ms=%.0f layout=%d\n",
        url.c_str(), docRes.success ? 1 : 0, domNodeCount,
        attempted, scripts.size(), attempted - failed - skipped, failed, skipped,
        (int)stats.parseFailures, (int)stats.runtimeFailures,
        stats.parseMs, stats.compileRunMs,
        layout ? 1 : 0);

    return failed == 0 ? 0 : 3;
}
