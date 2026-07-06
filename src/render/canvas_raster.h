#pragma once
//
// canvas_raster.h — <canvas> 2D backend built on Vertex's own software
// rasterizer (render/rasterizer.h). Phase 4 (last step) of the Linux
// windowing rewrite: replaces canvas_cairo.h, removing Cairo from Linux
// entirely. The Windows/macOS counterparts are D2DCanvasSurface
// (render/canvas_renderer.h) and CoreGraphicsCanvasSurface
// (render/canvas_coregraphics.h).
//
// Unlike those two (which delegate path building, curve tessellation, and
// stroking to their native 2D APIs), there's no OS primitive to lean on here
// — path commands are flattened into device-space polylines as they arrive
// (quadratic/cubic Beziers via fixed-subdivision sampling, matching this
// project's established "baseline first" pattern), and stroking is done by
// filling a quad per segment plus a small disc at each interior joint
// (a deliberately simple round-join approximation — the interface never
// exposes lineJoin/lineCap as configurable, so there's no fidelity lost
// versus what canvas_bridge.cpp can actually request).
//
#include "js/canvas_surface.h"
#include "render/raster_bitmap.h"
#include "render/rasterizer.h"
#include <functional>
#include <vector>

class RasterCanvasSurface : public ICanvasSurface {
public:
    // imageLookup resolves a decoded <img> URL to its cached RasterBitmap
    // (nullptr if not loaded/known) — same role as the Windows/macOS
    // backends' own cached-bitmap lookups.
    RasterCanvasSurface(int width, int height,
                        std::function<const RasterBitmap*(const std::string&)> imageLookup);
    ~RasterCanvasSurface() override = default;

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

    // Used by main_linux.cpp's box painter to composite this canvas's
    // current pixels onto the main framebuffer, exactly like a decoded <img>
    // bitmap. Returned pointer is owned by this surface (never released by
    // the caller) and stays valid until the next call to any mutating method.
    PlatBitmap AsPlatBitmap() const;

private:
    // x' = a*x + c*y + e ; y' = b*x + d*y + f — same convention as every
    // other 2D affine CTM (Cairo, Direct2D, Core Graphics).
    struct Mat2x3 {
        float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
        raster::Vec2 Apply(float x, float y) const { return {a * x + c * y + e, b * x + d * y + f}; }
    };
    struct SaveState {
        Mat2x3 transform;
        CssColor fillColor, strokeColor;
        float lineWidth, globalAlpha;
    };
    struct SubPath {
        std::vector<raster::Vec2> points;  // already in device space
        bool closed = false;
    };

    raster::Framebuffer m_fb;
    Mat2x3 m_transform;
    std::vector<SaveState> m_saveStack;
    CssColor m_fillColor{true, 0, 0, 0, 1};
    CssColor m_strokeColor{true, 0, 0, 0, 1};
    float m_lineWidth = 1.f;
    float m_globalAlpha = 1.f;
    std::function<const RasterBitmap*(const std::string&)> m_imageLookup;

    std::vector<SubPath> m_subpaths;
    bool m_haveCurrentPoint = false;
    raster::Vec2 m_currentPoint{};  // device space

    mutable RasterBitmap m_exposedBitmap;

    raster::ClipRect FullClip() const { return {0, 0, m_fb.width, m_fb.height}; }
    void AppendCubic(raster::Vec2 p0, raster::Vec2 p1, raster::Vec2 p2, raster::Vec2 p3);
    void FillDisc(raster::Vec2 center, float radius, PlatColor color);
    void StrokePolyline(const std::vector<raster::Vec2>& pts, bool closed, PlatColor color);
};
