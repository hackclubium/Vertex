#ifdef __linux__
//
// platform_linux.cpp — Linux backend: hand-rolled rasterizer + Pango/FreeType2.
//
// Phase 2 of the Linux windowing rewrite: geometric primitives now go
// through Vertex's own software rasterizer (src/render/rasterizer.h)
// instead of Cairo. Text still goes through Pango (deferred to phase 3's
// from-scratch font engine) but via its FreeType2 backend (pangoft2)
// instead of pangocairo, so this file no longer touches Cairo at all — a
// PangoLayout is rendered straight to an 8-bit coverage bitmap via
// pango_ft2_render_layout(), then alpha-composited onto the framebuffer
// with the requested color.
//
#include "platform/platform.h"
#include "render/rasterizer.h"
#include <pango/pangoft2.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// ── helpers ──────────────────────────────────────────────────────────────────

static std::string WideToUtf8(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

// PlatBitmap backing store. main_linux.cpp's image pipeline already
// premultiplies + swizzles decoded pixels to BGRA before calling
// CreateBitmap (historically to match Cairo's ARGB32 layout) — the same
// layout rasterizer.h's BlitBitmap expects, so no conversion is needed here.
struct RasterBitmap {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;
};

// ── Rasterizer + Pango/FreeType2 renderer ────────────────────────────────────

class LinuxRenderer : public IPlatformRenderer {
public:
    LinuxRenderer() {
        m_fontMap = pango_ft2_font_map_new();
        m_pangoCtx = pango_font_map_create_context(PANGO_FONT_MAP(m_fontMap));
    }
    ~LinuxRenderer() override {
        if (m_pangoCtx) g_object_unref(m_pangoCtx);
        if (m_fontMap) g_object_unref(m_fontMap);
    }

    bool Init(void* nativeWindow) override {
        m_widget = nativeWindow;
        return m_widget != nullptr;
    }

    void Resize(int width, int height) override {
        m_width = width; m_height = height;
    }

    void SetNativeContext(void* ctx) override { m_fb = (raster::Framebuffer*)ctx; }

    void BeginFrame() override {}
    void EndFrame() override { m_fb = nullptr; }

    void Clear(PlatColor c) override {
        if (!m_fb) return;
        raster::FillRect(*m_fb, 0, 0, (float)m_width, (float)m_height, c, CurrentClip());
    }

    void FillRect(float x, float y, float w, float h, PlatColor c) override {
        if (!m_fb) return;
        raster::FillRect(*m_fb, x, y, w, h, c, CurrentClip());
    }
    void DrawRect(float x, float y, float w, float h, PlatColor c, float sw) override {
        if (!m_fb) return;
        raster::StrokeRect(*m_fb, x, y, w, h, c, sw, CurrentClip());
    }
    void FillRoundedRect(float x, float y, float w, float h, float r, PlatColor c) override {
        if (!m_fb) return;
        raster::FillRoundedRect(*m_fb, x, y, w, h, r, c, CurrentClip());
    }
    void DrawLine(float x1, float y1, float x2, float y2, PlatColor c, float sw) override {
        if (!m_fb) return;
        raster::StrokeLine(*m_fb, x1, y1, x2, y2, c, sw, CurrentClip());
    }

    void PushClip(float x, float y, float w, float h) override {
        raster::ClipRect cur = CurrentClip();
        raster::ClipRect nc;
        nc.x0 = std::max(cur.x0, (int)std::floor(x));
        nc.y0 = std::max(cur.y0, (int)std::floor(y));
        nc.x1 = std::min(cur.x1, (int)std::ceil(x + w));
        nc.y1 = std::min(cur.y1, (int)std::ceil(y + h));
        m_clipStack.push_back(nc);
    }
    void PopClip() override {
        if (!m_clipStack.empty()) m_clipStack.pop_back();
    }

    PlatFont CreateFont(float size, bool bold, bool italic, bool mono, const std::string& family) override {
        PangoFontDescription* fd = pango_font_description_new();
        std::string fam = family.empty() ? (mono ? "monospace" : "sans-serif") : family;
        pango_font_description_set_family(fd, fam.c_str());
        pango_font_description_set_size(fd, (gint)(size * PANGO_SCALE));
        pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
        pango_font_description_set_style(fd, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
        return (PlatFont)fd;
    }
    void ReleaseFont(PlatFont font) override {
        if (font) pango_font_description_free((PangoFontDescription*)font);
    }
    float MeasureText(const std::wstring& text, PlatFont font) override {
        if (!font || text.empty()) return 0;
        std::string utf8 = WideToUtf8(text);
        PangoLayout* layout = pango_layout_new(m_pangoCtx);
        pango_layout_set_font_description(layout, (PangoFontDescription*)font);
        pango_layout_set_text(layout, utf8.c_str(), -1);
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        g_object_unref(layout);
        return (float)pw;
    }
    float SpaceWidth(PlatFont font) override { return MeasureText(L" ", font); }
    float FontHeight(PlatFont font) override {
        if (!font) return 16.f;
        PangoLayout* layout = pango_layout_new(m_pangoCtx);
        pango_layout_set_font_description(layout, (PangoFontDescription*)font);
        pango_layout_set_text(layout, "X", 1);
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        g_object_unref(layout);
        return (float)ph;
    }
    void DrawText(const std::wstring& text, float x, float y, float maxW, float maxH,
                  PlatFont font, PlatColor color, bool underline) override {
        (void)maxH;
        if (!m_fb || !font || text.empty()) return;
        std::string utf8 = WideToUtf8(text);
        PangoLayout* layout = pango_layout_new(m_pangoCtx);
        pango_layout_set_font_description(layout, (PangoFontDescription*)font);
        pango_layout_set_text(layout, utf8.c_str(), -1);
        pango_layout_set_width(layout, (int)(maxW * PANGO_SCALE));
        if (underline) {
            PangoAttrList* attrs = pango_attr_list_new();
            pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
            pango_layout_set_attributes(layout, attrs);
            pango_attr_list_unref(attrs);
        }

        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        if (pw <= 0 || ph <= 0) { g_object_unref(layout); return; }

        std::vector<uint8_t> buf((size_t)pw * (size_t)ph, 0);
        FT_Bitmap bitmap;
        memset(&bitmap, 0, sizeof(bitmap));
        bitmap.rows = (unsigned int)ph;
        bitmap.width = (unsigned int)pw;
        bitmap.pitch = pw;
        bitmap.buffer = buf.data();
        bitmap.num_grays = 256;
        bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
        pango_ft2_render_layout(&bitmap, layout, 0, 0);

        raster::BlitAlphaMask(*m_fb, buf.data(), pw, ph, pw,
                               (int)std::lround(x), (int)std::lround(y), color, CurrentClip());
        g_object_unref(layout);
    }

    PlatBitmap CreateBitmap(int width, int height, const uint8_t* rgbaPixels) override {
        if (!rgbaPixels || width <= 0 || height <= 0) return nullptr;
        auto* bmp = new RasterBitmap();
        bmp->width = width;
        bmp->height = height;
        bmp->bgra.assign(rgbaPixels, rgbaPixels + (size_t)width * (size_t)height * 4);
        return (PlatBitmap)bmp;
    }
    void ReleaseBitmap(PlatBitmap bmp) override { delete (RasterBitmap*)bmp; }
    void DrawBitmap(PlatBitmap bmp, float x, float y, float w, float h) override {
        if (!m_fb || !bmp) return;
        auto* b = (RasterBitmap*)bmp;
        if (b->width <= 0 || b->height <= 0) return;
        raster::BlitBitmap(*m_fb, b->bgra.data(), b->width, b->height, x, y, w, h, CurrentClip());
    }
    void DrawBitmapTiled(PlatBitmap bmp, float destX, float destY, float destW, float destH,
                         float tileW, float tileH, float offsetX, float offsetY,
                         bool repeatX, bool repeatY) override {
        if (!m_fb || !bmp) return;
        PushClip(destX, destY, destW, destH);
        float startX = destX + offsetX, startY = destY + offsetY;
        if (repeatX) while (startX > destX) startX -= tileW;
        if (repeatY) while (startY > destY) startY -= tileH;
        if (tileW < 1.f) tileW = 1.f;
        if (tileH < 1.f) tileH = 1.f;
        for (float ty = startY; ty < destY + destH; ty += tileH) {
            for (float tx = startX; tx < destX + destW; tx += tileW) {
                DrawBitmap(bmp, tx, ty, tileW, tileH);
                if (!repeatX) break;
            }
            if (!repeatY) break;
        }
        PopClip();
    }

    int Width() const override { return m_width; }
    int Height() const override { return m_height; }

private:
    void* m_widget = nullptr;
    raster::Framebuffer* m_fb = nullptr;
    int m_width = 800, m_height = 600;
    std::vector<raster::ClipRect> m_clipStack;
    PangoFontMap* m_fontMap = nullptr;
    PangoContext* m_pangoCtx = nullptr;

    raster::ClipRect CurrentClip() const {
        if (!m_clipStack.empty()) return m_clipStack.back();
        return raster::ClipRect{0, 0, m_width, m_height};
    }
};

std::unique_ptr<IPlatformRenderer> CreatePlatformRenderer() {
    return std::make_unique<LinuxRenderer>();
}

#endif // __linux__
