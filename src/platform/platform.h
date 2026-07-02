#pragma once
//
// platform.h — cross-platform abstraction for windowing and 2D rendering.
//
// The engine (HTML/CSS/JS/layout) is platform-independent. Only the window
// shell and the 2D renderer need per-OS implementations:
//   Windows: Win32 + Direct2D + DirectWrite
//   macOS:   Cocoa + Core Graphics + Core Text
//   Linux:   GTK3 + Cairo + Pango
//
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

// ── hit region (shared across all platforms) ─────────────────────────────────

struct HitRegion {
    float       x, y, w, h;
    std::string href;
};

// ── color ────────────────────────────────────────────────────────────────────

struct PlatColor { float r = 0, g = 0, b = 0, a = 1; };

// ── opaque handles ───────────────────────────────────────────────────────────
// Platform backends define these; callers treat them as opaque.

using PlatBitmap = void*;   // ID2D1Bitmap* on Windows, CGImageRef on macOS, cairo_surface_t* on Linux
using PlatFont   = void*;   // IDWriteTextFormat* on Windows, CTFontRef on macOS, PangoFontDescription* on Linux

// ── text measurement interface ───────────────────────────────────────────────
// (Already exists as ITextMeasure in box.h; the platform renderer implements it.)

// ── platform renderer ────────────────────────────────────────────────────────

class IPlatformRenderer {
public:
    virtual ~IPlatformRenderer() = default;

    // Lifecycle
    virtual bool Init(void* nativeWindow) = 0;  // HWND, NSView*, GtkWidget*
    // Set the native drawing context for the current frame. On Linux this is
    // the cairo_t* from GTK's draw signal; on other platforms it's a no-op.
    virtual void SetNativeContext(void* ctx) { (void)ctx; }
    virtual void Resize(int width, int height) = 0;

    // Frame
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Clear(PlatColor color) = 0;

    // Primitives
    virtual void FillRect(float x, float y, float w, float h, PlatColor color) = 0;
    virtual void DrawRect(float x, float y, float w, float h, PlatColor color, float strokeWidth = 1.f) = 0;
    virtual void FillRoundedRect(float x, float y, float w, float h, float radius, PlatColor color) = 0;
    virtual void DrawLine(float x1, float y1, float x2, float y2, PlatColor color, float strokeWidth = 1.f) = 0;

    // Clipping
    virtual void PushClip(float x, float y, float w, float h) = 0;
    virtual void PopClip() = 0;

    // Text
    virtual PlatFont CreateFont(float size, bool bold, bool italic, bool mono, const std::string& family) = 0;
    virtual void     ReleaseFont(PlatFont font) = 0;
    virtual float    MeasureText(const std::wstring& text, PlatFont font) = 0;
    virtual float    SpaceWidth(PlatFont font) = 0;
    virtual float    FontHeight(PlatFont font) = 0;
    virtual void     DrawText(const std::wstring& text, float x, float y, float maxW, float maxH,
                              PlatFont font, PlatColor color, bool underline = false) = 0;

    // Images (from RGBA pixel data decoded by stb_image)
    virtual PlatBitmap CreateBitmap(int width, int height, const uint8_t* rgbaPixels) = 0;
    virtual void       ReleaseBitmap(PlatBitmap bmp) = 0;
    virtual void       DrawBitmap(PlatBitmap bmp, float x, float y, float w, float h) = 0;

    // Bitmap brush for tiled/positioned backgrounds
    virtual void DrawBitmapTiled(PlatBitmap bmp, float destX, float destY, float destW, float destH,
                                 float tileW, float tileH, float offsetX, float offsetY,
                                 bool repeatX, bool repeatY) = 0;

    // Query
    virtual int Width() const = 0;
    virtual int Height() const = 0;
};

// ── platform window ──────────────────────────────────────────────────────────

struct PlatformEvent {
    enum Type {
        None, Close, Resize, Paint,
        MouseDown, MouseUp, MouseMove,
        KeyDown, KeyUp, Scroll,
        Timer, Custom
    } type = None;

    int x = 0, y = 0;           // mouse position
    int width = 0, height = 0;  // resize dimensions
    int keyCode = 0;
    std::string keyText;
    float scrollDelta = 0;       // vertical scroll amount
    int customId = 0;            // for WM_USER+N messages
    void* customData = nullptr;
};

class IPlatformWindow {
public:
    virtual ~IPlatformWindow() = default;

    virtual bool Create(const std::string& title, int width, int height) = 0;
    virtual void Show() = 0;
    virtual void SetTitle(const std::string& title) = 0;

    // URL bar
    virtual void SetUrlBarText(const std::string& text) = 0;
    virtual std::string GetUrlBarText() = 0;

    // Status bar
    virtual void SetStatusText(const std::string& text) = 0;

    // Invalidate (request repaint)
    virtual void Invalidate() = 0;

    // Scrollbar
    virtual void SetScrollRange(int total, int page, int pos) = 0;

    // Timers
    virtual int  SetTimer(int milliseconds) = 0;
    virtual void KillTimer(int id) = 0;

    // Post a custom message to the event queue (thread-safe)
    virtual void PostCustomEvent(int id, void* data) = 0;

    // Native window handle (for renderer init)
    virtual void* NativeHandle() = 0;

    // Get the associated renderer
    virtual IPlatformRenderer* GetRenderer() = 0;
};

// ── factory ──────────────────────────────────────────────────────────────────

// Each platform implements these. The app calls CreatePlatformWindow() and gets
// a window + renderer pair appropriate for the OS.
std::unique_ptr<IPlatformWindow> CreatePlatformWindow();
std::unique_ptr<IPlatformRenderer> CreatePlatformRenderer();
