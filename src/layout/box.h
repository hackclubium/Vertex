#pragma once
//
// box.h — the layout box tree produced by the layout engine.
//
// Vertex builds a real box tree (separate from the DOM) and computes geometry
// for every box in a layout pass, then paints the tree in a second pass. This
// replaces the old single-pass "paint while walking the DOM" renderer.
//
// All geometry is in *device pixels* (CSS px already multiplied by the zoom
// factor), with the document origin at (0,0). The paint pass applies scrolling
// and the chrome inset.
//
#include "css/style.h"
#include "html/dom.h"

#include <memory>
#include <string>
#include <vector>

// ─── text / image measurement interface ──────────────────────────────────────
// The layout engine needs to measure text and learn image intrinsic sizes.
// The renderer implements this with DirectWrite / WIC.

struct FontKey {
    float       size   = 15.f;   // device px
    bool        bold   = false;
    bool        italic = false;
    bool        mono   = false;
    std::string family;          // "" = default UI font

    bool operator==(const FontKey& o) const {
        return size == o.size && bold == o.bold && italic == o.italic
            && mono == o.mono && family == o.family;
    }
};

class ITextMeasure {
public:
    virtual ~ITextMeasure() = default;
    // Width of a string laid out on a single line.
    virtual float MeasureText(const std::wstring& s, const FontKey& f) = 0;
    // Width of a single space in the font.
    virtual float SpaceWidth(const FontKey& f) = 0;
    // Intrinsic image size in CSS px (returns false if not yet loaded).
    virtual bool  ImageIntrinsic(const std::string& url, float& w, float& h) = 0;
    // Notify that an image URL is needed (triggers async fetch).
    virtual void  RequestImage(const std::string& url) = 0;
};

// ─── box kinds ────────────────────────────────────────────────────────────────

enum class BoxKind {
    Block,        // block-level block container
    Inline,       // inline-level, non-atomic (span, a, em…)
    InlineBlock,  // inline-level atomic box establishing a BFC
    Replaced,     // img / object / form control (atomic, intrinsic size)
    Text,         // anonymous text run
    ListItem,     // block container that also paints a marker
    Table,        // simplified table wrapper
    TableRow,     // table row
    TableCell,    // table cell (block container)
    Break,        // <br>
};

struct LayoutBox;

// A piece of inline content placed on a line during inline layout.
struct InlineFrag {
    LayoutBox*   src   = nullptr;   // text run, replaced, or inline-block box
    std::wstring text;              // substring (text frags only)
    float        x = 0, y = 0;      // border-box top-left, document coords
    float        w = 0, h = 0;
    float        baseline = 0;      // distance from frag top to baseline
};

struct LineBox {
    float x = 0, y = 0, w = 0, h = 0;
    float baseline = 0;             // baseline offset from line top
    std::vector<InlineFrag> frags;
};

struct LayoutBox {
    const Node*   node  = nullptr;       // source DOM node (nullptr = anonymous)
    ComputedStyle style;
    BoxKind       kind  = BoxKind::Block;
    bool          anonymous = false;

    // Text run payload.
    std::wstring  text;
    // Replaced payload.
    std::string   replacedUrl;
    float         intrinsicW = 0, intrinsicH = 0;

    // ── used geometry (filled by layout) ────────────────────────────────────
    // (x, y) is the top-left of the *border box*, in document coordinates.
    float x = 0, y = 0;
    float contentW = 0, contentH = 0;
    float marginTop = 0, marginRight = 0, marginBottom = 0, marginLeft = 0;
    float borderTop = 0, borderRight = 0, borderBottom = 0, borderLeft = 0;
    float padTop = 0, padRight = 0, padBottom = 0, padLeft = 0;

    bool  establishesInline = false;     // children laid out as inline (line boxes)
    std::vector<LineBox> lines;          // inline formatting output

    std::vector<std::unique_ptr<LayoutBox>> kids;

    // Link / interaction data resolved at build time.
    std::string  href;                   // nearest <a href> (for hit testing)
    bool         download = false;        // nearest <a download>
    std::string  downloadName;

    // ── geometry helpers ─────────────────────────────────────────────────────
    float borderBoxW() const { return borderLeft + padLeft + contentW + padRight + borderRight; }
    float borderBoxH() const { return borderTop + padTop + contentH + padBottom + borderBottom; }
    float marginBoxW() const { return marginLeft + borderBoxW() + marginRight; }
    float marginBoxH() const { return marginTop + borderBoxH() + marginBottom; }
    float contentX()   const { return x + borderLeft + padLeft; }
    float contentY()   const { return y + borderTop + padTop; }

    bool isInlineLevel() const {
        return kind == BoxKind::Inline || kind == BoxKind::InlineBlock
            || kind == BoxKind::Replaced || kind == BoxKind::Text
            || kind == BoxKind::Break;
    }
    bool isPositioned() const { return style.positionMode != 0; }
    bool isOutOfFlow()  const { return style.positionMode == 2 || style.positionMode == 3; }
    bool isFloat()      const { return style.floatMode != 0 && !isOutOfFlow(); }
};
