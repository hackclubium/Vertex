#pragma once
#include "css/style.h"
#include <cstdint>
#include <string>
#include <vector>

// Platform-neutral drawing surface for a single <canvas> element's 2D
// context. dom_bridge.cpp/canvas_bridge.cpp only ever call through this
// interface — no D2D (or other platform graphics API) types leak into the
// shared js/ engine code. The concrete implementation (D2DCanvasSurface,
// src/render/canvas_renderer.h) lives next to the Windows renderer, since it
// needs a live Direct2D device to draw into an off-screen bitmap target.
class ICanvasSurface {
public:
    virtual ~ICanvasSurface() = default;

    virtual void Resize(int width, int height) = 0;

    virtual void SetFillStyle(const CssColor& color) = 0;
    virtual void SetStrokeStyle(const CssColor& color) = 0;
    virtual void SetLineWidth(float width) = 0;
    virtual void SetGlobalAlpha(float alpha) = 0;

    virtual void ClearRect(float x, float y, float w, float h) = 0;
    virtual void FillRect(float x, float y, float w, float h) = 0;
    virtual void StrokeRect(float x, float y, float w, float h) = 0;

    // Path construction. Coordinates are in the canvas's own (untransformed)
    // user space; the surface applies its current transform internally.
    virtual void BeginPath() = 0;
    virtual void ClosePath() = 0;
    virtual void MoveTo(float x, float y) = 0;
    virtual void LineTo(float x, float y) = 0;
    virtual void Rect(float x, float y, float w, float h) = 0;
    virtual void Arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw) = 0;
    virtual void BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) = 0;
    virtual void QuadraticCurveTo(float cpx, float cpy, float x, float y) = 0;
    virtual void Fill() = 0;
    virtual void Stroke() = 0;

    virtual void Save() = 0;
    virtual void Restore() = 0;
    virtual void Translate(float x, float y) = 0;
    virtual void Scale(float x, float y) = 0;
    virtual void Rotate(float radians) = 0;

    // Draws a previously-decoded image (looked up by the renderer's own
    // image cache via `imageUrl`) into this surface. dw/dh <= 0 means "use
    // the image's natural intrinsic size" (the 3-argument JS drawImage form).
    virtual void DrawImage(const std::string& imageUrl,
                           float dx, float dy, float dw, float dh) = 0;
};
