#ifdef __linux__
//
// platform_linux.cpp — Linux backend: GTK3 + Cairo + Pango.
//
// Build with: pkg-config --cflags --libs gtk+-3.0
//
#define _USE_MATH_DEFINES
#include "platform/platform.h"
#include <gtk/gtk.h>
#include <cmath>
#include <pango/pangocairo.h>
#include <cstring>
#include <algorithm>

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

// ── Cairo + Pango Renderer ───────────────────────────────────────────────────

class LinuxRenderer : public IPlatformRenderer {
public:
    ~LinuxRenderer() override = default;

    bool Init(void* nativeWindow) override {
        m_widget = (GtkWidget*)nativeWindow;
        return m_widget != nullptr;
    }

    void Resize(int width, int height) override {
        m_width = width; m_height = height;
    }

    void SetNativeContext(void* ctx) override { m_cr = (cairo_t*)ctx; }

    void BeginFrame() override { /* m_cr set via SetNativeContext from draw callback */ }
    void EndFrame() override { m_cr = nullptr; }

    void Clear(PlatColor c) override {
        if (!m_cr) return;
        cairo_set_source_rgba(m_cr, c.r, c.g, c.b, c.a);
        cairo_paint(m_cr);
    }

    void FillRect(float x, float y, float w, float h, PlatColor c) override {
        if (!m_cr) return;
        cairo_set_source_rgba(m_cr, c.r, c.g, c.b, c.a);
        cairo_rectangle(m_cr, x, y, w, h);
        cairo_fill(m_cr);
    }
    void DrawRect(float x, float y, float w, float h, PlatColor c, float sw) override {
        if (!m_cr) return;
        cairo_set_source_rgba(m_cr, c.r, c.g, c.b, c.a);
        cairo_set_line_width(m_cr, sw);
        cairo_rectangle(m_cr, x, y, w, h);
        cairo_stroke(m_cr);
    }
    void FillRoundedRect(float x, float y, float w, float h, float r, PlatColor c) override {
        if (!m_cr) return;
        cairo_set_source_rgba(m_cr, c.r, c.g, c.b, c.a);
        double deg = M_PI / 180.0;
        cairo_new_sub_path(m_cr);
        cairo_arc(m_cr, x + w - r, y + r, r, -90 * deg, 0);
        cairo_arc(m_cr, x + w - r, y + h - r, r, 0, 90 * deg);
        cairo_arc(m_cr, x + r, y + h - r, r, 90 * deg, 180 * deg);
        cairo_arc(m_cr, x + r, y + r, r, 180 * deg, 270 * deg);
        cairo_close_path(m_cr);
        cairo_fill(m_cr);
    }
    void DrawLine(float x1, float y1, float x2, float y2, PlatColor c, float sw) override {
        if (!m_cr) return;
        cairo_set_source_rgba(m_cr, c.r, c.g, c.b, c.a);
        cairo_set_line_width(m_cr, sw);
        cairo_move_to(m_cr, x1, y1);
        cairo_line_to(m_cr, x2, y2);
        cairo_stroke(m_cr);
    }

    void PushClip(float x, float y, float w, float h) override {
        if (!m_cr) return;
        cairo_save(m_cr);
        cairo_rectangle(m_cr, x, y, w, h);
        cairo_clip(m_cr);
    }
    void PopClip() override { if (m_cr) cairo_restore(m_cr); }

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
        if (!m_cr || !font || text.empty()) return 0;
        std::string utf8 = WideToUtf8(text);
        PangoLayout* layout = pango_cairo_create_layout(m_cr);
        pango_layout_set_font_description(layout, (PangoFontDescription*)font);
        pango_layout_set_text(layout, utf8.c_str(), -1);
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        g_object_unref(layout);
        return (float)pw;
    }
    float SpaceWidth(PlatFont font) override { return MeasureText(L" ", font); }
    float FontHeight(PlatFont font) override {
        if (!m_cr || !font) return 16.f;
        PangoLayout* layout = pango_cairo_create_layout(m_cr);
        pango_layout_set_font_description(layout, (PangoFontDescription*)font);
        pango_layout_set_text(layout, "X", 1);
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        g_object_unref(layout);
        return (float)ph;
    }
    void DrawText(const std::wstring& text, float x, float y, float maxW, float maxH,
                  PlatFont font, PlatColor color, bool underline) override {
        if (!m_cr || !font || text.empty()) return;
        std::string utf8 = WideToUtf8(text);
        cairo_set_source_rgba(m_cr, color.r, color.g, color.b, color.a);
        PangoLayout* layout = pango_cairo_create_layout(m_cr);
        pango_layout_set_font_description(layout, (PangoFontDescription*)font);
        pango_layout_set_text(layout, utf8.c_str(), -1);
        pango_layout_set_width(layout, (int)(maxW * PANGO_SCALE));
        if (underline) {
            PangoAttrList* attrs = pango_attr_list_new();
            pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
            pango_layout_set_attributes(layout, attrs);
            pango_attr_list_unref(attrs);
        }
        cairo_move_to(m_cr, x, y);
        pango_cairo_show_layout(m_cr, layout);
        g_object_unref(layout);
    }

    PlatBitmap CreateBitmap(int width, int height, const uint8_t* rgbaPixels) override {
        if (!rgbaPixels || width <= 0 || height <= 0) return nullptr;
        // Cairo expects ARGB32 (native-endian premultiplied); input is PBGRA from stb_image
        // swizzle (which already produces B,G,R,A order). On little-endian, Cairo's ARGB32
        // stores as B,G,R,A in memory — so PBGRA and Cairo ARGB32 are the same layout.
        int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
        unsigned char* data = (unsigned char*)malloc(stride * height);
        for (int row = 0; row < height; row++) {
            memcpy(data + row * stride, rgbaPixels + row * width * 4,
                   std::min((int)(width * 4), stride));
        }
        cairo_surface_t* surf = cairo_image_surface_create_for_data(
            data, CAIRO_FORMAT_ARGB32, width, height, stride);
        // Cairo doesn't own the data; attach it so it's freed when the surface dies.
        static cairo_user_data_key_t key;
        cairo_surface_set_user_data(surf, &key, data, free);
        return (PlatBitmap)surf;
    }
    void ReleaseBitmap(PlatBitmap bmp) override {
        if (bmp) cairo_surface_destroy((cairo_surface_t*)bmp);
    }
    void DrawBitmap(PlatBitmap bmp, float x, float y, float w, float h) override {
        if (!m_cr || !bmp) return;
        cairo_surface_t* surf = (cairo_surface_t*)bmp;
        int iw = cairo_image_surface_get_width(surf);
        int ih = cairo_image_surface_get_height(surf);
        if (iw <= 0 || ih <= 0) return;
        cairo_save(m_cr);
        cairo_translate(m_cr, x, y);
        cairo_scale(m_cr, w / iw, h / ih);
        cairo_set_source_surface(m_cr, surf, 0, 0);
        cairo_paint(m_cr);
        cairo_restore(m_cr);
    }
    void DrawBitmapTiled(PlatBitmap bmp, float destX, float destY, float destW, float destH,
                         float tileW, float tileH, float offsetX, float offsetY,
                         bool repeatX, bool repeatY) override {
        if (!m_cr || !bmp) return;
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
    GtkWidget* m_widget = nullptr;
    cairo_t* m_cr = nullptr;
    int m_width = 800, m_height = 600;
};

std::unique_ptr<IPlatformRenderer> CreatePlatformRenderer() {
    return std::make_unique<LinuxRenderer>();
}

#endif // __linux__
