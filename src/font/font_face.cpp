#include "font/font_face.h"

namespace {

constexpr int kCurveSegments = 8;

void FlattenQuadratic(std::vector<raster::Vec2>& out, raster::Vec2 p0, raster::Vec2 p1, raster::Vec2 p2) {
    for (int i = 1; i <= kCurveSegments; i++) {
        float t = (float)i / (float)kCurveSegments;
        float mt = 1.f - t;
        out.push_back({mt * mt * p0.x + 2.f * mt * t * p1.x + t * t * p2.x,
                        mt * mt * p0.y + 2.f * mt * t * p1.y + t * t * p2.y});
    }
}

// TrueType contours are on/off-curve point sequences where consecutive
// off-curve points imply an on-curve point at their midpoint. Flattens into
// a plain line-segment polygon in pixel space (y-flipped: font design units
// are y-up, screen/rasterizer space is y-down).
raster::Contour FlattenContour(const ttf::Contour& pts, float scale) {
    raster::Contour out;
    size_t n = pts.size();
    if (n == 0) return out;

    auto toScreen = [&](const ttf::Point& p) -> raster::Vec2 { return {p.x * scale, -p.y * scale}; };

    size_t startIdx = 0;
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (pts[i].onCurve) { startIdx = i; found = true; break; }
    }

    raster::Vec2 start;
    size_t firstIdx;
    if (found) {
        start = toScreen(pts[startIdx]);
        firstIdx = (startIdx + 1) % n;
    } else {
        ttf::Point synth{(pts[n - 1].x + pts[0].x) / 2.f, (pts[n - 1].y + pts[0].y) / 2.f, true};
        start = toScreen(synth);
        firstIdx = 0;
    }
    out.push_back(start);
    raster::Vec2 cur = start;

    raster::Vec2 pendingControl{};
    bool havePending = false;
    for (size_t k = 0; k < n; k++) {
        const ttf::Point& p = pts[(firstIdx + k) % n];
        raster::Vec2 pv = toScreen(p);
        if (p.onCurve) {
            if (havePending) {
                FlattenQuadratic(out, cur, pendingControl, pv);
                havePending = false;
            } else {
                out.push_back(pv);
            }
            cur = pv;
        } else {
            if (havePending) {
                raster::Vec2 mid{(pendingControl.x + pv.x) / 2.f, (pendingControl.y + pv.y) / 2.f};
                FlattenQuadratic(out, cur, pendingControl, mid);
                cur = mid;
                pendingControl = pv;
            } else {
                pendingControl = pv;
                havePending = true;
            }
        }
    }
    if (havePending) FlattenQuadratic(out, cur, pendingControl, start);
    return out;
}

}  // namespace

bool FontFace::Load(const std::string& path) { return m_font.LoadFromFile(path); }

std::vector<raster::Contour> FontFace::RenderGlyph(uint32_t codepoint, float sizePx) const {
    std::vector<raster::Contour> result;
    if (!m_font.IsLoaded() || m_font.UnitsPerEm() <= 0) return result;
    uint16_t gid = m_font.GlyphIndexForCodepoint(codepoint);
    if (gid == 0) return result;
    ttf::GlyphOutline outline = m_font.OutlineForGlyph(gid);
    float scale = sizePx / (float)m_font.UnitsPerEm();
    result.reserve(outline.contours.size());
    for (auto& contour : outline.contours) {
        raster::Contour flat = FlattenContour(contour, scale);
        if (flat.size() >= 2) result.push_back(std::move(flat));
    }
    return result;
}

float FontFace::AdvanceWidth(uint32_t codepoint, float sizePx) const {
    if (!m_font.IsLoaded() || m_font.UnitsPerEm() <= 0) return 0.f;
    uint16_t gid = m_font.GlyphIndexForCodepoint(codepoint);
    return m_font.AdvanceWidthForGlyph(gid) * sizePx / (float)m_font.UnitsPerEm();
}

float FontFace::Ascent(float sizePx) const {
    if (!m_font.IsLoaded() || m_font.UnitsPerEm() <= 0) return sizePx * 0.8f;
    return m_font.Ascender() * sizePx / (float)m_font.UnitsPerEm();
}

float FontFace::Descent(float sizePx) const {
    if (!m_font.IsLoaded() || m_font.UnitsPerEm() <= 0) return sizePx * 0.2f;
    return -m_font.Descender() * sizePx / (float)m_font.UnitsPerEm();
}

float FontFace::LineHeight(float sizePx) const { return Ascent(sizePx) + Descent(sizePx); }
