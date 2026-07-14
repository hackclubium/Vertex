// snapshot_svg.cpp - URL/file visual snapshot from real layout tree.
// Writes an SVG approximation: backgrounds, borders, text fragments, image boxes.
#include "css/stylesheet.h"
#include "html/parser.h"
#include "layout/layout_engine.h"
#include "network/resource_cache.h"
#include "network/text_decode.h"
#include "network/url.h"
#include "platform/browser_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

struct SvgMeasure : ITextMeasure {
    float MeasureText(const std::wstring& s, const FontKey& f) override { return (float)s.size() * f.size * 0.5f; }
    float SpaceWidth(const FontKey& f) override { return f.size * 0.3f; }
    bool ImageIntrinsic(const std::string&, float& w, float& h) override { w = 0; h = 0; return false; }
    void RequestImage(const std::string&) override {}
};

static std::string UrlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hx = [](char c) { if (c >= '0' && c <= '9') return c - '0'; c = (char)std::tolower((unsigned char)c); return 10 + (c - 'a'); };
            o += (char)(hx(s[i + 1]) * 16 + hx(s[i + 2]));
            i += 2;
        } else o += s[i];
    }
    return o;
}

static Stylesheet CollectCss(const Node* root) {
    Stylesheet sheet;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tagName == "style") {
            std::string css;
            for (auto& c : n->children) if (c->type == NodeType::Text) css += c->text;
            auto part = ParseStylesheet(css);
            if (part.rootRemBaseSet) { sheet.rootRemBase = part.rootRemBase; sheet.rootRemBaseSet = true; }
            for (auto& r : part.rules) sheet.rules.push_back(r);
        } else if (n->type == NodeType::Element && n->tagName == "link") {
            std::string rel = n->attr("rel"), low;
            for (char c : rel) low += (char)std::tolower((unsigned char)c);
            if (low.find("stylesheet") != std::string::npos) {
                const std::string pfx = "data:text/css,";
                std::string href = n->attr("href");
                if (href.rfind(pfx, 0) == 0) {
                    auto part = ParseStylesheet(UrlDecode(href.substr(pfx.size())));
                    if (part.rootRemBaseSet) { sheet.rootRemBase = part.rootRemBase; sheet.rootRemBaseSet = true; }
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

static std::string Esc(const std::wstring& w) {
    std::string out;
    for (wchar_t ch : w) {
        unsigned int c = (unsigned int)ch;
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c >= 32 && c < 127) out += (char)c;
        else { char buf[32]; snprintf(buf, sizeof buf, "&#%u;", c); out += buf; }
    }
    return out;
}

static std::string Color(const CssColor& c) {
    int r = std::clamp((int)(c.r * 255.f + 0.5f), 0, 255);
    int g = std::clamp((int)(c.g * 255.f + 0.5f), 0, 255);
    int b = std::clamp((int)(c.b * 255.f + 0.5f), 0, 255);
    char buf[64];
    if (c.a < 0.999f) snprintf(buf, sizeof buf, "rgba(%d,%d,%d,%.3f)", r, g, b, c.a);
    else snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    return buf;
}

static void Rect(std::ostream& out, float x, float y, float w, float h, const std::string& fill, const std::string& stroke = {}, float sw = 0) {
    if (w <= 0 || h <= 0) return;
    out << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << w << "\" height=\"" << h
        << "\" fill=\"" << fill << "\"";
    if (!stroke.empty() && sw > 0) out << " stroke=\"" << stroke << "\" stroke-width=\"" << sw << "\"";
    out << "/>\n";
}

static void PaintSvg(const LayoutBox& b, std::ostream& out) {
    if (b.style.visibilityHidden) return;
    if (b.style.bgColor.valid && b.style.bgColor.a > 0.001f)
        Rect(out, b.x, b.y, b.borderBoxW(), b.borderBoxH(), Color(b.style.bgColor));
    float bw = std::max(std::max(b.borderTop, b.borderRight), std::max(b.borderBottom, b.borderLeft));
    if (bw > 0)
        Rect(out, b.x, b.y, b.borderBoxW(), b.borderBoxH(), "none", Color(b.style.borderColor.valid ? b.style.borderColor : CssColor{true,0,0,0,1}), bw);
    if (b.kind == BoxKind::Replaced || (!b.replacedUrl.empty() && b.kids.empty())) {
        Rect(out, b.x, b.y, b.borderBoxW(), b.borderBoxH(), "#f3f4f6", "#cbd5e1", 1);
    }
    for (const auto& line : b.lines) {
        for (const auto& frag : line.frags) {
            if (frag.text.empty()) continue;
            CssColor color = frag.src ? frag.src->style.color : b.style.color;
            if (!color.valid) color = {true,0,0,0,1};
            float size = frag.src && frag.src->style.fontSize > 0 ? frag.src->style.fontSize : 16.f;
            out << "<text x=\"" << frag.x << "\" y=\"" << (frag.y + frag.baseline)
                << "\" font-size=\"" << size << "\" fill=\"" << Color(color) << "\">"
                << Esc(frag.text) << "</text>\n";
        }
    }
    for (const auto& k : b.kids) PaintSvg(*k, out);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: snapshot_svg file.html|url out.svg [width] [height]\n");
        return 1;
    }
    const std::string source = argv[1];
    const std::string outPath = argv[2];
    const float width = argc > 3 ? (float)atoi(argv[3]) : 1200.f;
    const float height = argc > 4 ? (float)atoi(argv[4]) : 800.f;
    std::string html, baseUrl;
    if (HasUrlScheme(source)) {
        FetchResult res = FetchResourceCached(source, 12 * 1024 * 1024, ResourceKind::Document);
        if (!res.success) { fprintf(stderr, "fetch failed: %s\n", res.error.c_str()); return 2; }
        html = DecodeTextToUtf8(res.body, res.contentType);
        baseUrl = res.finalUrl.empty() ? source : res.finalUrl;
    } else {
        std::ifstream f(source, std::ios::binary);
        if (!f) { fprintf(stderr, "open failed: %s\n", source.c_str()); return 2; }
        std::stringstream ss; ss << f.rdbuf(); html = ss.str();
    }
    auto dom = ParseHtml(html);
    if (!baseUrl.empty()) LoadExternalStylesheets(dom, baseUrl);
    Stylesheet sheet = CollectCss(dom.get());
    sheet.setViewport(width, height);
    SvgMeasure measure;
    LayoutInput in;
    in.document = dom.get(); in.sheet = &sheet; in.measure = &measure;
    in.viewportW = width; in.viewportH = height; in.zoom = 1.f; in.baseUrl = baseUrl;
    auto root = LayoutDocument(in);
    if (!root) return 3;
    std::ofstream out(outPath, std::ios::binary);
    if (!out) { fprintf(stderr, "write failed: %s\n", outPath.c_str()); return 4; }
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
        << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";
    PaintSvg(*root, out);
    out << "</svg>\n";
    printf("wrote %s\n", outPath.c_str());
    return 0;
}
