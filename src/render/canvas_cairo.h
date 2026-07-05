#pragma once
#include "js/canvas_surface.h"
#include <cairo.h>
#include <functional>

// Cairo-backed implementation of a <canvas> element's 2D drawing surface —
// the Linux counterpart to Windows' D2DCanvasSurface (src/render/canvas_renderer.h).
// Backed by an off-screen cairo_image_surface_t (CAIRO_FORMAT_ARGB32), so it
// composites into the main window exactly like a decoded <img> bitmap via
// IPlatformRenderer::DrawBitmap (src/platform/platform.h), which already
// accepts a raw cairo_surface_t* as a PlatBitmap.
//
// Unlike the D2D backend, this needs no manual path-command recording or
// hand-rolled arc-segment math: cairo_t already maintains its own current
// path and transform stack, which map almost 1:1 onto Canvas 2D semantics
// (paths persist across multiple fill()/stroke() calls until the next
// beginPath(), exactly like cairo_fill_preserve()/cairo_stroke_preserve()).
class CairoCanvasSurface : public ICanvasSurface {
public:
    // imageLookup resolves a decoded <img> URL to its cached cairo_surface_t*
    // (main_linux.cpp's g_images map) or nullptr if not loaded/known — the
    // engine-level canvas_bridge.cpp only ever has a URL, not a platform
    // bitmap handle, matching the Windows D2DCanvasSurface's use of
    // Renderer::FindCachedImageBitmap.
    CairoCanvasSurface(int width, int height,
                       std::function<cairo_surface_t*(const std::string&)> imageLookup);
    ~CairoCanvasSurface() override;

    void Resize(int width, int height) override;

    void SetFillStyle(const CssColor& color) override;
    void SetStrokeStyle(const CssColor& color) override;
    void SetLineWidth(float width) override;
    void SetGlobalAlpha(float alpha) override;

    void ClearRect(float x, float y, float w, float h) override;
    void FillRect(float x, float y, float w, float h) override;
    void StrokeRect(float x, float y, float w, float h) override;

    void BeginPath() override;
    void ClosePath() override;
    void MoveTo(float x, float y) override;
    void LineTo(float x, float y) override;
    void Rect(float x, float y, float w, float h) override;
    void Arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw) override;
    void BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) override;
    void QuadraticCurveTo(float cpx, float cpy, float x, float y) override;
    void Fill() override;
    void Stroke() override;

    void Save() override;
    void Restore() override;
    void Translate(float x, float y) override;
    void Scale(float x, float y) override;
    void Rotate(float radians) override;

    void DrawImage(const std::string& imageUrl, float dx, float dy, float dw, float dh) override;

    // Used by box_painter.h to composite this canvas's current pixels onto
    // the main render target, exactly like a decoded <img> bitmap.
    cairo_surface_t* GetSurface() const { return m_surface; }

private:
    cairo_surface_t* m_surface = nullptr;
    cairo_t* m_cr = nullptr;
    CssColor m_fillColor{true, 0, 0, 0, 1};
    CssColor m_strokeColor{true, 0, 0, 0, 1};
    float m_globalAlpha = 1.f;
    int m_width = 0, m_height = 0;
    std::function<cairo_surface_t*(const std::string&)> m_imageLookup;

    void CreateTarget(int width, int height);
    void ReleaseTarget();
    void ApplyFillColor();
    void ApplyStrokeColor();
};
