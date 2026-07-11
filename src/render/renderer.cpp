#include "render/renderer.h"
#include "layout/layout_engine.h"
#include "css/stylesheet.h"
#include "render/webfont.h"
#include "render/transition.h"
#include "render/animation.h"
#include "network/url.h"
#include "platform/chrome_theme.h"
#include "render/svg.h"
#include "render/canvas_renderer.h"
#include "codec/png.h"
#include "codec/jpeg.h"
#include "codec/webp.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#include <d2d1.h>
#include <dwrite.h>
#include <sstream>
#include <algorithm>
#include <functional>
#include <cmath>
#include <cwchar>
#include <cctype>
#include <chrono>

// ─── helpers ─────────────────────────────────────────────────────────────────

// Records a paint-time exception so a malformed page's failure mode is
// diagnosable instead of just silently blanking the content area. The old
// hardcoded "C:/tmp/vertex_crash.txt" doesn't exist on a normal Windows
// install, so fopen() failed silently and nothing was ever actually logged.
static void LogPaintCrash(const char* message) {
    char tempDir[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, tempDir);
    if (len == 0 || len >= MAX_PATH) return;
    std::string path = std::string(tempDir) + "vertex_crash.txt";
    FILE* f = fopen(path.c_str(), "a");
    if (!f) return;
    fprintf(f, "%s\n", message);
    fclose(f);
}

static D2D1_COLOR_F ToD2D(const CssColor& c) { return { c.r, c.g, c.b, c.a }; }
static D2D1_COLOR_F ThemeColor(vertex::chrome_theme::Rgb c, float a = 1.f) {
    return D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, a);
}
static constexpr float kMarginX = 32.f;
static constexpr float kMarginY =  8.f;

static bool HasUrlScheme(const std::string& url) {
    size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    size_t stop = url.find_first_of("/?#");
    if (stop != std::string::npos && stop < colon) return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = url[i];
        if (!std::isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
            return false;
    }
    return true;
}

static bool LooksLikeImageUrl(const std::string& url) {
    std::string low;
    for (char c : url) low += (char)std::tolower((unsigned char)c);
    return low.rfind("data:image/", 0) == 0
        || low.find(".png") != std::string::npos
        || low.find(".jpg") != std::string::npos
        || low.find(".jpeg") != std::string::npos
        || low.find(".gif") != std::string::npos
        || low.find(".webp") != std::string::npos
        || low.find(".bmp") != std::string::npos
        || low.find(".svg") != std::string::npos;
}

static bool LooksLikeSvgUrl(const std::string& url) {
    std::string low;
    for (char c : url) low += (char)std::tolower((unsigned char)c);
    return low.find(".svg") != std::string::npos
        || low.find("image/svg+xml") != std::string::npos;
}

static bool IsBreakableWhitespace(wchar_t c) {
    return c != 0x00A0 && iswspace(c);
}

std::string Renderer::ResolveUrl(const std::string& href, const std::string& base) {
    if (href.empty()) return base;
    if (HasUrlScheme(href)) return href;
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/')
        return "https:" + href;
    if (href[0] == '/') {
        size_t p = base.find("://");
        if (p == std::string::npos) return href;
        size_t sl = base.find('/', p + 3);
        return (sl == std::string::npos ? base : base.substr(0, sl)) + href;
    }
    size_t last = base.rfind('/');
    return (last == std::string::npos ? base : base.substr(0, last + 1)) + href;
}

ID2D1SolidColorBrush* Renderer::TempBrush(D2D1_COLOR_F color) {
    auto clampByte = [](float v) -> unsigned int {
        v = std::max(0.f, std::min(1.f, v));
        return (unsigned int)(v * 255.f + 0.5f);
    };
    unsigned int key =
        (clampByte(color.a) << 24)
        | (clampByte(color.r) << 16)
        | (clampByte(color.g) << 8)
        | clampByte(color.b);
    auto cached = m_tempBrushCache.find(key);
    if (cached != m_tempBrushCache.end()) return cached->second;

    ID2D1SolidColorBrush* b = nullptr;
    if (m_rt) m_rt->CreateSolidColorBrush(color, &b);
    if (b) {
        m_tempBrushes.push_back(b);
        m_tempBrushCache[key] = b;
    }
    return b;
}

// ─── init / teardown ─────────────────────────────────────────────────────────


void Renderer::CreateTabFont() {
    if (m_fmtTab) { m_fmtTab->Release(); m_fmtTab = nullptr; }
    if (m_fmtTabClose) { m_fmtTabClose->Release(); m_fmtTabClose = nullptr; }
    if (m_fmtTabPlus) { m_fmtTabPlus->Release(); m_fmtTabPlus = nullptr; }
    if (!m_dwrite) return;
    m_dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.f, L"en-us", &m_fmtTab);
    if (m_fmtTab) {
        m_fmtTab->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_fmtTab->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_fmtTab->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    m_dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.f, L"en-us", &m_fmtTabClose);
    if (m_fmtTabClose) {
        m_fmtTabClose->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_fmtTabClose->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    m_dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.f, L"en-us", &m_fmtTabPlus);
    if (m_fmtTabPlus) {
        m_fmtTabPlus->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_fmtTabPlus->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

bool Renderer::Init(HWND hwnd) {
    m_hwnd = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_factory)))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_dwrite))))
        return false;
    // WIC removed — stb_image handles image decoding (cross-platform).
    CreateTabFont();
    return EnsureTarget();
}

bool Renderer::EnsureTarget() {
    if (m_rt) return true;
    RECT rc; GetClientRect(m_hwnd, &rc);
    m_width  = (UINT)(rc.right  - rc.left);
    m_height = (UINT)(rc.bottom - rc.top);
    // Force 96 DPI so Direct2D DIPs == physical pixels. The layout engine works
    // in physical pixels (m_width/m_height from GetClientRect), so the render
    // target must use the same coordinate space. Otherwise on a high-DPI display
    // (e.g. 125% scaling) the render target would be in scaled DIPs while layout
    // is in physical px, and everything centered in layout space drifts off.
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
        96.f, 96.f);
    if (FAILED(m_factory->CreateHwndRenderTarget(
            rtProps,
            D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(m_width, m_height)),
            &m_rt)))
        return false;
    return CreateBrushes();
}

bool Renderer::CreateBrushes() {
    auto mk = [&](D2D1_COLOR_F c, ID2D1SolidColorBrush** b) {
        return SUCCEEDED(m_rt->CreateSolidColorBrush(c, b));
    };
    return mk({0,0,0,1},             &m_textBrush)
        && mk({.1f,.1f,.8f,1},       &m_linkBrush)
        && mk(ThemeColor(vertex::chrome_theme::Panel), &m_bgBrush)
        && mk(ThemeColor(vertex::chrome_theme::Line), &m_hrBrush)
        && mk({.96f,.96f,.96f,1},    &m_codeBgBrush)
        && mk({1.f,.949f,.463f,1},   &m_findBrush)
        && mk({.8f,.8f,.8f,1},       &m_quoteBrush)
        && mk(ThemeColor(vertex::chrome_theme::Rail), &m_tabBgBrush)
        && mk(ThemeColor(vertex::chrome_theme::Active), &m_tabActBrush)
        && mk(ThemeColor(vertex::chrome_theme::Panel), &m_tabInaBrush)
        && mk(ThemeColor(vertex::chrome_theme::Ink), &m_tabTxtBrush)
        && mk(ThemeColor(vertex::chrome_theme::Quiet), &m_tabClsBrush);
}

void Renderer::ReleaseBrushes() {
    auto r = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    r(m_textBrush); r(m_linkBrush); r(m_bgBrush); r(m_hrBrush);
    r(m_codeBgBrush); r(m_findBrush); r(m_quoteBrush);
    r(m_tabBgBrush); r(m_tabActBrush); r(m_tabInaBrush);
    r(m_tabTxtBrush); r(m_tabClsBrush);
}

void Renderer::ReleaseTarget() {
    ReleaseBrushes();
    for (auto* b : m_tempBrushes) if (b) b->Release();
    m_tempBrushes.clear();
    m_tempBrushCache.clear();
    for (auto& [url, bmp] : m_images) if (bmp) bmp->Release();
    m_images.clear();
    // Off-screen canvas surfaces are compatible render targets created
    // against m_rt's device — they can't outlive it, so drop them here too.
    // GetOrCreateCanvasSurface() lazily recreates on the next draw call.
    m_canvasSurfaces.clear();
    if (m_rt) { m_rt->Release(); m_rt = nullptr; }
}

void Renderer::DiscardTarget() { ReleaseTarget(); }

Renderer::~Renderer() {
    ReleaseTarget();
    auto r = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    r(m_fmtTab);
    r(m_fmtTabClose);
    r(m_fmtTabPlus);
    for (auto& [k, f] : m_fmtCache) if (f) f->Release();
    m_fmtCache.clear();
    r(m_dwrite); r(m_factory);
}

void Renderer::Resize(UINT w, UINT h) {
    m_width = w; m_height = h;
    if (m_rt) m_rt->Resize(D2D1::SizeU(w, h));
}

void Renderer::SetPaintDirtyRect(const RECT& rect) {
    m_hasPaintDirtyRect = rect.right > rect.left && rect.bottom > rect.top;
    if (!m_hasPaintDirtyRect) {
        m_paintDirtyTop = 0.f;
        m_paintDirtyBottom = (float)m_height;
        return;
    }
    m_paintDirtyTop = std::max(0.f, (float)rect.top);
    m_paintDirtyBottom = std::min((float)m_height, (float)rect.bottom);
    if (m_paintDirtyBottom < m_paintDirtyTop)
        std::swap(m_paintDirtyTop, m_paintDirtyBottom);
}

void Renderer::InvalidateLayout() {
    m_layoutRoot.reset();
    m_layoutBaseStyles.clear();
    m_layoutDocKey = nullptr;
    m_lastHoverNodeValid = false;
    m_lastHoverNode = nullptr;
    m_styleDocKey = nullptr;
    m_styleBaseUrlKey.clear();
    m_cachedSheet = Stylesheet{};
    m_cachedPageBg = CssColor{};
    m_cachedUsesHoverStyles = false;
    m_cachedHoverAffectsLayout = false;
    m_cachedHoverRestylesSubtree = false;
    m_layoutBoxesByNode.clear();
}

void Renderer::SetZoom(float z) {
    m_zoom = std::max(0.5f, std::min(3.f, z));
    // Cached text formats/measurements are size-dependent — invalidate them.
    for (auto& [k, f] : m_fmtCache) if (f) f->Release();
    m_fmtCache.clear();
    m_measureCache.clear();
}

// ─── image loading ────────────────────────────────────────────────────────────

void Renderer::ReceiveImage(const std::string& url, const std::vector<uint8_t>& bytes) {
    // Every completion, including fetch and decoder failures, must release the
    // in-flight slot. Otherwise one bad URL is stuck "loading" forever.
    m_loadingImages.erase(url);
    auto fail = [&]() { m_failedImages.insert(url); };
    if (!m_rt || bytes.empty()) { fail(); return; }

    // Decode external SVGs through Vertex's own SVG rasterizer before falling
    // back to hand-made PNG/JPEG decoders for raster formats.
    int w = 0, h = 0;
    std::vector<uint8_t> svgPixels;
    std::vector<uint8_t> decodedPixels;
    uint8_t* pixels = nullptr;
    if (LooksLikeSvgUrl(url) || svg::looksLikeSvgBytes(bytes)) {
        auto bmp = svg::renderSvgBytes(bytes, svg::SvgRasterMaxDimForBytes(bytes.size()));
        if (bmp.width > 0 && bmp.height > 0 && !bmp.pixels.empty()) {
            w = bmp.width;
            h = bmp.height;
            svgPixels = std::move(bmp.pixels);
            pixels = svgPixels.data();
        }
    }
    if (!pixels) {
        // Try project codecs only; WebP path is portable and hand-rolled.
        DecodedImage img = DecodePng(bytes.data(), bytes.size());
        if (!img.success) {
            img = DecodeJpeg(bytes.data(), bytes.size());
        }
        if (!img.success) {
            img = DecodeWebp(bytes.data(), bytes.size());
        }
        if (img.success && img.width > 0 && img.height > 0 && !img.rgba.empty()) {
            w = img.width;
            h = img.height;
            decodedPixels = std::move(img.rgba);
            pixels = decodedPixels.data();
        }
    }
    if (!pixels || w <= 0 || h <= 0) { fail(); return; }

    // stb_image outputs RGBA; Direct2D wants PBGRA (pre-multiplied, swizzled).
    for (int i = 0; i < w * h; ++i) {
        unsigned char* p = pixels + i * 4;
        unsigned char r = p[0], g = p[1], b = p[2], a = p[3];
        float af = a / 255.f;
        p[0] = (unsigned char)(b * af + 0.5f);  // B
        p[1] = (unsigned char)(g * af + 0.5f);  // G
        p[2] = (unsigned char)(r * af + 0.5f);  // R
        p[3] = a;                                 // A
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap* bmp = nullptr;
    HRESULT hr = m_rt->CreateBitmap(
        D2D1::SizeU((UINT32)w, (UINT32)h), pixels, (UINT32)(w * 4), props, &bmp);

    if (SUCCEEDED(hr) && bmp) {
        auto it = m_images.find(url);
        if (it != m_images.end() && it->second) it->second->Release();
        const bool affectsLayout = ImageDecodeAffectsLayout(url);
        m_images[url] = bmp;
        if (affectsLayout) {
            InvalidateLayout();
        } else if (m_hwnd) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        m_failedImages.erase(url);
    } else {
        fail();
    }
}

// ─── tab strip ───────────────────────────────────────────────────────────────

void Renderer::DrawTabStrip(const std::vector<TabEntry>& tabs, float h) {
    if (!m_rt || tabs.empty() || h < 4.f) return;
    m_tabHits.clear();

    float w      = (float)m_width;
    float newBtnW= h - 8.f;
    float railPad= 6.f;
    float avail  = w - newBtnW - railPad * 3.f;
    int   n      = (int)tabs.size();
    float tabW   = std::min(210.f, std::max(92.f, avail / (float)n));
    float tabH   = h - 7.f;
    float closeW = 20.f, pad = 12.f;
    float x      = railPad;
    auto accent = TempBrush(ThemeColor(vertex::chrome_theme::Accent));
    auto inactiveText = TempBrush(ThemeColor(vertex::chrome_theme::Quiet));
    auto plusStroke = TempBrush(ThemeColor(vertex::chrome_theme::Line));

    m_rt->FillRectangle(D2D1::RectF(0, 0, w, h), m_tabBgBrush);

    for (int i = 0; i < n; i++) {
        const auto& t = tabs[i];
        auto* fill = t.active ? m_tabActBrush : m_tabInaBrush;
        D2D1_ROUNDED_RECT tabRect = D2D1::RoundedRect(
            D2D1::RectF(x, 5.f, x + tabW - 4.f, 5.f + tabH), 7.f, 7.f);
        m_rt->FillRoundedRectangle(tabRect, fill);
        if (t.active) {
            m_rt->DrawRoundedRectangle(tabRect, m_hrBrush, 1.f);
            if (accent)
                m_rt->FillRectangle(D2D1::RectF(x + 10.f, 5.f, x + tabW - 18.f, 8.f), accent);
        }

        if (t.loading && accent) {
            float dot = x + 10.f;
            m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dot, 5.f + tabH * 0.5f), 3.f, 3.f), accent);
            float barLeft = x + 10.f;
            float barRight = x + tabW - 18.f;
            float barW = std::max(16.f, barRight - barLeft);
            float segW = std::min(54.f, std::max(18.f, barW * 0.34f));
            float travel = std::max(1.f, barW - segW);
            float phase = t.loadingProgress - std::floor(t.loadingProgress);
            float segX = barLeft + travel * phase;
            m_rt->FillRectangle(D2D1::RectF(segX, 5.f + tabH - 3.f, segX + segW, 5.f + tabH - 1.f), accent);
        }

        float textX = x + pad + (t.loading ? 10.f : 0.f);
        float textW = tabW - pad * 2 - closeW - (t.loading ? 10.f : 0.f);
        if (textW > 10.f && m_fmtTab) {
            std::wstring title = t.loading ? L"Loading..." : t.title;
            if (title.empty()) title = L"New Tab";
            m_rt->DrawText(title.c_str(), (UINT32)title.size(), m_fmtTab,
                D2D1::RectF(textX, 5.f, textX + textW, 5.f + tabH),
                t.active ? m_tabTxtBrush : inactiveText, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        float cx = x + tabW - closeW - 7.f, cy = 5.f + (tabH - 18.f) / 2.f;
        IDWriteTextFormat* closeFormat = m_fmtTabClose ? m_fmtTabClose : m_fmtTab;
        if (closeFormat)
            m_rt->DrawText(L"\x00D7", 1, closeFormat, D2D1::RectF(cx, cy - 2.f, cx + closeW, cy + 18.f), m_tabClsBrush);

        m_tabHits.push_back({ x, 5.f, tabW - 4.f, tabH, i, false });
        m_tabHits.push_back({ cx, cy, closeW, 18.f, i, true });
        x += tabW;
    }

    float nx = x + railPad;
    D2D1_ROUNDED_RECT newRect = D2D1::RoundedRect(
        D2D1::RectF(nx, 6.f, nx + newBtnW, 6.f + newBtnW),
        (float)vertex::chrome_theme::CornerRadius,
        (float)vertex::chrome_theme::CornerRadius);
    m_rt->FillRoundedRectangle(newRect, m_tabInaBrush);
    if (plusStroke) m_rt->DrawRoundedRectangle(newRect, plusStroke, 1.f);
    IDWriteTextFormat* plusFormat = m_fmtTabPlus ? m_fmtTabPlus : m_fmtTab;
    if (plusFormat)
        m_rt->DrawText(L"+", 1, plusFormat, D2D1::RectF(nx, 5.f, nx + newBtnW, 6.f + newBtnW), m_tabTxtBrush);
    m_tabHits.push_back({ nx, 6.f, newBtnW, newBtnW, -1, false });

    m_rt->DrawLine(D2D1::Point2F(0, h - 1.f), D2D1::Point2F(w, h - 1.f), m_hrBrush, 1.f);
}

int Renderer::HitTestTab(float x, float y) const {
    for (auto it = m_tabHits.rbegin(); it != m_tabHits.rend(); ++it)
        if (!it->isClose && x >= it->x && x < it->x + it->w
                         && y >= it->y && y < it->y + it->h)
            return it->idx;
    return -2;
}

bool Renderer::HitTestTabClose(float x, float y, int& outIdx) const {
    for (auto it = m_tabHits.rbegin(); it != m_tabHits.rend(); ++it)
        if (it->isClose && x >= it->x && x < it->x + it->w
                        && y >= it->y && y < it->y + it->h) {
            outIdx = it->idx; return true;
        }
    return false;
}


// ─── stylesheet collection + body bg ─────────────────────────────────────────

static std::string UrlDecodeSimple(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                c |= 0x20; if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                return 0;
            };
            out += (char)(hex(s[i+1]) * 16 + hex(s[i+2])); i += 2;
        } else { out += s[i]; }
    }
    return out;
}

static bool SelectorPartUsesHover(const CssSelectorPart& part) {
    for (const auto& pseudo : part.pseudos)
        if (pseudo == "hover") return true;
    for (const auto& list : part.notSelectorLists)
        for (const auto& selector : list)
            if (selector.find(":hover") != std::string::npos) return true;
    for (const auto& list : part.matchSelectorLists)
        for (const auto& selector : list)
            if (selector.find(":hover") != std::string::npos) return true;
    for (const auto& list : part.hasSelectorLists)
        for (const auto& selector : list)
            if (selector.find(":hover") != std::string::npos) return true;
    return false;
}

static bool StylesheetUsesHover(const Stylesheet& sheet) {
    for (const auto& rule : sheet.rules)
        for (const auto& part : rule.selector)
            if (SelectorPartUsesHover(part)) return true;
    return false;
}

static bool RuleUsesHover(const CssRule& rule) {
    for (const auto& part : rule.selector)
        if (SelectorPartUsesHover(part)) return true;
    return false;
}

static bool StyleMayAffectLayout(const ComputedStyle& s) {
    return s.display != 0
        || s.marginTopSet() || s.marginRightSet() || s.marginBottomSet() || s.marginLeftSet()
        || s.paddingTop >= 0 || s.paddingRight >= 0 || s.paddingBottom >= 0 || s.paddingLeft >= 0
        || s.borderWidth >= 0 || s.borderTopWidth >= 0 || s.borderRightWidth >= 0
        || s.borderBottomWidth >= 0 || s.borderLeftWidth >= 0
        || s.fontSize > 0 || !s.fontFamily.empty() || s.boldSet || s.italicSet
        || s.lineHeight > 0 || s.textAlignSet || s.textIndentSet || s.verticalAlignSet
        || s.textTransformSet || s.whiteSpaceSet || s.letterSpacingSet || s.wordBreakSet
        || s.columnCountSet || s.aspectRatioSet
        || s.width >= 0 || s.widthPercent >= 0 || s.widthCalcPercent >= 0
        || s.height >= 0 || s.heightPercent >= 0
        || s.maxWidth >= 0 || s.maxWidthPercent >= 0 || s.minWidth >= 0 || s.minWidthPercent >= 0
        || s.minHeight >= 0 || s.minHeightPercent >= 0 || s.maxHeight >= 0 || s.maxHeightPercent >= 0
        || s.contentSet || s.floatMode != 0 || s.clearMode != 0 || s.positionMode != 0
        || s.zIndexSet || s.overflowSet || s.topSet || s.rightSet || s.bottomSet || s.leftSet
        || s.widthKeyword != 0 || s.heightKeyword != 0 || s.boxSizingSet
        || s.flexDirectionSet || s.flexGrowSet || s.flexShrinkSet || s.flexBasisSet
        || s.flexWrapSet || s.alignSelfSet || s.flexGap >= 0 || s.alignItemsSet
        || s.justifyContentSet || s.gridTemplateColumnsSet || s.gridTemplateRowsSet
        || s.gridColumnStart != 0 || s.gridColumnEnd != 0 || s.gridRowStart != 0 || s.gridRowEnd != 0;
}

static bool StylesheetHoverAffectsLayout(const Stylesheet& sheet) {
    for (const auto& rule : sheet.rules) {
        if (RuleUsesHover(rule) && StyleMayAffectLayout(rule.style))
            return true;
    }
    return false;
}

static bool StylesheetHoverRestylesSubtree(const Stylesheet& sheet) {
    for (const auto& rule : sheet.rules) {
        for (size_t i = 0; i < rule.selector.size(); ++i) {
            if (SelectorPartUsesHover(rule.selector[i]) && i + 1 < rule.selector.size())
                return true;
        }
    }
    return false;
}

bool Renderer::UsesHoverStyles() const {
    return m_cachedUsesHoverStyles;
}

void Renderer::SetPrefersDarkScheme(bool dark) {
    if (m_prefersDarkScheme == dark) return;
    m_prefersDarkScheme = dark;
    InvalidateLayout();
    m_styleDocKey = nullptr;
}

Stylesheet Renderer::CollectStylesheet(const Node* root) {
    Stylesheet sheet;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tagName == "style") {
            std::string css;
            for (auto& c : n->children)
                if (c->type == NodeType::Text) css += c->text;
            Stylesheet part = ParseStylesheet(css);
            if (part.rootRemBaseSet) {
                sheet.rootRemBase = part.rootRemBase;
                sheet.rootRemBaseSet = true;
            }
            for (auto& r : part.rules) sheet.rules.push_back(r);
        } else if (n->type == NodeType::Element && n->tagName == "link") {
            std::string rel = n->attr("rel");
            std::string relLow; for (char c : rel) relLow += (char)std::tolower((unsigned char)c);
            if (relLow.find("stylesheet") != std::string::npos) {
                std::string href = n->attr("href");
                std::string hrefLow; for (char c : href) hrefLow += (char)std::tolower((unsigned char)c);
                const std::string prefix = "data:text/css,";
                if (hrefLow.rfind(prefix, 0) == 0) {
                    std::string css = UrlDecodeSimple(href.substr(prefix.size()));
                    Stylesheet part = ParseStylesheet(css);
                    if (part.rootRemBaseSet) {
                        sheet.rootRemBase = part.rootRemBase;
                        sheet.rootRemBaseSet = true;
                    }
                    for (auto& r : part.rules) sheet.rules.push_back(r);
                }
            }
        }
        for (auto& c : n->children) walk(c.get());
    };
    walk(root);
    sheet.rebuildRuleBuckets();
    return sheet;
}

void Renderer::CaptureLayoutBaseStyles(LayoutBox& box) {
    m_layoutBaseStyles[&box] = box.style;
    if (box.node)
        m_layoutBoxesByNode[box.node].push_back(&box);
    for (const auto& child : box.kids)
        CaptureLayoutBaseStyles(*child);
}

void Renderer::ApplyPaintOnlyHoverStyles(LayoutBox& box, const Stylesheet& sheet) {
    auto base = m_layoutBaseStyles.find(&box);
    if (base != m_layoutBaseStyles.end())
        box.style = base->second;

    if (box.node && box.node->type == NodeType::Element) {
        ComputedStyle resolved = sheet.resolve(box.node);
        ComputedStyle styled = box.style.inherit(resolved);
        ResolveStyleVariables(styled);
        box.style = styled;
    }

    for (auto& child : box.kids)
        ApplyPaintOnlyHoverStyles(*child, sheet);
}

void Renderer::ApplyPaintOnlyHoverStylesToChangedChain(const Stylesheet& sheet,
                                                       const Node* oldHover,
                                                       const Node* newHover) {
    std::set<LayoutBox*> touched;
    auto applyBox = [&](LayoutBox* box) {
        if (!box || !touched.insert(box).second) return;
        auto base = m_layoutBaseStyles.find(box);
        if (base != m_layoutBaseStyles.end())
            box->style = base->second;
        if (box->node && box->node->type == NodeType::Element) {
            ComputedStyle resolved = sheet.resolve(box->node);
            ComputedStyle styled = box->style.inherit(resolved);
            ResolveStyleVariables(styled);
            box->style = styled;
        }
    };
    auto applyChain = [&](const Node* hover) {
        for (const Node* cur = hover; cur; cur = cur->parent) {
            auto it = m_layoutBoxesByNode.find(cur);
            if (it == m_layoutBoxesByNode.end()) continue;
            for (LayoutBox* box : it->second)
                applyBox(box);
        }
    };
    applyChain(oldHover);
    applyChain(newHover);
}

void Renderer::RemoveHitRegionsInDirtyRect() {
    m_hits.erase(std::remove_if(m_hits.begin(), m_hits.end(),
        [&](const HitRegion& hit) {
            const float bottom = hit.y + hit.h;
            return bottom >= m_paintDirtyTop && hit.y <= m_paintDirtyBottom;
        }),
        m_hits.end());
    m_lastHitValid = false;
    m_lastHitHref.clear();
}

bool Renderer::ImageDecodeAffectsLayout(const std::string& url) const {
    if (!m_layoutRoot) return true;

    bool sawImageBox = false;
    std::vector<const LayoutBox*> stack;
    stack.push_back(m_layoutRoot.get());
    while (!stack.empty()) {
        const LayoutBox* box = stack.back();
        stack.pop_back();
        if (!box) continue;
        if (box->replacedUrl == url) {
            sawImageBox = true;
            if (box->intrinsicW <= 0.f || box->intrinsicH <= 0.f)
                return true;
        }
        for (const auto& child : box->kids)
            stack.push_back(child.get());
    }

    return !sawImageBox;
}

CssColor Renderer::FindBodyBgColor(const Node* root, const Stylesheet& sheet) {
    if (!root) return {};
    std::function<CssColor(const Node*)> find = [&](const Node* n) -> CssColor {
        if (!n) return {};
        if (n->type == NodeType::Element
            && (n->tagName == "body" || n->tagName == "html")) {
            auto cs = sheet.resolve(n);
            if (cs.bgColor.valid) return cs.bgColor;
        }
        for (auto& c : n->children) {
            auto bg = find(c.get());
            if (bg.valid) return bg;
        }
        return {};
    };
    return find(root);
}

// ─── public paint ─────────────────────────────────────────────────────────────

float Renderer::Paint(const std::shared_ptr<Node>& doc,
                      float scrollY,
                      const std::string& baseUrl,
                      float topInset,
                      float tabStripH,
                      const std::vector<TabEntry>* tabs,
                      bool repaintChrome)
{
    if (!EnsureTarget()) return 0.f;
    m_lastTimings = RendererTimings{};
    auto paintStart = std::chrono::steady_clock::now();
    if (!m_hasPaintDirtyRect) {
        m_paintDirtyTop = 0.f;
        m_paintDirtyBottom = (float)m_height;
    }
    auto resetPaintDirty = [&]() {
        m_hasPaintDirtyRect = false;
        m_paintDirtyTop = 0.f;
        m_paintDirtyBottom = (float)m_height;
    };

    for (auto* b : m_tempBrushes) if (b) b->Release();
    m_tempBrushes.clear();
    m_tempBrushCache.clear();
    m_lastHitValid = false;
    // m_anchorY is rebuilt only when the layout tree is rebuilt (see below).

    m_rt->BeginDraw();

    Stylesheet* sheet = nullptr;
    CssColor   pageBg;
    if (doc) {
        if (m_styleDocKey != doc.get() || m_styleBaseUrlKey != baseUrl) {
            auto styleStart = std::chrono::steady_clock::now();
            m_cachedSheet  = CollectStylesheet(doc.get());
            m_cachedSheet.setPreferredColorScheme(m_prefersDarkScheme);
            m_cachedPageBg = FindBodyBgColor(doc.get(), m_cachedSheet);
            m_cachedUsesHoverStyles = StylesheetUsesHover(m_cachedSheet);
            m_cachedHoverAffectsLayout = StylesheetHoverAffectsLayout(m_cachedSheet);
            m_cachedHoverRestylesSubtree = StylesheetHoverRestylesSubtree(m_cachedSheet);
            m_styleDocKey = doc.get();
            m_styleBaseUrlKey = baseUrl;
            if (!m_cachedSheet.fontFaces.empty())
                WebFontLoader::instance().loadFonts(m_cachedSheet, baseUrl, [this]() { InvalidateLayout(); });
            auto styleEnd = std::chrono::steady_clock::now();
            m_lastTimings.styleMs =
                std::chrono::duration<double, std::milli>(styleEnd - styleStart).count();
        }
        sheet = &m_cachedSheet;
        pageBg = m_cachedPageBg;
    }

    // transparent body bg → default white (HwndRenderTarget clear with a=0 shows window black)
    D2D1_COLOR_F bgF = (pageBg.valid && pageBg.a > 0.001f)
        ? ToD2D(pageBg)
        : D2D1::ColorF(1.f, 1.f, 1.f);
    {
        // Only fill the region actually being repainted this frame. A full
        // Clear() here used to wipe the whole render target to the page
        // background whenever repaintChrome was set (e.g. the URL bar frame
        // being invalidated on focus/click), even though the dirty rect —
        // and therefore the content repaint below — was confined to a thin
        // strip inside the toolbar. That left previously-painted content
        // erased with nothing redrawn over it: a blank page background.
        const float fillTop = repaintChrome ? m_paintDirtyTop : std::max(topInset, m_paintDirtyTop);
        const float fillBottom = std::min((float)m_height, m_paintDirtyBottom);
        if (fillBottom > fillTop)
            m_rt->FillRectangle(D2D1::RectF(0, fillTop, (float)m_width, fillBottom), TempBrush(bgF));
    }

    // Chrome area (toolbar + tab strip)
    if (repaintChrome && topInset > 0) {
        m_rt->FillRectangle(D2D1::RectF(0, 0, (float)m_width, topInset), m_bgBrush);
        if (tabStripH > 0) {
            m_rt->FillRectangle(D2D1::RectF(0, 0, (float)m_width, tabStripH), m_tabBgBrush);
            m_rt->DrawLine(D2D1::Point2F(0, topInset - 1.f), D2D1::Point2F((float)m_width, topInset - 1.f), m_hrBrush, 1.f);
        }
    }

    if (repaintChrome && tabs && !tabs->empty() && tabStripH > 0)
        DrawTabStrip(*tabs, tabStripH);

    if (doc) {
        if (m_imageDocKey != doc.get()) {
            m_imageDocKey = doc.get();
            m_failedImages.clear();
        }
        m_curBaseUrl = baseUrl;
        float docH = 0.f;
        // A malformed or pathological page must never take down the browser.
        try {
            // The render target draws in physical pixels (pinned to 96 DPI), and
            // the layout works in those same pixels. To make content render at the
            // right physical size on a high-DPI display (instead of tiny), fold the
            // monitor's DPI scale into the effective zoom: a 125% display lays out
            // 25% larger. Centering still holds because layout + paint share one
            // coordinate space (physical px).
            UINT dpi = m_hwnd ? GetDpiForWindow(m_hwnd) : 96;
            float dpiScale = (dpi >= 48) ? (float)dpi / 96.f : 1.f;
            float effZoom = m_zoom * dpiScale;
            // Rebuild the layout tree only when something that affects geometry
            // changed; scrolling reuses the cached tree.
            extern const Node* g_hoverNode;
            static const Node* prevHover = nullptr;
            bool hoverChanged = (g_hoverNode != prevHover);
            bool hoverMayAffectStyle = hoverChanged && sheet && m_cachedHoverAffectsLayout;
            std::map<const Node*, ComputedStyle> oldStyles;
            if (hoverMayAffectStyle && m_layoutRoot) {
                std::function<void(const LayoutBox&)> collect = [&](const LayoutBox& b) {
                    if (b.node && oldStyles.find(b.node) == oldStyles.end())
                        oldStyles.emplace(b.node, b.style);
                    for (const auto& k : b.kids) collect(*k);
                };
                collect(*m_layoutRoot);
            }
            SetCssHoverNode(g_hoverNode);
            if (hoverChanged && sheet && m_cachedUsesHoverStyles
                && !m_cachedHoverAffectsLayout && m_layoutRoot) {
                if (!m_cachedHoverRestylesSubtree)
                    ApplyPaintOnlyHoverStylesToChangedChain(*sheet, prevHover, g_hoverNode);
                else
                    ApplyPaintOnlyHoverStyles(*m_layoutRoot, *sheet);
            }
            bool reuse = m_layoutRoot
                      && m_layoutDocKey  == doc.get()
                      && m_layoutWKey    == m_width
                      && m_layoutHKey    == m_height
                      && m_layoutZoomKey == effZoom
                      && !AnimationManager::instance().hasActiveAnimations()
                      && !hoverMayAffectStyle;
            m_lastTimings.layoutReused = reuse;
            if (!reuse) {
                auto layoutStart = std::chrono::steady_clock::now();
                m_lastHoverNodeValid = false;
                m_lastHoverNode = nullptr;
                LayoutInput in;
                in.document  = doc.get();
                in.sheet     = sheet;
                in.measure   = this;
                in.viewportW = std::max(100.f, (float)m_width);
                in.viewportH = std::max(100.f, (float)m_height - topInset);
                in.zoom      = effZoom;
                in.baseUrl   = baseUrl;
                m_layoutRoot   = LayoutDocument(in);
                m_layoutDocKey = doc.get();
                m_layoutWKey   = m_width;
                m_layoutHKey   = m_height;
                m_layoutZoomKey= effZoom;
                m_anchorY.clear();
                if (m_layoutRoot) CollectAnchors(*m_layoutRoot);
                m_layoutBaseStyles.clear();
                m_layoutBoxesByNode.clear();
                if (m_layoutRoot) CaptureLayoutBaseStyles(*m_layoutRoot);
                if (m_layoutRoot && sheet) {
                    std::function<void(const LayoutBox&)> startAnimations = [&](const LayoutBox& b) {
                        if (b.node && b.style.animationSet)
                            AnimationManager::instance().ensureStarted(b.node, b.style, *sheet);
                        for (const auto& k : b.kids) startAnimations(*k);
                    };
                    startAnimations(*m_layoutRoot);
                }
                m_hoverCandidates.clear();
                if (m_layoutRoot) BuildHoverIndex(*m_layoutRoot);
                auto layoutEnd = std::chrono::steady_clock::now();
                m_lastTimings.layoutMs =
                    std::chrono::duration<double, std::milli>(layoutEnd - layoutStart).count();
            }
            if (hoverMayAffectStyle && m_layoutRoot && !oldStyles.empty()) {
                std::set<const Node*> transitioned;
                std::function<void(const LayoutBox&)> startTransitions = [&](const LayoutBox& b) {
                    if (b.node && transitioned.insert(b.node).second) {
                        auto it = oldStyles.find(b.node);
                        if (it != oldStyles.end())
                            TransitionManager::instance().onStyleChange(b.node, it->second, b.style);
                    }
                    for (const auto& k : b.kids) startTransitions(*k);
                };
                startTransitions(*m_layoutRoot);
            }
            const bool fullContentDirty = m_paintDirtyTop <= topInset
                && m_paintDirtyBottom >= (float)m_height;
            const bool rebuildHitRegions = !reuse
                || m_hitRegionScrollY != scrollY
                || fullContentDirty;
            if (rebuildHitRegions) {
                m_hits.clear();
                m_hitRegionScrollY = scrollY;
            } else {
                RemoveHitRegionsInDirtyRect();
            }
            prevHover = g_hoverNode;
            if (m_layoutRoot) {
                const float clipTop = std::max(topInset, m_paintDirtyTop);
                const float clipBottom = std::min((float)m_height, m_paintDirtyBottom);
                if (clipBottom > clipTop) {
                    m_rt->PushAxisAlignedClip(
                        D2D1::RectF(0, clipTop, (float)m_width, clipBottom),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    // PopAxisAlignedClip must run even if PaintBox throws — Direct2D
                    // requires every Push to be balanced by a Pop before EndDraw, or
                    // the render target's clip stack stays corrupted for every frame
                    // painted afterward (a single bad page can then blank out every
                    // page painted from then on, until the target is recreated).
                    try {
                        PaintBox(*m_layoutRoot, scrollY, topInset, false);
                    } catch (...) {
                        m_rt->PopAxisAlignedClip();
                        throw;
                    }
                    m_rt->PopAxisAlignedClip();
                }
                docH = m_layoutRoot->contentH + 32.f;
                // If transitions or animations are active, schedule another repaint.
                if ((TransitionManager::instance().hasActiveTransitions()
                     || AnimationManager::instance().hasActiveAnimations()) && m_hwnd)
                    InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        } catch (const std::exception& ex) {
            LogPaintCrash(("LAYOUT/PAINT exception: " + std::string(ex.what())).c_str());
        } catch (...) {
            LogPaintCrash("LAYOUT/PAINT unknown exception");
        }

        HRESULT hr = m_rt->EndDraw();
        auto paintEnd = std::chrono::steady_clock::now();
        m_lastTimings.paintMs =
            std::chrono::duration<double, std::milli>(paintEnd - paintStart).count();
        if (hr == D2DERR_RECREATE_TARGET) ReleaseTarget();
        resetPaintDirty();
        return docH;
    }

    m_hits.clear();
    m_hitRegionScrollY = -1.f;
    m_rt->EndDraw();
    auto paintEnd = std::chrono::steady_clock::now();
    m_lastTimings.paintMs =
        std::chrono::duration<double, std::milli>(paintEnd - paintStart).count();
    resetPaintDirty();
    return 0.f;
}

std::string Renderer::HitTest(float x, float y) const {
    if (m_lastHitValid
        && x >= m_lastHitRegion.x && x <= m_lastHitRegion.x + m_lastHitRegion.w
        && y >= m_lastHitRegion.y && y <= m_lastHitRegion.y + m_lastHitRegion.h) {
        return m_lastHitHref;
    }
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it)
        if (x >= it->x && x <= it->x + it->w
         && y >= it->y && y <= it->y + it->h) {
            m_lastHitRegion = *it;
            m_lastHitHref = UnwrapDuckDuckGoRedirect(UnwrapBingRedirect(it->href));
            m_lastHitDownload = it->download;
            m_lastHitDownloadName = it->downloadName;
            m_lastHitValid = true;
            return m_lastHitHref;
        }
    m_lastHitValid = false;
    m_lastHitHref.clear();
    m_lastHitDownload = false;
    m_lastHitDownloadName.clear();
    return {};
}

void Renderer::BuildHoverIndex(const LayoutBox& box) {
    // Only index boxes with nodes (elements) that can be hovered
    if (box.node && box.node->type == NodeType::Element) {
        const float w = box.borderBoxW();
        const float h = box.borderBoxH();
        const float area = std::max(0.f, w) * std::max(0.f, h);
        // Only index reasonably-sized boxes or inline elements or links
        if (area <= 100000.f || box.isInlineLevel() || !box.href.empty()) {
            m_hoverCandidates.push_back({ &box, box.x, box.y, w, h });
        }
    }
    for (const auto& kid : box.kids) {
        BuildHoverIndex(*kid);
    }
}

const Node* Renderer::HoverNodeAt(float x, float y, float scrollY, float topInset) const {
    if (!m_layoutRoot) return nullptr;
    const float docY = y + scrollY - topInset;
    if (m_lastHoverNodeValid
        && x >= m_lastHoverNodeRegion.x && x <= m_lastHoverNodeRegion.x + m_lastHoverNodeRegion.w
        && docY >= m_lastHoverNodeRegion.y && docY <= m_lastHoverNodeRegion.y + m_lastHoverNodeRegion.h) {
        return m_lastHoverNode;
    }

    // Use the prebuilt hover index instead of walking the full tree.
    // Iterate in reverse to find the deepest (most specific) match.
    // Early-exit on first match since we're going deepest-first.
    const LayoutBox* found = nullptr;
    const float viewportH = (float)m_height - topInset;
    for (auto it = m_hoverCandidates.rbegin(); it != m_hoverCandidates.rend(); ++it) {
        // Quick viewport culling: skip elements completely off-screen
        const float elemBottom = it->y - scrollY;
        const float elemTop = elemBottom - it->h;
        if (elemBottom < -100.f || elemTop > viewportH + 100.f) continue;
        
        if (x >= it->x && x <= it->x + it->w
            && docY >= it->y && docY <= it->y + it->h) {
            found = it->box;
            break;
        }
    }

    if (!found) {
        m_lastHoverNodeValid = false;
        m_lastHoverNode = nullptr;
        return nullptr;
    }

    const float w = found->borderBoxW();
    const float h = found->borderBoxH();
    m_lastHoverNodeRegion = { found->x, found->y, w, h, {} };
    m_lastHoverNode = found->node;
    m_lastHoverNodeValid = true;
    return found->node;
}

bool Renderer::LastHoverRegion(HitRegion& out) const {
    if (!m_lastHoverNodeValid) return false;
    out = m_lastHoverNodeRegion;
    return true;
}

bool Renderer::GetAnchorY(const std::string& anchor, float& outY) const {
    auto it = m_anchorY.find(anchor);
    if (it == m_anchorY.end()) return false;
    outY = it->second;
    return true;
}
