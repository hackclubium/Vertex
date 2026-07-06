#include "render/canvas_coregraphics.h"
#include <algorithm>
#include <cmath>

CoreGraphicsCanvasSurface::CoreGraphicsCanvasSurface(int width, int height,
                                                     std::function<CGImageRef(const std::string&)> imageLookup)
    : m_imageLookup(std::move(imageLookup)) {
    CreateTarget(width, height);
}

CoreGraphicsCanvasSurface::~CoreGraphicsCanvasSurface() { ReleaseTarget(); }

void CoreGraphicsCanvasSurface::CreateTarget(int width, int height) {
    ReleaseTarget();
    m_width = std::max(1, width);
    m_height = std::max(1, height);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    // data=NULL: CoreGraphics allocates and zero-fills the buffer, matching
    // the other two backends' guaranteed fully-transparent initial content.
    m_ctx = CGBitmapContextCreate(nullptr, m_width, m_height, 8, 0, cs,
                                   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (m_ctx) {
        CGContextTranslateCTM(m_ctx, 0, m_height);
        CGContextScaleCTM(m_ctx, 1, -1);
    }
}

void CoreGraphicsCanvasSurface::ReleaseTarget() {
    if (m_ctx) { CGContextRelease(m_ctx); m_ctx = nullptr; }
}

void CoreGraphicsCanvasSurface::Resize(int width, int height) {
    // Per spec, changing width/height resets the bitmap and drawing state —
    // a fresh CGContextRef naturally has no path/transform/save-stack left.
    CreateTarget(width, height);
}

void CoreGraphicsCanvasSurface::SetFillStyle(const CssColor& color)   { if (color.valid) m_fillColor = color; }
void CoreGraphicsCanvasSurface::SetStrokeStyle(const CssColor& color) { if (color.valid) m_strokeColor = color; }
void CoreGraphicsCanvasSurface::SetLineWidth(float width) { if (m_ctx && width > 0.f) CGContextSetLineWidth(m_ctx, width); }
void CoreGraphicsCanvasSurface::SetGlobalAlpha(float alpha) { m_globalAlpha = std::clamp(alpha, 0.f, 1.f); }

void CoreGraphicsCanvasSurface::ApplyFillColor() {
    if (!m_ctx) return;
    CGContextSetRGBFillColor(m_ctx, m_fillColor.r, m_fillColor.g, m_fillColor.b, m_fillColor.a * m_globalAlpha);
}

void CoreGraphicsCanvasSurface::ApplyStrokeColor() {
    if (!m_ctx) return;
    CGContextSetRGBStrokeColor(m_ctx, m_strokeColor.r, m_strokeColor.g, m_strokeColor.b, m_strokeColor.a * m_globalAlpha);
}

void CoreGraphicsCanvasSurface::ClearRect(float x, float y, float w, float h) {
    if (!m_ctx) return;
    CGContextClearRect(m_ctx, CGRectMake(x, y, w, h));
}

void CoreGraphicsCanvasSurface::FillRect(float x, float y, float w, float h) {
    if (!m_ctx) return;
    ApplyFillColor();
    CGContextFillRect(m_ctx, CGRectMake(x, y, w, h));
}

void CoreGraphicsCanvasSurface::StrokeRect(float x, float y, float w, float h) {
    if (!m_ctx) return;
    ApplyStrokeColor();
    CGContextStrokeRect(m_ctx, CGRectMake(x, y, w, h));
}

void CoreGraphicsCanvasSurface::BeginPath() { if (m_ctx) CGContextBeginPath(m_ctx); }
void CoreGraphicsCanvasSurface::ClosePath() { if (m_ctx) CGContextClosePath(m_ctx); }
void CoreGraphicsCanvasSurface::MoveTo(float x, float y) { if (m_ctx) CGContextMoveToPoint(m_ctx, x, y); }
void CoreGraphicsCanvasSurface::LineTo(float x, float y) { if (m_ctx) CGContextAddLineToPoint(m_ctx, x, y); }

void CoreGraphicsCanvasSurface::Rect(float x, float y, float w, float h) {
    if (m_ctx) CGContextAddRect(m_ctx, CGRectMake(x, y, w, h));
}

void CoreGraphicsCanvasSurface::Arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw) {
    if (!m_ctx) return;
    // CGContextAddArc's "clockwise" flag is defined in CG's native (pre-flip)
    // space, so it comes out visually inverted once the context's base y
    // mirror is applied — see the class comment. Center/radius/angles pass
    // through unmodified since those are plain point/scalar values, not
    // direction-sensitive.
    CGContextAddArc(m_ctx, x, y, radius, startAngle, endAngle, ccw ? 1 : 0);
}

void CoreGraphicsCanvasSurface::BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    if (m_ctx) CGContextAddCurveToPoint(m_ctx, cp1x, cp1y, cp2x, cp2y, x, y);
}

void CoreGraphicsCanvasSurface::QuadraticCurveTo(float cpx, float cpy, float x, float y) {
    if (m_ctx) CGContextAddQuadCurveToPoint(m_ctx, cpx, cpy, x, y);
}

void CoreGraphicsCanvasSurface::Fill() {
    if (!m_ctx) return;
    // CGContextFillPath (unlike Cairo's cairo_fill_preserve) consumes the
    // current path, so copy it first and re-add it after to keep preserve
    // semantics — a canvas path must survive a fill() for a later stroke()
    // or continued lineTo() calls.
    CGPathRef path = CGContextCopyPath(m_ctx);
    ApplyFillColor();
    CGContextFillPath(m_ctx);
    if (path) {
        CGContextAddPath(m_ctx, path);
        CGPathRelease(path);
    }
}

void CoreGraphicsCanvasSurface::Stroke() {
    if (!m_ctx) return;
    CGPathRef path = CGContextCopyPath(m_ctx);
    ApplyStrokeColor();
    CGContextStrokePath(m_ctx);
    if (path) {
        CGContextAddPath(m_ctx, path);
        CGPathRelease(path);
    }
}

void CoreGraphicsCanvasSurface::Save() { if (m_ctx) CGContextSaveGState(m_ctx); }
void CoreGraphicsCanvasSurface::Restore() { if (m_ctx) CGContextRestoreGState(m_ctx); }
void CoreGraphicsCanvasSurface::Translate(float x, float y) { if (m_ctx) CGContextTranslateCTM(m_ctx, x, y); }
void CoreGraphicsCanvasSurface::Scale(float x, float y) { if (m_ctx) CGContextScaleCTM(m_ctx, x, y); }
void CoreGraphicsCanvasSurface::Rotate(float radians) {
    // Negated for the same reason Arc()'s direction flag is inverted: the
    // context's single base y-mirror reverses rotational chirality, and
    // negating the angle is the standard correction for a flipped CG context.
    if (m_ctx) CGContextRotateCTM(m_ctx, -radians);
}

void CoreGraphicsCanvasSurface::DrawImage(const std::string& imageUrl, float dx, float dy, float dw, float dh) {
    if (!m_ctx || !m_imageLookup) return;
    CGImageRef img = m_imageLookup(imageUrl);
    if (!img) return;
    size_t iw = CGImageGetWidth(img);
    size_t ih = CGImageGetHeight(img);
    if (iw == 0 || ih == 0) return;
    if (dw <= 0.f) dw = (float)iw;
    if (dh <= 0.f) dh = (float)ih;
    CGContextSaveGState(m_ctx);
    CGContextSetAlpha(m_ctx, m_globalAlpha);
    CGContextDrawImage(m_ctx, CGRectMake(dx, dy, dw, dh), img);
    CGContextRestoreGState(m_ctx);
}
