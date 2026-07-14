#include "layout/layout_engine.h"
#include "render/animation.h"
#include "network/url.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
//  Vertex layout engine
//
//  Two phases, driven by LayoutDocument():
//    1. BuildBoxTree  — turn the styled DOM into a layout box tree, generating
//                       anonymous boxes and handling display/whitespace.
//    2. Layout        — block formatting context (vertical stacking, margins,
//                       widths, floats) + inline formatting context (line boxes,
//                       text wrapping, replaced/inline-block placement), then a
//                       positioned-layout pass for absolute/fixed/relative.
//
//  All geometry is device px (CSS px * zoom). (x, y) is the border-box origin.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Recursion-depth guard: real-world pages can nest deeply or (with broken
// markup) pathologically. Bail out gracefully instead of overflowing the stack.
thread_local int g_depth = 0;
constexpr int kMaxDepth = 512;
struct DepthScope { DepthScope() { ++g_depth; } ~DepthScope() { --g_depth; } };

constexpr float kNormalLH = 1.2f;   // line-height: normal factor
// A single text node should never be able to allocate unbounded layout,
// measurement, and paint buffers. This also contains malformed data URIs that
// accidentally reach text flow instead of a skipped raw-text element.
constexpr size_t kMaxTextNodeBytes = 16 * 1024;

float Ascent(float lineH, float fontSize) {
    // Distribute the leading half above / half below the em box; place the
    // baseline ~80% down the em box.
    float leading = lineH - fontSize;
    return leading * 0.5f + fontSize * 0.8f;
}

bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f'; }

std::wstring ToWide(const std::string& s) {
    std::wstring w;
    const size_t limit = std::min(s.size(), kMaxTextNodeBytes);
    w.reserve(limit);
    size_t i = 0;
    while (i < limit) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { w += (wchar_t)c; i += 1; }
        else if ((c >> 5) == 0x6 && i + 1 < limit) {
            w += (wchar_t)(((c & 0x1F) << 6) | (s[i+1] & 0x3F)); i += 2;
        } else if ((c >> 4) == 0xE && i + 2 < limit) {
            w += (wchar_t)(((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F)); i += 3;
        } else if ((c >> 3) == 0x1E && i + 3 < limit) {
            unsigned int cp = ((c & 0x07) << 18) | ((s[i+1] & 0x3F) << 12)
                            | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F);
            if constexpr (sizeof(wchar_t) >= 4) {
                w += (wchar_t)cp;  // 32-bit wchar_t (Linux/macOS): store raw codepoint
            } else {
                cp -= 0x10000;     // 16-bit wchar_t (Windows): UTF-16 surrogate pair
                w += (wchar_t)(0xD800 + (cp >> 10));
                w += (wchar_t)(0xDC00 + (cp & 0x3FF));
            }
            i += 4;
        } else { w += L'?'; i += 1; }
    }
    return w;
}

std::string TextContent(const Node* n) {
    if (!n) return {};
    if (n->type == NodeType::Text) return n->text;
    std::string out;
    for (const auto& child : n->children) out += TextContent(child.get());
    return out;
}

std::string SelectedOptionText(const Node* select) {
    if (!select) return {};
    const Node* first = nullptr;
    std::function<const Node*(const Node*)> walk = [&](const Node* n) -> const Node* {
        if (!n) return nullptr;
        if (n->type == NodeType::Element && n->tagName == "option") {
            if (!first) first = n;
            if (n->attrs.find("selected") != n->attrs.end()) return n;
        }
        for (const auto& child : n->children)
            if (auto* found = walk(child.get())) return found;
        return nullptr;
    };
    const Node* selected = walk(select);
    return TextContent(selected ? selected : first);
}

bool HasElementChild(const Node* n) {
    if (!n) return false;
    for (const auto& child : n->children)
        if (child && child->type == NodeType::Element) return true;
    return false;
}

bool HasAttr(const Node* n, const std::string& name) {
    return n && n->attrs.find(name) != n->attrs.end();
}

bool HasClass(const Node* n, const char* cls) {
    if (!n) return false;
    std::istringstream ss(n->attr("class"));
    std::string token;
    while (ss >> token)
        if (token == cls) return true;
    return false;
}

bool HasAncestorClass(const Node* n, const char* cls) {
    for (const Node* p = n ? n->parent : nullptr; p; p = p->parent)
        if (HasClass(p, cls)) return true;
    return false;
}

std::string LowerAscii(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// UA default display for an element tag (used when CSS doesn't set display).
// Returns one of the ComputedStyle display codes (1=block,2=inline,7=inline-block,
// 8=list-item,5=table,9=table-row,6=table-cell,10=row-group,14=caption).
int UaDisplay(const std::string& tag) {
    // Block-level
    static const char* blocks[] = {
        "html","body","div","p","section","article","main","header","footer",
        "aside","nav","figure","figcaption","blockquote","address","form","fieldset",
        "details","summary","hr","pre","dl","dt","dd","h1","h2","h3","h4","h5","h6",
        "ul","ol","center","legend"
    };
    for (auto* b : blocks) if (tag == b) return 1;
    if (tag == "li") return 8;
    if (tag == "table") return 5;
    if (tag == "caption") return 14;
    if (tag == "tr") return 9;
    if (tag == "td" || tag == "th") return 6;
    if (tag == "thead" || tag == "tbody" || tag == "tfoot") return 10;
    if (tag == "img" || tag == "input" || tag == "select" || tag == "textarea"
     || tag == "button" || tag == "object" || tag == "iframe" || tag == "canvas"
     || tag == "video" || tag == "embed" || tag == "svg")
        return 7;  // replaced/atomic → inline-block-ish
    return 2;       // default inline
}

bool IsSkippedTag(const std::string& tag) {
    return tag == "head" || tag == "script" || tag == "style" || tag == "meta"
        || tag == "link" || tag == "title" || tag == "noscript" || tag == "base"
        || tag == "template" || tag == "param" || tag == "source" || tag == "track"
        || tag == "col" || tag == "colgroup"
        || tag == "math";
}

bool TagIsReplacedImage(const Node* n, const std::string& baseUrl, std::string& urlOut) {
    if (!n) return false;
    const std::string& tag = n->tagName;
    if (tag == "img") {
        auto firstSrcsetUrl = [](const std::string& srcset) {
            const size_t begin = srcset.find_first_not_of(" \t\n");
            if (begin == std::string::npos) return std::string{};
            if (srcset.rfind("data:", begin) == begin) {
                const size_t end = srcset.find_first_of(" \t\n", begin);
                return srcset.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            }
            const size_t end = srcset.find_first_of(" \t\n,", begin);
            return srcset.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        };
        std::string src = n->attr("src");
        const bool inlinePlaceholder = src.rfind("data:", 0) == 0;
        // Lazy loaders commonly leave a 1px data: placeholder in src. The
        // real resource must win or the browser will render only that pixel.
        if (src.empty() || inlinePlaceholder) {
            for (const char* name : { "data-src", "data-lazy-src", "data-original" }) {
                const std::string candidate = n->attr(name);
                if (!candidate.empty()) { src = candidate; break; }
            }
        }
        if (src.empty() || src.rfind("data:", 0) == 0) {
            // srcset = "url1 1x, url2 2x" or "url1 480w, url2 800w" — take the
            // first candidate's URL (the part before the first space).
            std::string ss = n->attr("srcset");
            if (ss.empty()) ss = n->attr("data-srcset");
            src = firstSrcsetUrl(ss);
        }
        if (src.empty() && n->parent && n->parent->tagName == "picture") {
            for (const auto& child : n->parent->children) {
                if (child->type != NodeType::Element || child->tagName != "source") continue;
                std::string type = child->attr("type");
                for (char& c : type) c = (char)std::tolower((unsigned char)c);
                // The built-in WIC path cannot decode these without an optional
                // system codec, so prefer a later WebP/raster source instead.
                if (type == "image/avif" || type == "image/svg+xml") continue;
                std::string candidate = child->attr("srcset");
                if (candidate.empty()) candidate = child->attr("data-srcset");
                if (candidate.empty()) candidate = child->attr("src");
                src = firstSrcsetUrl(candidate);
                if (!src.empty()) break;
            }
        }
        if (src.empty()) return false;
        urlOut = ResolveUrlAgainstBase(src, baseUrl);
        return true;
    }
    if (tag == "object") {
        std::string data = n->attr("data");
        std::string low; for (char c : data) low += (char)std::tolower((unsigned char)c);
        bool imgish = low.rfind("data:image/", 0) == 0
            || low.find(".png") != std::string::npos || low.find(".jpg") != std::string::npos
            || low.find(".jpeg") != std::string::npos || low.find(".gif") != std::string::npos
            || low.find(".webp") != std::string::npos || low.find(".bmp") != std::string::npos;
        if (!imgish) return false;
        urlOut = ResolveUrlAgainstBase(data, baseUrl);
        return true;
    }
    return false;
}

bool TagIsMedia(const Node* n, const std::string& baseUrl, std::string& urlOut) {
    if (!n || (n->tagName != "video" && n->tagName != "audio")) return false;
    std::string src = n->attr("src");
    if (src.empty()) {
        for (const auto& child : n->children) {
            if (!child || child->type != NodeType::Element || child->tagName != "source") continue;
            src = child->attr("src");
            if (!src.empty()) break;
        }
    }
    if (!src.empty()) urlOut = ResolveUrlAgainstBase(src, baseUrl);
    return true;
}

// ─── inheritance + UA defaults ───────────────────────────────────────────────

void InheritInto(ComputedStyle& s, const ComputedStyle& parent) {
    if (!s.color.valid && parent.color.valid)            s.color = parent.color;
    if (s.fontSize <= 0 && parent.fontSize > 0)          s.fontSize = parent.fontSize;
    if (!s.boldSet && parent.boldSet)   { s.bold = parent.bold;     s.boldSet = true; }
    if (!s.italicSet && parent.italicSet){ s.italic = parent.italic; s.italicSet = true; }
    if (s.fontFamily.empty())                            s.fontFamily = parent.fontFamily;
    if (s.lineHeight <= 0 && parent.lineHeight > 0)      s.lineHeight = parent.lineHeight;
    if (!s.textAlignSet && parent.textAlignSet) { s.textAlign = parent.textAlign; s.textAlignSet = true; }
    if (!s.textIndentSet && parent.textIndentSet) { s.textIndent = parent.textIndent; s.textIndentSet = true; }
    if (!s.letterSpacingSet && parent.letterSpacingSet) { s.letterSpacing = parent.letterSpacing; s.letterSpacingSet = true; }
    if (!s.wordBreakSet && parent.wordBreakSet) { s.wordBreak = parent.wordBreak; s.wordBreakSet = true; }
    if (!s.textTransformSet && parent.textTransformSet) { s.textTransform = parent.textTransform; s.textTransformSet = true; }
    if (!s.whiteSpaceSet && parent.whiteSpaceSet) {
        s.whiteSpaceNowrap = parent.whiteSpaceNowrap;
        s.whiteSpacePre = parent.whiteSpacePre; s.whiteSpaceSet = true;
    }
    if (!s.visibilitySet && parent.visibilitySet) { s.visibilityHidden = parent.visibilityHidden; s.visibilitySet = true; }
    if (!s.listStyleSet && parent.listStyleSet)   { s.listStyleNone = parent.listStyleNone; s.listStyleSet = true; }
    if (parent.underline) s.underline = true;   // text-decoration propagates visually
    if (parent.lineThrough) s.lineThrough = true;
    for (const auto& [name, value] : parent.customProperties)
        if (s.customProperties.find(name) == s.customProperties.end())
            s.customProperties[name] = value;
}

void ApplyUaDefaults(const std::string& tag, ComputedStyle& s) {
    auto setSize = [&](float px) { if (s.fontSize <= 0) s.fontSize = px; };
    auto setBold = [&]() { if (!s.boldSet) { s.bold = true; s.boldSet = true; } };
    auto setItalic = [&]() { if (!s.italicSet) { s.italic = true; s.italicSet = true; } };
    auto vMargin = [&](float v) {
        if (!s.marginTopSet())    s.marginTop = v;
        if (!s.marginBottomSet()) s.marginBottom = v;
    };
    auto setMono = [&]() { if (s.fontFamily.empty()) s.fontFamily = "monospace"; };

    if (tag == "body") {
        if (!s.marginTopSet())    s.marginTop = 8;
        if (!s.marginBottomSet()) s.marginBottom = 8;
        if (!s.marginLeftSet())   s.marginLeft = 8;
        if (!s.marginRightSet())  s.marginRight = 8;
    }
    else if (tag == "h1") { setSize(32); setBold(); vMargin(21); }
    else if (tag == "h2") { setSize(24); setBold(); vMargin(19); }
    else if (tag == "h3") { setSize(19); setBold(); vMargin(18); }
    else if (tag == "h4") { setSize(16); setBold(); vMargin(16); }
    else if (tag == "h5") { setSize(13); setBold(); vMargin(15); }
    else if (tag == "h6") { setSize(11); setBold(); vMargin(15); }
    else if (tag == "p")  { vMargin(14); }
    else if (tag == "blockquote") { vMargin(14); if (s.marginLeft == kCssNotSet) {} if (!s.marginLeftSet()) s.marginLeft = 40; if (!s.marginRightSet()) s.marginRight = 40; }
    else if (tag == "figure") { vMargin(14); if (!s.marginLeftSet()) s.marginLeft = 40; if (!s.marginRightSet()) s.marginRight = 40; }
    else if (tag == "ul" || tag == "ol") { vMargin(14); if (s.paddingLeft < 0) s.paddingLeft = 40; }
    else if (tag == "dl") { vMargin(14); }
    else if (tag == "dd") { if (!s.marginLeftSet()) s.marginLeft = 40; }
    else if (tag == "pre") { vMargin(12); setMono(); s.whiteSpacePre = true; s.whiteSpaceSet = true; }
    else if (tag == "hr")  { vMargin(8); }
    else if (tag == "b" || tag == "strong" || tag == "th") { setBold(); }
    else if (tag == "i" || tag == "em" || tag == "cite" || tag == "var" || tag == "dfn") { setItalic(); }
    else if (tag == "address") { setItalic(); }
    else if (tag == "code" || tag == "tt" || tag == "kbd" || tag == "samp") { setMono(); }
    else if (tag == "small") { if (s.fontSize <= 0) s.fontSize = 13; }
    else if (tag == "a") { if (!s.underline) s.underline = true; }
    else if (tag == "u" || tag == "ins") { if (!s.noUnderline) s.underline = true; }
    else if (tag == "s" || tag == "strike" || tag == "del") { s.lineThrough = true; }
    else if (tag == "sub") {
        if (!s.verticalAlignSet) { s.verticalAlign = 1; s.verticalAlignSet = true; }
        if (s.fontSize > 0) s.fontSize *= 0.8f;
    }
    else if (tag == "sup") {
        if (!s.verticalAlignSet) { s.verticalAlign = 2; s.verticalAlignSet = true; }
        if (s.fontSize > 0) s.fontSize *= 0.8f;
    }
}

// ─── box construction ────────────────────────────────────────────────────────

struct BuildCtx {
    const Stylesheet* sheet;
    ITextMeasure*     measure;
    float             zoom;
    std::string       baseUrl;
};

BoxKind KindFromDisplay(int disp, bool replaced) {
    if (replaced) {
        // Atomic. block display → still atomic but block-level-positioned.
        return (disp == 1) ? BoxKind::InlineBlock /*treated as atomic block below*/
                           : BoxKind::Replaced;
    }
    switch (disp) {
        case 1:  return BoxKind::Block;
        case 2:  return BoxKind::Inline;
        case 12: return BoxKind::Block;
        case 7:  return BoxKind::InlineBlock;
        case 8:  return BoxKind::ListItem;
        case 5:  return BoxKind::Table;
        case 9:  return BoxKind::TableRow;
        case 6:  return BoxKind::TableCell;
        case 10: return BoxKind::Block;  // row group → block
        case 14: return BoxKind::Block;  // caption → block above/below table grid
        default: return BoxKind::Block;
    }
}

std::unique_ptr<LayoutBox> BuildBox(const Node* node, const ComputedStyle& parentStyle,
                                    const BuildCtx& bc, const std::string& inheritedHref,
                                    const ComputedStyle* precomputedStyle = nullptr);

// Build children list (with pseudo-elements and anonymous-box fixup applied
// by the caller). Whitespace-only text between blocks is dropped later.
void BuildChildren(const Node* node, const ComputedStyle& style, const BuildCtx& bc,
                   const std::string& href, std::vector<std::unique_ptr<LayoutBox>>& out) {
    // ::before
    // (generated content handled by caller via pseudo nodes — skipped here for brevity)
    for (auto& child : node->children) {
        if (child->type == NodeType::Text) {
            // Whitespace handling: collapse runs of whitespace to single spaces
            // unless white-space: pre. Empty/whitespace runs become a single space
            // box (may be dropped during fixup if between blocks).
            bool pre = style.whiteSpacePre;
            std::wstring w = ToWide(child->text);
            if (w.empty()) continue;
            std::wstring outw;
            if (pre) {
                outw = w;
            } else {
                bool prevSpace = false;
                for (wchar_t c : w) {
                    if (IsSpace(c)) {
                        if (!prevSpace) { outw += L' '; prevSpace = true; }
                    } else { outw += c; prevSpace = false; }
                }
            }
            if (outw.empty()) continue;
            auto tb = std::make_unique<LayoutBox>();
            tb->kind = BoxKind::Text;
            tb->anonymous = true;
            tb->node = node;          // for inherited paint style
            tb->style = style;        // carries font/color/etc.
            tb->text = outw;
            tb->href = href;
            if (node) {
                for (const Node* a = node->parent; a; a = a->parent) {
                    if (a->tagName == "a" && a->attrs.find("download") != a->attrs.end()) {
                        tb->download = true;
                        tb->downloadName = a->attr("download");
                        break;
                    }
                }
            }
            out.push_back(std::move(tb));
        } else if (child->type == NodeType::Element) {
            ComputedStyle childStyle = bc.sheet ? bc.sheet->resolve(child.get()) : ComputedStyle{};
            InheritInto(childStyle, style);
            ApplyUaDefaults(child->tagName, childStyle);
            ResolveStyleVariables(childStyle);
            if (bc.sheet && childStyle.animationSet) {
                AnimationManager::instance().ensureStarted(child.get(), childStyle, *bc.sheet);
                AnimationManager::instance().applyAnimation(child.get(), childStyle);
            }

            std::string childHref = href;
            if (child->tagName == "a") {
                std::string rawHref = child->attr("href");
                if (!rawHref.empty())
                    childHref = ResolveUrlAgainstBase(rawHref, bc.baseUrl);
            }
            if (childStyle.isDisplayContents()) {
                BuildChildren(child.get(), childStyle, bc, childHref, out);
                continue;
            }
            auto cb = BuildBox(child.get(), style, bc, childHref, &childStyle);
            if (cb) out.push_back(std::move(cb));
        }
    }
}

// Apply anonymous block fixup: if `box` (a block container) has any block-level
// child, wrap each contiguous run of inline-level children in an anonymous block.
ComputedStyle AnonymousBlockStyleFrom(const ComputedStyle& parent) {
    ComputedStyle s;
    s.display = 1;

    // Anonymous block boxes inherit text-facing properties from their parent,
    // but layout-affecting properties keep their initial values. Copying the
    // full parent style can accidentally create a new BFC and make inline runs
    // ignore sibling floats.
    s.color = parent.color;
    s.fontSize = parent.fontSize;
    s.fontFamily = parent.fontFamily;
    if (parent.boldSet) { s.bold = parent.bold; s.boldSet = true; }
    if (parent.italicSet) { s.italic = parent.italic; s.italicSet = true; }
    s.underline = parent.underline;
    s.noUnderline = parent.noUnderline;
    s.lineThrough = parent.lineThrough;
    s.lineHeight = parent.lineHeight;
    if (parent.textAlignSet) { s.textAlign = parent.textAlign; s.textAlignSet = true; }
    if (parent.textIndentSet) { s.textIndent = parent.textIndent; s.textIndentSet = true; }
    if (parent.textTransformSet) {
        s.textTransform = parent.textTransform;
        s.textTransformSet = true;
    }
    if (parent.whiteSpaceSet) {
        s.whiteSpaceNowrap = parent.whiteSpaceNowrap;
        s.whiteSpacePre = parent.whiteSpacePre;
        s.whiteSpaceSet = true;
    }
    if (parent.letterSpacingSet) {
        s.letterSpacing = parent.letterSpacing;
        s.letterSpacingSet = true;
    }
    if (parent.wordBreakSet) {
        s.wordBreak = parent.wordBreak;
        s.wordBreakSet = true;
    }
    if (parent.visibilitySet) {
        s.visibilityHidden = parent.visibilityHidden;
        s.visibilitySet = true;
    }
    if (parent.listStyleSet) {
        s.listStyleNone = parent.listStyleNone;
        s.listStyleSet = true;
    }
    if (parent.textShadowSet) {
        s.textShadowX = parent.textShadowX;
        s.textShadowY = parent.textShadowY;
        s.textShadowBlur = parent.textShadowBlur;
        s.textShadowColor = parent.textShadowColor;
        s.textShadowSet = true;
    }
    s.customProperties = parent.customProperties;
    return s;
}

void AnonymousFixup(LayoutBox* box) {
    bool hasBlock = false, hasInline = false;
    for (auto& k : box->kids) {
        if (k->isInlineLevel()) hasInline = true;
        else hasBlock = true;
    }
    if (!hasBlock) {
        // Pure inline container → establishes an inline formatting context.
        // Drop leading/trailing whitespace-only text boxes.
        box->establishesInline = !box->kids.empty();
        return;
    }
    if (!hasInline) return;  // pure block container, nothing to do

    std::vector<std::unique_ptr<LayoutBox>> rebuilt;
    std::vector<std::unique_ptr<LayoutBox>> run;
    auto flushRun = [&]() {
        if (run.empty()) return;
        // Drop runs that are only whitespace text.
        bool onlyWs = true;
        for (auto& r : run) {
            if (r->kind != BoxKind::Text) { onlyWs = false; break; }
            for (wchar_t c : r->text) if (!IsSpace(c)) { onlyWs = false; break; }
            if (!onlyWs) break;
        }
        if (onlyWs) { run.clear(); return; }
        auto anon = std::make_unique<LayoutBox>();
        anon->kind = BoxKind::Block;
        anon->anonymous = true;
        anon->node = nullptr;
        anon->href = box->href;
        anon->style = AnonymousBlockStyleFrom(box->style);
        anon->establishesInline = true;
        for (auto& r : run) anon->kids.push_back(std::move(r));
        run.clear();
        rebuilt.push_back(std::move(anon));
    };
    for (auto& k : box->kids) {
        if (k->isInlineLevel()) run.push_back(std::move(k));
        else { flushRun(); rebuilt.push_back(std::move(k)); }
    }
    flushRun();
    box->kids = std::move(rebuilt);
}

// Generated content: build a ::before / ::after box if a rule sets `content`.
std::unique_ptr<LayoutBox> BuildPseudo(const Node* el, const char* which,
                                       const ComputedStyle& parentStyle, const BuildCtx& bc,
                                       const std::string& href) {
    if (!bc.sheet) return nullptr;
    Node tmp;
    tmp.type = NodeType::Element;
    tmp.tagName = el->tagName;
    tmp.attrs = el->attrs;
    tmp.attrs["_vertex_pseudo"] = which;
    tmp.parent = el->parent;
    ComputedStyle s = bc.sheet->resolve(&tmp);
    if (!s.contentSet) return nullptr;
    InheritInto(s, parentStyle);
    ApplyUaDefaults(el->tagName, s);
    ResolveStyleVariables(s);

    auto box = std::make_unique<LayoutBox>();
    box->node = nullptr;
    box->href = href;
    box->style = s;
    int disp = s.display != 0 ? s.display : 2;  // inline by default
    if (disp == 3) return nullptr;
    box->kind = KindFromDisplay(disp, false);

    if (!s.content.empty()) {
        auto t = std::make_unique<LayoutBox>();
        t->kind = BoxKind::Text; t->anonymous = true; t->style = s;
        t->text = ToWide(s.content); t->href = href;
        box->kids.push_back(std::move(t));
    }
    if (box->kind == BoxKind::Block || box->kind == BoxKind::InlineBlock
     || box->kind == BoxKind::ListItem) AnonymousFixup(box.get());
    else box->establishesInline = !box->kids.empty();
    return box;
}

std::unique_ptr<LayoutBox> BuildBox(const Node* node, const ComputedStyle& parentStyle,
                                    const BuildCtx& bc, const std::string& inheritedHref,
                                    const ComputedStyle* precomputedStyle) {
    if (!node || node->type != NodeType::Element) return nullptr;
    DepthScope _d; if (g_depth > kMaxDepth) return nullptr;
    const std::string& tag = node->tagName;
    if (IsSkippedTag(tag)) return nullptr;
    if (HasAttr(node, "hidden")) return nullptr;
    if (HasClass(node, "oo-ui-element-hidden")) return nullptr;
    if (HasClass(node, "vector-menu-item--collapsible")
        && HasAncestorClass(node, "vector-user-links-main")) return nullptr;
    if (HasClass(node, "vector-menu-item--collapsible")
        && HasAncestorClass(node, "vector-collapsible")) return nullptr;
    if (tag == "dialog" && !HasAttr(node, "open")) return nullptr;
    if (tag == "input" && LowerAscii(node->attr("type")) == "hidden") return nullptr;

    ComputedStyle s = precomputedStyle ? *precomputedStyle
        : (bc.sheet ? bc.sheet->resolve(node) : ComputedStyle{});
    if (!precomputedStyle) {
        InheritInto(s, parentStyle);
        ApplyUaDefaults(tag, s);
        ResolveStyleVariables(s);
        if (bc.sheet && s.animationSet) {
            AnimationManager::instance().ensureStarted(node, s, *bc.sheet);
            AnimationManager::instance().applyAnimation(node, s);
        }
    }
    if (s.isDisplayNone()) return nullptr;

    // Effective display.
    int disp = s.display != 0 ? s.display : UaDisplay(tag);
    if (disp == 3) return nullptr;

    std::string href = inheritedHref;
    bool download = false;
    std::string downloadName;
    if (tag == "a" && !node->attr("href").empty())
        href = ResolveUrlAgainstBase(node->attr("href"), bc.baseUrl);
    if (tag == "a" && node->attrs.find("download") != node->attrs.end()) {
        download = true;
        downloadName = node->attr("download");
    } else {
        for (const Node* a = node->parent; a; a = a->parent) {
            if (a->tagName == "a" && a->attrs.find("download") != a->attrs.end()) {
                download = true;
                downloadName = a->attr("download");
                break;
            }
        }
    }

    std::string imgUrl;
    bool replaced = TagIsReplacedImage(node, bc.baseUrl, imgUrl);
    std::string mediaUrl;
    bool media = TagIsMedia(node, bc.baseUrl, mediaUrl);
    bool formControl = (tag == "input" || tag == "textarea" || tag == "select"
                     || (tag == "button" && !HasElementChild(node)));
    bool isSvg = (tag == "svg");
    bool isCanvas = (tag == "canvas");

    auto box = std::make_unique<LayoutBox>();
    box->node = node;
    box->style = s;
    box->href = href;
    box->download = download;
    box->downloadName = downloadName;

    if (tag == "br") {
        box->kind = BoxKind::Break;
        return box;
    }

    if (replaced || media || formControl || isSvg || isCanvas) {
        // Atomic replaced box. inline-block if not display:block.
        box->kind = (disp == 1) ? BoxKind::InlineBlock : BoxKind::Replaced;
        if (disp == 1) box->kind = BoxKind::InlineBlock; // atomic, block-positioned handled by flow
        box->replacedUrl = media ? mediaUrl : imgUrl;
        if (replaced && bc.measure) {
            float iw = 0, ih = 0;
            if (bc.measure->ImageIntrinsic(imgUrl, iw, ih)) {
                box->intrinsicW = iw; box->intrinsicH = ih;
            } else {
                bc.measure->RequestImage(imgUrl);
                // Reserve space from the width/height attributes so layout is
                // stable while images stream in (avoids a relayout storm and
                // keeps replaced boxes from collapsing to 0×0 mid-load).
                auto attrPx = [&](const char* a) -> float {
                    std::string v = node->attr(a);
                    if (v.empty()) return 0.f;
                    try { return (float)std::stof(v); } catch (...) { return 0.f; }
                };
                box->intrinsicW = attrPx("width");
                box->intrinsicH = attrPx("height");
            }
        }
        if (media) {
            box->intrinsicW = 300.f;
            box->intrinsicH = (tag == "audio") ? 32.f : 150.f;
        }
        if (formControl) {
            if (tag == "textarea") {
                box->intrinsicW = 180.f;
                box->intrinsicH = 64.f;
            } else if (tag == "button") {
                float textW = 0.f;
                if (bc.measure) {
                    FontKey fk;
                    fk.size = std::max(1.f, s.fontSize > 0 ? s.fontSize : 14.f);
                    fk.bold = s.bold;
                    fk.italic = s.italic;
                    fk.family = s.fontFamily;
                    textW = bc.measure->MeasureText(ToWide(TextContent(node)), fk);
                }
                box->intrinsicW = std::max(56.f, textW + 24.f);
                box->intrinsicH = 32.f;
            } else if (tag == "select") {
                float textW = 0.f;
                if (bc.measure) {
                    FontKey fk;
                    fk.size = std::max(1.f, s.fontSize > 0 ? s.fontSize : 14.f);
                    fk.family = s.fontFamily;
                    textW = bc.measure->MeasureText(ToWide(SelectedOptionText(node)), fk);
                }
                box->intrinsicW = std::max(56.f, textW + 28.f);
                box->intrinsicH = 32.f;
            } else {
                box->intrinsicW = 180.f;
                box->intrinsicH = 32.f;
            }
        }
        if (isSvg) {
            // Parse viewBox or width/height for intrinsic size.
            auto attrF = [&](const char* a) -> float {
                std::string v = node->attr(a);
                if (v.empty()) return 0.f;
                try { return std::stof(v); } catch (...) { return 0.f; }
            };
            float w = attrF("width"), h = attrF("height");
            std::string vb = node->attr("viewBox");
            if (vb.empty()) vb = node->attr("viewbox");
            if ((w <= 0 || h <= 0) && !vb.empty()) {
                float vx=0,vy=0,vw=0,vh=0;
                std::istringstream ss(vb); char c;
                if (ss >> vx >> c >> vy >> c >> vw >> c >> vh) { if (vw>0) w=vw; if (vh>0) h=vh; }
                else { ss.clear(); ss.str(vb); ss >> vx >> vy >> vw >> vh; if (vw>0) w=vw; if (vh>0) h=vh; }
            }
            if (w > 0) box->intrinsicW = w;
            if (h > 0) box->intrinsicH = h;
            box->replacedUrl = "__svg__";
        }
        if (isCanvas) {
            // Canvas's width/height attributes are its drawing-buffer size
            // in device pixels, not CSS sizing — distinct from every other
            // replaced element here. Default is 300x150 per spec when unset.
            auto attrInt = [&](const char* a, int def) -> int {
                std::string v = node->attr(a);
                if (v.empty()) return def;
                try { return std::max(0, std::stoi(v)); } catch (...) { return def; }
            };
            box->intrinsicW = (float)attrInt("width", 300);
            box->intrinsicH = (float)attrInt("height", 150);
            box->replacedUrl = "__canvas__";
        }
        return box;
    }

    box->kind = KindFromDisplay(disp, false);

    // Generated content: ::before then real children then ::after.
    if (auto before = BuildPseudo(node, "before", s, bc, href))
        box->kids.push_back(std::move(before));
    BuildChildren(node, s, bc, href, box->kids);
    if (auto after = BuildPseudo(node, "after", s, bc, href))
        box->kids.push_back(std::move(after));

    // List items: ensure a marker is accounted for at paint time (no box here).
    // Block containers / cells / list-items: apply anonymous fixup.
    if (box->kind == BoxKind::Block || box->kind == BoxKind::ListItem
     || box->kind == BoxKind::TableCell || box->kind == BoxKind::InlineBlock) {
        AnonymousFixup(box.get());
    } else if (box->kind == BoxKind::Inline) {
        // If an inline box contains block-level children (e.g. <a> with
        // <strong display:block> inside), promote it to a block container
        // so the block children get proper layout.
        bool hasBlockChild = false;
        for (auto& k : box->kids)
            if (!k->isInlineLevel()) { hasBlockChild = true; break; }
        if (hasBlockChild) {
            box->kind = BoxKind::Block;
            AnonymousFixup(box.get());
        } else {
            box->establishesInline = false; // inline boxes contribute to parent's IFC
        }
    } else if (box->kind == BoxKind::Table || box->kind == BoxKind::TableRow) {
        // tables handled structurally during layout
    }
    return box;
}

// ─── length resolution ───────────────────────────────────────────────────────

struct Engine {
    const LayoutInput& in;
    float Z;  // zoom
    std::unordered_map<const LayoutBox*, float> maxContentCache;

    explicit Engine(const LayoutInput& i) : in(i), Z(i.zoom) {}

    float px(float cssPx) const { return cssPx * Z; }

    FontKey fontFor(const ComputedStyle& s) const {
        FontKey f;
        f.size = std::min(px(s.fontSize > 0 ? s.fontSize : 16.f), 40.f);
        f.bold = s.bold; f.italic = s.italic;
        f.family = s.fontFamily;
        // monospace detection by family name
        std::string fl; for (char c : s.fontFamily) fl += (char)std::tolower((unsigned char)c);
        f.mono = (fl.find("consol") != std::string::npos || fl.find("mono") != std::string::npos
               || fl.find("courier") != std::string::npos);
        return f;
    }

    float lineHeightFor(const ComputedStyle& s) const {
        if (s.lineHeight > 0) return px(s.lineHeight);
        float fs = px(s.fontSize > 0 ? s.fontSize : 16.f);
        return fs * kNormalLH;
    }

    // Resolve used box-model edges (border, padding) for a box, in device px.
    void resolveEdges(LayoutBox& b) {
        const ComputedStyle& s = b.style;
        b.padTop    = s.paddingTop    >= 0 ? px(s.paddingTop)    : 0;
        b.padRight  = s.paddingRight  >= 0 ? px(s.paddingRight)  : 0;
        b.padBottom = s.paddingBottom >= 0 ? px(s.paddingBottom) : 0;
        b.padLeft   = s.paddingLeft   >= 0 ? px(s.paddingLeft)   : 0;
        float bw = s.borderWidth >= 0 ? s.borderWidth : 0;
        b.borderTop    = px(s.borderTopWidth    >= 0 ? s.borderTopWidth    : bw);
        b.borderRight  = px(s.borderRightWidth  >= 0 ? s.borderRightWidth  : bw);
        b.borderBottom = px(s.borderBottomWidth >= 0 ? s.borderBottomWidth : bw);
        b.borderLeft   = px(s.borderLeftWidth   >= 0 ? s.borderLeftWidth   : bw);
    }

    // Resolve a used width value from a CSS length/percentage against `cbW`.
    // Returns -1 for auto.
    float usedWidth(const ComputedStyle& s, float cbW) {
        if (s.widthCalcPercent >= 0) return std::max(0.f, cbW * (s.widthCalcPercent / 100.f) + px(s.widthCalcOffset));
        if (s.widthPercent >= 0) return cbW * (s.widthPercent / 100.f);
        if (s.width >= 0) return px(s.width);
        return -1;  // auto
    }
    float usedHeight(const ComputedStyle& s, float cbH) {
        if (s.height >= 0) return px(s.height);
        if (s.heightCalcPercent >= 0 && cbH >= 0)
            return std::max(0.f, cbH * (s.heightCalcPercent / 100.f) + px(s.heightCalcOffset));
        if (s.heightPercent >= 0 && cbH >= 0) return cbH * (s.heightPercent / 100.f);
        return -1;  // auto
    }
    // Resolve max/min-width against the containing block width (-1 = none).
    float usedMaxWidth(const ComputedStyle& s, float cbW) {
        if (s.maxWidth >= 0) return px(s.maxWidth);
        if (s.maxWidthPercent >= 0) return cbW * (s.maxWidthPercent / 100.f);
        return -1;
    }
    float usedMinWidth(const ComputedStyle& s, float cbW) {
        if (s.minWidth >= 0) return px(s.minWidth);
        if (s.minWidthPercent >= 0) return cbW * (s.minWidthPercent / 100.f);
        return -1;
    }
    float usedMaxHeight(const ComputedStyle& s, float cbH) {
        if (s.maxHeight >= 0) return px(s.maxHeight);
        if (s.maxHeightPercent >= 0 && cbH >= 0) return cbH * (s.maxHeightPercent / 100.f);
        return -1;
    }
    float usedMinHeight(const ComputedStyle& s, float cbH) {
        if (s.minHeight >= 0) return px(s.minHeight);
        if (s.minHeightPercent >= 0 && cbH >= 0) return cbH * (s.minHeightPercent / 100.f);
        return -1;
    }

    // Horizontal margin+border+padding of a box, in device px (auto margins → 0).
    float hExtra(const ComputedStyle& s) {
        float ml = s.marginLeftSet()  && !s.isMarginAuto(s.marginLeft)  ? px(s.marginLeft)  : 0;
        float mr = s.marginRightSet() && !s.isMarginAuto(s.marginRight) ? px(s.marginRight) : 0;
        float pl = s.paddingLeft  >= 0 ? px(s.paddingLeft)  : 0;
        float pr = s.paddingRight >= 0 ? px(s.paddingRight) : 0;
        float bw = s.borderWidth >= 0 ? s.borderWidth : 0;
        float bl = px(s.borderLeftWidth  >= 0 ? s.borderLeftWidth  : bw);
        float br = px(s.borderRightWidth >= 0 ? s.borderRightWidth : bw);
        return ml + mr + pl + pr + bl + br;
    }

    // Max-content width of a box's *content* (device px): the width it would
    // take if nothing wrapped. Used for shrink-to-fit (float / inline-block /
    // absolutely positioned with auto width).
    float maxContent(LayoutBox& box) {
        DepthScope _d; if (g_depth > kMaxDepth) return 0;
        const ComputedStyle& s = box.style;
        if (s.width >= 0) return px(s.width);
        if (s.widthCalcPercent >= 0) return 0;  // % component handled by caller
        if (s.widthPercent >= 0) return 0;  // % handled by caller
        auto cached = maxContentCache.find(&box);
        if (cached != maxContentCache.end()) return cached->second;

        if (box.kind == BoxKind::Text) {
            FontKey f = fontFor(box.style);
            float result = in.measure ? in.measure->MeasureText(box.text, f) : (float)box.text.size() * f.size * 0.5f;
            if (s.minWidth >= 0) result = std::max(result, px(s.minWidth));
            maxContentCache[&box] = result;
            return result;
        }
        if (box.kind == BoxKind::Replaced || (!box.replacedUrl.empty() && box.kids.empty())) {
            float result = box.intrinsicW > 0 ? px(box.intrinsicW) : 0;
            if (s.minWidth >= 0) result = std::max(result, px(s.minWidth));
            maxContentCache[&box] = result;
            return result;
        }
        if (box.establishesInline || box.kind == BoxKind::Inline
         || box.kind == BoxKind::InlineBlock) {
            // Inline content sits on one line: sum child outer widths.
            float sum = 0;
            for (auto& k : box.kids) sum += maxContent(*k) + hExtra(k->style);
            if (s.minWidth >= 0) sum = std::max(sum, px(s.minWidth));
            maxContentCache[&box] = sum;
            return sum;
        }
        // Block container: widest child.
        float w = 0;
        for (auto& k : box.kids) {
            if (k->isOutOfFlow()) continue;
            w = std::max(w, maxContent(*k) + hExtra(k->style));
        }
        if (s.minWidth >= 0) w = std::max(w, px(s.minWidth));
        maxContentCache[&box] = w;
        return w;
    }

    // ── forward decls ────────────────────────────────────────────────────────
    void layoutBlockChildren(LayoutBox& box, std::vector<LayoutBox*>& positionedOut,
                             struct FloatCtx* inherited = nullptr);
    void layoutFlex(LayoutBox& box, std::vector<LayoutBox*>& positionedOut);
    void layoutGrid(LayoutBox& box, std::vector<LayoutBox*>& positionedOut);
    void layoutTable(LayoutBox& box);
    float layoutInline(LayoutBox& box, struct FloatCtx* fctx);
    void layoutBox(LayoutBox& box, float cbX, float cbW, float cbH,
                   std::vector<LayoutBox*>& positionedOut, struct FloatCtx* parentFloats,
                   bool shrinkToFit = false);
    void layoutPositioned(LayoutBox& root, std::vector<LayoutBox*>& positioned);
    void collectPositioned(LayoutBox& box, LayoutBox* nearestPositioned,
                           LayoutBox* nearestFixedContainingBlock,
                           std::vector<std::pair<LayoutBox*, LayoutBox*>>& out);
};

// ─── float context ───────────────────────────────────────────────────────────

struct FloatBox { float top, bottom, left, right; int side; };  // side:1 left,2 right
struct FloatCtx {
    std::vector<FloatBox> floats;
    float cbLeft = 0, cbRight = 0;   // BFC content edges

    float leftEdge(float y, float h) const {
        float e = cbLeft;
        for (auto& f : floats)
            if (f.side == 1 && y < f.bottom && (y + h) > f.top) e = std::max(e, f.right);
        return e;
    }
    float rightEdge(float y, float h) const {
        float e = cbRight;
        for (auto& f : floats)
            if (f.side == 2 && y < f.bottom && (y + h) > f.top) e = std::min(e, f.left);
        return e;
    }
    float nextBreakBelow(float y) const {
        float best = 1e9f;
        for (auto& f : floats) if (f.bottom > y) best = std::min(best, f.bottom);
        return best == 1e9f ? y : best;
    }
    float clearTo(float y, int clearMode) const {
        float r = y;
        for (auto& f : floats) {
            if (clearMode == 3 || (clearMode == 1 && f.side == 1) || (clearMode == 2 && f.side == 2))
                r = std::max(r, f.bottom);
        }
        return r;
    }
    float lowestBottom() const { float r = 0; for (auto& f : floats) r = std::max(r, f.bottom); return r; }
};

// ─── margin collapsing helper ────────────────────────────────────────────────
float collapseMargins(float a, float b) {
    if (a >= 0 && b >= 0) return std::max(a, b);
    if (a < 0 && b < 0)   return std::min(a, b);
    return a + b;
}

static void TranslateSubtree(LayoutBox& box, float dx, float dy);

// A box establishes a *new* block formatting context when it is floated,
// out-of-flow, a flex/table container, or has non-visible overflow. Such a box
// does NOT let its ancestors' floats intrude into its content — text inside it
// must not wrap around an outside float. Everything else inherits intruding
// floats so paragraphs wrap around a sibling float (e.g. a Wikipedia infobox).
static bool establishesNewBfc(const ComputedStyle& s) {
    return s.floatMode != 0
        || s.positionMode == 2 || s.positionMode == 3
        || s.isDisplayFlowRoot()
        || s.isDisplayFlex() || s.isDisplayInlineBlock()
        || s.isDisplayTableCell()
        || s.overflowHidden;
}

// ─── block layout ────────────────────────────────────────────────────────────

static void TranslateSubtree(LayoutBox& box, float dx, float dy);

// Lay out a single box (any kind) whose border-box top is to be placed by the
// caller. The caller sets box.x/box.y for the border-box origin BEFORE calling
// for in-flow blocks (we set x here from cbX + margins). Returns via box fields.
void Engine::layoutBox(LayoutBox& box, float cbX, float cbW, float cbH,
                       std::vector<LayoutBox*>& positionedOut, FloatCtx* parentFloats,
                       bool shrinkToFit) {
    DepthScope _d; if (g_depth > kMaxDepth) { box.contentW = std::max(0.f, cbW); return; }
    const ComputedStyle& s = box.style;
    resolveEdges(box);

    // Margins (auto handled for centering below).
    bool mlAuto = s.isMarginAuto(s.marginLeft);
    bool mrAuto = s.isMarginAuto(s.marginRight);
    box.marginTop    = s.marginTopSet()    && !s.isMarginAuto(s.marginTop)    ? px(s.marginTop)    : 0;
    box.marginBottom = s.marginBottomSet() && !s.isMarginAuto(s.marginBottom) ? px(s.marginBottom) : 0;
    box.marginLeft   = (s.marginLeftSet()  && !mlAuto) ? px(s.marginLeft)  : 0;
    box.marginRight  = (s.marginRightSet() && !mrAuto) ? px(s.marginRight) : 0;

    // Resolve width.
    float w = usedWidth(s, cbW);
    float bp = box.borderLeft + box.padLeft + box.borderRight + box.padRight;
    // box-sizing: border-box — the specified width already includes padding and
    // border, so subtract them to get the content width.
    bool borderBox = (s.boxSizing == 1);
    if (borderBox && w >= 0) w = std::max(0.f, w - bp);
    // Vertical border+padding, used to convert border-box heights to content-box.
    float vbp = box.borderTop + box.padTop + box.borderBottom + box.padBottom;
    auto bbHeight = [&](float h) { return (borderBox && h >= 0) ? std::max(0.f, h - vbp) : h; };
    // Intrinsic-sizing keywords (min-content/max-content/fit-content).
    if (s.widthKeyword != 0) {
        float mc = maxContent(box);
        if (s.widthKeyword == 3)  // fit-content: clamp max-content to available
            w = std::min(mc, std::max(0.f, cbW - box.marginLeft - box.marginRight - bp));
        else                      // min/max-content approximated to max-content
            w = mc;
    }
    bool isBlockLevel = (box.kind == BoxKind::Block || box.kind == BoxKind::ListItem
                      || box.kind == BoxKind::Table || box.kind == BoxKind::TableRow
                      || box.kind == BoxKind::TableCell);
    bool atomic = (box.kind == BoxKind::InlineBlock || box.kind == BoxKind::Replaced);

    if (w < 0) {
        if (atomic || shrinkToFit) {
            float avail = std::max(0.f, cbW - box.marginLeft - box.marginRight - bp);
            float mc = maxContent(box);
            w = std::min(mc, avail);
            if (atomic && box.intrinsicW > 0 && w <= 0) w = px(box.intrinsicW);
        } else {
            // block: fill containing block minus margins + borders + padding.
            w = std::max(0.f, cbW - box.marginLeft - box.marginRight - bp);
        }
    }
    // min/max width clamp. Under border-box these constraints also include
    // padding+border, so reduce them to content-box before clamping.
    { float mw = usedMaxWidth(s, cbW); if (mw >= 0) { if (borderBox) mw = std::max(0.f, mw - bp); w = std::min(w, mw); } }
    { float nw = usedMinWidth(s, cbW); if (nw >= 0) { if (borderBox) nw = std::max(0.f, nw - bp); w = std::max(w, nw); } }
    box.contentW = w;

    // Auto-margin horizontal centering for block-level with definite width.
    if (isBlockLevel && (mlAuto || mrAuto)) {
        float free = cbW - w - bp - box.marginLeft - box.marginRight;
        if (free < 0) free = 0;
        if (mlAuto && mrAuto) { box.marginLeft = box.marginRight = free / 2; }
        else if (mlAuto)      { box.marginLeft = free; }
        else if (mrAuto)      { box.marginRight = free; }
    }

    box.x = cbX + box.marginLeft;  // border-box left

    // Replaced: resolve height from intrinsic / CSS, no children.
    if (box.kind == BoxKind::Replaced || (!box.replacedUrl.empty() && box.kids.empty())) {
        float h = bbHeight(usedHeight(s, cbH));
        if (box.intrinsicW > 0 && box.contentW <= 0) box.contentW = px(box.intrinsicW);
        if (h < 0) {
            if (box.intrinsicW > 0 && box.intrinsicH > 0 && box.contentW > 0)
                h = box.contentW * (box.intrinsicH / box.intrinsicW);
            else h = box.intrinsicH > 0 ? px(box.intrinsicH) : 0;
        }
        { float mh = usedMaxHeight(s, cbH); if (mh >= 0) h = std::min(h, bbHeight(mh)); }
        { float nh = usedMinHeight(s, cbH); if (nh >= 0) h = std::max(h, bbHeight(nh)); }
        box.contentH = h;
        return;
    }

    // Tables lay out specially: cells in rows, shrink-to-fit overall width.
    if (box.kind == BoxKind::Table) {
        layoutTable(box);
        float eh = bbHeight(usedHeight(s, cbH));
        if (eh >= 0) box.contentH = eh;
        { float mh = usedMaxHeight(s, cbH); if (mh >= 0) box.contentH = std::min(box.contentH, bbHeight(mh)); }
        { float nh = usedMinHeight(s, cbH); if (nh >= 0) box.contentH = std::max(box.contentH, bbHeight(nh)); }
        return;
    }

    // Establish inline formatting context or block formatting.
    float explicitH = bbHeight(usedHeight(s, cbH));

    if (s.isDisplayFlex()) {
        std::vector<LayoutBox*> positioned;
        layoutFlex(box, positioned);
        for (auto* p : positioned) positionedOut.push_back(p);
        if (explicitH >= 0) box.contentH = explicitH;
    } else if (s.isDisplayGrid()) {
        std::vector<LayoutBox*> positioned;
        layoutGrid(box, positioned);
        for (auto* p : positioned) positionedOut.push_back(p);
        if (explicitH >= 0) box.contentH = explicitH;
    } else if (box.establishesInline) {
        if (explicitH >= 0) box.contentH = explicitH;
        FloatCtx local;
        local.cbLeft = box.contentX();
        local.cbRight = box.contentX() + box.contentW;
        // Inherit floats that intrude from the parent block formatting context
        // (e.g. a float:right infobox that is a sibling of this text block) so
        // line boxes wrap around the float instead of painting over it. All
        // float geometry is document-absolute, so it composes directly.
        if (parentFloats && !establishesNewBfc(s)) {
            for (const auto& f : parentFloats->floats)
                if (f.bottom > box.contentY()) local.floats.push_back(f);
        }
        // Note: box.y must already be set by caller (we use contentY()).
        float h = layoutInline(box, &local);
        box.contentH = explicitH >= 0 ? explicitH : h;
    } else {
        std::vector<LayoutBox*> positioned;
        FloatCtx* inherit = (parentFloats && !establishesNewBfc(s)) ? parentFloats : nullptr;
        layoutBlockChildren(box, positioned, inherit);
        for (auto* p : positioned) positionedOut.push_back(p);
        if (explicitH >= 0) box.contentH = explicitH;

        // Multi-column layout: distribute children across N columns.
        if (s.columnCountSet && s.columnCount > 1 && !box.kids.empty()) {
            int cols = s.columnCount;
            if (cols <= 0) cols = 1; // guard against invalid column count
            float gap = px(s.columnGap);
            float colW = (box.contentW - gap * (cols - 1)) / cols;

            // Compute total content height from all children.
            float totalH = box.contentH;
            float targetH = totalH / cols;  // ideal column height

            // Assign each child to a column based on running height.
            float runH = 0;
            int curCol = 0;
            float colY = box.contentY();
            for (auto& k : box.kids) {
                if (k->isOutOfFlow()) continue;
                float childH = k->marginBoxH();
                if (runH + childH > targetH && curCol < cols - 1 && runH > 0) {
                    curCol++;
                    runH = 0;
                }
                float dx = curCol * (colW + gap) - (k->x - box.contentX());
                float dy = colY + runH - k->y;
                // Resize child to column width.
                float oldW = k->contentW;
                k->contentW = std::max(0.f, colW - (k->borderBoxW() - oldW));
                TranslateSubtree(*k, box.contentX() + curCol * (colW + gap) + k->marginLeft - k->x, dy);
                runH += childH;
            }
            // Content height = tallest column.
            float maxColH = 0;
            float colHeights[16] = {};
            for (auto& k : box.kids) {
                if (k->isOutOfFlow()) continue;
                float relX = k->x - box.contentX();
                int c = (int)(relX / (colW + gap + 0.1f));
                if (c >= 0 && c < cols && c < 16)
                    colHeights[c] = std::max(colHeights[c], k->y + k->marginBoxH() - box.contentY());
            }
            for (int c = 0; c < std::min(cols, 16); ++c)
                maxColH = std::max(maxColH, colHeights[c]);
            box.contentH = maxColH;
        }
    }

    // aspect-ratio: if height is auto and aspect-ratio is set, derive height from width.
    if (s.aspectRatioSet && s.aspectRatio > 0 && explicitH < 0)
        box.contentH = box.contentW / s.aspectRatio;

    // min/max height clamp on the final content height.
    { float mh = usedMaxHeight(s, cbH); if (mh >= 0) box.contentH = std::min(box.contentH, bbHeight(mh)); }
    { float nh = usedMinHeight(s, cbH); if (nh >= 0) box.contentH = std::max(box.contentH, bbHeight(nh)); }
}

// Lay out the in-flow block-level children of `box`, stacking vertically with
// margin collapsing and float handling. Sets box.contentH.
void Engine::layoutBlockChildren(LayoutBox& box, std::vector<LayoutBox*>&
                                 positionedOut, FloatCtx* inherited) {
    DepthScope _d; if (g_depth > kMaxDepth) return;
    FloatCtx fctx;
    fctx.cbLeft  = box.contentX();
    fctx.cbRight = box.contentX() + box.contentW;
    // Inherit intruding floats from an ancestor BFC so a float (e.g. an infobox)
    // keeps excluding content nested inside sibling wrapper blocks. Geometry is
    // document-absolute. These don't belong to this box, so they must not
    // stretch its height — track where the owned floats begin.
    if (inherited) {
        for (const auto& f : inherited->floats)
            if (f.bottom > box.contentY()) fctx.floats.push_back(f);
    }
    const size_t inheritedFloatCount = fctx.floats.size();
    const float specifiedHeight = usedHeight(box.style, -1.f);
    const float verticalBp = box.borderTop + box.padTop + box.borderBottom + box.padBottom;
    const float childCbH = specifiedHeight >= 0
        ? ((box.style.boxSizing == 1) ? std::max(0.f, specifiedHeight - verticalBp) : specifiedHeight)
        : -1.f;

    float cursorY = box.contentY();
    float prevMarginBottom = 0;
    bool first = true;

    // Special structural handling: TableRow lays children side-by-side.
    if (box.kind == BoxKind::TableRow) {
        float x = box.contentX();
        float maxH = 0;
        int cells = 0;
        for (auto& k : box.kids) if (k->kind == BoxKind::TableCell) cells++;
        for (auto& k : box.kids) {
            if (k->kind != BoxKind::TableCell) continue;
            float cellW = k->style.width >= 0 ? px(k->style.width)
                        : (box.contentW / std::max(1, cells));
            k->y = cursorY;
            std::vector<LayoutBox*> pos;
            layoutBox(*k, x, cellW, childCbH, pos, &fctx);
            for (auto* p : pos) positionedOut.push_back(p);
            maxH = std::max(maxH, k->marginBoxH());
            x += k->marginBoxW();
        }
        // Stretch cells to row height.
        for (auto& k : box.kids)
            if (k->kind == BoxKind::TableCell)
                k->contentH = std::max(k->contentH, maxH - k->borderBoxH() + k->contentH);
        box.contentH = maxH;
        return;
    }

    for (auto& kptr : box.kids) {
        LayoutBox* k = kptr.get();

        // Out-of-flow: defer to positioned pass.
        if (k->isOutOfFlow()) { positionedOut.push_back(k); continue; }

        // Float: place on its side at the current y.
        if (k->isFloat()) {
            float fy = cursorY;
            if (k->style.clearMode != 0)
                fy = fctx.clearTo(fy, k->style.clearMode);
            std::vector<LayoutBox*> pos;
            // Floats with auto width shrink-to-fit their content.
            layoutBox(*k, box.contentX(), box.contentW, childCbH, pos, &fctx, /*shrinkToFit=*/true);
            for (auto* p : pos) positionedOut.push_back(p);
            float fw = k->marginBoxW();
            // Find a vertical band where the float fits horizontally.
            float availL = fctx.leftEdge(fy, k->marginBoxH());
            float availR = fctx.rightEdge(fy, k->marginBoxH());
            while ((availR - availL) < fw && fctx.nextBreakBelow(fy) > fy) {
                fy = fctx.nextBreakBelow(fy);
                availL = fctx.leftEdge(fy, k->marginBoxH());
                availR = fctx.rightEdge(fy, k->marginBoxH());
            }
            float targetX = k->style.floatMode == 1
                ? availL + k->marginLeft
                : availR - k->marginBoxW() + k->marginLeft;
            float targetY = fy + k->marginTop;
            TranslateSubtree(*k, targetX - k->x, targetY - k->y);
            fctx.floats.push_back({ fy, fy + k->marginBoxH(),
                                    k->x - k->marginLeft, k->x - k->marginLeft + k->borderBoxW(),
                                    k->style.floatMode });
            continue;
        }

        // Clearance.
        if (k->style.clearMode != 0) {
            float cleared = fctx.clearTo(cursorY, k->style.clearMode);
            if (cleared > cursorY) { cursorY = cleared; prevMarginBottom = 0; }
        }

        // Resolve this child's top margin for collapsing.
        float childMTop = k->style.marginTopSet() && !k->style.isMarginAuto(k->style.marginTop)
                        ? px(k->style.marginTop) : 0;
        if (first) cursorY += childMTop;                       // (no parent/child collapse)
        else       cursorY += collapseMargins(prevMarginBottom, childMTop);

        k->y = cursorY;  // border-box top
        std::vector<LayoutBox*> pos;
        layoutBox(*k, box.contentX(), box.contentW, childCbH, pos, &fctx);
        for (auto* p : pos) positionedOut.push_back(p);

        cursorY = k->y + k->borderBoxH();
        prevMarginBottom = k->marginBottom;
        first = false;
    }

    // Bottom edge includes the last child's bottom margin and any floats this
    // box owns (inherited ancestor floats do not stretch it).
    float contentBottom = cursorY + prevMarginBottom;
    for (size_t fi = inheritedFloatCount; fi < fctx.floats.size(); ++fi)
        contentBottom = std::max(contentBottom, fctx.floats[fi].bottom);
    box.contentH = std::max(0.f, contentBottom - box.contentY());
}

// Shift a laid-out box and all its descendants/line boxes by (dx, dy). Needed
// because layout assigns document-absolute coordinates, so moving a flex item
// after the fact (align-items center/end) must move its whole subtree.
static void TranslateSubtree(LayoutBox& box, float dx, float dy) {
    box.x += dx; box.y += dy;
    for (auto& line : box.lines) {
        line.x += dx; line.y += dy;
        for (auto& frag : line.frags) { frag.x += dx; frag.y += dy; }
    }
    for (auto& kid : box.kids) TranslateSubtree(*kid, dx, dy);
}

void Engine::layoutFlex(LayoutBox& box, std::vector<LayoutBox*>& positionedOut) {
    std::vector<LayoutBox*> items;
    for (auto& child : box.kids) {
        if (child->isOutOfFlow()) positionedOut.push_back(child.get());
        else items.push_back(child.get());
    }
    if (items.empty()) { box.contentH = 0; return; }

    const float gap = px(std::max(0.f, box.style.flexGap));
    const float cbHeight = usedHeight(box.style, -1.f);
    const bool column = (box.style.flexDirection == 1);
    const bool wrap = (box.style.flexWrap != 0);
    const bool wrapReverse = (box.style.flexWrap == 2);
    const int containerAlign = box.style.alignItems;  // 0=stretch,1=start,2=center,3=end

    // ── column direction: simple vertical stacking ─────────────────────────
    if (column) {
        float cursorY = box.contentY();
        for (LayoutBox* item : items) {
            item->y = cursorY;
            std::vector<LayoutBox*> positioned;
            layoutBox(*item, box.contentX(), box.contentW, cbHeight, positioned, nullptr);
            for (auto* p : positioned) positionedOut.push_back(p);
            cursorY += item->marginBoxH() + gap;
        }
        box.contentH = std::max(0.f, cursorY - box.contentY() - gap);
        return;
    }

    // ── row direction: flex-wrap, grow, shrink, basis, align-self ───────────

    struct FlexItem { LayoutBox* box; float basis; float grow; float shrink; float extra; float minMain; };

    // Compute hypothetical main sizes (basis + hExtra).
    std::vector<FlexItem> allItems;
    for (LayoutBox* item : items) {
        float basis;
        float minMain = 0.f;
        if (item->style.flexBasisSet && item->style.flexBasis >= 0)
            basis = px(item->style.flexBasis);
        else {
            basis = usedWidth(item->style, box.contentW);
            if (basis < 0) {
                basis = maxContent(*item);
                minMain = basis;
            }
        }
        basis = std::max(0.f, basis);
        float grow = item->style.flexGrowSet ? item->style.flexGrow : 0.f;
        float shrink = item->style.flexShrinkSet ? item->style.flexShrink : 1.f;
        float extra = hExtra(item->style);
        allItems.push_back({ item, basis, grow, shrink, extra, minMain });
    }

    // Split items into flex lines (all on one line if nowrap).
    struct FlexLine { std::vector<size_t> indices; float maxH = 0; float lineY = 0; };
    std::vector<FlexLine> lines;
    {
        FlexLine cur;
        float lineUsed = 0;
        for (size_t i = 0; i < allItems.size(); ++i) {
            float itemMain = allItems[i].basis + allItems[i].extra;
            float gapBefore = cur.indices.empty() ? 0 : gap;
            if (wrap && !cur.indices.empty() && (lineUsed + gapBefore + itemMain) > box.contentW) {
                lines.push_back(std::move(cur));
                cur = FlexLine{};
                lineUsed = 0;
                gapBefore = 0;
            }
            cur.indices.push_back(i);
            lineUsed += gapBefore + itemMain;
        }
        if (!cur.indices.empty()) lines.push_back(std::move(cur));
    }

    // Layout each flex line.
    float cursorY = box.contentY();
    for (FlexLine& line : lines) {
        float used = gap * std::max(0, (int)line.indices.size() - 1);
        float growTotal = 0, shrinkTotal = 0;
        for (size_t idx : line.indices) {
            used += allItems[idx].basis + allItems[idx].extra;
            growTotal += allItems[idx].grow;
            shrinkTotal += allItems[idx].shrink;
        }

        float slack = box.contentW - used;  // positive = free space, negative = overflow
        float justifyOffset = 0.f;
        float lineGap = gap;
        if (slack > 0 && growTotal <= 0) {
            if (box.style.justifyContent == 1) {
                justifyOffset = slack / 2.f;
            } else if (box.style.justifyContent == 2) {
                justifyOffset = slack;
            } else if (box.style.justifyContent == 3 && line.indices.size() > 1) {
                lineGap += slack / static_cast<float>(line.indices.size() - 1);
            }
        }
        float cursorX = box.contentX() + justifyOffset;

        for (size_t idx : line.indices) {
            FlexItem& fi = allItems[idx];
            LayoutBox& child = *fi.box;

            float w = fi.basis;
            if (slack > 0 && growTotal > 0)
                w += slack * fi.grow / growTotal;
            else if (slack < 0 && shrinkTotal > 0)
                w += slack * fi.shrink / shrinkTotal;
            w = std::max(w, fi.minMain);
            w = std::max(0.f, w);

            float marginLeft = child.style.marginLeftSet() && !child.style.isMarginAuto(child.style.marginLeft)
                ? px(child.style.marginLeft) : 0.f;
            const float oldWidth = child.style.width;
            const float oldPercent = child.style.widthPercent;
            const float oldCalcPercent = child.style.widthCalcPercent;
            const float oldCalcOffset = child.style.widthCalcOffset;
            child.style.width = w / std::max(0.01f, Z);
            child.style.widthPercent = -1;
            child.style.widthCalcPercent = -1;
            child.style.widthCalcOffset = 0;
            child.y = cursorY;
            std::vector<LayoutBox*> positioned;
            layoutBox(child, cursorX - marginLeft, box.contentW, cbHeight, positioned, nullptr);
            for (auto* p : positioned) positionedOut.push_back(p);
            child.style.width = oldWidth;
            child.style.widthPercent = oldPercent;
            child.style.widthCalcPercent = oldCalcPercent;
            child.style.widthCalcOffset = oldCalcOffset;
            cursorX += child.marginBoxW() + lineGap;
            line.maxH = std::max(line.maxH, child.marginBoxH());
        }

        line.lineY = cursorY;

        // Cross-axis alignment for this line.
        float lineH = line.maxH;
        for (size_t idx : line.indices) {
            LayoutBox& child = *allItems[idx].box;
            int align = (child.style.alignSelfSet && child.style.alignSelf >= 0)
                      ? child.style.alignSelf : containerAlign;
            float boxH = child.marginBoxH();
            if (align == 0) {
                if (child.style.height < 0 && child.style.heightPercent < 0
                    && child.kind != BoxKind::Replaced && boxH < lineH)
                    child.contentH += (lineH - boxH);
            } else if (align == 2) {
                TranslateSubtree(child, 0.f, (lineH - boxH) / 2.f);
            } else if (align == 3) {
                TranslateSubtree(child, 0.f, lineH - boxH);
            }
        }

        cursorY += lineH + gap;
    }

    float totalH = std::max(0.f, cursorY - box.contentY() - gap);
    float crossH = usedHeight(box.style, -1.f);
    box.contentH = crossH >= 0 ? crossH : totalH;

    // wrap-reverse: flip line order vertically.
    if (wrapReverse && lines.size() > 1) {
        float top = box.contentY();
        float bottom = top + box.contentH;
        for (auto& line : lines) {
            float lineBottom = line.lineY + line.maxH;
            float mirrorY = bottom - (lineBottom - top);
            float dy = mirrorY - line.lineY;
            for (size_t idx : line.indices)
                TranslateSubtree(*allItems[idx].box, 0.f, dy);
                }
            }
        }
// ─── table layout (auto algorithm) ───────────────────────────────────────────
// ─── grid layout ─────────────────────────────────────────────────────────────
// Handles fixed, percentage, fr, and auto columns with gap. Items are placed
// in document order into the explicit column template, wrapping to new rows.
void Engine::layoutGrid(LayoutBox& box, std::vector<LayoutBox*>& positionedOut) {
    DepthScope _d; if (g_depth > kMaxDepth) return;
    const auto& tracks = box.style.gridTemplateColumns;
    const float gap = box.style.flexGap >= 0 ? px(box.style.flexGap) : 0.f;
    const float avail = box.contentW;

    // Collect in-flow children.
    std::vector<LayoutBox*> items;
    for (auto& k : box.kids) {
        if (k->isOutOfFlow()) { positionedOut.push_back(k.get()); continue; }
        items.push_back(k.get());
    }
    if (items.empty()) { box.contentH = 0; return; }

    // Resolve column widths. Track tokens: "Npx", "Nrem", "N%", "Nfr", "minmax(...)", "auto".
    size_t cols = tracks.empty() ? items.size() : tracks.size();
    std::vector<float> colW(cols, 0.f);
    float fixedSum = 0.f, frSum = 0.f;
    std::vector<float> frVals(cols, 0.f);
    for (size_t i = 0; i < cols; ++i) {
        if (i >= tracks.size() || tracks[i] == "auto") {
            frVals[i] = 1.f; frSum += 1.f; continue;
        }
        const std::string& t = tracks[i];
        if (t == "min-content" || t == "max-content" || t.rfind("fit-content(", 0) == 0) {
            colW[i] = 0.f;
            continue;
        }
        std::string track = t;
        if (track.rfind("minmax(", 0) == 0 && track.back() == ')') {
            size_t comma = track.find(',');
            if (comma != std::string::npos) track = track.substr(comma + 1, track.size() - comma - 2);
            while (!track.empty() && std::isspace((unsigned char)track.front())) track.erase(track.begin());
            while (!track.empty() && std::isspace((unsigned char)track.back())) track.pop_back();
        }
        size_t frPos = track.find("fr");
        if (frPos != std::string::npos) {
            float f = 1.f;
            try { f = std::stof(track.substr(0, frPos)); } catch (...) {}
            if (f < 0) f = 0;
            frVals[i] = f; frSum += f;
        } else if (!track.empty() && track.back() == '%') {
            float pct = 0;
            try { pct = std::stof(track.substr(0, track.size() - 1)); } catch (...) {}
            colW[i] = avail * (pct / 100.f);
            fixedSum += colW[i];
        } else {
            // Try to parse as a length value (e.g. "12.25rem", "200px" or bare "200").
            float v = -1;
            try { v = std::stof(track); } catch (...) {}
            if (v >= 0) {
                float unit = 1.f;
                if (track.find("rem") != std::string::npos || track.find("em") != std::string::npos) unit = 16.f;
                colW[i] = px(v * unit); fixedSum += colW[i];
            }
            else { frVals[i] = 1.f; frSum += 1.f; }
        }
    }
    // Distribute remaining space to fr tracks.
    float gapTotal = gap * std::max(0, (int)cols - 1);
    float frSpace = std::max(0.f, avail - fixedSum - gapTotal);
    if (frSum > 0) {
        for (size_t i = 0; i < cols; ++i)
            if (frVals[i] > 0) colW[i] = frSpace * (frVals[i] / frSum);
    }

    // Column x offsets.
    std::vector<float> colX(cols);
    { float x = box.contentX();
      for (size_t i = 0; i < cols; ++i) { colX[i] = x; x += colW[i] + gap; } }

    // Resolve row track heights (if grid-template-rows is set).
    const auto& rowTracks = box.style.gridTemplateRows;

    // Build a grid: assign items to (row, col) cells.
    // Items with explicit grid-column/grid-row go first; auto items fill gaps.
    auto resolveLineName = [&](const std::string& name, bool endLine) -> int {
        if (name.empty()) return 0;
        const std::string wanted = name + (endLine ? "-end" : "-start");
        const auto& lines = box.style.gridTemplateColumnLineNames;
        for (size_t i = 0; i < lines.size(); ++i)
            for (const auto& candidate : lines[i])
                if (candidate == wanted || candidate == name) return (int)i + 1;
        return 0;
    };
    struct AreaPlacement { int row = 0, col = 0, rowEnd = 0, colEnd = 0; };
    auto resolveArea = [&](const std::string& name) -> AreaPlacement {
        AreaPlacement out;
        if (name.empty()) return out;
        const auto& areas = box.style.gridTemplateAreas;
        for (size_t r = 0; r < areas.size(); ++r) {
            for (size_t c = 0; c < areas[r].size(); ++c) {
                if (areas[r][c] != name) continue;
                if (out.row == 0) {
                    out.row = (int)r + 1;
                    out.col = (int)c + 1;
                    out.rowEnd = out.row + 1;
                    out.colEnd = out.col + 1;
                } else {
                    out.row = std::min(out.row, (int)r + 1);
                    out.col = std::min(out.col, (int)c + 1);
                    out.rowEnd = std::max(out.rowEnd, (int)r + 2);
                    out.colEnd = std::max(out.colEnd, (int)c + 2);
                }
            }
        }
        return out;
    };
    size_t maxRow = 0;
    struct Cell { int row; int col; int rowSpan; int colSpan; LayoutBox* item; };
    std::vector<Cell> placed;
    std::vector<LayoutBox*> autoItems;
    for (auto* item : items) {
        int cs = item->style.gridColumnStart, ce = item->style.gridColumnEnd;
        int rs = item->style.gridRowStart, re = item->style.gridRowEnd;
        if (!item->style.gridAreaName.empty()) {
            AreaPlacement area = resolveArea(item->style.gridAreaName);
            if (area.col) { cs = area.col; ce = area.colEnd; }
            if (area.row) { rs = area.row; re = area.rowEnd; }
        }
        if (cs <= 0) cs = resolveLineName(item->style.gridColumnStartName, false);
        if (ce <= 0) ce = resolveLineName(item->style.gridColumnEndName, true);
        if (cs > 0 || rs > 0) {
            int c = cs > 0 ? cs - 1 : 0;  // 1-based to 0-based
            int r = rs > 0 ? rs - 1 : 0;
            int cspan = (ce > cs) ? ce - cs : 1;
            int rspan = (re > rs) ? re - rs : 1;
            placed.push_back({r, c, rspan, cspan, item});
            maxRow = std::max(maxRow, (size_t)(r + rspan));
        } else {
            autoItems.push_back(item);
        }
    }
    // Auto-place remaining items in row-major order, skipping occupied cells.
    std::set<std::pair<int,int>> occupied;
    for (auto& p : placed)
        for (int r = p.row; r < p.row + p.rowSpan; ++r)
            for (int c = p.col; c < p.col + p.colSpan; ++c)
                occupied.insert({r, c});
    int autoRow = 0, autoCol = 0;
    for (auto* item : autoItems) {
        int attempts = 0;
        while (occupied.count({autoRow, autoCol})) {
            if (++autoCol >= (int)cols) { autoCol = 0; ++autoRow; }
            if (++attempts > (int)(cols * (maxRow + autoItems.size() + 1))) {
                // Bail out if we've searched too many cells (grid likely full)
                autoRow = (int)maxRow;
                autoCol = 0;
                break;
            }
        }
        placed.push_back({autoRow, autoCol, 1, 1, item});
        occupied.insert({autoRow, autoCol});
        maxRow = std::max(maxRow, (size_t)(autoRow + 1));
        if (++autoCol >= (int)cols) { autoCol = 0; ++autoRow; }
    }

    // Layout each cell and track row heights.
    size_t numRows = maxRow > 0 ? maxRow : 1;
    std::vector<float> rowH(numRows, 0.f);
    for (auto& cell : placed) {
        LayoutBox& item = *cell.item;
        int c = std::min(cell.col, (int)cols - 1);
        float cellW = 0;
        for (int ci = c; ci < std::min(c + cell.colSpan, (int)cols); ++ci) cellW += colW[ci];
        if (cell.colSpan > 1) cellW += gap * (cell.colSpan - 1);
        float savedW = item.style.width, savedPct = item.style.widthPercent;
        float savedCalcPct = item.style.widthCalcPercent, savedCalcOffset = item.style.widthCalcOffset;
        item.style.width = std::max(0.f, cellW - hExtra(item.style)) / std::max(0.01f, Z);
        item.style.widthPercent = -1;
        item.style.widthCalcPercent = -1;
        item.style.widthCalcOffset = 0;
        std::vector<LayoutBox*> pos;
        layoutBox(item, colX[c], cellW, usedHeight(box.style, -1.f), pos, nullptr);
        for (auto* p : pos) positionedOut.push_back(p);
        item.style.width = savedW; item.style.widthPercent = savedPct;
        item.style.widthCalcPercent = savedCalcPct; item.style.widthCalcOffset = savedCalcOffset;
        int r = cell.row;
        if (r < (int)rowH.size()) rowH[r] = std::max(rowH[r], item.marginBoxH());
    }

    // Compute row Y positions.
    std::vector<float> rowY(numRows);
    float y = box.contentY();
    for (size_t r = 0; r < numRows; ++r) { rowY[r] = y; y += rowH[r] + gap; }

    // Position each item at its (row, col).
    for (auto& cell : placed) {
        LayoutBox& item = *cell.item;
        int c = std::min(cell.col, (int)cols - 1);
        int r = std::min(cell.row, (int)numRows - 1);
        float targetX = colX[c] + item.marginLeft;
        float targetY = rowY[r] + item.marginTop;
        TranslateSubtree(item, targetX - item.x, targetY - item.y);
    }

    box.contentH = std::max(0.f, y - gap - box.contentY());
}

// A real fixed-grid auto table layout: column widths are SHARED across rows
// (so cells line up vertically), each column sizes to its max-content, the
// table shrinks to fit its content (capped at the available width), and colspan
// is honoured. Direct cell children with no <tr> form an implicit anonymous row
// (Acid2's display:table <ul> relies on this).
void Engine::layoutTable(LayoutBox& box) {
    DepthScope _d; if (g_depth > kMaxDepth) return;
    const float x0 = box.contentX();
    const float y0 = box.contentY();
    const float spacing = box.style.borderSpacing >= 0 ? px(box.style.borderSpacing) : 0;
    const float avail = box.contentW;  // set by layoutBox (explicit width or fill)

    auto cellSpan = [](LayoutBox* c) {
        int n = 1;
        if (c->node) { try { n = std::max(1, std::stoi(c->node->attr("colspan"))); } catch (...) {} }
        return n;
    };
    auto rowSpan = [](LayoutBox* c) {
        int n = 1;
        if (c->node) { try { n = std::max(1, std::stoi(c->node->attr("rowspan"))); } catch (...) {} }
        return n;
    };
    auto parseCssWidthHint = [](std::string value) {
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) return std::string{};
            return s.substr(a, b - a + 1);
        };
        value = trim(value);
        if (value.empty()) return -1.f;
        if (!value.empty() && value.back() == '%') return -1.f;
        try { return std::max(0.f, std::stof(value)); } catch (...) { return -1.f; }
    };
    auto parseColWidthHint = [&](const Node* col) {
        if (!col) return -1.f;
        float attr = parseCssWidthHint(col->attr("width"));
        if (attr >= 0) return attr;
        std::string style = col->attr("style");
        std::string low = LowerAscii(style);
        size_t widthPos = low.find("width");
        if (widthPos == std::string::npos) return -1.f;
        size_t colon = low.find(':', widthPos);
        if (colon == std::string::npos) return -1.f;
        size_t semi = low.find(';', colon + 1);
        return parseCssWidthHint(style.substr(colon + 1, semi == std::string::npos ? std::string::npos : semi - colon - 1));
    };

    // Group children into rows (explicit <tr>/row-group, or one implicit row).
    std::vector<std::vector<LayoutBox*>> rows;
    std::vector<LayoutBox*> implicit;
    std::vector<LayoutBox*> rowBoxes;  // parallel to `rows`; nullptr for implicit
    std::vector<LayoutBox*> captions;
    for (auto& k : box.kids) {
        if (k->isOutOfFlow()) continue;
        std::string tag = k->node ? k->node->tagName : "";
        bool isCaption = k->style.isDisplayTableCaption() || tag == "caption";
        if (isCaption) {
            if (!implicit.empty()) { rows.push_back(implicit); rowBoxes.push_back(nullptr); implicit.clear(); }
            captions.push_back(k.get());
            continue;
        }
        bool isRowGroup = k->style.isDisplayTableRowGroup()
            || tag == "thead" || tag == "tbody" || tag == "tfoot";
        bool isRow = k->kind == BoxKind::TableRow || k->style.isDisplayTableRow();
        if (isRowGroup) {
            if (!implicit.empty()) { rows.push_back(implicit); rowBoxes.push_back(nullptr); implicit.clear(); }
            for (auto& rowPtr : k->kids) {
                if (rowPtr->isOutOfFlow()) continue;
                bool childIsRow = rowPtr->kind == BoxKind::TableRow || rowPtr->style.isDisplayTableRow();
                if (!childIsRow) continue;
                std::vector<LayoutBox*> cells;
                for (auto& c : rowPtr->kids) if (!c->isOutOfFlow()) cells.push_back(c.get());
                rows.push_back(std::move(cells));
                rowBoxes.push_back(rowPtr.get());
            }
        } else if (isRow) {
            if (!implicit.empty()) { rows.push_back(implicit); rowBoxes.push_back(nullptr); implicit.clear(); }
            std::vector<LayoutBox*> cells;
            for (auto& c : k->kids) if (!c->isOutOfFlow()) cells.push_back(c.get());
            rows.push_back(std::move(cells));
            rowBoxes.push_back(k.get());
        } else {
            implicit.push_back(k.get());
        }
    }
    if (!implicit.empty()) { rows.push_back(implicit); rowBoxes.push_back(nullptr); }
    if (rows.empty()) { box.contentW = 0; box.contentH = 0; return; }

    struct PlacedCell {
        LayoutBox* cell = nullptr;
        size_t row = 0;
        size_t col = 0;
        int colSpan = 1;
        int rowSpan = 1;
    };

    std::vector<PlacedCell> placed;
    std::vector<int> occupied;
    size_t cols = 0;
    size_t rowCount = rows.size();
    for (size_t r = 0; r < rows.size(); ++r) {
        size_t ci = 0;
        for (auto* cell : rows[r]) {
            while (ci < occupied.size() && occupied[ci] > 0) ++ci;
            int cspan = cellSpan(cell);
            int rspan = rowSpan(cell);
            if (ci + static_cast<size_t>(cspan) > occupied.size())
                occupied.resize(ci + static_cast<size_t>(cspan), 0);
            placed.push_back({ cell, r, ci, cspan, rspan });
            cols = std::max(cols, ci + static_cast<size_t>(cspan));
            rowCount = std::max(rowCount, r + static_cast<size_t>(rspan));
            for (int s = 0; s < cspan; ++s)
                occupied[ci + static_cast<size_t>(s)] = std::max(occupied[ci + static_cast<size_t>(s)], rspan);
            ci += static_cast<size_t>(cspan);
        }
        for (int& n : occupied)
            if (n > 0) --n;
    }
    if (cols == 0 || placed.empty()) { box.contentW = 0; box.contentH = 0; return; }
    // Per-column max-content. Single-column cells set their column directly;
    // spanning cells add a constraint that the spanned columns sum wide enough.
    std::vector<float> colW(cols, 0.f);
    if (box.node) {
        size_t hintIndex = 0;
        std::function<void(const Node*)> collectColHints = [&](const Node* n) {
            if (!n || hintIndex >= cols) return;
            if (n->type == NodeType::Element && n->tagName == "col") {
                float hint = parseColWidthHint(n);
                if (hint >= 0 && hintIndex < colW.size())
                    colW[hintIndex] = std::max(colW[hintIndex], px(hint));
                ++hintIndex;
                return;
            }
            if (n->type == NodeType::Element && n->tagName != "table"
                && n->tagName != "colgroup") {
                return;
            }
            for (const auto& child : n->children)
                collectColHints(child.get());
        };
        collectColHints(box.node);
    }
    for (const auto& pc : placed) {
        int span = pc.colSpan;
        size_t ci = pc.col;
        float want = maxContent(*pc.cell) + hExtra(pc.cell->style);
        if (pc.cell->style.width >= 0) want = px(pc.cell->style.width) + hExtra(pc.cell->style);
        if (span == 1) {
            if (ci < cols) colW[ci] = std::max(colW[ci], want);
        } else {
            float have = 0;
            for (int s = 0; s < span && ci + static_cast<size_t>(s) < cols; ++s)
                have += colW[ci + static_cast<size_t>(s)];
            if (want > have && span > 0) {
                float add = (want - have) / span;
                for (int s = 0; s < span && ci + static_cast<size_t>(s) < cols; ++s)
                    colW[ci + static_cast<size_t>(s)] += add;
            }
        }
    }

    float natural = spacing;
    for (float w : colW) natural += w + spacing;

    // Auto width → shrink to content (capped at available). Explicit/percent
    // width → fill the resolved width, distributing slack across columns.
    bool autoWidth = !(box.style.width >= 0 || box.style.widthPercent >= 0 || box.style.widthCalcPercent >= 0);
    float targetW = autoWidth ? std::min(natural, avail) : std::max(avail, 0.f);
    if (natural > 0.01f && std::abs(targetW - natural) > 0.5f) {
        float scale = (targetW - spacing * (cols + 1)) /
                      std::max(0.01f, natural - spacing * (cols + 1));
        if (scale < 0) scale = 0;
        for (float& w : colW) w *= scale;
    }

    std::vector<float> colX(cols, 0.f);
    { float x = x0 + spacing; for (size_t i = 0; i < cols; ++i) { colX[i] = x; x += colW[i] + spacing; } }

    float rowBaseY = y0;
    for (LayoutBox* caption : captions) {
        caption->y = rowBaseY;
        std::vector<LayoutBox*> pos;
        layoutBox(*caption, x0, targetW, -1.f, pos, nullptr);
        rowBaseY += caption->marginBoxH();
    }
    float captionH = rowBaseY - y0;

    std::vector<float> rowH(rowCount, 0.f);
    for (const auto& pc : placed) {
        float cw = 0;
        for (int s = 0; s < pc.colSpan && pc.col + static_cast<size_t>(s) < cols; ++s)
            cw += colW[pc.col + static_cast<size_t>(s)] + (s > 0 ? spacing : 0);
        float cx = pc.col < cols ? colX[pc.col] : x0 + spacing;
        pc.cell->y = rowBaseY + spacing;
        // Pin the cell to its shared column width (override auto) so columns align.
        float savedW = pc.cell->style.width;
        float savedPct = pc.cell->style.widthPercent;
        float savedCalcPct = pc.cell->style.widthCalcPercent;
        float savedCalcOffset = pc.cell->style.widthCalcOffset;
        pc.cell->style.width = std::max(0.f, cw - hExtra(pc.cell->style)) / std::max(0.01f, Z);
        pc.cell->style.widthPercent = -1;
        pc.cell->style.widthCalcPercent = -1;
        pc.cell->style.widthCalcOffset = 0;
        std::vector<LayoutBox*> pos;
        layoutBox(*pc.cell, cx, cw, -1.f, pos, nullptr);
        pc.cell->style.width = savedW;
        pc.cell->style.widthPercent = savedPct;
        pc.cell->style.widthCalcPercent = savedCalcPct;
        pc.cell->style.widthCalcOffset = savedCalcOffset;

        if (pc.rowSpan <= 1 && pc.row < rowH.size())
            rowH[pc.row] = std::max(rowH[pc.row], pc.cell->borderBoxH());
    }

    for (const auto& pc : placed) {
        if (pc.rowSpan <= 1) continue;
        float have = 0;
        size_t last = std::min(rowH.size(), pc.row + static_cast<size_t>(pc.rowSpan));
        for (size_t r = pc.row; r < last; ++r)
            have += rowH[r] + (r > pc.row ? spacing : 0);
        float need = pc.cell->borderBoxH();
        if (need > have && last > pc.row) {
            float add = (need - have) / static_cast<float>(last - pc.row);
            for (size_t r = pc.row; r < last; ++r)
                rowH[r] += add;
        }
    }

    std::vector<float> rowTop(rowCount, rowBaseY + spacing);
    float y = rowBaseY + spacing;
    for (size_t r = 0; r < rowCount; ++r) {
        rowTop[r] = y;
        y += rowH[r] + spacing;
    }

    for (const auto& pc : placed) {
        size_t last = std::min(rowH.size(), pc.row + static_cast<size_t>(pc.rowSpan));
        float targetH = 0;
        for (size_t r = pc.row; r < last; ++r)
            targetH += rowH[r] + (r > pc.row ? spacing : 0);
        float extra = targetH - pc.cell->borderBoxH();
        if (extra > 0) pc.cell->contentH += extra;
        float targetX = pc.col < cols ? colX[pc.col] : x0 + spacing;
        float targetY = pc.row < rowTop.size() ? rowTop[pc.row] : y0 + spacing;
        TranslateSubtree(*pc.cell, targetX - pc.cell->x, targetY - pc.cell->y);
    }

    for (size_t r = 0; r < rows.size() && r < rowBoxes.size(); ++r) {
        if (rowBoxes[r]) {
            rowBoxes[r]->x = x0;
            rowBoxes[r]->y = (r < rowTop.size() ? rowTop[r] : rowBaseY + spacing) - spacing;
            rowBoxes[r]->contentW = targetW;
            rowBoxes[r]->contentH = r < rowH.size() ? rowH[r] : 0.f;
        }
    }

    box.contentW = std::max(0.f, targetW);
    box.contentH = std::max(0.f, captionH + (y - rowBaseY));
}

// ─── inline layout ───────────────────────────────────────────────────────────

struct InlineItem {
    enum Type { Word, Space, Atomic, Break } type;
    std::wstring text;
    float        width = 0;
    LayoutBox*   box = nullptr;     // style source (text) or atomic box
    std::vector<LayoutBox*> owners;  // inline ancestors owning this fragment
    FontKey      font;
    float        ascent = 0, lineH = 0;
    int          vAlign = 0;        // effective vertical-align from inline ancestors
};

// Flatten inline-level content into a stream of items. `parentVAlign` carries the
// vertical-align of the enclosing inline box down to its text/atomic descendants,
// since vertical-align applies to the inline element, not the text node it wraps.
static void CollectInline(Engine& E, LayoutBox* box, std::vector<InlineItem>& items,
                          int parentVAlign = 0, float parentW = 1e6f, float parentH = -1.f,
                          std::vector<LayoutBox*> owners = {}) {
    DepthScope _d; if (g_depth > kMaxDepth) return;
    if (box->isOutOfFlow()) return;
    int va = box->style.verticalAlignSet ? box->style.verticalAlign : parentVAlign;
    if (box->kind == BoxKind::Text) {
        FontKey f = E.fontFor(box->style);
        float lh = E.lineHeightFor(box->style);
        float asc = Ascent(lh, f.size);
        const std::wstring& t = box->text;
        size_t i = 0;
        while (i < t.size()) {
            if (IsSpace(t[i])) {
                InlineItem it; it.type = InlineItem::Space; it.box = box; it.font = f;
                it.owners = owners;
                it.width = E.in.measure ? E.in.measure->SpaceWidth(f) : f.size * 0.3f;
                it.ascent = asc; it.lineH = lh; it.vAlign = va;
                items.push_back(it);
                i++;
                continue;
            }
            size_t j = i;
            while (j < t.size() && !IsSpace(t[j])) j++;
            std::wstring word = t.substr(i, j - i);
            InlineItem it; it.type = InlineItem::Word; it.text = word; it.box = box; it.font = f;
            it.owners = owners;
            it.width = E.in.measure ? E.in.measure->MeasureText(word, f) : (float)word.size() * f.size * 0.5f;
            // letter-spacing: add extra width per character
            if (box->style.letterSpacingSet && box->style.letterSpacing != 0) {
                float extra = box->style.letterSpacing * E.Z * (float)word.size();
                if (std::isfinite(extra) && extra < 1e6f) it.width += extra;
            }
            it.ascent = asc; it.lineH = lh; it.vAlign = va;
            items.push_back(it);
            i = j;
        }
        return;
    }
    if (box->kind == BoxKind::Break) {
        InlineItem it; it.type = InlineItem::Break; it.box = box;
        it.owners = owners;
        items.push_back(it);
        return;
    }
    if (box->kind == BoxKind::InlineBlock || box->kind == BoxKind::Replaced) {
        // Atomic: lay it out to learn its size. Use the parent's content width
        // as the containing block when the element has a percentage width
        // (otherwise shrink-to-fit with a huge available width).
        float cbW = (box->style.widthPercent >= 0 || box->style.widthCalcPercent >= 0 || box->style.maxWidthPercent >= 0)
                  ? parentW : 1e6f;
        float cbH = (box->style.heightPercent >= 0 || box->style.heightCalcPercent >= 0
                  || box->style.minHeightPercent >= 0 || box->style.maxHeightPercent >= 0)
                  ? parentH : -1.f;
        std::vector<LayoutBox*> pos;
        E.layoutBox(*box, 0, cbW, cbH, pos, nullptr);
        InlineItem it; it.type = InlineItem::Atomic; it.box = box;
        it.owners = owners;
        it.width = box->marginBoxW();
        FontKey f = E.fontFor(box->style);
        it.font = f; it.lineH = box->marginBoxH();
        it.ascent = box->marginBoxH();  // fallback: baseline at bottom for replaced
        if (box->kind == BoxKind::InlineBlock && !box->lines.empty()) {
            const LineBox& last = box->lines.back();
            it.ascent = box->marginTop + (last.y - box->y) + last.baseline;
            it.ascent = std::clamp(it.ascent, 0.f, box->marginBoxH());
        }
        it.vAlign = va;
        items.push_back(it);
        return;
    }
    // Inline box (span/a/em…): recurse into children, carrying its vertical-align.
    if (box->kind == BoxKind::Inline) owners.push_back(box);
    for (auto& k : box->kids) CollectInline(E, k.get(), items, va, parentW, parentH, owners);
}

float Engine::layoutInline(LayoutBox& box, FloatCtx* fctx) {
    DepthScope _d; if (g_depth > kMaxDepth) return 0;
    std::vector<InlineItem> items;
    struct InlineBounds { float minX = 0, minY = 0, maxX = 0, maxY = 0; bool set = false; };
    std::unordered_map<LayoutBox*, InlineBounds> inlineBounds;
    auto addInlineBound = [&](LayoutBox* owner, const InlineFrag& frag) {
        if (!owner) return;
        auto& b = inlineBounds[owner];
        if (!b.set) {
            b.minX = frag.x; b.minY = frag.y; b.maxX = frag.x + frag.w; b.maxY = frag.y + frag.h;
            b.set = true;
        } else {
            b.minX = std::min(b.minX, frag.x); b.minY = std::min(b.minY, frag.y);
            b.maxX = std::max(b.maxX, frag.x + frag.w); b.maxY = std::max(b.maxY, frag.y + frag.h);
        }
    };
    float inlineCbH = usedHeight(box.style, box.contentH >= 0 ? box.contentH : -1.f);
    if (inlineCbH >= 0 && box.style.boxSizing == 1) {
        const float vbp = box.borderTop + box.padTop + box.borderBottom + box.padBottom;
        inlineCbH = std::max(0.f, inlineCbH - vbp);
    }
    for (auto& k : box.kids) CollectInline(*this, k.get(), items, 0, box.contentW, inlineCbH);

    float cLeft  = box.contentX();
    float cRight = box.contentX() + box.contentW;
    float top    = box.contentY();
    bool nowrap  = box.style.whiteSpaceNowrap;
    int  align   = box.style.textAlignSet ? box.style.textAlign : 0;

    float y = top;
    size_t i = 0;
    box.lines.clear();

    float indent = px(box.style.textIndent);  // applies to the first line only
    bool firstLine = true;

    while (i < items.size()) {
        // Determine usable horizontal band at this y (consult floats).
        float probeH = 0;
        for (size_t k = i; k < items.size() && k < i + 1; ++k) probeH = std::max(probeH, items[k].lineH);
        if (probeH <= 0) probeH = lineHeightFor(box.style);
        float lineLeft  = fctx ? fctx->leftEdge(y, probeH) : cLeft;
        float lineRight = fctx ? fctx->rightEdge(y, probeH) : cRight;
        if (lineRight <= lineLeft && fctx) {
            float nb = fctx->nextBreakBelow(y);
            if (nb > y) { y = nb; continue; }
            lineLeft = cLeft; lineRight = cRight;
        }
        float avail = lineRight - lineLeft;
        // text-indent narrows the first line's usable width.
        float lineIndent = (firstLine && indent < avail) ? indent : 0.f;
        avail -= lineIndent;

        // Greedily pack a line.
        LineBox line;
        line.x = lineLeft; line.y = y; line.w = 0;
        float lineWidth = 0, lineH = 0, lineAsc = 0;
        bool any = false, brk = false;

        // Skip leading spaces.
        while (i < items.size() && items[i].type == InlineItem::Space) i++;

        size_t lineStart = i;
        std::vector<size_t> chosen;
        while (i < items.size()) {
            InlineItem& it = items[i];
            if (it.type == InlineItem::Break) { brk = true; i++; break; }
            float adv = it.width;
            if (it.type == InlineItem::Space) {
                // pending space: only counts if a word follows on this line
                if (!any) { i++; continue; }
                // look ahead
                adv = it.width;
            }
            if (any && !nowrap && (lineWidth + adv) > avail && it.type != InlineItem::Space) {
                // word-break: break-all/break-word — split the word if it alone exceeds avail.
                if (!any && box.style.wordBreak >= 1 && it.type == InlineItem::Word && it.text.size() > 1) {
                    // Character-level break: find how many chars fit.
                    float charW = it.width / (float)it.text.size();
                    int fitChars = std::max(1, (int)((avail - lineWidth) / charW));
                    if (fitChars < (int)it.text.size()) {
                        // Split: first part goes on this line, rest becomes a new item.
                        InlineItem rest = it;
                        rest.text = it.text.substr(fitChars);
                        rest.width = charW * rest.text.size();
                        it.text = it.text.substr(0, fitChars);
                        it.width = charW * fitChars;
                        items.insert(items.begin() + i + 1, rest);
                    }
                }
                break;  // wrap before this item
            }
            chosen.push_back(i);
            lineWidth += adv;
            lineH = std::max(lineH, it.lineH);
            lineAsc = std::max(lineAsc, it.ascent);
            any = true;
            i++;
        }
        // Trim a trailing space from the line width.
        while (!chosen.empty() && items[chosen.back()].type == InlineItem::Space) {
            lineWidth -= items[chosen.back()].width;
            chosen.pop_back();
        }
        if (lineH <= 0) lineH = lineHeightFor(box.style);
        if (lineAsc <= 0) lineAsc = Ascent(lineH, fontFor(box.style).size);

        // Horizontal alignment offset.
        float offset = 0;
        if (align == 2) offset = avail - lineWidth;         // right
        else if (align == 1) offset = (avail - lineWidth) / 2; // center
        if (offset < 0) offset = 0;

        // text-align: justify — spread slack across the spaces, but never on the
        // last line of the block or a line ended by <br>.
        float justifyExtra = 0.f;
        if (align == 3 && !brk && i < items.size()) {
            int spaceCount = 0;
            for (size_t idx : chosen)
                if (items[idx].type == InlineItem::Space) ++spaceCount;
            float slack = avail - lineWidth;
            if (spaceCount > 0 && slack > 0) justifyExtra = slack / spaceCount;
        }

        // Place fragments.
        float fx = lineLeft + lineIndent + offset;
        for (size_t idx : chosen) {
            InlineItem& it = items[idx];
            if (it.type == InlineItem::Space) { fx += it.width + justifyExtra; continue; }
            InlineFrag frag;
            frag.src = it.box;
            frag.x = fx;
            frag.w = it.width;
            bool atomic = (it.type == InlineItem::Atomic);
            if (atomic) {
                // bottom-align to baseline: top so that bottom sits on baseline.
                frag.h = it.box->marginBoxH();
                frag.baseline = it.ascent;
                frag.y = y + lineAsc - frag.baseline;
            } else {
                frag.text = it.text;
                frag.h = it.lineH;
                frag.baseline = it.ascent;
                frag.y = y + (lineAsc - it.ascent);
            }
            // vertical-align: shift the fragment relative to the baseline / line box.
            if (it.vAlign != 0) {
                float fsz = it.font.size > 0 ? it.font.size : it.lineH;
                switch (it.vAlign) {
                    case 1: frag.y += fsz * 0.15f; break;             // sub
                    case 2: frag.y -= fsz * 0.30f; break;             // super
                    case 3: frag.y = y + (lineH - frag.h) * 0.5f; break; // middle
                    case 4: frag.y = y; break;                        // top
                    case 5: frag.y = y + lineH - frag.h; break;       // bottom
                }
            }
            if (atomic) {
                // Place the already-laid-out atomic subtree at the final
                // inline position. Its child line boxes were measured at a
                // temporary origin, so move the whole subtree together.
                float nx = fx + it.box->marginLeft;
                float ny = frag.y + it.box->marginTop;
                TranslateSubtree(*it.box, nx - it.box->x, ny - it.box->y);
            }
            if (it.box && it.box->kind == BoxKind::Text) addInlineBound(it.box, frag);
            for (LayoutBox* owner : it.owners) addInlineBound(owner, frag);
            line.frags.push_back(frag);
            fx += it.width;
        }
        line.w = lineWidth; line.h = lineH; line.baseline = lineAsc;
        if (!line.frags.empty() || brk) box.lines.push_back(line);

        firstLine = false;
        y += lineH;
        if (lineStart == i && !brk) { i++; }  // safety: avoid infinite loop
    }
    for (auto& [inlineBox, b] : inlineBounds) {
        if (!b.set) continue;
        inlineBox->x = b.minX;
        inlineBox->y = b.minY;
        inlineBox->contentW = std::max(0.f, b.maxX - b.minX);
        inlineBox->contentH = std::max(0.f, b.maxY - b.minY);
    }
    return y - top;
}

// ─── positioned layout ───────────────────────────────────────────────────────

// Walk the tree collecting out-of-flow boxes paired with their nearest
// positioned ancestor (the containing block), so we can lay them out after
// normal flow has fixed everyone's geometry.
void Engine::collectPositioned(LayoutBox& box, LayoutBox* nearestPositioned,
                               LayoutBox* nearestFixedContainingBlock,
                               std::vector<std::pair<LayoutBox*, LayoutBox*>>& out) {
    DepthScope _d; if (g_depth > kMaxDepth) return;
    const bool establishesTransformContainingBlock = box.style.transformSet;
    LayoutBox* nextNearest = (box.isPositioned() || establishesTransformContainingBlock)
        ? &box : nearestPositioned;
    LayoutBox* nextFixedContainingBlock = establishesTransformContainingBlock
        ? &box : nearestFixedContainingBlock;
    for (auto& k : box.kids) {
        if (k->isOutOfFlow()) {
            out.push_back({
                k.get(),
                k->style.positionMode == 3 ? nextFixedContainingBlock : nextNearest
            });
            continue;
        }
        collectPositioned(*k, nextNearest, nextFixedContainingBlock, out);
    }
}

void Engine::layoutPositioned(LayoutBox& root, std::vector<LayoutBox*>& /*unused*/) {
    std::vector<std::pair<LayoutBox*, LayoutBox*>> items;
    collectPositioned(root, &root, nullptr, items);

    for (auto& [b, cb] : items) {
        const ComputedStyle& s = b->style;
        // Containing block rect (padding box for positioned ancestor, else viewport/root).
        float cbX, cbY, cbW, cbH;
        if (s.positionMode == 3 || !cb) {
            cbX = 0; cbY = 0; cbW = in.viewportW; cbH = in.viewportH;
        } else {
            cbX = cb->contentX(); cbY = cb->contentY();
            cbW = cb->contentW;   cbH = cb->contentH;
        }

        resolveEdges(*b);
        const bool mlAuto = s.isMarginAuto(s.marginLeft);
        const bool mrAuto = s.isMarginAuto(s.marginRight);
        b->marginTop    = s.marginTopSet()    && !s.isMarginAuto(s.marginTop)    ? px(s.marginTop)    : 0;
        b->marginBottom = s.marginBottomSet() && !s.isMarginAuto(s.marginBottom) ? px(s.marginBottom) : 0;
        b->marginLeft   = s.marginLeftSet()   && !mlAuto ? px(s.marginLeft)   : 0;
        b->marginRight  = s.marginRightSet()  && !mrAuto ? px(s.marginRight)  : 0;

        float bpX = b->borderLeft + b->padLeft + b->borderRight + b->padRight;
        float bpY = b->borderTop + b->padTop + b->borderBottom + b->padBottom;
        bool borderBox = (s.boxSizing == 1);
        auto contentBoxHeight = [&](float h) {
            return (borderBox && h >= 0) ? std::max(0.f, h - bpY) : h;
        };

        // Resolve width.
        float w = usedWidth(s, cbW);
        if (borderBox && w >= 0)
            w = std::max(0.f, w - bpX);
        if (s.widthKeyword != 0) {
            float mc = maxContent(*b);
            if (s.widthKeyword == 3)
                w = std::min(mc, std::max(0.f, cbW - b->marginLeft - b->marginRight - bpX));
            else
                w = mc;
        }
        if (w < 0) {
            if (s.leftSet && s.rightSet) {
                float l = s.leftPercent ? cbW*(s.left/100.f) : px(s.left);
                float r = s.rightPercent ? cbW*(s.right/100.f) : px(s.right);
                w = std::max(0.f, cbW - l - r - b->marginLeft - b->marginRight - bpX);
            }
            else
                w = std::min(maxContent(*b), std::max(0.f, cbW - bpX));  // shrink-to-fit
        }
        { float mw = usedMaxWidth(s, cbW); if (mw >= 0) { if (borderBox) mw = std::max(0.f, mw - bpX); w = std::min(w, mw); } }
        { float nw = usedMinWidth(s, cbW); if (nw >= 0) { if (borderBox) nw = std::max(0.f, nw - bpX); w = std::max(w, nw); } }
        b->contentW = w;

        auto resolveH = [&](float v, bool isPct) { return isPct ? cbW * (v / 100.f) : px(v); };
        auto resolveV = [&](float v, bool isPct) { return isPct ? cbH * (v / 100.f) : px(v); };
        if (s.leftSet && s.rightSet && (mlAuto || mrAuto)) {
            float l = resolveH(s.left, s.leftPercent);
            float r = resolveH(s.right, s.rightPercent);
            float free = cbW - l - r - b->borderBoxW() - b->marginLeft - b->marginRight;
            if (free < 0) free = 0;
            if (mlAuto && mrAuto) {
                b->marginLeft = b->marginRight = free / 2.f;
            } else if (mlAuto) {
                b->marginLeft = free;
            } else if (mrAuto) {
                b->marginRight = free;
            }
        }

        b->x = 0; b->y = 0;  // children laid out relative to origin; translated below
        if (b->kind == BoxKind::Replaced || (!b->replacedUrl.empty() && b->kids.empty())) {
            // A positioned image: resolve height from CSS or the intrinsic
            // aspect ratio (the normal-flow replaced path in layoutBox is never
            // reached for out-of-flow boxes, so do it here too).
            float h = contentBoxHeight(usedHeight(s, cbH));
            if (b->contentW <= 0 && b->intrinsicW > 0) b->contentW = px(b->intrinsicW);
            if (h < 0) {
                if (b->intrinsicW > 0 && b->intrinsicH > 0 && b->contentW > 0)
                    h = b->contentW * (b->intrinsicH / b->intrinsicW);
                else h = b->intrinsicH > 0 ? px(b->intrinsicH) : 0;
            }
            { float mh = usedMaxHeight(s, cbH); if (mh >= 0) h = std::min(h, contentBoxHeight(mh)); }
            { float nh = usedMinHeight(s, cbH); if (nh >= 0) h = std::max(h, contentBoxHeight(nh)); }
            b->contentH = h;
        } else {
            // Lay out the subtree (block or inline) to learn height.
            std::vector<LayoutBox*> dummy;
            if (b->establishesInline) {
                float eh = contentBoxHeight(usedHeight(s, cbH));
                if (eh >= 0) b->contentH = eh;
                FloatCtx local; local.cbLeft = b->contentX(); local.cbRight = b->contentX() + b->contentW;
                float h = layoutInline(*b, &local);
                b->contentH = eh >= 0 ? eh : h;
            } else {
                layoutBlockChildren(*b, dummy);
                float eh = contentBoxHeight(usedHeight(s, cbH));
                if (eh >= 0) b->contentH = eh;
            }
            { float mh = usedMaxHeight(s, cbH); if (mh >= 0) b->contentH = std::min(b->contentH, contentBoxHeight(mh)); }
            { float nh = usedMinHeight(s, cbH); if (nh >= 0) b->contentH = std::max(b->contentH, contentBoxHeight(nh)); }
        }

        // Final border-box position. Percentages resolve against the CB dimensions.
        float bx, by;
        if (s.leftSet)       bx = cbX + resolveH(s.left, s.leftPercent) + b->marginLeft;
        else if (s.rightSet) bx = cbX + cbW - resolveH(s.right, s.rightPercent) - b->borderBoxW() - b->marginRight;
        else                 bx = cbX;  // static-ish fallback
        if (s.topSet)        by = cbY + resolveV(s.top, s.topPercent) + b->marginTop;
        else if (s.bottomSet)by = cbY + cbH - resolveV(s.bottom, s.bottomPercent) - b->borderBoxH() - b->marginBottom;
        else                 by = cbY;

        float dx = bx - b->x, dy = by - b->y;
        // Translate the whole subtree (children were laid out relative to b->x/y=0).
        std::function<void(LayoutBox&, float, float)> shift = [&](LayoutBox& n, float ddx, float ddy) {
            n.x += ddx; n.y += ddy;
            for (auto& ln : n.lines) {
                ln.x += ddx; ln.y += ddy;
                for (auto& fr : ln.frags) { fr.x += ddx; fr.y += ddy; }
            }
            for (auto& c : n.kids) shift(*c, ddx, ddy);
        };
        shift(*b, dx, dy);

        // Recurse: positioned descendants of this box relative to it.
        std::vector<LayoutBox*> nested;
        layoutPositioned(*b, nested);
    }
}

// ─── relative offset pass ────────────────────────────────────────────────────
void ApplyRelativeOffsets(Engine& E, LayoutBox& box) {
    for (auto& k : box.kids) ApplyRelativeOffsets(E, *k);
    if (box.style.positionMode == 1 || box.style.positionMode == 4) {
        float dx = 0, dy = 0;
        if (box.style.leftSet)       dx = E.px(box.style.left);
        else if (box.style.rightSet) dx = -E.px(box.style.right);
        if (box.style.topSet)        dy = E.px(box.style.top);
        else if (box.style.bottomSet)dy = -E.px(box.style.bottom);
        std::function<void(LayoutBox&)> shift = [&](LayoutBox& n) {
            n.x += dx; n.y += dy;
            for (auto& ln : n.lines) { ln.x += dx; ln.y += dy; for (auto& fr : ln.frags) { fr.x += dx; fr.y += dy; } }
            for (auto& c : n.kids) shift(*c);
        };
        shift(box);
    }
}

float ScrollableBottom(const LayoutBox& box, bool includeSelf, float clipBottom = std::numeric_limits<float>::infinity()) {
    if (box.style.positionMode == 3) return 0.f;
    float bottom = includeSelf ? box.y + box.borderBoxH() + box.marginBottom : box.contentY();
    const std::string tag = box.node ? box.node->tagName : "";
    if (box.style.overflowHidden && tag != "html" && tag != "body")
        clipBottom = std::min(clipBottom, box.y + box.borderBoxH());
    for (const auto& child : box.kids)
        bottom = std::max(bottom, ScrollableBottom(*child, true, clipBottom));
    for (const auto& line : box.lines)
        for (const auto& frag : line.frags)
            bottom = std::max(bottom, frag.y + frag.h);
    return std::min(bottom, clipBottom);
}

} // namespace

// ─── public entry point ──────────────────────────────────────────────────────

std::unique_ptr<LayoutBox> LayoutDocument(const LayoutInput& in) {
    if (!in.document) return nullptr;
    g_depth = 0; // Reset thread_local depth counter for this layout pass

    float cssW = in.viewportW / std::max(0.01f, in.zoom);
    float cssH = in.viewportH / std::max(0.01f, in.zoom);
    if (in.sheet) in.sheet->setViewport(cssW, cssH);
    SetCssViewport(cssW, cssH);

    BuildCtx bc{ in.sheet, in.measure, in.zoom, in.baseUrl };

    // Root style: defaults (16px black, sans-serif).
    ComputedStyle rootStyle;
    rootStyle.fontSize = 16;
    rootStyle.color = { true, 0, 0, 0, 1 };

    // Find <html> (or use the document node).
    const Node* htmlNode = nullptr;
    std::function<const Node*(const Node*)> findHtml = [&](const Node* n) -> const Node* {
        if (!n) return nullptr;
        if (n->type == NodeType::Element && n->tagName == "html") return n;
        for (auto& c : n->children) if (auto* r = findHtml(c.get())) return r;
        return nullptr;
    };
    htmlNode = findHtml(in.document);

    auto root = std::make_unique<LayoutBox>();
    root->kind = BoxKind::Block;
    root->anonymous = true;
    root->style = rootStyle;
    root->contentW = in.viewportW;

    if (htmlNode) {
        if (auto hb = BuildBox(htmlNode, rootStyle, bc, "")) root->kids.push_back(std::move(hb));
    } else {
        // No <html>: build from body or document children directly.
        for (auto& c : in.document->children)
            if (auto cb = BuildBox(c.get(), rootStyle, bc, "")) root->kids.push_back(std::move(cb));
    }
    AnonymousFixup(root.get());

    Engine E(in);
    root->x = 0; root->y = 0;
    // The initial containing block: content origin at (0,0), width = viewport.
    root->contentW = in.viewportW;
    std::vector<LayoutBox*> positioned;
    if (root->establishesInline) {
        FloatCtx local; local.cbLeft = 0; local.cbRight = in.viewportW;
        float h = E.layoutInline(*root, &local);
        root->contentH = h;
    } else {
        E.layoutBlockChildren(*root, positioned);
    }

    // Relative offsets, then absolutely/fixed positioned boxes.
    ApplyRelativeOffsets(E, *root);
    std::vector<LayoutBox*> dummy;
    E.layoutPositioned(*root, dummy);
    root->contentH = std::max(root->contentH,
        std::max(0.f, ScrollableBottom(*root, false) - root->contentY()));

    return root;
}
