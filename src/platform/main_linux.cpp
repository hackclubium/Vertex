#ifdef __linux__
//
// main_linux.cpp — Linux application shell for Vertex browser.
//
// Raw XCB window + event loop — no GTK3, no GLib main loop. Unlike Win32
// (BUTTON/EDIT/STATIC controls) or Cocoa (NSButton/NSTextField), X11 has no
// built-in widget toolkit, so this file hand-draws the toolbar (back/
// forward/reload/home buttons, URL entry, status label) and hand-tests
// clicks/keystrokes against it, the same way the box-tree painter already
// hand-draws page content. Drawing goes through Vertex's own software
// rasterizer (src/render/rasterizer.h), font engine (src/font/), and
// <canvas> backend (src/render/canvas_raster.h), and presents via a raw
// xcb_put_image blit — Linux has no third-party dependency left at all.
//
#include <xcb/xcb.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>

#include "platform/platform.h"
#include "platform/chrome.h"
#include "platform/chrome_theme.h"
#include "platform/box_painter.h"
#include "platform/plat_text_measure.h"
#include "network/resource_cache.h"
#include "render/svg.h"
#include "render/canvas_raster.h"
#include "render/raster_bitmap.h"
#include "render/rasterizer.h"
#include "render/webfont.h"
#include "codec/png.h"
#include "codec/jpeg.h"
#include "layout/layout_engine.h"
#include "css/stylesheet.h"
#include "js/dom_bridge.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ── standard X11 keysym values ───────────────────────────────────────────────
// We use raw XCB (not Xlib), so there's no X11/keysymdef.h available without
// pulling in libX11. These numeric values are part of the stable X Window
// System protocol spec, not GTK/Cairo — defining them directly here is the
// same kind of "own the constants" move as the DEFLATE/PNG chunk-type tables.
constexpr xcb_keysym_t XK_BackSpace = 0xFF08;
constexpr xcb_keysym_t XK_Return    = 0xFF0D;
constexpr xcb_keysym_t XK_Escape    = 0xFF1B;
constexpr xcb_keysym_t XK_Delete    = 0xFFFF;
constexpr xcb_keysym_t XK_Home      = 0xFF50;
constexpr xcb_keysym_t XK_Left      = 0xFF51;
constexpr xcb_keysym_t XK_Right     = 0xFF53;
constexpr xcb_keysym_t XK_End       = 0xFF57;
// Keysyms 0x20..0x7E map directly onto ASCII/Latin-1 by protocol definition.

// ── XCB window state ─────────────────────────────────────────────────────────

static xcb_connection_t* g_conn = nullptr;
static xcb_screen_t*     g_screen = nullptr;
static xcb_window_t      g_win = 0;
static xcb_visualtype_t* g_visual = nullptr;
static xcb_gcontext_t    g_gc = 0;
static uint8_t           g_depth = 24;
static raster::Framebuffer g_fb;
static xcb_atom_t        g_wmDeleteWindow = XCB_ATOM_NONE;
static int               g_width = 1280, g_height = 800;
static bool              g_running = true;
static bool              g_needsRedraw = true;

static void RequestRedraw() { g_needsRedraw = true; }

// ── cross-thread task queue ──────────────────────────────────────────────────
// Replaces GLib's g_idle_add: background threads (image/page fetches) call
// PostToMainThread(); the main loop wakes from poll() via the eventfd and
// drains the queue on the UI thread. This is Linux's equivalent of Windows'
// PostMessageW(WM_USER+n) or macOS's dispatch_async(dispatch_get_main_queue()).

static int g_wakeFd = -1;
static std::mutex g_taskMutex;
static std::queue<std::function<void()>> g_tasks;

static void PostToMainThread(std::function<void()> fn) {
    { std::lock_guard<std::mutex> lock(g_taskMutex); g_tasks.push(std::move(fn)); }
    uint64_t one = 1;
    ssize_t ignore = write(g_wakeFd, &one, sizeof(one));
    (void)ignore;
}

static void DrainMainThreadTasks() {
    uint64_t val;
    ssize_t ignore = read(g_wakeFd, &val, sizeof(val));
    (void)ignore;
    std::queue<std::function<void()>> local;
    { std::lock_guard<std::mutex> lock(g_taskMutex); std::swap(local, g_tasks); }
    while (!local.empty()) { local.front()(); local.pop(); }
}

// ── keyboard: keycode -> keysym via the core XCB protocol ───────────────────
// (no xcb-util-keysyms dependency — GetKeyboardMapping is core protocol)

struct KeyboardMap {
    uint8_t minKeycode = 0, maxKeycode = 0;
    uint8_t keysymsPerKeycode = 0;
    std::vector<xcb_keysym_t> syms;

    xcb_keysym_t KeysymFor(xcb_keycode_t kc, int col) const {
        if (kc < minKeycode || kc > maxKeycode || keysymsPerKeycode == 0) return 0;
        int idx = (kc - minKeycode) * keysymsPerKeycode + col;
        if (idx < 0 || idx >= (int)syms.size()) return 0;
        return syms[idx];
    }
};
static KeyboardMap g_keymap;

static void LoadKeyboardMap() {
    const xcb_setup_t* setup = xcb_get_setup(g_conn);
    g_keymap.minKeycode = setup->min_keycode;
    g_keymap.maxKeycode = setup->max_keycode;
    int count = g_keymap.maxKeycode - g_keymap.minKeycode + 1;
    if (count <= 0) return;
    xcb_get_keyboard_mapping_cookie_t cookie =
        xcb_get_keyboard_mapping(g_conn, g_keymap.minKeycode, (uint8_t)count);
    xcb_get_keyboard_mapping_reply_t* reply =
        xcb_get_keyboard_mapping_reply(g_conn, cookie, nullptr);
    if (!reply) return;
    g_keymap.keysymsPerKeycode = reply->keysyms_per_keycode;
    xcb_keysym_t* syms = xcb_get_keyboard_mapping_keysyms(reply);
    int total = xcb_get_keyboard_mapping_keysyms_length(reply);
    g_keymap.syms.assign(syms, syms + total);
    free(reply);
}

// ── misc helpers ──────────────────────────────────────────────────────────────

static PlatColor RgbToPlat(vertex::chrome_theme::Rgb c, float a = 1.f) {
    return { c.r / 255.f, c.g / 255.f, c.b / 255.f, a };
}

static std::wstring Utf8ToWide(const std::string& s) {
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { i++; continue; }
        if (i + (size_t)len > s.size()) break;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out += (wchar_t)cp;
        i += len;
    }
    return out;
}

static xcb_atom_t InternAtom(xcb_connection_t* c, const char* name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(c, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(c, cookie, nullptr);
    xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

static void SetWindowTitle(const std::string& t) {
    if (!g_conn || !g_win) return;
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_win,
        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)t.size(), t.c_str());
    xcb_flush(g_conn);
}

// ── dark-theme detection (XSETTINGS protocol) + window icon ──────────────────
// GTK read "gtk-application-prefer-dark-theme" via its own GObject property,
// which came from the desktop environment's XSETTINGS daemon under the hood.
// Now that GTK is gone, this reads the same underlying protocol directly:
// find the XSETTINGS selection owner for this screen, then parse its
// _XSETTINGS_SETTINGS property (a small binary TLV list) for a dark-theme
// hint. Best-effort and one-shot (checked once at startup, unlike GTK's live
// notify:: signal) — if no XSETTINGS daemon is running, this simply finds no
// owner and falls back to the static/light icon.
static bool ReadXSettingsPreferDark() {
    if (!g_conn || !g_screen) return false;
    std::string selName = "_XSETTINGS_S0";
    xcb_atom_t selAtom = InternAtom(g_conn, selName.c_str());
    if (selAtom == XCB_ATOM_NONE) return false;
    xcb_get_selection_owner_cookie_t oc = xcb_get_selection_owner(g_conn, selAtom);
    xcb_get_selection_owner_reply_t* orp = xcb_get_selection_owner_reply(g_conn, oc, nullptr);
    xcb_window_t owner = orp ? orp->owner : XCB_WINDOW_NONE;
    free(orp);
    if (owner == XCB_WINDOW_NONE) return false;

    xcb_atom_t settingsAtom = InternAtom(g_conn, "_XSETTINGS_SETTINGS");
    if (settingsAtom == XCB_ATOM_NONE) return false;
    xcb_get_property_cookie_t pc = xcb_get_property(g_conn, 0, owner, settingsAtom, settingsAtom, 0, 16384);
    xcb_get_property_reply_t* pr = xcb_get_property_reply(g_conn, pc, nullptr);
    if (!pr) return false;

    int len = xcb_get_property_value_length(pr);
    const uint8_t* data = (const uint8_t*)xcb_get_property_value(pr);
    bool preferDark = false;
    if (len >= 8) {
        uint8_t byteOrder = data[0];
        auto rd32 = [&](int off) -> uint32_t {
            uint32_t v; memcpy(&v, data + off, 4);
            return byteOrder == 0 ? v : __builtin_bswap32(v);
        };
        auto rd16 = [&](int off) -> uint16_t {
            uint16_t v; memcpy(&v, data + off, 2);
            return byteOrder == 0 ? v : __builtin_bswap16(v);
        };
        uint32_t nSettings = rd32(4);
        int pos = 8;
        for (uint32_t i = 0; i < nSettings && pos + 4 <= len; i++) {
            uint8_t type = data[pos];
            uint16_t nameLen = rd16(pos + 2);
            int nameStart = pos + 4;
            int namePad = (nameLen + 3) & ~3;
            if (nameStart + namePad + 4 > len) break;
            std::string name((const char*)data + nameStart, nameLen);
            int valPos = nameStart + namePad + 4;  // skip 4-byte last-change-serial
            if (type == 0) {  // Integer
                if (valPos + 4 > len) break;
                int32_t val = (int32_t)rd32(valPos);
                if (name == "Gtk/ApplicationPreferDarkTheme" && val == 1) preferDark = true;
                pos = valPos + 4;
            } else if (type == 1) {  // String
                if (valPos + 4 > len) break;
                uint32_t sLen = rd32(valPos);
                int sPad = (int)((sLen + 3) & ~3u);
                if (valPos + 4 + sPad > len) break;
                std::string val((const char*)data + valPos + 4, sLen);
                if (name == "Net/ThemeName") {
                    std::string lower;
                    for (char c : val) lower += (char)std::tolower((unsigned char)c);
                    if (lower.find("dark") != std::string::npos) preferDark = true;
                }
                pos = valPos + 4 + sPad;
            } else if (type == 2) {  // Color: 4x uint16, no name-keyed data we need
                pos = valPos + 8;
            } else {
                break;  // unrecognized type — can't reliably skip past it
            }
        }
    }
    free(pr);
    return preferDark;
}

static void SetWindowIcon(bool preferDark) {
    if (!g_conn || !g_win) return;
    std::vector<std::string> candidates = {
        preferDark ? "vertex_icon_light.png" : "vertex_icon.png",
        std::string("src/platform/") + (preferDark ? "vertex_icon_light.png" : "vertex_icon.png"),
    };
    for (const auto& path : candidates) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); continue; }
        std::vector<uint8_t> bytes((size_t)sz);
        size_t nread = fread(bytes.data(), 1, bytes.size(), f);
        fclose(f);
        if (nread != bytes.size()) continue;

        DecodedImage img = DecodePng(bytes.data(), bytes.size());
        if (!img.success || img.width <= 0 || img.height <= 0) continue;

        std::vector<uint32_t> icon;
        icon.reserve(2 + (size_t)img.width * (size_t)img.height);
        icon.push_back((uint32_t)img.width);
        icon.push_back((uint32_t)img.height);
        for (int i = 0; i < img.width * img.height; i++) {
            uint8_t r = img.rgba[i * 4 + 0], g = img.rgba[i * 4 + 1],
                    b = img.rgba[i * 4 + 2], a = img.rgba[i * 4 + 3];
            icon.push_back(((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        }
        xcb_atom_t netWmIcon = InternAtom(g_conn, "_NET_WM_ICON");
        xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_win, netWmIcon,
            XCB_ATOM_CARDINAL, 32, (uint32_t)icon.size(), icon.data());
        xcb_flush(g_conn);
        return;
    }
}

// ── browser state ─────────────────────────────────────────────────────────────

static BrowserChrome g_chrome;
static auto& g_tabs      = g_chrome.state.tabs;
static auto& g_activeTab = g_chrome.state.activeTab;
static auto& g_js        = g_chrome.state.js;
static auto& g_formState = g_chrome.state.form;
static auto& g_updater   = g_chrome.state.updater;

const Node* g_hoverNode = nullptr;  // Not static - CSS system needs extern access

static Semaphore g_imageFetchGate(6);

static std::unique_ptr<IPlatformRenderer> g_renderer;
static std::unique_ptr<PlatTextMeasure> g_measure;
static std::unique_ptr<LayoutBox> g_layoutRoot;
static std::map<std::string, PlatBitmap> g_images;
static std::set<std::string> g_loadingImages;
static std::set<std::string> g_failedImages;
static std::map<std::string, PlatFont> g_fontCache;

static std::map<const Node*, std::unique_ptr<RasterCanvasSurface>> g_canvasSurfaces;
static std::map<const Node*, PlatBitmap> g_canvasBitmaps;

static ICanvasSurface* GetOrCreateCanvasSurface(Node* n) {
    if (!n) return nullptr;
    auto it = g_canvasSurfaces.find(n);
    if (it != g_canvasSurfaces.end()) return it->second.get();
    auto parseIntAttr = [&](const char* a, int def) -> int {
        std::string v = n->attr(a);
        if (v.empty()) return def;
        try { return std::max(0, std::stoi(v)); } catch (...) { return def; }
    };
    int w = parseIntAttr("width", 300);
    int h = parseIntAttr("height", 150);
    // g_images and the canvas backend now share the exact same RasterBitmap
    // type (render/raster_bitmap.h), so canvas.drawImage(<already-decoded
    // img>) can look the image straight up here — no per-platform bitmap
    // format mismatch to work around anymore.
    auto surface = std::make_unique<RasterCanvasSurface>(w, h,
        [](const std::string& url) -> const RasterBitmap* {
            auto imgIt = g_images.find(url);
            return imgIt != g_images.end() ? (const RasterBitmap*)imgIt->second : nullptr;
        });
    ICanvasSurface* raw = surface.get();
    g_canvasSurfaces[n] = std::move(surface);
    return raw;
}

static Tab& CurTab() { return g_tabs[g_activeTab]; }

static const char* UrlBadgeText(const std::string& url) {
    if (url.rfind("vertex://", 0) == 0 || url.rfind("felix://", 0) == 0) return "H";
    if (url.rfind("https://", 0) == 0) return "S";
    if (url.rfind("http://", 0) == 0) return "i";
    return "?";
}

// ── hand-rolled URL bar text editing ─────────────────────────────────────────
// Mirrors form_state.h's cursor/insert/backspace/delete pattern (already used
// for HTML <input> fields) rather than inventing new editing logic — the URL
// bar used to be a native GtkEntry that handled this itself.
struct UrlEditState {
    std::string text;
    size_t cursorPos = 0;
    bool focused = false;

    void setText(const std::string& t) { text = t; cursorPos = t.size(); }
    void insertChar(char c) {
        if (cursorPos > text.size()) cursorPos = text.size();
        text.insert(text.begin() + cursorPos, c);
        cursorPos++;
    }
    void backspace() {
        if (cursorPos > 0 && cursorPos <= text.size()) { text.erase(text.begin() + cursorPos - 1); cursorPos--; }
    }
    void deleteChar() {
        if (cursorPos < text.size()) text.erase(text.begin() + cursorPos);
    }
    void left()  { if (cursorPos > 0) cursorPos--; }
    void right() { if (cursorPos < text.size()) cursorPos++; }
    void home()  { cursorPos = 0; }
    void end()   { cursorPos = text.size(); }
};
static UrlEditState g_urlEdit;
static std::string g_urlBadgeText = "H";
static std::string g_statusText;

struct FindState {
    bool visible = false;
    std::string query;
    size_t cursorPos = 0;
    void insertChar(char c) { query.insert(cursorPos++, 1, c); }
    void backspace() { if (cursorPos > 0) { query.erase(--cursorPos, 1); } }
    void deleteChar() { if (cursorPos < query.size()) query.erase(cursorPos, 1); }
    void left() { if (cursorPos > 0) cursorPos--; }
    void right() { if (cursorPos < query.size()) cursorPos++; }
    void home() { cursorPos = 0; }
    void end() { cursorPos = query.size(); }
};
static FindState g_findState;

// Persistent hit regions for link-click detection (populated each paint).
static std::vector<HitRegion> g_hits;

static std::string HitTestLink(float x, float y) {
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
        if (x >= it->x && x <= it->x + it->w
         && y >= it->y && y <= it->y + it->h)
            return UnwrapBingRedirect(it->href);
    return {};
}

static void InvalidateHoverRegions(const HitRegion* oldRegion, const HitRegion* newRegion) {
    if (!oldRegion && !newRegion) {
        RequestRedraw();
        return;
    }
    // For simplicity, just redraw the entire content area when hover changes.
    // Could optimize to invalidate just the two regions like Windows does.
    RequestRedraw();
}

// ── image loading pipeline ───────────────────────────────────────────────────

struct LinuxImageMsg {
    std::string url;
    std::vector<uint8_t> bytes;
};

static void FetchImageAsync(const std::string& url);

static void ProcessImage(const std::string& url, const std::vector<uint8_t>& bytes) {
    if (!g_renderer || bytes.empty()) { g_failedImages.insert(url); return; }
    auto looksLikeSvgUrl = [](const std::string& u) {
        std::string low;
        for (char c : u) low += (char)std::tolower((unsigned char)c);
        return low.find(".svg") != std::string::npos
            || low.find("image/svg+xml") != std::string::npos;
    };
    int w = 0, h = 0;
    std::vector<uint8_t> svgPixels;
    std::vector<uint8_t> decodedPixels;
    uint8_t* pixels = nullptr;
    if (looksLikeSvgUrl(url) || svg::looksLikeSvgBytes(bytes)) {
        auto bmp = svg::renderSvgBytes(bytes, svg::SvgRasterMaxDimForBytes(bytes.size()));
        if (bmp.width > 0 && bmp.height > 0 && !bmp.pixels.empty()) {
            w = bmp.width;
            h = bmp.height;
            svgPixels = std::move(bmp.pixels);
            pixels = svgPixels.data();
        }
    }
    if (!pixels) {
        // Try PNG first, then JPEG
        DecodedImage img = DecodePng(bytes.data(), bytes.size());
        if (!img.success) {
            img = DecodeJpeg(bytes.data(), bytes.size());
        }
        if (img.success && img.width > 0 && img.height > 0 && !img.rgba.empty()) {
            w = img.width;
            h = img.height;
            decodedPixels = std::move(img.rgba);
            pixels = decodedPixels.data();
        }
    }
    if (!pixels || w <= 0 || h <= 0) { g_failedImages.insert(url); return; }
    // stb_image outputs straight RGBA; the rasterizer's blit expects
    // premultiplied BGRA (matching this project's established convention —
    // originally for Cairo's ARGB32, now shared by rasterizer.h's BlitBitmap).
    for (int i = 0; i < w * h; ++i) {
        unsigned char* p = pixels + i * 4;
        unsigned char r = p[0], g = p[1], b = p[2], a = p[3];
        float af = a / 255.f;
        p[0] = (unsigned char)(b * af + 0.5f);
        p[1] = (unsigned char)(g * af + 0.5f);
        p[2] = (unsigned char)(r * af + 0.5f);
        p[3] = a;
    }
    PlatBitmap bmp = g_renderer->CreateBitmap(w, h, pixels);
    if (bmp) {
        auto it = g_images.find(url);
        if (it != g_images.end() && it->second) g_renderer->ReleaseBitmap(it->second);
        g_images[url] = bmp;
        g_measure->loadedImages[url] = { (float)w, (float)h };
        g_layoutRoot.reset();  // force relayout
        RequestRedraw();
    } else {
        g_failedImages.insert(url);
    }
}

static void FetchImageAsync(const std::string& url) {
    if (g_loadingImages.count(url) || g_failedImages.count(url) || g_images.count(url)) return;
    g_loadingImages.insert(url);
    std::thread([url]() {
        g_imageFetchGate.acquire();
        auto res = FetchResourceCached(url, 32 * 1024 * 1024, ResourceKind::Image);
        g_imageFetchGate.release();
        std::vector<uint8_t> bytes;
        if (res.success && !res.body.empty())
            bytes = std::vector<uint8_t>(res.body.begin(), res.body.end());
        PostToMainThread([url, bytes]() {
            g_loadingImages.erase(url);
            ProcessImage(url, bytes);
        });
    }).detach();
}

static Stylesheet CollectCSS(const Node* root) {
    Stylesheet sheet;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tagName == "style") {
            std::string css; for (auto& c : n->children) if (c->type == NodeType::Text) css += c->text;
            auto part = ParseStylesheet(css);
            if (part.rootRemBaseSet) {
                sheet.rootRemBase = part.rootRemBase;
                sheet.rootRemBaseSet = true;
            }
            for (auto& r : part.rules) sheet.rules.push_back(r);
        }
        for (auto& c : n->children) walk(c.get());
    };
    walk(root);
    sheet.rebuildRuleBuckets();
    return sheet;
}

// ── toolbar / status bar (hand-drawn — X11 has no native widget toolkit) ────

struct Rect { float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
};
static Rect g_btnRects[4];
static Rect g_urlBadgeRect;
static Rect g_urlEntryRect;

static void DrawToolbar() {
    using namespace vertex::chrome_theme;
    if (!g_renderer) return;
    g_renderer->FillRect(0, 0, (float)g_width, (float)ToolbarHeight, RgbToPlat(Panel));

    float x = (float)Margin;
    float y = (ToolbarHeight - ButtonHeight) / 2.f;
    static const char* labels[4] = { "←", "→", "↻", "⌂" }; // back, fwd, reload, home

    PlatFont btnFont = g_renderer->CreateFont(14, false, false, false, "");
    for (int i = 0; i < 4; i++) {
        g_btnRects[i] = { x, y, (float)ButtonWidth, (float)ButtonHeight };
        g_renderer->FillRoundedRect(x, y, (float)ButtonWidth, (float)ButtonHeight, (float)CornerRadius, RgbToPlat(Active));
        g_renderer->DrawRect(x, y, (float)ButtonWidth, (float)ButtonHeight, RgbToPlat(Line), 1.f);
        std::wstring wlabel = Utf8ToWide(labels[i]);
        float tw = g_renderer->MeasureText(wlabel, btnFont);
        g_renderer->DrawText(wlabel, x + (ButtonWidth - tw) / 2.f, y + 6, (float)ButtonWidth, (float)ButtonHeight, btnFont, RgbToPlat(Ink));
        x += ButtonWidth + Gap;
    }
    g_renderer->ReleaseFont(btnFont);

    g_urlBadgeRect = { x, y, 22.f, (float)ButtonHeight };
    PlatFont badgeFont = g_renderer->CreateFont(13, true, false, false, "");
    g_renderer->DrawText(Utf8ToWide(g_urlBadgeText), x, y + 6, 22.f, (float)ButtonHeight, badgeFont, RgbToPlat(Accent));
    g_renderer->ReleaseFont(badgeFont);
    x += 22.f + Gap;

    float entryW = std::max(40.f, (float)g_width - x - Margin);
    g_urlEntryRect = { x, y, entryW, (float)ButtonHeight };
    g_renderer->FillRoundedRect(x, y, entryW, (float)ButtonHeight, (float)CornerRadius, RgbToPlat(Active));
    g_renderer->DrawRect(x, y, entryW, (float)ButtonHeight, RgbToPlat(g_urlEdit.focused ? Accent : Line), 1.f);

    PlatFont urlFont = g_renderer->CreateFont(14, false, false, false, "");
    bool showPlaceholder = g_urlEdit.text.empty() && !g_urlEdit.focused;
    std::wstring shown = Utf8ToWide(showPlaceholder ? "Enter URL or search..." : g_urlEdit.text);
    g_renderer->DrawText(shown, x + 10, y + 6, entryW - 20, (float)ButtonHeight, urlFont,
                          RgbToPlat(showPlaceholder ? Quiet : Ink));
    if (g_urlEdit.focused) {
        float caretX = x + 10 + g_renderer->MeasureText(Utf8ToWide(g_urlEdit.text.substr(0, g_urlEdit.cursorPos)), urlFont);
        g_renderer->DrawLine(caretX, y + 5, caretX, y + ButtonHeight - 5.f, RgbToPlat(Ink), 1.f);
    }
    g_renderer->ReleaseFont(urlFont);
}

static void DrawStatusBar() {
    using namespace vertex::chrome_theme;
    if (!g_renderer) return;
    float y = (float)(g_height - StatusHeight);
    g_renderer->FillRect(0, y, (float)g_width, (float)StatusHeight, RgbToPlat(Rail));
    if (!g_statusText.empty()) {
        PlatFont f = g_renderer->CreateFont(11, false, false, false, "");
        g_renderer->DrawText(Utf8ToWide(g_statusText), (float)Margin, y + 4, (float)g_width - 2.f * Margin,
                              (float)StatusHeight, f, RgbToPlat(Ink));
        g_renderer->ReleaseFont(f);
    }
}

static void DrawFindBar() {
    using namespace vertex::chrome_theme;
    if (!g_renderer || !g_findState.visible) return;
    
    float barH = 36.f;
    float y = (float)(g_height - StatusHeight - barH);
    float x = (float)Margin;
    float w = 300.f;
    
    g_renderer->FillRoundedRect(x, y, w, barH, (float)CornerRadius, RgbToPlat(Rail));
    g_renderer->DrawRect(x, y, w, barH, RgbToPlat(Line), 1.f);
    
    PlatFont labelFont = g_renderer->CreateFont(12, false, false, false, "");
    g_renderer->DrawText(L"Find:", x + 10, y + 10, 40.f, barH, labelFont, RgbToPlat(Ink));
    g_renderer->ReleaseFont(labelFont);
    
    float entryX = x + 50;
    float entryW = w - 60;
    g_renderer->FillRoundedRect(entryX, y + 6, entryW, 24.f, (float)CornerRadius, RgbToPlat(Active));
    g_renderer->DrawRect(entryX, y + 6, entryW, 24.f, RgbToPlat(Accent), 1.f);
    
    PlatFont findFont = g_renderer->CreateFont(13, false, false, false, "");
    std::wstring wquery = Utf8ToWide(g_findState.query);
    g_renderer->DrawText(wquery, entryX + 6, y + 11, entryW - 12, 24.f, findFont, RgbToPlat(Ink));
    
    float caretX = entryX + 6 + g_renderer->MeasureText(Utf8ToWide(g_findState.query.substr(0, g_findState.cursorPos)), findFont);
    g_renderer->DrawLine(caretX, y + 9, caretX, y + 27, RgbToPlat(Ink), 1.f);
    
    g_renderer->ReleaseFont(findFont);
}
                              (float)StatusHeight, f, RgbToPlat(Quiet));
        g_renderer->ReleaseFont(f);
    }
}

// ── drawing ──────────────────────────────────────────────────────────────────

static void Present() {
    if (!g_conn || !g_win || !g_gc || g_fb.pixels.empty()) return;
    xcb_put_image(g_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, g_win, g_gc,
        (uint16_t)g_fb.width, (uint16_t)g_fb.height, 0, 0, 0, g_depth,
        (uint32_t)g_fb.pixels.size(), g_fb.pixels.data());
    xcb_flush(g_conn);
}

static void DoDraw() {
    using namespace vertex::chrome_theme;

    if (!g_renderer) {
        g_renderer = CreatePlatformRenderer();
        g_renderer->Init((void*)&g_win);
        g_measure = std::make_unique<PlatTextMeasure>(g_renderer.get());
        g_measure->onImageRequest = FetchImageAsync;
    }

    g_fb.Resize(g_width, g_height);
    g_renderer->Resize(g_width, g_height);
    g_renderer->SetNativeContext(&g_fb);
    g_renderer->Clear({1, 1, 1, 1});

    DrawToolbar();
    DrawStatusBar();
    DrawFindBar();

    int contentH = std::max(0, g_height - ToolbarHeight - StatusHeight);
    // Mirrors what cairo_translate(0, ToolbarHeight) + cairo_clip used to do:
    // topInset shifts every content y-coordinate down past the toolbar
    // (box_painter.h already supports this — it's the same field Windows
    // uses for its tab-strip offset), and PushClip guarantees content never
    // paints over the toolbar/status bar regardless of the culling logic's
    // own (slightly more permissive) bounds check against Height().
    g_renderer->PushClip(0, (float)ToolbarHeight, (float)g_width, (float)contentH);

    if (g_tabs.empty() || !CurTab().page || !CurTab().page->dom) {
        PlatFont font = g_renderer->CreateFont(16, false, false, false, "");
        g_renderer->DrawText(CurTab().loading ? L"Loading..." : L"Navigate to a URL",
                             20, (float)ToolbarHeight + 20, 800, 30, font, {0.5f, 0.5f, 0.5f, 1});
        g_renderer->ReleaseFont(font);
        g_renderer->PopClip();
        Present();
        return;
    }

    Tab& tab = CurTab();
    try {
        Stylesheet sheet = CollectCSS(tab.page->dom.get());
        if (!sheet.fontFaces.empty()) {
            WebFontLoader::instance().loadFonts(sheet, tab.page->url, []() {
                PostToMainThread([]() { RequestRedraw(); });
            });
        }
        LayoutInput in;
        in.document = tab.page->dom.get();
        in.sheet = &sheet;
        in.measure = g_measure.get();
        in.viewportW = (float)g_width;
        in.viewportH = (float)contentH;
        in.zoom = 1.f;
        in.baseUrl = tab.page->url;
        g_layoutRoot = LayoutDocument(in);
        if (g_layoutRoot) {
            PaintState ps;
            ps.r = g_renderer.get();
            ps.scrollY = tab.scrollY;
            ps.topInset = (float)ToolbarHeight;
            ps.baseUrl = tab.page->url;
            ps.images = &g_images;
            g_canvasBitmaps.clear();
            for (auto& [node, surface] : g_canvasSurfaces)
                g_canvasBitmaps[node] = surface->AsPlatBitmap();
            ps.canvasSurfaces = &g_canvasBitmaps;
            g_hits.clear();
            ps.hits = &g_hits;
            ps.fontCache = &g_fontCache;
            ps.form = &g_formState;
            PaintBoxTree(ps, *g_layoutRoot);
            tab.docHeight = g_layoutRoot->contentH + 32.f;
        }
    } catch (...) { /* keep the browser alive */ }

    g_renderer->PopClip();
    Present();
}

// ── navigation ───────────────────────────────────────────────────────────────

static void platformFetch(int tabIdx, const std::string& url) {
    std::thread([tabIdx, url]() {
        auto res = FetchResourceCached(url, 12 * 1024 * 1024, ResourceKind::Document);
        auto* page = new Page();
        page->url = url;
        if (res.success && !res.body.empty()) {
            page->dom = ParseHtml(DecodeTextToUtf8(res.body, res.contentType, true));
            LoadExternalStylesheets(page->dom, page->url);
        } else {
            page->error = res.error;
        }
        PostToMainThread([tabIdx, page]() {
            g_chrome.onPageReady(tabIdx, page);
            SetWindowTitle(g_chrome.state.title());
            RequestRedraw();
        });
    }).detach();
}

// ── input handling ───────────────────────────────────────────────────────────

static void OnButtonPress(uint8_t button, int x, int y) {
    using namespace vertex::chrome_theme;
    if (g_tabs.empty()) return;

    if (button == 4) { CurTab().scrollY = std::max(0.f, CurTab().scrollY - 60.f); RequestRedraw(); return; }
    if (button == 5) { CurTab().scrollY += 60.f; RequestRedraw(); return; }
    if (button != 1) return;

    if (y < ToolbarHeight) {
        for (int i = 0; i < 4; i++) {
            if (g_btnRects[i].contains((float)x, (float)y)) {
                g_urlEdit.focused = false;
                switch (i) {
                case 0: g_chrome.back(); break;
                case 1: g_chrome.forward(); break;
                case 2: g_chrome.reload(); break;
                case 3: g_chrome.home(); break;
                }
                RequestRedraw();
                return;
            }
        }
        if (g_urlEntryRect.contains((float)x, (float)y)) {
            g_urlEdit.focused = true;
            g_formState.blur();
            RequestRedraw();
            return;
        }
        g_urlEdit.focused = false;
        RequestRedraw();
        return;
    }

    if (y >= g_height - StatusHeight) return;

    g_urlEdit.focused = false;
    int contentY = y - ToolbarHeight;
    if (g_layoutRoot) {
        Node* input = FormState::hitTestInput(*g_layoutRoot, (float)x, (float)contentY, CurTab().scrollY, 0);
        if (input) {
            g_formState.focus(input);
            RequestRedraw();
            return;
        }
        g_formState.blur();
        std::string href = HitTestLink((float)x, (float)y);
        if (!href.empty()) {
            g_chrome.navigate(href);
            return;
        }
        RequestRedraw();
    }
}

static void NavigateFromUrlBar() {
    std::string url = g_urlEdit.text;
    g_urlEdit.focused = false;
    if (!url.empty()) g_chrome.navigate(url);
    RequestRedraw();
}

static void OnKeyPress(xcb_keycode_t kc, uint16_t state) {
    bool shift = (state & XCB_MOD_MASK_SHIFT) != 0;
    bool ctrl = (state & XCB_MOD_MASK_CONTROL) != 0;
    xcb_keysym_t sym = g_keymap.KeysymFor(kc, shift ? 1 : 0);
    if (!sym) return;

    // Global shortcuts (work everywhere)
    if (ctrl && !shift && sym == 'l') {
        g_urlEdit.focused = true;
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == 't') {
        g_chrome.newTab();
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == 'w') {
        g_chrome.closeTab(g_activeTab);
        RequestRedraw();
        return;
    }
    if (ctrl && sym >= '1' && sym <= '9') {
        int targetIdx = sym - '1';
        if (targetIdx < (int)g_tabs.size()) {
            g_chrome.switchTab(targetIdx);
            RequestRedraw();
        }
        return;
    }
    if (ctrl && !shift && (sym == '=' || sym == '+')) {
        float zoom = g_renderer->GetZoom();
        g_renderer->SetZoom(std::min(5.0f, zoom + 0.1f));
        g_layoutRoot.reset();
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == '-') {
        float zoom = g_renderer->GetZoom();
        g_renderer->SetZoom(std::max(0.25f, zoom - 0.1f));
        g_layoutRoot.reset();
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == '0') {
        g_renderer->SetZoom(1.0f);
        g_layoutRoot.reset();
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == 'r') {
        g_chrome.reload();
        return;
    }
    if (sym == XK_F5) {
        g_chrome.reload();
        return;
    }
    if (sym == XK_Escape && CurTab().loading) {
        CurTab().loading = false;
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == 'f') {
        g_findState.visible = true;
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == 'g') {
        if (g_renderer && !g_findState.query.empty()) {
            g_renderer->FindNext(true);
            RequestRedraw();
        }
        return;
    }
    if (ctrl && shift && sym == 'g') {
        if (g_renderer && !g_findState.query.empty()) {
            g_renderer->FindNext(false);
            RequestRedraw();
        }
        return;
    }

    if (g_findState.visible) {
        if (sym == XK_Escape) { 
            g_findState.visible = false; 
            if (g_renderer) g_renderer->SetSearchQuery(L"");
            RequestRedraw(); 
            return; 
        }
        if (sym == XK_Return) {
            if (g_renderer && !g_findState.query.empty()) {
                g_renderer->FindNext(true);
                RequestRedraw();
            }
            return;
        }
        if (sym == XK_BackSpace) { 
            g_findState.backspace(); 
            if (g_renderer) {
                std::wstring wq(g_findState.query.begin(), g_findState.query.end());
                g_renderer->SetSearchQuery(wq);
            }
            RequestRedraw(); 
            return; 
        }
        if (sym == XK_Delete) { 
            g_findState.deleteChar(); 
            if (g_renderer) {
                std::wstring wq(g_findState.query.begin(), g_findState.query.end());
                g_renderer->SetSearchQuery(wq);
            }
            RequestRedraw(); 
            return; 
        }
        if (sym == XK_Left) { g_findState.left(); RequestRedraw(); return; }
        if (sym == XK_Right) { g_findState.right(); RequestRedraw(); return; }
        if (sym == XK_Home) { g_findState.home(); RequestRedraw(); return; }
        if (sym == XK_End) { g_findState.end(); RequestRedraw(); return; }
        if (sym >= 0x20 && sym < 0x7F) { 
            g_findState.insertChar((char)sym); 
            if (g_renderer) {
                std::wstring wq(g_findState.query.begin(), g_findState.query.end());
                g_renderer->SetSearchQuery(wq);
            }
            RequestRedraw(); 
            return; 
        }
        return;
    }

    if (g_urlEdit.focused) {
        if (sym == XK_Return)     { NavigateFromUrlBar(); return; }
        if (sym == XK_BackSpace)  { g_urlEdit.backspace(); RequestRedraw(); return; }
        if (sym == XK_Delete)     { g_urlEdit.deleteChar(); RequestRedraw(); return; }
        if (sym == XK_Left)       { g_urlEdit.left(); RequestRedraw(); return; }
        if (sym == XK_Right)      { g_urlEdit.right(); RequestRedraw(); return; }
        if (sym == XK_Home)       { g_urlEdit.home(); RequestRedraw(); return; }
        if (sym == XK_End)        { g_urlEdit.end(); RequestRedraw(); return; }
        if (sym == XK_Escape)     { g_urlEdit.focused = false; RequestRedraw(); return; }
        if (sym >= 0x20 && sym < 0x7F) { g_urlEdit.insertChar((char)sym); RequestRedraw(); return; }
        return;
    }

    if (!g_formState.focusedInput) return;
    if (sym == XK_Return) {
        std::string url = g_formState.buildFormQuery();
        if (!url.empty()) {
            g_formState.blur();
            if (url[0] == '/') {
                std::string base = CurTab().page ? CurTab().page->url : "";
                size_t scheme = base.find("://");
                if (scheme != std::string::npos) {
                    size_t slash = base.find('/', scheme + 3);
                    url = base.substr(0, slash) + url;
                }
            }
            g_urlEdit.setText(url);
            g_chrome.navigate(url);
        }
        RequestRedraw();
        return;
    }
    if (sym == XK_BackSpace) { g_formState.backspace(); RequestRedraw(); return; }
    if (sym == XK_Delete)    { g_formState.deleteChar(); RequestRedraw(); return; }
    if (sym == XK_Left && g_formState.cursorPos > 0) { g_formState.cursorPos--; RequestRedraw(); return; }
    if (sym == XK_Right) {
        std::string v = g_formState.getValue(g_formState.focusedInput);
        if (g_formState.cursorPos < v.size()) g_formState.cursorPos++;
        RequestRedraw(); return;
    }
    if (sym == XK_Home) { g_formState.cursorPos = 0; RequestRedraw(); return; }
    if (sym == XK_End)  { g_formState.cursorPos = g_formState.getValue(g_formState.focusedInput).size(); RequestRedraw(); return; }
    if (sym == XK_Escape) { g_formState.blur(); RequestRedraw(); return; }
    if (sym >= 0x20 && sym < 0x7F) { g_formState.insertChar((char)sym); RequestRedraw(); return; }
}

// ── XCB event loop ───────────────────────────────────────────────────────────

static void HandleEvent(xcb_generic_event_t* ev) {
    uint8_t type = ev->response_type & ~0x80;
    switch (type) {
    case XCB_EXPOSE:
        RequestRedraw();
        break;
    case XCB_CONFIGURE_NOTIFY: {
        auto* cfg = (xcb_configure_notify_event_t*)ev;
        if (cfg->width != g_width || cfg->height != g_height) {
            g_width = cfg->width; g_height = cfg->height;
            RequestRedraw();
        }
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        auto* cm = (xcb_client_message_event_t*)ev;
        if (cm->data.data32[0] == g_wmDeleteWindow) g_running = false;
        break;
    }
    case XCB_BUTTON_PRESS: {
        auto* bp = (xcb_button_press_event_t*)ev;
        OnButtonPress(bp->detail, bp->event_x, bp->event_y);
        break;
    }
    case XCB_KEY_PRESS: {
        auto* kp = (xcb_key_press_event_t*)ev;
        OnKeyPress(kp->detail, kp->state);
        break;
    }
    case XCB_MOTION_NOTIFY: {
        auto* mp = (xcb_motion_notify_event_t*)ev;
        int y = mp->event_y;
        if (y >= vertex::chrome_theme::ToolbarHeight && y < g_height - vertex::chrome_theme::StatusHeight) {
            std::string href = HitTestLink((float)mp->event_x, (float)y);
            if (g_statusText != href) {
                g_statusText = href;
                RequestRedraw();
            }
            // Track :hover node for CSS hover styles (throttled adaptively).
            if (g_layoutRoot && g_renderer && g_renderer->UsesHoverStyles()) {
                static timespec lastHoverTime = {0, 0};
                timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                auto msElapsed = [](const timespec& start, const timespec& end) {
                    return (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
                };
                // Adaptive throttle: 33ms (30Hz) for simple pages, 50ms (20Hz) for complex ones
                size_t candidateCount = g_renderer->HoverCandidateCount();
                long throttleMs = (candidateCount > 500) ? 50 : 33;
                if (msElapsed(lastHoverTime, now) >= throttleMs) {
                    HitRegion oldHoverRegion{};
                    bool hadOldHoverRegion = g_renderer->LastHoverRegion(oldHoverRegion);
                    const Node* hover = g_renderer->HoverNodeAt(
                        (float)mp->event_x, (float)y, CurTab().scrollY, (float)vertex::chrome_theme::ToolbarHeight);
                    lastHoverTime = now;
                    if (hover != g_hoverNode) {
                        HitRegion newHoverRegion{};
                        bool hasNewHoverRegion = g_renderer->LastHoverRegion(newHoverRegion);
                        g_hoverNode = hover;
                        InvalidateHoverRegions(
                            hadOldHoverRegion ? &oldHoverRegion : nullptr,
                            hasNewHoverRegion ? &newHoverRegion : nullptr);
                    }
                }
            } else if (g_hoverNode) {
                g_hoverNode = nullptr;
                RequestRedraw();
            }
        }
        break;
    }
        break;
    }
}

// ── window setup ──────────────────────────────────────────────────────────────

static xcb_visualtype_t* FindVisual(xcb_screen_t* screen) {
    xcb_depth_iterator_t depthIt = xcb_screen_allowed_depths_iterator(screen);
    for (; depthIt.rem; xcb_depth_next(&depthIt)) {
        xcb_visualtype_iterator_t visIt = xcb_depth_visuals_iterator(depthIt.data);
        for (; visIt.rem; xcb_visualtype_next(&visIt)) {
            if (screen->root_visual == visIt.data->visual_id)
                return visIt.data;
        }
    }
    return nullptr;
}

static bool CreateXcbWindow(int width, int height) {
    g_conn = xcb_connect(nullptr, nullptr);
    if (!g_conn || xcb_connection_has_error(g_conn)) return false;
    g_screen = xcb_setup_roots_iterator(xcb_get_setup(g_conn)).data;
    if (!g_screen) return false;
    g_visual = FindVisual(g_screen);
    if (!g_visual) return false;
    g_depth = g_screen->root_depth;

    g_win = xcb_generate_id(g_conn);
    uint32_t valueMask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        g_screen->white_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION
    };
    xcb_create_window(g_conn, XCB_COPY_FROM_PARENT, g_win, g_screen->root,
        0, 0, (uint16_t)width, (uint16_t)height, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, g_screen->root_visual,
        valueMask, values);

    const char* title = "Vertex";
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_win,
        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);

    xcb_atom_t wmProtocols = InternAtom(g_conn, "WM_PROTOCOLS");
    g_wmDeleteWindow = InternAtom(g_conn, "WM_DELETE_WINDOW");
    if (wmProtocols != XCB_ATOM_NONE && g_wmDeleteWindow != XCB_ATOM_NONE) {
        xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_win,
            wmProtocols, XCB_ATOM_ATOM, 32, 1, &g_wmDeleteWindow);
    }

    g_gc = xcb_generate_id(g_conn);
    xcb_create_gc(g_conn, g_gc, g_win, 0, nullptr);

    xcb_map_window(g_conn, g_win);
    xcb_flush(g_conn);
    LoadKeyboardMap();
    SetWindowIcon(ReadXSettingsPreferDark());
    return true;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    g_wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    {
        std::string exePath = "/proc/self/exe";
        char buf[4096] = {};
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) { buf[len] = 0; exePath = buf; }
        Updater::applyPendingUpdate(exePath);
        g_updater.onStatusChanged = []() {
            g_statusText = g_updater.statusMessage;
            RequestRedraw();
        };
        g_updater.checkForUpdateAsync(exePath);
    }

    if (!CreateXcbWindow(g_width, g_height)) {
        return 1;
    }

    g_chrome.cb.repaint = []() { RequestRedraw(); };
    g_chrome.cb.setTitle = [](const std::string& t) { SetWindowTitle(t); };
    g_chrome.cb.setAddressText = [](const std::string& u) {
        g_urlEdit.setText(u);
        g_urlBadgeText = UrlBadgeText(u);
        RequestRedraw();
    };
    g_chrome.cb.setStatusText = [](const std::string& s) { g_statusText = s; RequestRedraw(); };
    g_chrome.cb.getCanvasSurface = [](Node* n) { return GetOrCreateCanvasSurface(n); };
    g_chrome.cb.scrollIntoView = [](Node* target) {
        if (!target || !g_layoutRoot) return;
        std::function<bool(const LayoutBox*, float&)> findBox =
            [&](const LayoutBox* box, float& y) -> bool {
                if (!box) return false;
                if (box->node == target) { y = box->y; return true; }
                for (const auto& child : box->kids)
                    if (findBox(child.get(), y)) return true;
                for (const auto& line : box->lines) {
                    for (const auto& frag : line.frags) {
                        if (frag.src && frag.src->node == target) { y = frag.y; return true; }
                    }
                }
                return false;
            };
        float y = 0.f;
        if (!findBox(g_layoutRoot.get(), y)) return;
        CurTab().scrollY = std::max(0.f, y - 16.f);
        RequestRedraw();
    };
    g_chrome.onNavigateRequested = platformFetch;
    g_chrome.init();
    g_urlEdit.setText(g_tabs.empty() ? "vertex://home" : CurTab().url);
    g_urlBadgeText = UrlBadgeText(g_urlEdit.text);

    int xcbFd = xcb_get_file_descriptor(g_conn);
    auto lastPump = std::chrono::steady_clock::now();
    while (g_running) {
        struct pollfd fds[2];
        fds[0].fd = xcbFd;  fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = g_wakeFd; fds[1].events = POLLIN; fds[1].revents = 0;
        poll(fds, 2, 16);

        if (fds[1].revents & POLLIN) DrainMainThreadTasks();

        xcb_generic_event_t* ev;
        while (g_running && (ev = xcb_poll_for_event(g_conn)) != nullptr) {
            HandleEvent(ev);
            free(ev);
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastPump >= std::chrono::milliseconds(16)) {
            g_chrome.pumpJs();
            lastPump = now;
        }

        if (g_needsRedraw) {
            DoDraw();
            g_needsRedraw = false;
        }
    }

    if (g_conn) xcb_disconnect(g_conn);
    return 0;
}

#endif // __linux__
