#pragma once
//
// layout_engine.h — builds a layout box tree from the styled DOM and computes
// geometry (block formatting, inline formatting, floats, positioning).
//
#include "layout/box.h"
#include "css/stylesheet.h"
#include "html/dom.h"

#include <memory>
#include <string>

struct LayoutInput {
    const Node*       document = nullptr;
    Stylesheet*       sheet    = nullptr;
    ITextMeasure*     measure  = nullptr;
    float             viewportW = 800.f;  // device px
    float             viewportH = 600.f;  // device px
    float             zoom      = 1.f;
    std::string       baseUrl;
};

// Build + lay out the document. Returns the root box (initial containing block),
// whose contentH is the total document height in device px.
std::unique_ptr<LayoutBox> LayoutDocument(const LayoutInput& in);
