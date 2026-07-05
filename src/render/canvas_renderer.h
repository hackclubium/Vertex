#pragma once
#include "js/canvas_surface.h"
#include <d2d1.h>
#include <vector>
#include <memory>

class Renderer;

// Direct2D-backed implementation of a <canvas> element's 2D drawing surface.
// Owns an off-screen ID2D1BitmapRenderTarget (created via
// CreateCompatibleRenderTarget against the main Renderer's live device), so
// canvas draw calls reuse the exact same D2D primitives (geometries, arcs,
// beziers, bitmaps) the rest of Vertex's rendering already depends on —
// no separate rasterizer, no third-party 2D graphics library.
//
// Path building records high-level commands (not a live-open geometry sink):
// canvas paths can be extended across multiple fill()/stroke() calls before
// the next beginPath(), which doesn't fit a sink that's Closed() once used.
// Each Fill()/Stroke() replays the recorded commands into a fresh
// ID2D1PathGeometry, so arcs/beziers still get native D2D tessellation
// quality with no hand-written curve flattening.
class D2DCanvasSurface : public ICanvasSurface {
public:
    D2DCanvasSurface(Renderer& owner, ID2D1Factory* factory, ID2D1RenderTarget* mainTarget,
                      int width, int height);
    ~D2DCanvasSurface() override;

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

    // Used by box_paint.cpp to composite this canvas's current pixels onto
    // the main render target, exactly like a decoded <img> bitmap.
    ID2D1Bitmap* GetBitmap() const;

    // Test/debug-only: reads back one pixel via GDI interop (the off-screen
    // target is created GDI-compatible for exactly this reason). Not part of
    // ICanvasSurface — only used by the standalone pixel-verification probe.
    bool DebugReadPixel(int x, int y, uint8_t outRgba[4]) const;

private:
    Renderer& m_owner;
    ID2D1Factory* m_factory = nullptr;
    ID2D1RenderTarget* m_mainTarget = nullptr;   // for CreateCompatibleRenderTarget on resize
    ID2D1BitmapRenderTarget* m_target = nullptr;
    CssColor m_fillColor{true, 0, 0, 0, 1};
    CssColor m_strokeColor{true, 0, 0, 0, 1};
    float m_lineWidth = 1.f;
    float m_globalAlpha = 1.f;
    int m_width = 0, m_height = 0;

    enum class PathCmd { MoveTo, LineTo, Arc, Bezier, Quad, Close };
    struct PathOp {
        PathCmd cmd;
        float x = 0, y = 0;                 // MoveTo/LineTo/Bezier&Quad end point, Arc center
        float x2 = 0, y2 = 0;                // Bezier cp1, Quad cp
        float x3 = 0, y3 = 0;                // Bezier cp2
        float radius = 0, startAngle = 0, endAngle = 0;
        bool ccw = false;
    };
    std::vector<PathOp> m_path;

    struct SaveState {
        D2D1_MATRIX_3X2_F transform;
        CssColor fillColor, strokeColor;
        float lineWidth, globalAlpha;
    };
    std::vector<SaveState> m_saveStack;
    D2D1_MATRIX_3X2_F m_transform = D2D1::Matrix3x2F::Identity();

    void CreateTarget(int width, int height);
    void ReleaseTarget();
    // Caller must Release() the returned brush.
    ID2D1SolidColorBrush* MakeBrush(const CssColor& color, float alpha) const;
    ID2D1PathGeometry* BuildGeometry() const;
};
