#pragma once
#include "js/canvas_surface.h"
#include <CoreGraphics/CoreGraphics.h>
#include <functional>

// CoreGraphics-backed implementation of a <canvas> element's 2D drawing
// surface — the macOS counterpart to Windows' D2DCanvasSurface
// (src/render/canvas_renderer.h) and Linux's CairoCanvasSurface
// (src/render/canvas_cairo.h).
//
// Unlike those two backends, CGImageRef (macOS's PlatBitmap, see
// platform/platform.h) is immutable — it's a snapshot, not a live handle —
// so this surface keeps drawing into a persistent CGContextRef bitmap
// context and hands out a *fresh* CGImageRef snapshot on demand via
// CreateSnapshot(), which the caller owns (must CGImageRelease it), mirroring
// the existing MacRenderer::CreateBitmap/ReleaseBitmap ownership convention
// in platform_macos.mm.
//
// CoreGraphics' native coordinate space is bottom-left-origin/y-up, unlike
// Cairo (natively y-down, matching Canvas 2D directly) or D2D. To let path
// coordinates (MoveTo/LineTo/Rect/curves/arc-center) pass straight through
// unmodified — same as the other two backends — CreateTarget() applies a
// one-time flip (Translate(0,height) + Scale(1,-1)) to the bitmap context,
// identical to the trick MacRenderer::BeginFrame uses for the main window.
// That single mirror reverses chirality for direction-sensitive operations,
// which is why Arc()'s clockwise flag and Rotate()'s angle are inverted
// below even though every point-passing method needs no adjustment at all.
class CoreGraphicsCanvasSurface : public ICanvasSurface {
public:
    // imageLookup resolves a decoded <img> URL to its cached CGImageRef
    // (main_macos.mm's g_images map) or nullptr if not loaded/known.
    CoreGraphicsCanvasSurface(int width, int height,
                              std::function<CGImageRef(const std::string&)> imageLookup);
    ~CoreGraphicsCanvasSurface() override;

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

    // Used by main_macos.mm to composite this canvas's current pixels onto
    // the main render target, exactly like a decoded <img> bitmap. Returns a
    // new CGImageRef the caller owns (must CGImageRelease) — CGImageRef is
    // immutable, so unlike Cairo's GetSurface() this can't return a live,
    // always-current handle.
    CGImageRef CreateSnapshot() const {
        return m_ctx ? CGBitmapContextCreateImage(m_ctx) : nullptr;
    }

private:
    CGContextRef m_ctx = nullptr;
    CssColor m_fillColor{true, 0, 0, 0, 1};
    CssColor m_strokeColor{true, 0, 0, 0, 1};
    float m_globalAlpha = 1.f;
    int m_width = 0, m_height = 0;
    std::function<CGImageRef(const std::string&)> m_imageLookup;

    void CreateTarget(int width, int height);
    void ReleaseTarget();
    void ApplyFillColor();
    void ApplyStrokeColor();
};
