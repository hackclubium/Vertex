#ifdef _WIN32
//
// platform_win32.cpp — Windows backend: Win32 windowing + Direct2D rendering.
//
#include "platform/platform.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <commctrl.h>
#include <map>
#include <string>
#include <algorithm>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

// Windows macros that collide with our method names.
#undef CreateFont
#undef DrawText

// ── helpers ──────────────────────────────────────────────────────────────────

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static D2D1_COLOR_F ToD2D(PlatColor c) { return { c.r, c.g, c.b, c.a }; }

// ── D2D Renderer ─────────────────────────────────────────────────────────────

class Win32Renderer : public IPlatformRenderer {
public:
    ~Win32Renderer() override { Release(); }

    bool Init(void* nativeWindow) override {
        m_hwnd = (HWND)nativeWindow;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_factory)))
            return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&m_dwrite))))
            return false;
        return EnsureTarget();
    }

    void Resize(int width, int height) override {
        m_width = width; m_height = height;
        if (m_rt) m_rt->Resize(D2D1::SizeU(width, height));
    }

    void BeginFrame() override {
        EnsureTarget();
        for (auto* b : m_tempBrushes) if (b) b->Release();
        m_tempBrushes.clear();
        if (m_rt) m_rt->BeginDraw();
    }
    void EndFrame() override {
        if (m_rt) {
            HRESULT hr = m_rt->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) { if (m_rt) { m_rt->Release(); m_rt = nullptr; } }
        }
    }
    void Clear(PlatColor color) override {
        if (m_rt) m_rt->Clear(ToD2D(color));
    }

    void FillRect(float x, float y, float w, float h, PlatColor color) override {
        if (!m_rt) return;
        auto* b = TempBrush(ToD2D(color));
        if (b) m_rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b);
    }
    void DrawRect(float x, float y, float w, float h, PlatColor color, float strokeWidth) override {
        if (!m_rt) return;
        auto* b = TempBrush(ToD2D(color));
        if (b) m_rt->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), b, strokeWidth);
    }
    void FillRoundedRect(float x, float y, float w, float h, float radius, PlatColor color) override {
        if (!m_rt) return;
        auto* b = TempBrush(ToD2D(color));
        if (b) m_rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius), b);
    }
    void DrawLine(float x1, float y1, float x2, float y2, PlatColor color, float strokeWidth) override {
        if (!m_rt) return;
        auto* b = TempBrush(ToD2D(color));
        if (b) m_rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, strokeWidth);
    }

    void PushClip(float x, float y, float w, float h) override {
        if (m_rt) m_rt->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_ALIASED);
    }
    void PopClip() override { if (m_rt) m_rt->PopAxisAlignedClip(); }

    PlatFont CreateFont(float size, bool bold, bool italic, bool mono, const std::string& family) override {
        if (!m_dwrite) return nullptr;
        std::wstring fam = family.empty()
            ? (mono ? L"Consolas" : L"Segoe UI")
            : ToWide(family);
        IDWriteTextFormat* fmt = nullptr;
        m_dwrite->CreateTextFormat(fam.c_str(), nullptr,
            bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_REGULAR,
            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &fmt);
        return (PlatFont)fmt;
    }
    void ReleaseFont(PlatFont font) override {
        if (font) ((IDWriteTextFormat*)font)->Release();
    }
    float MeasureText(const std::wstring& text, PlatFont font) override {
        if (!m_dwrite || !font || text.empty()) return 0;
        auto* fmt = (IDWriteTextFormat*)font;
        IDWriteTextLayout* lay = nullptr;
        m_dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 100000.f, 10000.f, &lay);
        if (!lay) return 0;
        DWRITE_TEXT_METRICS m;
        lay->GetMetrics(&m);
        lay->Release();
        return m.widthIncludingTrailingWhitespace;
    }
    float SpaceWidth(PlatFont font) override { return MeasureText(L" ", font); }
    float FontHeight(PlatFont font) override {
        if (!font) return 16.f;
        auto* fmt = (IDWriteTextFormat*)font;
        return fmt->GetFontSize() * 1.2f;
    }
    void DrawText(const std::wstring& text, float x, float y, float maxW, float maxH,
                  PlatFont font, PlatColor color, bool underline) override {
        if (!m_rt || !m_dwrite || !font || text.empty()) return;
        auto* fmt = (IDWriteTextFormat*)font;
        auto* brush = TempBrush(ToD2D(color));
        if (!brush) return;
        IDWriteTextLayout* lay = nullptr;
        m_dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, maxW, maxH, &lay);
        if (!lay) return;
        if (underline) {
            DWRITE_TEXT_RANGE all{ 0, (UINT32)text.size() };
            lay->SetUnderline(TRUE, all);
        }
        m_rt->DrawTextLayout(D2D1::Point2F(x, y), lay, brush);
        lay->Release();
    }

    PlatBitmap CreateBitmap(int width, int height, const uint8_t* rgbaPixels) override {
        if (!m_rt || !rgbaPixels || width <= 0 || height <= 0) return nullptr;
        // Caller provides PBGRA (pre-multiplied, swizzled by ReceiveImage).
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ID2D1Bitmap* bmp = nullptr;
        m_rt->CreateBitmap(D2D1::SizeU(width, height), rgbaPixels, width * 4, props, &bmp);
        return (PlatBitmap)bmp;
    }
    void ReleaseBitmap(PlatBitmap bmp) override {
        if (bmp) ((ID2D1Bitmap*)bmp)->Release();
    }
    void DrawBitmap(PlatBitmap bmp, float x, float y, float w, float h) override {
        if (!m_rt || !bmp) return;
        m_rt->DrawBitmap((ID2D1Bitmap*)bmp, D2D1::RectF(x, y, x + w, y + h));
    }
    void DrawBitmapTiled(PlatBitmap bmp, float destX, float destY, float destW, float destH,
                         float tileW, float tileH, float offsetX, float offsetY,
                         bool repeatX, bool repeatY) override {
        if (!m_rt || !bmp) return;
        PushClip(destX, destY, destW, destH);
        float startX = destX + offsetX, startY = destY + offsetY;
        if (repeatX) while (startX > destX) startX -= tileW;
        if (repeatY) while (startY > destY) startY -= tileH;
        if (tileW < 1.f) tileW = 1.f;
        if (tileH < 1.f) tileH = 1.f;
        for (float ty = startY; ty < destY + destH; ty += tileH) {
            for (float tx = startX; tx < destX + destW; tx += tileW) {
                m_rt->DrawBitmap((ID2D1Bitmap*)bmp, D2D1::RectF(tx, ty, tx + tileW, ty + tileH));
                if (!repeatX) break;
            }
            if (!repeatY) break;
        }
        PopClip();
    }

    int Width() const override { return m_width; }
    int Height() const override { return m_height; }

private:
    HWND m_hwnd = nullptr;
    ID2D1Factory* m_factory = nullptr;
    ID2D1HwndRenderTarget* m_rt = nullptr;
    IDWriteFactory* m_dwrite = nullptr;
    int m_width = 800, m_height = 600;
    std::vector<ID2D1SolidColorBrush*> m_tempBrushes;

    bool EnsureTarget() {
        if (m_rt) return true;
        if (!m_factory || !m_hwnd) return false;
        RECT rc; GetClientRect(m_hwnd, &rc);
        m_width = rc.right; m_height = rc.bottom;
        return SUCCEEDED(m_factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(m_width, m_height)),
            &m_rt));
    }
    void Release() {
        for (auto* b : m_tempBrushes) if (b) b->Release();
        m_tempBrushes.clear();
        auto r = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
        r(m_rt); r(m_dwrite); r(m_factory);
    }
    ID2D1SolidColorBrush* TempBrush(D2D1_COLOR_F c) {
        ID2D1SolidColorBrush* b = nullptr;
        if (m_rt) m_rt->CreateSolidColorBrush(c, &b);
        if (b) m_tempBrushes.push_back(b);
        return b;
    }
};

// ── factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<IPlatformRenderer> CreatePlatformRenderer() {
    return std::make_unique<Win32Renderer>();
}

// Note: CreatePlatformWindow() is not implemented here because the current
// main.cpp manages the Win32 window directly. The interface exists for future
// refactoring where main.cpp becomes platform-agnostic.

#endif // _WIN32
