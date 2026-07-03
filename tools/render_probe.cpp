// render_probe.cpp - deterministic layout/paint diagnostics for fixtures.
//
// This is intentionally renderer-free: it uses the real HTML/CSS/layout engine,
// then emits stable text for metrics, layout boxes, and conceptual paint order.
#include "css/stylesheet.h"
#include "html/parser.h"
#include "layout/layout_engine.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

struct ProbeMeasure : ITextMeasure {
    float MeasureText(const std::wstring& s, const FontKey& f) override {
        return static_cast<float>(s.size()) * f.size * 0.5f;
    }
    float SpaceWidth(const FontKey& f) override { return f.size * 0.3f; }
    bool ImageIntrinsic(const std::string&, float& w, float& h) override {
        w = 0;
        h = 0;
        return false;
    }
    void RequestImage(const std::string&) override {}
};

static std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return 10 + (c - 'a');
            };
            out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

static Stylesheet CollectCss(const Node* root) {
    Stylesheet sheet;
    std::function<void(const Node*)> walk = [&](const Node* node) {
        if (!node) return;
        if (node->type == NodeType::Element && node->tagName == "style") {
            std::string css;
            for (auto& child : node->children)
                if (child->type == NodeType::Text) css += child->text;
            auto part = ParseStylesheet(css);
            if (part.rootRemBaseSet) {
                sheet.rootRemBase = part.rootRemBase;
                sheet.rootRemBaseSet = true;
            }
            for (auto& rule : part.rules) sheet.rules.push_back(rule);
        } else if (node->type == NodeType::Element && node->tagName == "link") {
            std::string rel = node->attr("rel");
            std::string low;
            for (char c : rel) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (low.find("stylesheet") != std::string::npos) {
                std::string href = node->attr("href");
                const std::string pfx = "data:text/css,";
                if (href.rfind(pfx, 0) == 0) {
                    auto part = ParseStylesheet(UrlDecode(href.substr(pfx.size())));
                    if (part.rootRemBaseSet) {
                        sheet.rootRemBase = part.rootRemBase;
                        sheet.rootRemBaseSet = true;
                    }
                    for (auto& rule : part.rules) sheet.rules.push_back(rule);
                }
            }
        }
        for (auto& child : node->children) walk(child.get());
    };
    walk(root);
    sheet.rebuildRuleBuckets();
    return sheet;
}

static const char* KindName(BoxKind kind) {
    switch (kind) {
        case BoxKind::Block: return "block";
        case BoxKind::Inline: return "inline";
        case BoxKind::InlineBlock: return "iblock";
        case BoxKind::Replaced: return "replaced";
        case BoxKind::Text: return "text";
        case BoxKind::ListItem: return "list-item";
        case BoxKind::Table: return "table";
        case BoxKind::TableRow: return "table-row";
        case BoxKind::TableCell: return "table-cell";
        case BoxKind::Break: return "break";
    }
    return "unknown";
}

static std::string Label(const LayoutBox& box) {
    if (box.kind == BoxKind::Text) return "#text";
    if (!box.node) return box.anonymous ? "(anonymous)" : "(box)";
    std::string label = box.node->tagName;
    std::string id = box.node->attr("id");
    if (!id.empty()) label += "#" + id;
    std::string cls = box.node->attr("class");
    if (!cls.empty()) {
        label += ".";
        for (char c : cls) label += (c == ' ' ? '.' : c);
    }
    return label;
}

static bool IsRealCssBox(const LayoutBox& box) {
    return box.kind != BoxKind::Text && box.kind != BoxKind::Break;
}

static int EffectivePosition(const LayoutBox& box) {
    return IsRealCssBox(box) ? box.style.positionMode : 0;
}

static std::string EffectiveZ(const LayoutBox& box) {
    if (!IsRealCssBox(box) || !box.style.zIndexSet) return "auto";
    return std::to_string(box.style.zIndex);
}

struct Metrics {
    int boxes = 0;
    int positioned = 0;
    int floats = 0;
    int links = 0;
    int lineBoxes = 0;
    int maxDepth = 0;
};

static void CollectMetrics(const LayoutBox& box, int depth, Metrics& metrics) {
    ++metrics.boxes;
    metrics.maxDepth = std::max(metrics.maxDepth, depth);
    if (EffectivePosition(box)) ++metrics.positioned;
    if (box.isFloat()) ++metrics.floats;
    if (!box.href.empty()) ++metrics.links;
    metrics.lineBoxes += static_cast<int>(box.lines.size());
    for (auto& child : box.kids) CollectMetrics(*child, depth + 1, metrics);
}

static void DumpLayout(const LayoutBox& box, int depth) {
    std::string indent(static_cast<size_t>(depth) * 2, ' ');
    printf("%slayout %s %s x=%.0f y=%.0f w=%.0f h=%.0f pos=%d z=%s\n",
           indent.c_str(), KindName(box.kind), Label(box).c_str(),
           box.x, box.y, box.borderBoxW(), box.borderBoxH(),
           EffectivePosition(box), EffectiveZ(box).c_str());
    for (auto& child : box.kids) DumpLayout(*child, depth + 1);
}

static int PaintZ(const LayoutBox* box) {
    return IsRealCssBox(*box) && box->style.zIndexSet ? box->style.zIndex : 0;
}

static void PaintOrder(const LayoutBox& box, std::vector<const LayoutBox*>& out) {
    out.push_back(&box);

    bool simple = true;
    for (auto& child : box.kids) {
        if (child->isOutOfFlow() || child->isFloat() || child->style.positionMode == 1
            || child->style.zIndexSet) {
            simple = false;
            break;
        }
    }

    if (simple) {
        for (auto& child : box.kids) PaintOrder(*child, out);
        return;
    }

    std::vector<const LayoutBox*> negZ, inflow, floats, posZ;
    for (auto& child : box.kids) {
        const LayoutBox* item = child.get();
        if (item->isOutOfFlow()) {
            if (item->style.zIndexSet && item->style.zIndex < 0) negZ.push_back(item);
            else posZ.push_back(item);
        } else if (item->isFloat()) {
            floats.push_back(item);
        } else if (item->style.positionMode == 1) {
            posZ.push_back(item);
        } else {
            inflow.push_back(item);
        }
    }
    auto byZ = [](const LayoutBox* a, const LayoutBox* b) { return PaintZ(a) < PaintZ(b); };
    std::stable_sort(negZ.begin(), negZ.end(), byZ);
    std::stable_sort(posZ.begin(), posZ.end(), byZ);

    for (auto* item : negZ) PaintOrder(*item, out);
    for (auto* item : inflow) PaintOrder(*item, out);
    for (auto* item : floats) PaintOrder(*item, out);
    for (auto* item : posZ) PaintOrder(*item, out);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: render_probe file.html [viewport-width]\n");
        return 1;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        fprintf(stderr, "render_probe: could not open %s\n", argv[1]);
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    auto dom = ParseHtml(buffer.str());
    Stylesheet sheet = CollectCss(dom.get());
    ProbeMeasure measure;

    LayoutInput input;
    input.document = dom.get();
    input.sheet = &sheet;
    input.measure = &measure;
    input.viewportW = argc > 2 ? static_cast<float>(atoi(argv[2])) : 1200.f;
    input.viewportH = 800.f;
    input.zoom = 1.f;

    auto layout = LayoutDocument(input);
    if (!layout) return 1;

    Metrics metrics;
    CollectMetrics(*layout, 0, metrics);
    printf("metrics boxes=%d positioned=%d floats=%d links=%d lineBoxes=%d maxDepth=%d\n",
           metrics.boxes, metrics.positioned, metrics.floats, metrics.links,
           metrics.lineBoxes, metrics.maxDepth);

    DumpLayout(*layout, 0);

    std::vector<const LayoutBox*> order;
    PaintOrder(*layout, order);
    for (size_t i = 0; i < order.size(); ++i) {
        const LayoutBox* box = order[i];
        printf("paint-order %zu %s %s z=%s\n", i, KindName(box->kind), Label(*box).c_str(),
               EffectiveZ(*box).c_str());
    }
    return 0;
}
