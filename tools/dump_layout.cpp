// dump_layout.cpp — offline layout diagnostic. Parses an HTML file, runs the
// real layout engine with a stub text measurer, and prints every box's
// geometry + key style. Lets us debug layout without the GUI.
#include "html/parser.h"
#include "css/stylesheet.h"
#include "network/resource_cache.h"
#include "network/text_decode.h"
#include "layout/layout_engine.h"
#include "network/url.h"
#include "platform/browser_core.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <functional>
#include <cctype>

struct StubMeasure : ITextMeasure {
    float MeasureText(const std::wstring& s, const FontKey& f) override { return (float)s.size() * f.size * 0.5f; }
    float SpaceWidth(const FontKey& f) override { return f.size * 0.3f; }
    bool  ImageIntrinsic(const std::string&, float& w, float& h) override { w = 0; h = 0; return false; }
    void  RequestImage(const std::string&) override {}
};

static std::string UrlDecode(const std::string& s) {
    std::string o; for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hx = [](char c){ if (c>='0'&&c<='9') return c-'0'; c|=0x20; return 10+(c-'a'); };
            o += (char)(hx(s[i+1])*16 + hx(s[i+2])); i += 2;
        } else o += s[i];
    }
    return o;
}

static Stylesheet CollectCss(const Node* root) {
    Stylesheet sheet;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tagName == "style") {
            std::string css; for (auto& c : n->children) if (c->type == NodeType::Text) css += c->text;
            auto part = ParseStylesheet(css);
            if (part.rootRemBaseSet) {
                sheet.rootRemBase = part.rootRemBase;
                sheet.rootRemBaseSet = true;
            }
            for (auto& r : part.rules) sheet.rules.push_back(r);
        } else if (n->type == NodeType::Element && n->tagName == "link") {
            std::string rel = n->attr("rel"), low;
            for (char c : rel) low += (char)std::tolower((unsigned char)c);
            if (low.find("stylesheet") != std::string::npos) {
                std::string href = n->attr("href");
                const std::string pfx = "data:text/css,";
                if (href.rfind(pfx, 0) == 0) {
                    auto part = ParseStylesheet(UrlDecode(href.substr(pfx.size())));
                    if (part.rootRemBaseSet) {
                        sheet.rootRemBase = part.rootRemBase;
                        sheet.rootRemBaseSet = true;
                    }
                    for (auto& r : part.rules) sheet.rules.push_back(r);
                }
            }
        }
        for (auto& c : n->children) walk(c.get());
    };
    walk(root);
    sheet.rebuildRuleBuckets();
    return sheet;
}

static const char* KindName(BoxKind k) {
    switch (k) {
        case BoxKind::Block: return "block"; case BoxKind::Inline: return "inline";
        case BoxKind::InlineBlock: return "iblock"; case BoxKind::Replaced: return "img";
        case BoxKind::Text: return "text"; case BoxKind::ListItem: return "li";
        case BoxKind::Table: return "table"; case BoxKind::TableRow: return "tr";
        case BoxKind::TableCell: return "td"; case BoxKind::Break: return "br";
    }
    return "?";
}

static void Dump(const LayoutBox& b, int depth) {
    std::string ind(depth * 2, ' ');
    std::string tag = b.node && b.node->type == NodeType::Element ? b.node->tagName : (b.anonymous ? "(anon)" : "");
    std::string cls = b.node ? b.node->attr("class") : "";
    char bg[64] = "";
    if (b.style.bgColor.valid)
        snprintf(bg, sizeof bg, " bg=%.1f,%.1f,%.1f,%.1f", b.style.bgColor.r, b.style.bgColor.g, b.style.bgColor.b, b.style.bgColor.a);
    char pos[24] = "";
    if (b.style.positionMode) snprintf(pos, sizeof pos, " pos=%d", b.style.positionMode);
    char flt[16] = "";
    if (b.style.floatMode) snprintf(flt, sizeof flt, " float=%d", b.style.floatMode);
    printf("%s%s<%s%s%s> x=%.0f y=%.0f w=%.0f h=%.0f%s%s%s\n",
           ind.c_str(), KindName(b.kind), tag.c_str(),
           cls.empty() ? "" : ".", cls.c_str(),
           b.x, b.y, b.borderBoxW(), b.borderBoxH(), bg, pos, flt);
    for (auto& k : b.kids) Dump(*k, depth + 1);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: dump_layout file.html|url [viewport_width]\n"); return 1; }
    const std::string source = argv[1];
    std::string html;
    std::string baseUrl;
    if (HasUrlScheme(source)) {
        FetchResult res = FetchResourceCached(source, 12 * 1024 * 1024, ResourceKind::Document);
        if (!res.success) { fprintf(stderr, "fetch failed: %s\n", res.error.c_str()); return 2; }
        html = DecodeTextToUtf8(res.body, res.contentType);
        baseUrl = res.finalUrl.empty() ? source : res.finalUrl;
    } else {
        std::ifstream f(source, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        html = ss.str();
    }

    auto dom = ParseHtml(html);
    if (!baseUrl.empty()) LoadExternalStylesheets(dom, baseUrl);
    Stylesheet sheet = CollectCss(dom.get());
    sheet.setViewport(argc > 2 ? (float)atoi(argv[2]) : 1200.f, 800.f);
    StubMeasure measure;

    LayoutInput in;
    in.document = dom.get();
    in.sheet = &sheet;
    in.measure = &measure;
    in.viewportW = argc > 2 ? (float)atoi(argv[2]) : 1200.f;
    in.viewportH = 800.f;
    in.zoom = 1.f;
    in.baseUrl = baseUrl;

    auto root = LayoutDocument(in);
    if (root) Dump(*root, 0);
    return 0;
}
