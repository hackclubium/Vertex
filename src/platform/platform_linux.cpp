#ifdef __linux__
//
// platform_linux.cpp — Linux backend: hand-rolled rasterizer + font engine.
//
// Phase 3 of the Linux windowing rewrite: text now goes through Vertex's own
// TrueType parser + glyph rasterizer (src/font/) instead of Pango, cutting
// the last Cairo/Pango/fontconfig dependency from the main renderer (Cairo
// is still linked for <canvas>'s own backend, deferred to phase 4). Font
// discovery replaces fontconfig with a directory scan — see
// BuildFontIndex() below.
//
#include "platform/platform.h"
#include "platform/linux_font_registry.h"
#include "render/rasterizer.h"
#include "font/font_face.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

// PlatBitmap backing store. main_linux.cpp's image pipeline already
// premultiplies + swizzles decoded pixels to BGRA before calling
// CreateBitmap (historically to match Cairo's ARGB32 layout) — the same
// layout rasterizer.h's BlitBitmap expects, so no conversion is needed here.
struct RasterBitmap {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;
};

// PlatFont backing store: a lightweight descriptor (mirrors what
// PangoFontDescription used to be) — the actual parsed font file is
// resolved+cached separately (see GetFontFace below), matching how Pango
// itself kept a font map cache distinct from the lightweight description.
struct PlatFontDesc {
    float size = 12.f;
    bool bold = false, italic = false, mono = false;
    std::string family;
};

// ── font discovery (replaces fontconfig) ─────────────────────────────────────
// Scans the standard Linux font directories once, indexing each TrueType
// file's family/subfamily (read straight from its own 'name' table) so
// CreateFont's (family, bold, italic) request can be resolved to a file
// path without a fontconfig database.

namespace {

struct FontIndexEntry {
    std::string path;
    std::string familyLower;
    bool bold = false;
    bool italic = false;
};

std::vector<FontIndexEntry> g_fontIndex;
bool g_fontIndexBuilt = false;

std::string ToLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (char)std::tolower((unsigned char)c);
    return out;
}

void ScanFontDir(const std::string& dir, std::vector<std::string>& outFiles) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc) continue;
        std::string path = it->path().string();
        std::string lower = ToLower(path);
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".ttf") == 0)
            outFiles.push_back(path);
    }
}

void BuildFontIndex() {
    if (g_fontIndexBuilt) return;
    g_fontIndexBuilt = true;

    std::vector<std::string> files;
    ScanFontDir("/usr/share/fonts", files);
    ScanFontDir("/usr/local/share/fonts", files);
    if (const char* home = getenv("HOME")) {
        ScanFontDir(std::string(home) + "/.fonts", files);
        ScanFontDir(std::string(home) + "/.local/share/fonts", files);
    }

    for (const auto& path : files) {
        ttf::Font probe;
        if (!probe.LoadFromFile(path)) continue;  // CFF/OTF, TTC, or corrupt — skip
        if (probe.FamilyName().empty()) continue;
        std::string subLower = ToLower(probe.SubfamilyName());
        FontIndexEntry entry;
        entry.path = path;
        entry.familyLower = ToLower(probe.FamilyName());
        entry.bold = subLower.find("bold") != std::string::npos;
        entry.italic = subLower.find("italic") != std::string::npos || subLower.find("oblique") != std::string::npos;
        g_fontIndex.push_back(std::move(entry));
    }
}

const FontIndexEntry* BestMatchForFamily(const std::string& familyLower, bool bold, bool italic) {
    const FontIndexEntry* best = nullptr;
    int bestScore = -1;
    for (const auto& e : g_fontIndex) {
        if (e.familyLower != familyLower) continue;
        int score = (e.bold == bold ? 2 : 0) + (e.italic == italic ? 1 : 0);
        if (score > bestScore) { bestScore = score; best = &e; }
    }
    return best;
}

// Resolves a CreateFont() request to a concrete font file. `family` is
// whatever the CSS cascade produced (src/css/stylesheet.cpp's font-family
// handler already maps generic/Windows-flavored names like "sans-serif" ->
// "Segoe UI" before this is ever called) — since none of those literal
// names are installed on Linux, this always falls through to the DejaVu
// family Vertex ships alongside every major distro's font set, the same
// "at least render legible text" fallback Pango/fontconfig would have had
// to do for the same unmatched names.
std::string FindFontFile(const std::string& family, bool bold, bool italic, bool mono) {
    BuildFontIndex();
    if (!family.empty()) {
        if (const FontIndexEntry* e = BestMatchForFamily(ToLower(family), bold, italic)) return e->path;
    }
    std::string fallback = mono ? "dejavu sans mono" : "dejavu sans";
    if (const FontIndexEntry* e = BestMatchForFamily(fallback, bold, italic)) return e->path;
    // Last resort: any font at all, ignoring bold/italic/family entirely.
    return g_fontIndex.empty() ? std::string() : g_fontIndex.front().path;
}

std::map<std::string, std::shared_ptr<FontFace>> g_fontFaceCache;

std::shared_ptr<FontFace> GetFontFace(const std::string& path) {
    if (path.empty()) return nullptr;
    auto it = g_fontFaceCache.find(path);
    if (it != g_fontFaceCache.end()) return it->second;
    auto face = std::make_shared<FontFace>();
    if (!face->Load(path)) { g_fontFaceCache[path] = nullptr; return nullptr; }
    g_fontFaceCache[path] = face;
    return face;
}

}  // namespace

void RegisterLinuxWebFont(const std::string& path) {
    BuildFontIndex();  // ensure the static scan has already happened first
    ttf::Font probe;
    if (!probe.LoadFromFile(path) || probe.FamilyName().empty()) return;
    std::string subLower = ToLower(probe.SubfamilyName());
    FontIndexEntry entry;
    entry.path = path;
    entry.familyLower = ToLower(probe.FamilyName());
    entry.bold = subLower.find("bold") != std::string::npos;
    entry.italic = subLower.find("italic") != std::string::npos || subLower.find("oblique") != std::string::npos;
    g_fontIndex.push_back(std::move(entry));
}

// ── Rasterizer + from-scratch font engine renderer ───────────────────────────

class LinuxRenderer : public IPlatformRenderer {
public:
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
        return (PlatFont)new PlatFontDesc{size, bold, italic, mono, family};
    }
    void ReleaseFont(PlatFont font) override { delete (PlatFontDesc*)font; }

    float MeasureText(const std::wstring& text, PlatFont font) override {
        auto* desc = (PlatFontDesc*)font;
        if (!desc || text.empty()) return 0.f;
        auto face = FaceFor(desc);
        if (!face) return 0.f;
        float w = 0.f;
        for (wchar_t ch : text) w += face->AdvanceWidth((uint32_t)ch, desc->size);
        return w;
    }
    float SpaceWidth(PlatFont font) override { return MeasureText(L" ", font); }
    float FontHeight(PlatFont font) override {
        auto* desc = (PlatFontDesc*)font;
        if (!desc) return 16.f;
        auto face = FaceFor(desc);
        return face ? face->LineHeight(desc->size) : desc->size * 1.2f;
    }
    void DrawText(const std::wstring& text, float x, float y, float maxW, float maxH,
                  PlatFont font, PlatColor color, bool underline) override {
        (void)maxH;
        auto* desc = (PlatFontDesc*)font;
        if (!m_fb || !desc || text.empty()) return;
        auto face = FaceFor(desc);
        if (!face) return;

        raster::ClipRect clip = CurrentClip();
        float penX = x;
        float baselineY = y + face->Ascent(desc->size);
        for (wchar_t wc : text) {
            if (maxW > 0.f && penX > x + maxW) break;  // no wrapping — layout already positions single lines
            uint32_t cp = (uint32_t)wc;
            auto contours = face->RenderGlyph(cp, desc->size);
            for (auto& contour : contours) {
                for (auto& pt : contour) { pt.x += penX; pt.y += baselineY; }
            }
            if (!contours.empty()) raster::FillPath(*m_fb, contours, color, clip);
            penX += face->AdvanceWidth(cp, desc->size);
        }
        if (underline) {
            float uy = baselineY + std::max(1.f, desc->size * 0.08f);
            raster::StrokeLine(*m_fb, x, uy, penX, uy, color, 1.f, clip);
        }
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

    raster::ClipRect CurrentClip() const {
        if (!m_clipStack.empty()) return m_clipStack.back();
        return raster::ClipRect{0, 0, m_width, m_height};
    }

    static std::shared_ptr<FontFace> FaceFor(const PlatFontDesc* desc) {
        std::string path = FindFontFile(desc->family, desc->bold, desc->italic, desc->mono);
        return GetFontFace(path);
    }
};

std::unique_ptr<IPlatformRenderer> CreatePlatformRenderer() {
    return std::make_unique<LinuxRenderer>();
}

#endif // __linux__
