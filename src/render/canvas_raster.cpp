#include "render/canvas_raster.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

RasterCanvasSurface::RasterCanvasSurface(int width, int height,
                                         std::function<const RasterBitmap*(const std::string&)> imageLookup)
    : m_imageLookup(std::move(imageLookup)) {
    m_fb.Resize(std::max(1, width), std::max(1, height));
}

void RasterCanvasSurface::Resize(int width, int height) {
    // Per spec, changing width/height resets both the bitmap and all drawing
    // state — explicitly re-zero even if the size is unchanged (Resize()
    // only reallocates on an actual size change).
    m_fb.Resize(std::max(1, width), std::max(1, height));
    std::fill(m_fb.pixels.begin(), m_fb.pixels.end(), 0);
    m_transform = Mat2x3{};
    m_saveStack.clear();
    m_fillColor = {true, 0, 0, 0, 1};
    m_strokeColor = {true, 0, 0, 0, 1};
    m_lineWidth = 1.f;
    m_globalAlpha = 1.f;
    m_subpaths.clear();
    m_haveCurrentPoint = false;
}

void RasterCanvasSurface::SetFillStyle(const CssColor& color)   { if (color.valid) m_fillColor = color; }
void RasterCanvasSurface::SetStrokeStyle(const CssColor& color) { if (color.valid) m_strokeColor = color; }
void RasterCanvasSurface::SetLineWidth(float width) { if (width > 0.f) m_lineWidth = width; }
void RasterCanvasSurface::SetGlobalAlpha(float alpha) { m_globalAlpha = std::clamp(alpha, 0.f, 1.f); }

void RasterCanvasSurface::ClearRect(float x, float y, float w, float h) {
    if (w == 0.f || h == 0.f) return;
    raster::Vec2 p0 = m_transform.Apply(x, y), p1 = m_transform.Apply(x + w, y);
    raster::Vec2 p2 = m_transform.Apply(x + w, y + h), p3 = m_transform.Apply(x, y + h);
    float minX = std::min({p0.x, p1.x, p2.x, p3.x}), maxX = std::max({p0.x, p1.x, p2.x, p3.x});
    float minY = std::min({p0.y, p1.y, p2.y, p3.y}), maxY = std::max({p0.y, p1.y, p2.y, p3.y});
    raster::ClearRect(m_fb, minX, minY, maxX - minX, maxY - minY, FullClip());
}

void RasterCanvasSurface::FillRect(float x, float y, float w, float h) {
    if (w == 0.f || h == 0.f) return;
    std::vector<raster::Contour> contours(1);
    contours[0] = {m_transform.Apply(x, y), m_transform.Apply(x + w, y),
                    m_transform.Apply(x + w, y + h), m_transform.Apply(x, y + h)};
    PlatColor col{m_fillColor.r, m_fillColor.g, m_fillColor.b, m_fillColor.a * m_globalAlpha};
    raster::FillPath(m_fb, contours, col, FullClip());
}

void RasterCanvasSurface::StrokeRect(float x, float y, float w, float h) {
    if (w == 0.f || h == 0.f) return;
    std::vector<raster::Vec2> pts = {m_transform.Apply(x, y), m_transform.Apply(x + w, y),
                                      m_transform.Apply(x + w, y + h), m_transform.Apply(x, y + h)};
    PlatColor col{m_strokeColor.r, m_strokeColor.g, m_strokeColor.b, m_strokeColor.a * m_globalAlpha};
    StrokePolyline(pts, /*closed=*/true, col);
}

void RasterCanvasSurface::BeginPath() {
    m_subpaths.clear();
    m_haveCurrentPoint = false;
}

void RasterCanvasSurface::ClosePath() {
    if (m_subpaths.empty() || m_subpaths.back().points.empty()) return;
    m_subpaths.back().closed = true;
    // Per spec, closePath() also moves the current point to the subpath's
    // first point, so a subsequent lineTo/curve continues from there.
    m_currentPoint = m_subpaths.back().points.front();
    m_haveCurrentPoint = true;
}

void RasterCanvasSurface::MoveTo(float x, float y) {
    raster::Vec2 p = m_transform.Apply(x, y);
    m_subpaths.push_back({});
    m_subpaths.back().points.push_back(p);
    m_currentPoint = p;
    m_haveCurrentPoint = true;
}

void RasterCanvasSurface::LineTo(float x, float y) {
    if (m_subpaths.empty()) { MoveTo(x, y); return; }  // spec: behaves like moveTo with no current subpath
    raster::Vec2 p = m_transform.Apply(x, y);
    m_subpaths.back().points.push_back(p);
    m_currentPoint = p;
    m_haveCurrentPoint = true;
}

void RasterCanvasSurface::Rect(float x, float y, float w, float h) {
    m_subpaths.push_back({});
    auto& sp = m_subpaths.back();
    sp.points = {m_transform.Apply(x, y), m_transform.Apply(x + w, y),
                 m_transform.Apply(x + w, y + h), m_transform.Apply(x, y + h)};
    sp.closed = true;
    // Per spec, rect() leaves the current point at the rect's start corner
    // (as if a fresh subpath had just been moveTo'd there).
    m_currentPoint = sp.points.front();
    m_haveCurrentPoint = true;
}

void RasterCanvasSurface::Arc(float cx, float cy, float radius, float startAngle, float endAngle, bool ccw) {
    if (radius < 0.f) return;  // spec: negative radius is an error; no-op defensively
    float sx = cx + radius * std::cos(startAngle);
    float sy = cy + radius * std::sin(startAngle);
    LineTo(sx, sy);  // implicit connecting line from wherever the pen is (Cairo/canvas-spec semantics)

    float delta = endAngle - startAngle;
    if (ccw) { while (delta > 0.f) delta -= 2.f * kPi; }
    else     { while (delta < 0.f) delta += 2.f * kPi; }

    int segs = std::max(8, (int)(std::fabs(delta) / (kPi / 16.f)));
    for (int i = 1; i <= segs; i++) {
        float t = startAngle + delta * (float)i / (float)segs;
        m_subpaths.back().points.push_back(m_transform.Apply(cx + radius * std::cos(t), cy + radius * std::sin(t)));
    }
    float endX = cx + radius * std::cos(startAngle + delta);
    float endY = cy + radius * std::sin(startAngle + delta);
    m_currentPoint = m_transform.Apply(endX, endY);
    m_haveCurrentPoint = true;
}

void RasterCanvasSurface::AppendCubic(raster::Vec2 p0, raster::Vec2 p1, raster::Vec2 p2, raster::Vec2 p3) {
    if (m_subpaths.empty()) {
        m_subpaths.push_back({});
        m_subpaths.back().points.push_back(p0);
    }
    const int segs = 16;
    for (int i = 1; i <= segs; i++) {
        float t = (float)i / (float)segs, mt = 1.f - t;
        float x = mt * mt * mt * p0.x + 3.f * mt * mt * t * p1.x + 3.f * mt * t * t * p2.x + t * t * t * p3.x;
        float y = mt * mt * mt * p0.y + 3.f * mt * mt * t * p1.y + 3.f * mt * t * t * p2.y + t * t * t * p3.y;
        m_subpaths.back().points.push_back({x, y});
    }
}

void RasterCanvasSurface::BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    raster::Vec2 c1 = m_transform.Apply(cp1x, cp1y);
    raster::Vec2 c2 = m_transform.Apply(cp2x, cp2y);
    raster::Vec2 end = m_transform.Apply(x, y);
    if (!m_haveCurrentPoint) { m_currentPoint = c1; m_haveCurrentPoint = true; }
    AppendCubic(m_currentPoint, c1, c2, end);
    m_currentPoint = end;
}

void RasterCanvasSurface::QuadraticCurveTo(float cpx, float cpy, float x, float y) {
    raster::Vec2 cp = m_transform.Apply(cpx, cpy);
    raster::Vec2 end = m_transform.Apply(x, y);
    if (!m_haveCurrentPoint) { m_currentPoint = cp; m_haveCurrentPoint = true; }
    // Exact degree elevation (quadratic -> cubic), affine-invariant so doing
    // this in already-transformed device space is equivalent to elevating
    // in user space first — matches CairoCanvasSurface's own approach.
    raster::Vec2 cur = m_currentPoint;
    raster::Vec2 c1{cur.x + 2.f / 3.f * (cp.x - cur.x), cur.y + 2.f / 3.f * (cp.y - cur.y)};
    raster::Vec2 c2{end.x + 2.f / 3.f * (cp.x - end.x), end.y + 2.f / 3.f * (cp.y - end.y)};
    AppendCubic(cur, c1, c2, end);
    m_currentPoint = end;
}

void RasterCanvasSurface::Fill() {
    std::vector<raster::Contour> contours;
    for (auto& sp : m_subpaths) if (sp.points.size() >= 2) contours.push_back(sp.points);
    if (contours.empty()) return;
    PlatColor col{m_fillColor.r, m_fillColor.g, m_fillColor.b, m_fillColor.a * m_globalAlpha};
    raster::FillPath(m_fb, contours, col, FullClip());
}

void RasterCanvasSurface::FillDisc(raster::Vec2 center, float radius, PlatColor color) {
    if (radius <= 0.05f) return;
    const int segs = 12;
    raster::Contour c;
    c.reserve(segs);
    for (int i = 0; i < segs; i++) {
        float t = (float)i / (float)segs * 2.f * kPi;
        c.push_back({center.x + radius * std::cos(t), center.y + radius * std::sin(t)});
    }
    std::vector<raster::Contour> contours{c};
    raster::FillPath(m_fb, contours, color, FullClip());
}

void RasterCanvasSurface::StrokePolyline(const std::vector<raster::Vec2>& pts, bool closed, PlatColor color) {
    size_t n = pts.size();
    if (n < 2) return;  // zero-length subpath: nothing to draw under the default butt cap
    size_t segCount = closed ? n : n - 1;
    for (size_t i = 0; i < segCount; i++) {
        raster::Vec2 a = pts[i], b = pts[(i + 1) % n];
        raster::StrokeLine(m_fb, a.x, a.y, b.x, b.y, color, m_lineWidth, FullClip());
    }
    // Round joins at every interior vertex (all vertices, for a closed path)
    // cover the gap between adjacent butt-capped segment quads. The
    // interface has no lineJoin/lineCap setters, so this fixed approximation
    // costs no fidelity actually reachable from JS.
    float half = m_lineWidth * 0.5f;
    size_t jointStart = closed ? 0 : 1;
    size_t jointEnd = closed ? n : n - 1;
    for (size_t i = jointStart; i < jointEnd; i++) FillDisc(pts[i], half, color);
}

void RasterCanvasSurface::Stroke() {
    PlatColor col{m_strokeColor.r, m_strokeColor.g, m_strokeColor.b, m_strokeColor.a * m_globalAlpha};
    for (auto& sp : m_subpaths) StrokePolyline(sp.points, sp.closed, col);
}

void RasterCanvasSurface::Save() {
    m_saveStack.push_back({m_transform, m_fillColor, m_strokeColor, m_lineWidth, m_globalAlpha});
}
void RasterCanvasSurface::Restore() {
    if (m_saveStack.empty()) return;
    const SaveState& s = m_saveStack.back();
    m_transform = s.transform;
    m_fillColor = s.fillColor;
    m_strokeColor = s.strokeColor;
    m_lineWidth = s.lineWidth;
    m_globalAlpha = s.globalAlpha;
    m_saveStack.pop_back();
}

void RasterCanvasSurface::Translate(float x, float y) {
    float e = m_transform.a * x + m_transform.c * y + m_transform.e;
    float f = m_transform.b * x + m_transform.d * y + m_transform.f;
    m_transform.e = e;
    m_transform.f = f;
}
void RasterCanvasSurface::Scale(float x, float y) {
    m_transform.a *= x;
    m_transform.c *= y;
    m_transform.b *= x;
    m_transform.d *= y;
}
void RasterCanvasSurface::Rotate(float radians) {
    float cs = std::cos(radians), sn = std::sin(radians);
    float a = m_transform.a, b = m_transform.b, c = m_transform.c, d = m_transform.d;
    m_transform.a = a * cs + c * sn;
    m_transform.c = -a * sn + c * cs;
    m_transform.b = b * cs + d * sn;
    m_transform.d = -b * sn + d * cs;
}

void RasterCanvasSurface::DrawImage(const std::string& imageUrl, float dx, float dy, float dw, float dh) {
    if (!m_imageLookup) return;
    const RasterBitmap* bmp = m_imageLookup(imageUrl);
    if (!bmp || bmp->width <= 0 || bmp->height <= 0) return;
    if (dw <= 0.f) dw = (float)bmp->width;
    if (dh <= 0.f) dh = (float)bmp->height;
    // BlitBitmap doesn't support rotation; for a pure translate/scale
    // transform (the overwhelming common case) this bounding box is exact.
    // A rotated transform degrades to an unrotated approximation — the same
    // tradeoff already made for ClearRect above.
    raster::Vec2 topLeft = m_transform.Apply(dx, dy);
    raster::Vec2 botRight = m_transform.Apply(dx + dw, dy + dh);
    float x0 = std::min(topLeft.x, botRight.x), x1 = std::max(topLeft.x, botRight.x);
    float y0 = std::min(topLeft.y, botRight.y), y1 = std::max(topLeft.y, botRight.y);
    raster::BlitBitmap(m_fb, bmp->bgra.data(), bmp->width, bmp->height, x0, y0, x1 - x0, y1 - y0, FullClip(), m_globalAlpha);
}

PlatBitmap RasterCanvasSurface::AsPlatBitmap() const {
    m_exposedBitmap.width = m_fb.width;
    m_exposedBitmap.height = m_fb.height;
    m_exposedBitmap.bgra = m_fb.pixels;
    return (PlatBitmap)&m_exposedBitmap;
}
