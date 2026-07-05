#include "render/canvas_cairo.h"
#include <algorithm>
#include <cmath>

CairoCanvasSurface::CairoCanvasSurface(int width, int height,
                                       std::function<cairo_surface_t*(const std::string&)> imageLookup)
    : m_imageLookup(std::move(imageLookup)) {
    CreateTarget(width, height);
}

CairoCanvasSurface::~CairoCanvasSurface() { ReleaseTarget(); }

void CairoCanvasSurface::CreateTarget(int width, int height) {
    ReleaseTarget();
    m_width = std::max(1, width);
    m_height = std::max(1, height);
    // cairo_image_surface_create() guarantees fully-transparent initial
    // content, matching the D2D backend's explicit Clear(0,0,0,0) — no
    // extra paint needed here.
    m_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, m_width, m_height);
    m_cr = cairo_create(m_surface);
}

void CairoCanvasSurface::ReleaseTarget() {
    if (m_cr) { cairo_destroy(m_cr); m_cr = nullptr; }
    if (m_surface) { cairo_surface_destroy(m_surface); m_surface = nullptr; }
}

void CairoCanvasSurface::Resize(int width, int height) {
    // Per spec, changing width/height resets the bitmap and drawing state —
    // a fresh cairo_t naturally has no path/transform/save-stack left over.
    CreateTarget(width, height);
}

void CairoCanvasSurface::SetFillStyle(const CssColor& color)   { if (color.valid) m_fillColor = color; }
void CairoCanvasSurface::SetStrokeStyle(const CssColor& color) { if (color.valid) m_strokeColor = color; }
void CairoCanvasSurface::SetLineWidth(float width) { if (m_cr && width > 0.f) cairo_set_line_width(m_cr, width); }
void CairoCanvasSurface::SetGlobalAlpha(float alpha) { m_globalAlpha = std::clamp(alpha, 0.f, 1.f); }

void CairoCanvasSurface::ApplyFillColor() {
    if (!m_cr) return;
    cairo_set_source_rgba(m_cr, m_fillColor.r, m_fillColor.g, m_fillColor.b, m_fillColor.a * m_globalAlpha);
}

void CairoCanvasSurface::ApplyStrokeColor() {
    if (!m_cr) return;
    cairo_set_source_rgba(m_cr, m_strokeColor.r, m_strokeColor.g, m_strokeColor.b, m_strokeColor.a * m_globalAlpha);
}

void CairoCanvasSurface::ClearRect(float x, float y, float w, float h) {
    if (!m_cr) return;
    cairo_save(m_cr);
    cairo_rectangle(m_cr, x, y, w, h);
    cairo_clip(m_cr);
    cairo_set_operator(m_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(m_cr);
    cairo_restore(m_cr);
}

void CairoCanvasSurface::FillRect(float x, float y, float w, float h) {
    if (!m_cr) return;
    cairo_save(m_cr);
    ApplyFillColor();
    cairo_rectangle(m_cr, x, y, w, h);
    cairo_fill(m_cr);
    cairo_restore(m_cr);
}

void CairoCanvasSurface::StrokeRect(float x, float y, float w, float h) {
    if (!m_cr) return;
    cairo_save(m_cr);
    ApplyStrokeColor();
    cairo_rectangle(m_cr, x, y, w, h);
    cairo_stroke(m_cr);
    cairo_restore(m_cr);
}

void CairoCanvasSurface::BeginPath() { if (m_cr) cairo_new_path(m_cr); }
void CairoCanvasSurface::ClosePath() { if (m_cr) cairo_close_path(m_cr); }
void CairoCanvasSurface::MoveTo(float x, float y) { if (m_cr) cairo_move_to(m_cr, x, y); }
void CairoCanvasSurface::LineTo(float x, float y) { if (m_cr) cairo_line_to(m_cr, x, y); }

void CairoCanvasSurface::Rect(float x, float y, float w, float h) {
    if (m_cr) cairo_rectangle(m_cr, x, y, w, h);
}

void CairoCanvasSurface::Arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw) {
    if (!m_cr) return;
    if (ccw) cairo_arc_negative(m_cr, x, y, radius, startAngle, endAngle);
    else     cairo_arc(m_cr, x, y, radius, startAngle, endAngle);
}

void CairoCanvasSurface::BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    if (m_cr) cairo_curve_to(m_cr, cp1x, cp1y, cp2x, cp2y, x, y);
}

void CairoCanvasSurface::QuadraticCurveTo(float cpx, float cpy, float x, float y) {
    if (!m_cr) return;
    // Cairo has no native quadratic bezier — elevate to cubic via the
    // standard degree-elevation formula (control points at 2/3 along the
    // line from the current point / end point to the quadratic control
    // point), which is exact, not an approximation.
    double curX = 0, curY = 0;
    cairo_get_current_point(m_cr, &curX, &curY);
    double cp1x = curX + 2.0 / 3.0 * (cpx - curX);
    double cp1y = curY + 2.0 / 3.0 * (cpy - curY);
    double cp2x = x + 2.0 / 3.0 * (cpx - x);
    double cp2y = y + 2.0 / 3.0 * (cpy - y);
    cairo_curve_to(m_cr, cp1x, cp1y, cp2x, cp2y, x, y);
}

void CairoCanvasSurface::Fill() {
    if (!m_cr) return;
    cairo_save(m_cr);
    ApplyFillColor();
    cairo_fill_preserve(m_cr);
    cairo_restore(m_cr);
}

void CairoCanvasSurface::Stroke() {
    if (!m_cr) return;
    cairo_save(m_cr);
    ApplyStrokeColor();
    cairo_stroke_preserve(m_cr);
    cairo_restore(m_cr);
}

void CairoCanvasSurface::Save() { if (m_cr) cairo_save(m_cr); }
void CairoCanvasSurface::Restore() { if (m_cr) cairo_restore(m_cr); }
void CairoCanvasSurface::Translate(float x, float y) { if (m_cr) cairo_translate(m_cr, x, y); }
void CairoCanvasSurface::Scale(float x, float y) { if (m_cr) cairo_scale(m_cr, x, y); }
void CairoCanvasSurface::Rotate(float radians) { if (m_cr) cairo_rotate(m_cr, radians); }

void CairoCanvasSurface::DrawImage(const std::string& imageUrl, float dx, float dy, float dw, float dh) {
    if (!m_cr || !m_imageLookup) return;
    cairo_surface_t* img = m_imageLookup(imageUrl);
    if (!img) return;
    int iw = cairo_image_surface_get_width(img);
    int ih = cairo_image_surface_get_height(img);
    if (iw <= 0 || ih <= 0) return;
    if (dw <= 0.f) dw = (float)iw;
    if (dh <= 0.f) dh = (float)ih;
    cairo_save(m_cr);
    cairo_translate(m_cr, dx, dy);
    cairo_scale(m_cr, dw / iw, dh / ih);
    cairo_set_source_surface(m_cr, img, 0, 0);
    cairo_paint_with_alpha(m_cr, m_globalAlpha);
    cairo_restore(m_cr);
}
