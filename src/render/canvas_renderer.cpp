#include "render/canvas_renderer.h"
#include "render/renderer.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = 3.14159265358979323846f;

D2D1_COLOR_F ToD2DColor(const CssColor& c, float alpha) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a * alpha);
}

D2D1_POINT_2F PointAtAngle(float cx, float cy, float r, float angle) {
    return D2D1::Point2F(cx + r * std::cos(angle), cy + r * std::sin(angle));
}

struct ArcParams { float x, y, radius, startAngle, endAngle; bool ccw; };

// Normalizes an arc's angular sweep to (0, 2*pi] respecting direction, and
// appends it to an open geometry sink figure. ID2D1's AddArc can't sweep a
// full turn (start == end degenerates to nothing), so a full-circle sweep is
// split into two half-circle arcs.
void AddArcSegment(ID2D1GeometrySink* sink, bool& hasCurrent, D2D1_POINT_2F& current,
                   const ArcParams& a) {
    D2D1_POINT_2F start = PointAtAngle(a.x, a.y, a.radius, a.startAngle);
    if (!hasCurrent) {
        sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
        hasCurrent = true;
        current = start;
    } else if (std::abs(current.x - start.x) > 0.01f || std::abs(current.y - start.y) > 0.01f) {
        sink->AddLine(start);
        current = start;
    }

    D2D1_SWEEP_DIRECTION sweep = a.ccw ? D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE
                                       : D2D1_SWEEP_DIRECTION_CLOCKWISE;
    D2D1_SIZE_F size = D2D1::SizeF(a.radius, a.radius);

    // Check the RAW (unnormalized) sweep magnitude first: per spec, a delta
    // of >= 2*pi means "draw the full circle" — e.g. arc(x,y,r,0,2*Math.PI).
    // Normalizing modulo 2*pi *before* this check would wrap 2*pi down to a
    // zero-length arc, silently drawing nothing (the actual bug this fixes).
    float rawDelta = a.endAngle - a.startAngle;
    if (std::abs(rawDelta) >= 2.f * kPi - 1e-3f) {
        // Full circle: AddArc can't sweep 360 degrees in one segment.
        float mid = a.startAngle + (a.ccw ? -kPi : kPi);
        D2D1_POINT_2F midPt = PointAtAngle(a.x, a.y, a.radius, mid);
        sink->AddArc(D2D1::ArcSegment(midPt, size, 0.f, sweep, D2D1_ARC_SIZE_SMALL));
        D2D1_POINT_2F endPt = PointAtAngle(a.x, a.y, a.radius, mid + (a.ccw ? -kPi : kPi));
        sink->AddArc(D2D1::ArcSegment(endPt, size, 0.f, sweep, D2D1_ARC_SIZE_SMALL));
        current = endPt;
        return;
    }

    float delta = rawDelta;
    if (a.ccw) {
        while (delta > 0.f) delta -= 2.f * kPi;
        while (delta <= -2.f * kPi) delta += 2.f * kPi;
    } else {
        while (delta < 0.f) delta += 2.f * kPi;
        while (delta >= 2.f * kPi) delta -= 2.f * kPi;
    }
    float absDelta = std::abs(delta);
    if (absDelta < 1e-4f) return;

    D2D1_POINT_2F end = PointAtAngle(a.x, a.y, a.radius, a.startAngle + delta);
    D2D1_ARC_SIZE arcSize = absDelta > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
    sink->AddArc(D2D1::ArcSegment(end, size, 0.f, sweep, arcSize));
    current = end;
}
} // namespace

D2DCanvasSurface::D2DCanvasSurface(Renderer& owner, ID2D1Factory* factory,
                                   ID2D1RenderTarget* mainTarget, int width, int height)
    : m_owner(owner), m_factory(factory), m_mainTarget(mainTarget) {
    CreateTarget(width, height);
}

D2DCanvasSurface::~D2DCanvasSurface() { ReleaseTarget(); }

void D2DCanvasSurface::CreateTarget(int width, int height) {
    ReleaseTarget();
    m_width = std::max(1, width);
    m_height = std::max(1, height);
    if (!m_mainTarget) return;
    // GDI-compatible so DebugReadPixel() (pixel-verification probe) can read
    // back pixels via GetDC/GetPixel — the legacy ID2D1RenderTarget API has
    // no direct CPU-mappable bitmap readback of its own.
    m_mainTarget->CreateCompatibleRenderTarget(
        D2D1::SizeF((float)m_width, (float)m_height),
        D2D1::SizeU((UINT32)m_width, (UINT32)m_height),
        D2D1::PixelFormat(),
        D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_GDI_COMPATIBLE,
        &m_target);
    if (m_target) {
        m_target->BeginDraw();
        m_target->Clear(D2D1::ColorF(0, 0, 0, 0));
        m_target->EndDraw();
    }
}

void D2DCanvasSurface::ReleaseTarget() {
    if (m_target) { m_target->Release(); m_target = nullptr; }
}

void D2DCanvasSurface::Resize(int width, int height) {
    // Per spec, changing width/height resets the bitmap and drawing state.
    CreateTarget(width, height);
    m_path.clear();
    m_saveStack.clear();
    m_transform = D2D1::Matrix3x2F::Identity();
}

ID2D1Bitmap* D2DCanvasSurface::GetBitmap() const {
    if (!m_target) return nullptr;
    // GetBitmap() AddRefs; release immediately and hand back a borrowed
    // pointer — the bitmap render target itself keeps the underlying bitmap
    // alive for as long as m_target exists, and the caller (box_paint.cpp)
    // only ever uses this synchronously within a single paint call.
    ID2D1Bitmap* bmp = nullptr;
    m_target->GetBitmap(&bmp);
    if (bmp) bmp->Release();
    return bmp;
}

bool D2DCanvasSurface::DebugReadPixel(int x, int y, uint8_t outRgba[4]) const {
    if (!m_target) return false;
    ID2D1GdiInteropRenderTarget* interop = nullptr;
    if (FAILED(m_target->QueryInterface(&interop)) || !interop) return false;
    // GetDC() is only valid between BeginDraw/EndDraw; every draw method
    // above already closes its own bracket before returning, so open one
    // here purely to read, without issuing any drawing commands.
    m_target->BeginDraw();
    HDC hdc = nullptr;
    bool ok = false;
    if (SUCCEEDED(interop->GetDC(D2D1_DC_INITIALIZE_MODE_COPY, &hdc)) && hdc) {
        COLORREF c = GetPixel(hdc, x, y);
        interop->ReleaseDC(nullptr);
        if (c != CLR_INVALID) {
            outRgba[0] = GetRValue(c);
            outRgba[1] = GetGValue(c);
            outRgba[2] = GetBValue(c);
            outRgba[3] = 255;   // GDI has no alpha channel; opaque canvas bg reads as 255
            ok = true;
        }
    }
    m_target->EndDraw();
    interop->Release();
    return ok;
}

ID2D1SolidColorBrush* D2DCanvasSurface::MakeBrush(const CssColor& color, float alpha) const {
    if (!m_target) return nullptr;
    ID2D1SolidColorBrush* b = nullptr;
    m_target->CreateSolidColorBrush(ToD2DColor(color, alpha), &b);
    return b;
}

void D2DCanvasSurface::SetFillStyle(const CssColor& color)   { if (color.valid) m_fillColor = color; }
void D2DCanvasSurface::SetStrokeStyle(const CssColor& color) { if (color.valid) m_strokeColor = color; }
void D2DCanvasSurface::SetLineWidth(float width)             { if (width > 0.f) m_lineWidth = width; }
void D2DCanvasSurface::SetGlobalAlpha(float alpha)           { m_globalAlpha = std::clamp(alpha, 0.f, 1.f); }

void D2DCanvasSurface::ClearRect(float x, float y, float w, float h) {
    if (!m_target) return;
    m_target->BeginDraw();
    m_target->SetTransform(m_transform);
    // ClearRect must actually erase to transparent, not paint — D2D's Clear()
    // wipes the whole target, so instead punch a transparent rect via a
    // fully-erasing FillRectangle using a "copy" blend isn't exposed on
    // ID2D1RenderTarget directly; PushAxisAlignedClip + Clear(transparent)
    // scoped to the rect achieves the same result.
    D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + h);
    m_target->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    m_target->Clear(D2D1::ColorF(0, 0, 0, 0));
    m_target->PopAxisAlignedClip();
    m_target->SetTransform(D2D1::Matrix3x2F::Identity());
    m_target->EndDraw();
}

void D2DCanvasSurface::FillRect(float x, float y, float w, float h) {
    if (!m_target) return;
    ID2D1SolidColorBrush* brush = MakeBrush(m_fillColor, m_globalAlpha);
    if (!brush) return;
    m_target->BeginDraw();
    m_target->SetTransform(m_transform);
    m_target->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
    m_target->SetTransform(D2D1::Matrix3x2F::Identity());
    m_target->EndDraw();
    brush->Release();
}

void D2DCanvasSurface::StrokeRect(float x, float y, float w, float h) {
    if (!m_target) return;
    ID2D1SolidColorBrush* brush = MakeBrush(m_strokeColor, m_globalAlpha);
    if (!brush) return;
    m_target->BeginDraw();
    m_target->SetTransform(m_transform);
    m_target->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), brush, m_lineWidth);
    m_target->SetTransform(D2D1::Matrix3x2F::Identity());
    m_target->EndDraw();
    brush->Release();
}

void D2DCanvasSurface::BeginPath() { m_path.clear(); }

void D2DCanvasSurface::ClosePath() {
    PathOp op; op.cmd = PathCmd::Close;
    m_path.push_back(op);
}

void D2DCanvasSurface::MoveTo(float x, float y) {
    PathOp op; op.cmd = PathCmd::MoveTo; op.x = x; op.y = y;
    m_path.push_back(op);
}

void D2DCanvasSurface::LineTo(float x, float y) {
    PathOp op; op.cmd = PathCmd::LineTo; op.x = x; op.y = y;
    m_path.push_back(op);
}

void D2DCanvasSurface::Rect(float x, float y, float w, float h) {
    MoveTo(x, y);
    LineTo(x + w, y);
    LineTo(x + w, y + h);
    LineTo(x, y + h);
    ClosePath();
}

void D2DCanvasSurface::Arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw) {
    PathOp op; op.cmd = PathCmd::Arc;
    op.x = x; op.y = y; op.radius = radius;
    op.startAngle = startAngle; op.endAngle = endAngle; op.ccw = ccw;
    m_path.push_back(op);
}

void D2DCanvasSurface::BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    PathOp op; op.cmd = PathCmd::Bezier;
    op.x2 = cp1x; op.y2 = cp1y; op.x3 = cp2x; op.y3 = cp2y; op.x = x; op.y = y;
    m_path.push_back(op);
}

void D2DCanvasSurface::QuadraticCurveTo(float cpx, float cpy, float x, float y) {
    PathOp op; op.cmd = PathCmd::Quad;
    op.x2 = cpx; op.y2 = cpy; op.x = x; op.y = y;
    m_path.push_back(op);
}

ID2D1PathGeometry* D2DCanvasSurface::BuildGeometry() const {
    if (!m_factory) return nullptr;
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(m_factory->CreatePathGeometry(&geo)) || !geo) return nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geo->Open(&sink)) || !sink) { geo->Release(); return nullptr; }

    bool hasCurrent = false;
    D2D1_POINT_2F current{0, 0};
    bool figureOpen = false;

    auto ensureFigure = [&](D2D1_POINT_2F pt) {
        if (!figureOpen) {
            sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED);
            figureOpen = true;
        }
    };

    for (const auto& op : m_path) {
        switch (op.cmd) {
        case PathCmd::MoveTo:
            if (figureOpen) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            figureOpen = false;
            current = D2D1::Point2F(op.x, op.y);
            hasCurrent = true;
            break;
        case PathCmd::LineTo:
            if (!hasCurrent) { current = D2D1::Point2F(op.x, op.y); hasCurrent = true; }
            ensureFigure(current);
            current = D2D1::Point2F(op.x, op.y);
            sink->AddLine(current);
            break;
        case PathCmd::Bezier: {
            D2D1_POINT_2F end = D2D1::Point2F(op.x, op.y);
            if (!hasCurrent) { current = end; hasCurrent = true; }
            ensureFigure(current);
            sink->AddBezier(D2D1::BezierSegment(
                D2D1::Point2F(op.x2, op.y2), D2D1::Point2F(op.x3, op.y3), end));
            current = end;
            break;
        }
        case PathCmd::Quad: {
            D2D1_POINT_2F end = D2D1::Point2F(op.x, op.y);
            if (!hasCurrent) { current = end; hasCurrent = true; }
            ensureFigure(current);
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(op.x2, op.y2), end));
            current = end;
            break;
        }
        case PathCmd::Arc: {
            bool wasOpen = figureOpen;
            ArcParams ap{op.x, op.y, op.radius, op.startAngle, op.endAngle, op.ccw};
            bool arcHasCurrent = wasOpen;
            AddArcSegment(sink, arcHasCurrent, current, ap);
            figureOpen = true;
            hasCurrent = true;
            break;
        }
        case PathCmd::Close:
            if (figureOpen) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            figureOpen = false;
            break;
        }
    }
    if (figureOpen) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return geo;
}

void D2DCanvasSurface::Fill() {
    if (!m_target) return;
    ID2D1PathGeometry* geo = BuildGeometry();
    if (!geo) return;
    ID2D1SolidColorBrush* brush = MakeBrush(m_fillColor, m_globalAlpha);
    if (brush) {
        m_target->BeginDraw();
        m_target->SetTransform(m_transform);
        m_target->FillGeometry(geo, brush);
        m_target->SetTransform(D2D1::Matrix3x2F::Identity());
        m_target->EndDraw();
        brush->Release();
    }
    geo->Release();
}

void D2DCanvasSurface::Stroke() {
    if (!m_target) return;
    ID2D1PathGeometry* geo = BuildGeometry();
    if (!geo) return;
    ID2D1SolidColorBrush* brush = MakeBrush(m_strokeColor, m_globalAlpha);
    if (brush) {
        m_target->BeginDraw();
        m_target->SetTransform(m_transform);
        m_target->DrawGeometry(geo, brush, m_lineWidth);
        m_target->SetTransform(D2D1::Matrix3x2F::Identity());
        m_target->EndDraw();
        brush->Release();
    }
    geo->Release();
}

void D2DCanvasSurface::Save() {
    m_saveStack.push_back({m_transform, m_fillColor, m_strokeColor, m_lineWidth, m_globalAlpha});
}

void D2DCanvasSurface::Restore() {
    if (m_saveStack.empty()) return;
    const auto& s = m_saveStack.back();
    m_transform = s.transform;
    m_fillColor = s.fillColor;
    m_strokeColor = s.strokeColor;
    m_lineWidth = s.lineWidth;
    m_globalAlpha = s.globalAlpha;
    m_saveStack.pop_back();
}

void D2DCanvasSurface::Translate(float x, float y) {
    m_transform = D2D1::Matrix3x2F::Translation(x, y) * m_transform;
}

void D2DCanvasSurface::Scale(float x, float y) {
    m_transform = D2D1::Matrix3x2F::Scale(x, y) * m_transform;
}

void D2DCanvasSurface::Rotate(float radians) {
    m_transform = D2D1::Matrix3x2F::Rotation(radians * 180.f / kPi) * m_transform;
}

void D2DCanvasSurface::DrawImage(const std::string& imageUrl, float dx, float dy, float dw, float dh) {
    if (!m_target) return;
    ID2D1Bitmap* bmp = m_owner.FindCachedImageBitmap(imageUrl);
    if (!bmp) return;
    if (dw <= 0.f || dh <= 0.f) {
        float iw = 0.f, ih = 0.f;
        if (m_owner.ImageIntrinsic(imageUrl, iw, ih)) { dw = iw; dh = ih; }
        else return;
    }
    m_target->BeginDraw();
    m_target->SetTransform(m_transform);
    m_target->DrawBitmap(bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh));
    m_target->SetTransform(D2D1::Matrix3x2F::Identity());
    m_target->EndDraw();
}
