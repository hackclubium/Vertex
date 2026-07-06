#pragma once
//
// font_face.h — renders TrueType glyph outlines (truetype.h) into flattened
// contours the rasterizer (render/rasterizer.h) can fill directly.
//
// Part of Vertex's zero-third-party-dependency push (phase 3 of the Linux
// windowing rewrite — replaces Pango). Quadratic Bezier segments are
// flattened with a fixed subdivision count (8 segments), matching this
// project's established "baseline first" pattern rather than adaptive
// flatness testing.
//
#include "font/truetype.h"
#include "render/rasterizer.h"
#include <cstdint>
#include <string>
#include <vector>

class FontFace {
public:
    bool Load(const std::string& path);
    bool IsLoaded() const { return m_font.IsLoaded(); }

    // Flattened glyph outline in pixel space, y-down, with (0,0) at this
    // glyph's own baseline-left origin — the caller translates by the
    // current pen position (and baseline y) before filling. Empty if the
    // font has no glyph for this codepoint or the glyph has no outline
    // (e.g. space).
    std::vector<raster::Contour> RenderGlyph(uint32_t codepoint, float sizePx) const;
    float AdvanceWidth(uint32_t codepoint, float sizePx) const;
    float Ascent(float sizePx) const;
    float Descent(float sizePx) const;  // positive: distance below the baseline
    float LineHeight(float sizePx) const;

private:
    ttf::Font m_font;
};
