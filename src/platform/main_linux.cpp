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
#include "platform/profile.h"
#include "platform/downloads.h"
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

// ── clipboard (ICCCM selection ownership) ─────────────────────────────────
// X11 has no "set clipboard" call: to publish text you become the owner of
// the CLIPBOARD selection, then answer XCB_SELECTION_REQUEST events (fired
// when another app pastes) by writing g_clipboardText into the requestor's
// property. See HandleEvent's XCB_SELECTION_REQUEST case.
static std::string g_clipboardText;

static void SetClipboardText(const std::string& text) {
    if (!g_conn || !g_win) return;
    g_clipboardText = text;
    xcb_atom_t clipboard = InternAtom(g_conn, "CLIPBOARD");
    xcb_set_selection_owner(g_conn, g_win, clipboard, XCB_CURRENT_TIME);
    xcb_set_selection_owner(g_conn, g_win, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);
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

// Profile data storage
static vertex::profile::ProfilePaths g_profilePaths;
static std::vector<vertex::downloads::DownloadRecord> g_downloads;
static std::vector<std::vector<std::string>> g_bookmarks;
static std::vector<std::vector<std::string>> g_history;

static std::unique_ptr<IPlatformRenderer> g_renderer;
static std::unique_ptr<PlatTextMeasure> g_measure;
static std::unique_ptr<LayoutBox> g_layoutRoot;
static std::map<std::string, PlatBitmap> g_images;
static std::set<std::string> g_loadingImages;
static std::set<std::string> g_failedImages;
static std::map<std::string, PlatFont> g_fontCache;

static float g_zoom = 1.0f;

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

struct ContextMenu {
    bool visible = false;
    float x = 0, y = 0;
    std::string linkUrl;
};
static ContextMenu g_contextMenu;

// Persistent hit regions for link-click detection (populated each paint).
static std::vector<HitRegion> g_hits;

static std::string HitTestLink(float x, float y) {
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
        if (x >= it->x && x <= it->x + it->w
         && y >= it->y && y <= it->y + it->h)
            return UnwrapBingRedirect(it->href);
    return {};
}

// Find the deepest node at the given position by traversing the layout tree
static const Node* FindNodeAt(const LayoutBox* box, float x, float y, float scrollY, float topInset) {
    if (!box) return nullptr;
    
    // Check if point is within this box's bounds (adjusted for scroll and inset)
    float boxTop = box->y + topInset - scrollY;
    float boxBottom = boxTop + box->borderBoxH();
    float boxLeft = box->x;
    float boxRight = boxLeft + box->borderBoxW();
    
    if (x < boxLeft || x > boxRight || y < boxTop || y > boxBottom)
        return nullptr;
    
    // Check children first (deepest first)
    for (auto& child : box->kids) {
        if (const Node* found = FindNodeAt(child.get(), x, y, scrollY, topInset))
            return found;
    }
    
    // Check inline fragments
    for (auto& line : box->lines) {
        for (auto& frag : line.frags) {
            if (frag.src && frag.src->node) {
                float fragTop = frag.y + topInset - scrollY;
                float fragBottom = fragTop + frag.h;
                float fragLeft = frag.x;
                float fragRight = fragLeft + frag.w;
                
                if (x >= fragLeft && x <= fragRight && y >= fragTop && y <= fragBottom)
                    return frag.src->node;
            }
        }
    }
    
    // Return this box's node if no children matched
    return box->node;
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

// ── profile data helpers ─────────────────────────────────────────────────────

static std::string HtmlEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

static void AppendHistoryRecord(const std::string& url, const std::string& title) {
    if (g_profilePaths.historyFile.empty()) return;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    vertex::profile::AppendTsvRow(g_profilePaths.historyFile, {std::to_string(ms), url, title});
}

static void AppendBookmarkRecord(const std::string& url, const std::string& title) {
    if (g_profilePaths.bookmarksFile.empty()) return;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    vertex::profile::AppendTsvRow(g_profilePaths.bookmarksFile, {std::to_string(ms), url, title});
}

static void AppendDownloadRecord(const vertex::downloads::DownloadRecord& rec) {
    if (g_profilePaths.downloadsFile.empty()) return;
    g_downloads.push_back(rec);
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    vertex::profile::AppendTsvRow(g_profilePaths.downloadsFile, {
        std::to_string(ms), rec.url, rec.path, rec.filename,
        rec.success ? "success" : "failed", rec.error
    });
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

static void DrawContextMenu() {
    using namespace vertex::chrome_theme;
    if (!g_renderer || !g_contextMenu.visible) return;
    
    std::vector<std::string> items;
    if (!g_contextMenu.linkUrl.empty()) {
        items.push_back("Open Link");
        items.push_back("Open Link in New Tab");
        items.push_back("Copy Link");
    } else {
        items.push_back("Back");
        items.push_back("Forward");
        items.push_back("Reload");
        items.push_back("Add Bookmark");
        items.push_back("View Bookmarks");
    }
    
    float menuW = 200.f;
    float itemH = 28.f;
    float menuH = itemH * items.size();
    float x = g_contextMenu.x;
    float y = g_contextMenu.y;
    
    // Keep menu on screen
    if (x + menuW > g_width) x = g_width - menuW;
    if (y + menuH > g_height) y = g_height - menuH;
    
    g_renderer->FillRoundedRect(x, y, menuW, menuH, (float)CornerRadius, RgbToPlat(Rail));
    g_renderer->DrawRect(x, y, menuW, menuH, RgbToPlat(Line), 2.f);
    
    PlatFont menuFont = g_renderer->CreateFont(13, false, false, false, "");
    for (size_t i = 0; i < items.size(); i++) {
        float itemY = y + i * itemH;
        g_renderer->DrawText(Utf8ToWide(items[i]), x + 12, itemY + 7, menuW - 24, itemH, menuFont, RgbToPlat(Ink));
        if (i + 1 < items.size()) {
            g_renderer->DrawLine(x + 8, itemY + itemH, x + menuW - 8, itemY + itemH, RgbToPlat(Line), 1.f);
        }
    }
    g_renderer->ReleaseFont(menuFont);
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
    DrawContextMenu();

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
        in.zoom = g_zoom;
        in.baseUrl = tab.page->url;
        g_layoutRoot = LayoutDocument(in);
        if (g_layoutRoot) {
            PaintState ps;
            ps.r = g_renderer.get();
            ps.scrollY = tab.scrollY;
            ps.topInset = (float)ToolbarHeight;
            ps.zoom = g_zoom;
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

static std::string AppPageCss() {
    return "body{margin:0;padding:20px;font:14px/1.5 system-ui,sans-serif;background:#f9fbff;color:#1f2937}"
           "main{max-width:800px;margin:0 auto;background:#fff;padding:32px;border-radius:8px;box-shadow:0 1px 3px rgba(0,0,0,.1)}"
           "h1{margin:0 0 24px;font-size:24px;font-weight:600;color:#111827}"
           ".item{margin:16px 0;padding:12px 0;border-bottom:1px solid #e5e7eb}"
           ".item:last-child{border:none}"
           ".name{font-weight:500;color:#111827;text-decoration:none;display:block}"
           ".name:hover{color:#2563eb}"
           ".meta{margin-top:4px;font-size:13px;color:#6b7280;word-break:break-all}"
           "code{background:#f3f4f6;padding:2px 6px;border-radius:4px;font-size:12px}";
}

static std::string HistoryPageHtml() {
    std::string html = "<html><head><title>History</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>History</h1>";
    bool any = false;
    for (auto it = g_history.rbegin(); it != g_history.rend(); ++it) {
        if (it->size() < 2) continue;
        std::string url = (*it)[1];
        std::string title = it->size() > 2 && !(*it)[2].empty() ? (*it)[2] : url;
        html += "<div class=\"item\"><a class=\"name\" href=\"" + HtmlEscape(url) + "\">"
            + HtmlEscape(title) + "</a><div class=\"meta\">" + HtmlEscape(url) + "</div></div>";
        any = true;
    }
    if (!any) html += "<p>No history yet.</p>";
    html += "</main></body></html>";
    return html;
}

static std::string BookmarksPageHtml() {
    std::string html = "<html><head><title>Bookmarks</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>Bookmarks</h1>";
    bool any = false;
    for (auto it = g_bookmarks.rbegin(); it != g_bookmarks.rend(); ++it) {
        if (it->size() < 2) continue;
        std::string url = (*it)[1];
        std::string title = it->size() > 2 && !(*it)[2].empty() ? (*it)[2] : url;
        html += "<div class=\"item\"><a class=\"name\" href=\"" + HtmlEscape(url) + "\">"
            + HtmlEscape(title) + "</a><div class=\"meta\">" + HtmlEscape(url) + "</div></div>";
        any = true;
    }
    if (!any) html += "<p>No bookmarks yet.</p>";
    html += "</main></body></html>";
    return html;
}

static std::string DownloadsPageHtml() {
    std::string html = "<html><head><title>Downloads</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>Downloads</h1>";
    bool any = false;
    for (auto it = g_downloads.rbegin(); it != g_downloads.rend(); ++it) {
        std::string status = it->success ? "Complete" : "Failed";
        std::string color = it->success ? "#10b981" : "#ef4444";
        html += "<div class=\"item\"><div class=\"name\">" + HtmlEscape(it->filename) 
            + "</div><div class=\"meta\">Path: <code>" + HtmlEscape(it->path) 
            + "</code><br>Status: <span style=\"color:" + color + "\">" + status + "</span>";
        if (!it->error.empty()) html += "<br>Error: " + HtmlEscape(it->error);
        html += "</div></div>";
        any = true;
    }
    if (!any) html += "<p>No downloads yet.</p>";
    html += "</main></body></html>";
    return html;
}

static std::string SettingsPageHtml() {
    return "<html><head><title>Settings</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>Settings</h1>"
        "<div class=\"item\"><div class=\"name\">Profile</div>"
        "<div class=\"meta\"><code>" + HtmlEscape(g_profilePaths.profileRoot) + "</code></div></div>"
        "<div class=\"item\"><div class=\"name\">Cache</div>"
        "<div class=\"meta\"><code>" + HtmlEscape(g_profilePaths.cacheProfileRoot) + "</code></div></div>"
        "<div class=\"item\"><div class=\"name\">Controls</div>"
        "<div class=\"meta\"><a href=\"vertex://history\">History</a> | "
        "<a href=\"vertex://bookmarks\">Bookmarks</a> | "
        "<a href=\"vertex://downloads\">Downloads</a> | "
        "<a href=\"vertex://site-data\">Site data</a></div></div>"
        "<div class=\"item\"><div class=\"name\">Current defaults</div>"
        "<div class=\"meta\">JavaScript on | Images on | Cache on | Search engine Bing</div></div>"
        "</main></body></html>";
}

static std::string SiteDataPageHtml() {
    return "<html><head><title>Site Data</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>Site Data</h1>"
        "<div class=\"item\"><div class=\"name\">Storage root</div><div class=\"meta\"><code>"
        + HtmlEscape(g_profilePaths.profileRoot) + "</code></div></div>"
        "<div class=\"item\"><div class=\"name\">History entries</div><div class=\"meta\">"
        + std::to_string(g_history.size()) + "</div></div>"
        "<div class=\"item\"><div class=\"name\">Bookmarks</div><div class=\"meta\">"
        + std::to_string(g_bookmarks.size()) + "</div></div>"
        "<div class=\"item\"><div class=\"name\">Downloads</div><div class=\"meta\">"
        + std::to_string(g_downloads.size()) + "</div></div>"
        "<div class=\"item\"><div class=\"name\">Local storage</div><div class=\"meta\"><code>"
        + HtmlEscape(g_profilePaths.localStorageDir) + "</code></div></div>"
        "<div class=\"item\"><div class=\"name\">Cookies</div><div class=\"meta\"><code>"
        + HtmlEscape(g_profilePaths.cookiesFile) + "</code></div></div>"
        "</main></body></html>";
}

static void platformFetch(int tabIdx, const std::string& url) {
    // Handle internal pages
    if (url == "vertex://history") {
        auto* page = new Page();
        page->url = url;
        page->dom = ParseHtml(HistoryPageHtml());
        PostToMainThread([tabIdx, page]() {
            g_chrome.onPageReady(tabIdx, page);
            SetWindowTitle(g_chrome.state.title());
            RequestRedraw();
        });
        return;
    }
    if (url == "vertex://bookmarks") {
        auto* page = new Page();
        page->url = url;
        page->dom = ParseHtml(BookmarksPageHtml());
        PostToMainThread([tabIdx, page]() {
            g_chrome.onPageReady(tabIdx, page);
            SetWindowTitle(g_chrome.state.title());
            RequestRedraw();
        });
        return;
    }
    if (url == "vertex://downloads") {
        auto* page = new Page();
        page->url = url;
        page->dom = ParseHtml(DownloadsPageHtml());
        PostToMainThread([tabIdx, page]() {
            g_chrome.onPageReady(tabIdx, page);
            SetWindowTitle(g_chrome.state.title());
            RequestRedraw();
        });
        return;
    }
    if (url == "vertex://settings") {
        auto* page = new Page();
        page->url = url;
        page->dom = ParseHtml(SettingsPageHtml());
        PostToMainThread([tabIdx, page]() {
            g_chrome.onPageReady(tabIdx, page);
            SetWindowTitle(g_chrome.state.title());
            RequestRedraw();
        });
        return;
    }
    if (url == "vertex://site-data") {
        auto* page = new Page();
        page->url = url;
        page->dom = ParseHtml(SiteDataPageHtml());
        PostToMainThread([tabIdx, page]() {
            g_chrome.onPageReady(tabIdx, page);
            SetWindowTitle(g_chrome.state.title());
            RequestRedraw();
        });
        return;
    }
    
    // Regular network fetch
    std::thread([tabIdx, url]() {
        auto res = FetchResourceCached(url, 12 * 1024 * 1024, ResourceKind::Document);
        auto* page = new Page();
        page->url = url;
        if (res.success && !res.body.empty()) {
            page->dom = ParseHtml(DecodeTextToUtf8(res.body, res.contentType, true));
            if (!res.finalUrl.empty() && res.finalUrl != url)
                page->url = res.finalUrl;
            LoadExternalStylesheets(page->dom, page->url);
            LoadExternalScriptSources(page->dom, page->url);
        } else {
            page->error = res.error;
        }
        PostToMainThread([tabIdx, page]() {
            g_chrome.onPageReady(tabIdx, page);
            SetWindowTitle(g_chrome.state.title());
            // Record to history (skip internal pages)
            if (page->url.rfind("vertex://", 0) != 0 && tabIdx >= 0 && tabIdx < (int)g_tabs.size()) {
                std::string title = g_tabs[tabIdx].title;
                if (title.empty()) title = page->url;
                AppendHistoryRecord(page->url, title);
            }
            RequestRedraw();
        });
    }).detach();
}

// ── input handling ───────────────────────────────────────────────────────────

static void OnButtonPress(uint8_t button, int x, int y) {
    using namespace vertex::chrome_theme;
    if (g_tabs.empty()) return;

    // Mouse wheel scrolling
    if (button == 4 || button == 5) {
        float delta = (button == 4) ? -60.f : 60.f;
        CurTab().scrollY = std::max(0.f, CurTab().scrollY + delta);
        RequestRedraw();
        return;
    }
    
    // Right-click - show context menu
    if (button == 3 && y >= ToolbarHeight && y < g_height - StatusHeight) {
        g_contextMenu.visible = true;
        g_contextMenu.x = (float)x;
        g_contextMenu.y = (float)y;
        g_contextMenu.linkUrl = HitTestLink((float)x, (float)y);
        RequestRedraw();
        return;
    }
    
    if (button != 1) return;
    
    // Left-click - check context menu first
    if (g_contextMenu.visible) {
        std::vector<std::string> items;
        if (!g_contextMenu.linkUrl.empty()) {
            items.push_back("Open Link");
            items.push_back("Open Link in New Tab");
            items.push_back("Copy Link");
        } else {
            items.push_back("Back");
            items.push_back("Forward");
            items.push_back("Reload");
        }
        
        float menuW = 200.f;
        float itemH = 28.f;
        float menuX = g_contextMenu.x;
        float menuY = g_contextMenu.y;
        if (menuX + menuW > g_width) menuX = g_width - menuW;
        
        if (x >= menuX && x < menuX + menuW && y >= menuY && y < menuY + itemH * items.size()) {
            int itemIdx = (int)((y - menuY) / itemH);
            g_contextMenu.visible = false;
            
            if (!g_contextMenu.linkUrl.empty()) {
                if (itemIdx == 0) { g_chrome.navigate(g_contextMenu.linkUrl); }
                else if (itemIdx == 1) { 
                    int newIdx = g_chrome.newTab(g_contextMenu.linkUrl);
                    g_chrome.switchTab(newIdx);
                }
                else if (itemIdx == 2) { SetClipboardText(g_contextMenu.linkUrl); }
            } else {
                if (itemIdx == 0) { g_chrome.back(); }
                else if (itemIdx == 1) { g_chrome.forward(); }
                else if (itemIdx == 2) { g_chrome.reload(); }
                else if (itemIdx == 3) { 
                    // Add Bookmark
                    if (!g_tabs.empty()) {
                        AppendBookmarkRecord(CurTab().url, CurTab().title);
                    }
                }
                else if (itemIdx == 4) { 
                    // View Bookmarks
                    g_chrome.navigate("vertex://bookmarks");
                }
            }
            RequestRedraw();
            return;
        }
        g_contextMenu.visible = false;
    }

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
        
        // Check for download links
        if (g_layoutRoot) {
            for (const auto& hit : g_hits) {
                if (x >= hit.x && x <= hit.x + hit.w && y >= hit.y && y <= hit.y + hit.h) {
                    if (hit.download) {
                        // Start download in background thread
                        std::string url = hit.href;
                        std::string downloadAttr = hit.downloadName;
                        std::thread([url, downloadAttr]() {
                            auto res = FetchUrl(url);
                            auto rec = vertex::downloads::SaveFetchedBody(url, res, downloadAttr);
                            PostToMainThread([rec]() {
                                AppendDownloadRecord(rec);
                            });
                        }).detach();
                        return;
                    }
                    break;
                }
            }
        }
        
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
        g_zoom = std::min(5.0f, g_zoom + 0.1f);
        g_layoutRoot.reset();
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == '-') {
        g_zoom = std::max(0.25f, g_zoom - 0.1f);
        g_layoutRoot.reset();
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == '0') {
        g_zoom = 1.0f;
        g_layoutRoot.reset();
        RequestRedraw();
        return;
    }
    if (ctrl && !shift && sym == 'r') {
        g_chrome.reload();
        return;
    }
    if (ctrl && !shift && sym == 'h') {
        g_chrome.navigate("vertex://history");
        return;
    }
    if (ctrl && !shift && sym == 'b') {
        g_chrome.navigate("vertex://bookmarks");
        return;
    }
    if (ctrl && !shift && sym == 'j') {
        g_chrome.navigate("vertex://downloads");
        return;
    }
    if (sym == XK_Escape && CurTab().loading) {
        CurTab().loading = false;
        RequestRedraw();
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
    case XCB_SELECTION_REQUEST: {
        auto* sr = (xcb_selection_request_event_t*)ev;
        xcb_atom_t utf8 = InternAtom(g_conn, "UTF8_STRING");
        xcb_selection_notify_event_t notify{};
        notify.response_type = XCB_SELECTION_NOTIFY;
        notify.time = sr->time;
        notify.requestor = sr->requestor;
        notify.selection = sr->selection;
        notify.target = sr->target;
        notify.property = XCB_ATOM_NONE;
        if (sr->target == utf8 || sr->target == XCB_ATOM_STRING) {
            xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, sr->requestor,
                sr->property, sr->target, 8,
                (uint32_t)g_clipboardText.size(), g_clipboardText.c_str());
            notify.property = sr->property;
        }
        xcb_send_event(g_conn, 0, sr->requestor, XCB_EVENT_MASK_NO_EVENT, (const char*)&notify);
        xcb_flush(g_conn);
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
            
            // Track hover for CSS :hover styles (throttled to avoid excessive redraws)
            if (g_layoutRoot) {
                static timespec lastHoverTime = {0, 0};
                timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                auto msElapsed = [](const timespec& start, const timespec& end) {
                    return (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
                };
                
                // Throttle to 50ms (20Hz) to avoid excessive relayout on hover
                if (msElapsed(lastHoverTime, now) >= 50) {
                    float contentY = y - vertex::chrome_theme::ToolbarHeight + CurTab().scrollY;
                    const Node* hover = FindNodeAt(g_layoutRoot.get(), (float)mp->event_x, contentY, 
                                                    CurTab().scrollY, (float)vertex::chrome_theme::ToolbarHeight);
                    lastHoverTime = now;
                    
                    if (hover != g_hoverNode) {
                        g_hoverNode = hover;
                        SetCssHoverNode(hover);  // Tell CSS system which node is hovered
                        // Force relayout because CSS :hover rules may apply
                        g_layoutRoot.reset();
                        RequestRedraw();
                    }
                }
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
    
    // Initialize profile paths and load existing data
    g_profilePaths = vertex::profile::DefaultPaths();
    vertex::profile::EnsureDirectories(g_profilePaths);
    g_history = vertex::profile::ReadTsvRows(g_profilePaths.historyFile, 1000);
    g_bookmarks = vertex::profile::ReadTsvRows(g_profilePaths.bookmarksFile, 1000);
    auto downloadRows = vertex::profile::ReadTsvRows(g_profilePaths.downloadsFile, 1000);
    for (const auto& row : downloadRows) {
        if (row.size() >= 6) {
            vertex::downloads::DownloadRecord rec;
            rec.url = row.size() > 1 ? row[1] : "";
            rec.path = row.size() > 2 ? row[2] : "";
            rec.filename = row.size() > 3 ? row[3] : "";
            rec.success = row.size() > 4 && row[4] == "success";
            rec.error = row.size() > 5 ? row[5] : "";
            g_downloads.push_back(rec);
        }
    }
    
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
