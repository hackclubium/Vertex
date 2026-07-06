#ifdef __linux__
//
// main_linux.cpp — Linux application shell for Vertex browser.
//
// Creates a GTK3 window with a toolbar (back/forward/reload/URL entry), a
// GtkDrawingArea for rendering, and drives the browser core.
//
#include <gtk/gtk.h>
#include "platform/platform.h"
#include "platform/chrome.h"
#include "platform/chrome_theme.h"
#include "platform/box_painter.h"
#include "platform/plat_text_measure.h"
#include "network/resource_cache.h"
#include "render/svg.h"
#include "render/canvas_cairo.h"
#include "render/webfont.h"
#include "third_party/stb_image.h"
#include <set>
#include <map>
#include <memory>
#include "layout/layout_engine.h"
#include "css/stylesheet.h"
#include "js/dom_bridge.h"
#include <cctype>
#include <string>
#include <vector>

// ── globals ──────────────────────────────────────────────────────────────────

static GtkWidget* g_window;
static GtkWidget* g_urlEntry;
static GtkWidget* g_urlBadge;
static GtkWidget* g_statusLabel;
static GtkWidget* g_drawingArea;

static BrowserChrome g_chrome;
static auto& g_tabs      = g_chrome.state.tabs;
static auto& g_activeTab = g_chrome.state.activeTab;
static auto& g_js        = g_chrome.state.js;
static auto& g_formState = g_chrome.state.form;
static auto& g_updater   = g_chrome.state.updater;

static bool IsLinuxDarkTheme() {
    GtkSettings* settings = gtk_settings_get_default();
    if (!settings) return false;
    gboolean preferDark = FALSE;
    gchar* themeName = nullptr;
    g_object_get(settings,
        "gtk-application-prefer-dark-theme", &preferDark,
        "gtk-theme-name", &themeName,
        nullptr);
    std::string lowerTheme;
    if (themeName) {
        for (const char* p = themeName; *p; ++p)
            lowerTheme += (char)std::tolower((unsigned char)*p);
        g_free(themeName);
    }
    return preferDark || lowerTheme.find("dark") != std::string::npos;
}

static void SetLinuxWindowIcon(GtkWidget* window, const char* argv0) {
    const char* iconName = IsLinuxDarkTheme() ? "vertex_icon_light.png" : "vertex_icon.png";
    std::vector<std::string> candidates;
    if (argv0 && *argv0) {
        std::string exe(argv0);
        size_t slash = exe.find_last_of('/');
        if (slash != std::string::npos)
            candidates.push_back(exe.substr(0, slash + 1) + iconName);
    }
    candidates.push_back(iconName);
    candidates.push_back(std::string("src/platform/") + iconName);

    for (const auto& path : candidates) {
        if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) continue;
        GError* error = nullptr;
        if (gtk_window_set_icon_from_file(GTK_WINDOW(window), path.c_str(), &error)) {
            if (error) g_error_free(error);
            return;
        }
        if (error) g_error_free(error);
    }
}

static void OnLinuxThemeChanged(GtkSettings*, GParamSpec*, gpointer) {
    if (g_window)
        SetLinuxWindowIcon(g_window, nullptr);
}
static Semaphore g_imageFetchGate(6);

static std::unique_ptr<IPlatformRenderer> g_renderer;
static std::unique_ptr<PlatTextMeasure> g_measure;
static std::unique_ptr<LayoutBox> g_layoutRoot;
static std::map<std::string, PlatBitmap> g_images;
static std::set<std::string> g_loadingImages;
static std::set<std::string> g_failedImages;
static std::map<std::string, PlatFont> g_fontCache;

// <canvas> 2D backend. Owns each canvas element's Cairo surface (content
// persists across repaints, unlike PaintState which is rebuilt per frame);
// g_canvasBitmaps mirrors the current PlatBitmap for each, rebuilt right
// before painting since Resize() (canvas.width/height writes) swaps in a
// new cairo_surface_t* that this cache would otherwise go stale against.
static std::map<const Node*, std::unique_ptr<CairoCanvasSurface>> g_canvasSurfaces;
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
    auto surface = std::make_unique<CairoCanvasSurface>(w, h,
        [](const std::string& url) -> cairo_surface_t* {
            auto imgIt = g_images.find(url);
            return imgIt != g_images.end() ? (cairo_surface_t*)imgIt->second : nullptr;
        });
    ICanvasSurface* raw = surface.get();
    g_canvasSurfaces[n] = std::move(surface);
    return raw;
}

static Tab& CurTab() { return g_tabs[g_activeTab]; }

static std::string CssRgb(vertex::chrome_theme::Rgb c) {
    return "rgb(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ")";
}

static const char* UrlBadgeText(const std::string& url) {
    if (url.rfind("vertex://", 0) == 0 || url.rfind("felix://", 0) == 0) return "H";
    if (url.rfind("https://", 0) == 0) return "S";
    if (url.rfind("http://", 0) == 0) return "i";
    return "?";
}

static void SetUrlBadge(const std::string& url) {
    if (g_urlBadge)
        gtk_label_set_text(GTK_LABEL(g_urlBadge), UrlBadgeText(url));
}

static void AddStyleClass(GtkWidget* widget, const char* className) {
    if (!widget) return;
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), className);
}

static void ApplyChromeTheme(
    GtkWidget* window,
    GtkWidget* toolbar,
    GtkWidget* status,
    GtkWidget* urlEntry,
    GtkWidget* urlBadge,
    const std::vector<GtkWidget*>& buttons) {
    using namespace vertex::chrome_theme;
    AddStyleClass(toolbar, "vertex-toolbar");
    AddStyleClass(status, "vertex-status");
    AddStyleClass(urlEntry, "vertex-url");
    AddStyleClass(urlBadge, "vertex-url-badge");
    for (GtkWidget* button : buttons) {
        AddStyleClass(button, "vertex-command");
        gtk_widget_set_size_request(button, ButtonWidth, ButtonHeight);
    }
    gtk_widget_set_size_request(status, -1, StatusHeight);

    GtkCssProvider* provider = gtk_css_provider_new();
    std::string css =
        ".vertex-toolbar {"
        " background: " + CssRgb(Panel) + ";"
        " padding: " + std::to_string(Margin) + "px;"
        "}"
        ".vertex-command {"
        " min-width: " + std::to_string(ButtonWidth) + "px;"
        " min-height: " + std::to_string(ButtonHeight) + "px;"
        " padding: 0 8px;"
        " border-radius: " + std::to_string(CornerRadius) + "px;"
        " border: 1px solid " + CssRgb(Line) + ";"
        " background: " + CssRgb(Active) + ";"
        " color: " + CssRgb(Ink) + ";"
        " font-weight: 600;"
        "}"
        ".vertex-command:disabled {"
        " background: " + CssRgb(Disabled) + ";"
        " color: " + CssRgb(DisabledText) + ";"
        "}"
        ".vertex-command:hover {"
        " background: " + CssRgb(Hover) + ";"
        " border-color: " + CssRgb(Accent) + ";"
        "}"
        ".vertex-command:active { background: " + CssRgb(Pressed) + "; }"
        ".vertex-url {"
        " min-height: " + std::to_string(ButtonHeight) + "px;"
        " padding: 0 10px;"
        " border-radius: " + std::to_string(CornerRadius) + "px;"
        " border: 1px solid " + CssRgb(Line) + ";"
        " background: " + CssRgb(Active) + ";"
        " color: " + CssRgb(Ink) + ";"
        " font-size: 14px;"
        "}"
        ".vertex-url:focus {"
        " border-color: " + CssRgb(Accent) + ";"
        " box-shadow: 0 0 0 2px " + CssRgb(AccentSoft) + ";"
        "}"
        ".vertex-url-badge {"
        " min-width: 22px;"
        " min-height: " + std::to_string(ButtonHeight) + "px;"
        " color: " + CssRgb(Accent) + ";"
        " font-weight: 700;"
        "}"
        ".vertex-status {"
        " background: " + CssRgb(Rail) + ";"
        " color: " + CssRgb(Quiet) + ";"
        " padding: 3px " + std::to_string(Margin) + "px;"
        " font-size: 11px;"
        "}";
    gtk_css_provider_load_from_data(provider, css.c_str(), -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gtk_widget_get_screen(window),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// ── image loading pipeline ───────────────────────────────────────────────────

struct LinuxImageMsg {
    std::string url;
    std::vector<uint8_t> bytes;
};

static void ProcessImage(const std::string& url, const std::vector<uint8_t>& bytes) {
    if (!g_renderer || bytes.empty()) { g_failedImages.insert(url); return; }
    auto looksLikeSvgUrl = [](const std::string& u) {
        std::string low;
        for (char c : u) low += (char)std::tolower((unsigned char)c);
        return low.find(".svg") != std::string::npos
            || low.find("image/svg+xml") != std::string::npos;
    };
    int w = 0, h = 0, channels = 0;
    unsigned char* stbiPixels = nullptr;
    std::vector<uint8_t> svgPixels;
    uint8_t* pixels = nullptr;
    bool fromStbi = false;
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
        stbiPixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &channels, 4);
        pixels = stbiPixels;
        fromStbi = true;
    }
    if (!pixels || w <= 0 || h <= 0) { if (stbiPixels) stbi_image_free(stbiPixels); g_failedImages.insert(url); return; }
    // stb_image outputs RGBA; Cairo wants ARGB32 (BGRA premultiplied on little-endian).
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
    if (fromStbi) stbi_image_free(stbiPixels);
    if (bmp) {
        auto it = g_images.find(url);
        if (it != g_images.end() && it->second) g_renderer->ReleaseBitmap(it->second);
        g_images[url] = bmp;
        g_measure->loadedImages[url] = { (float)w, (float)h };
        g_layoutRoot.reset();  // force relayout
        if (g_drawingArea) gtk_widget_queue_draw(g_drawingArea);
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
        auto* msg = new LinuxImageMsg{ url, {} };
        if (res.success && !res.body.empty())
            msg->bytes = std::vector<uint8_t>(res.body.begin(), res.body.end());
        g_idle_add([](gpointer data) -> gboolean {
            auto* m = static_cast<LinuxImageMsg*>(data);
            g_loadingImages.erase(m->url);
            ProcessImage(m->url, m->bytes);
            delete m;
            return G_SOURCE_REMOVE;
        }, msg);
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

// ── drawing ──────────────────────────────────────────────────────────────────

static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    (void)data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int w = alloc.width, h = alloc.height;

    if (!g_renderer) {
        g_renderer = CreatePlatformRenderer();
        g_renderer->Init(widget);
        g_measure = std::make_unique<PlatTextMeasure>(g_renderer.get());
        g_measure->onImageRequest = FetchImageAsync;
    }
    g_renderer->Resize(w, h);
    g_renderer->SetNativeContext(cr);
    g_renderer->Clear({1, 1, 1, 1});

    if (g_tabs.empty() || !CurTab().page || !CurTab().page->dom) {
        PlatFont font = g_renderer->CreateFont(16, false, false, false, "");
        g_renderer->DrawText(CurTab().loading ? L"Loading..." : L"Navigate to a URL",
                             20, 20, 800, 30, font, {0.5f, 0.5f, 0.5f, 1});
        g_renderer->ReleaseFont(font);
        return FALSE;
    }

    Tab& tab = CurTab();
    try {
        Stylesheet sheet = CollectCSS(tab.page->dom.get());
        if (!sheet.fontFaces.empty()) {
            // loadFonts() dedupes by resolved URL internally, so calling it
            // every paint (this function has no doc-level style cache to
            // gate it on, unlike Windows' Renderer) is cheap after the first
            // frame. The download runs on a background thread; hop back to
            // the GTK main loop before touching g_drawingArea.
            WebFontLoader::instance().loadFonts(sheet, tab.page->url, []() {
                g_idle_add([](gpointer) -> gboolean {
                    if (g_drawingArea) gtk_widget_queue_draw(g_drawingArea);
                    return G_SOURCE_REMOVE;
                }, nullptr);
            });
        }
        LayoutInput in;
        in.document = tab.page->dom.get();
        in.sheet = &sheet;
        in.measure = g_measure.get();
        in.viewportW = (float)w;
        in.viewportH = (float)h;
        in.zoom = 1.f;
        in.baseUrl = tab.page->url;
        g_layoutRoot = LayoutDocument(in);
        if (g_layoutRoot) {
            std::vector<HitRegion> hits;
            PaintState ps;
            ps.r = g_renderer.get();
            ps.scrollY = tab.scrollY;
            ps.topInset = 0;
            ps.baseUrl = tab.page->url;
            ps.images = &g_images;
            // Rebuild from the live surfaces (not cached) since Resize()
            // (canvas.width/height writes) swaps in a new cairo_surface_t*
            // each time — reading it fresh here avoids ever compositing a
            // stale/dangling pointer.
            g_canvasBitmaps.clear();
            for (auto& [node, surface] : g_canvasSurfaces)
                g_canvasBitmaps[node] = surface->GetSurface();
            ps.canvasSurfaces = &g_canvasBitmaps;
            ps.hits = &hits;
            ps.fontCache = &g_fontCache;
            ps.form = &g_formState;
            PaintBoxTree(ps, *g_layoutRoot);
            tab.docHeight = g_layoutRoot->contentH + 32.f;
        }
    } catch (...) { /* keep the browser alive */ }
    return FALSE;
}

// ── URL bar ──────────────────────────────────────────────────────────────────

// Platform-owned fetch: called by g_chrome.onNavigateRequested.
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
        // Post result back to GTK main thread with the correct tab index.
        struct Msg { int idx; Page* p; };
        auto* msg = new Msg{tabIdx, page};
        g_idle_add([](gpointer data) -> gboolean {
            auto* m = static_cast<Msg*>(data);
            g_chrome.onPageReady(m->idx, m->p);
            gtk_window_set_title(GTK_WINDOW(g_window), g_chrome.state.title().c_str());
            gtk_widget_queue_draw(g_drawingArea);
            delete m;
            return G_SOURCE_REMOVE;
        }, msg);
    }).detach();
}

static void on_url_activate(GtkEntry* entry, gpointer data) {
    (void)data;
    const gchar* text = gtk_entry_get_text(entry);
    if (!text || g_tabs.empty()) return;
    g_chrome.navigate(std::string(text));
    gtk_widget_queue_draw(g_drawingArea);
}

// ── toolbar buttons ──────────────────────────────────────────────────────────

static void on_back(GtkWidget* w, gpointer d) {
    (void)w; (void)d;
    g_chrome.back();
    gtk_entry_set_text(GTK_ENTRY(g_urlEntry), CurTab().url.c_str());
    gtk_widget_queue_draw(g_drawingArea);
}

static void on_forward(GtkWidget* w, gpointer d) {
    (void)w; (void)d;
    g_chrome.forward();
    gtk_entry_set_text(GTK_ENTRY(g_urlEntry), CurTab().url.c_str());
    gtk_widget_queue_draw(g_drawingArea);
}

static void on_reload(GtkWidget* w, gpointer d) {
    (void)w; (void)d;
    g_chrome.reload();
    gtk_widget_queue_draw(g_drawingArea);
}

static void on_home(GtkWidget* w, gpointer d) {
    (void)w; (void)d;
    g_chrome.home();
    gtk_entry_set_text(GTK_ENTRY(g_urlEntry), "vertex://home");
    SetUrlBadge("vertex://home");
    gtk_widget_queue_draw(g_drawingArea);
}

// ── scroll ───────────────────────────────────────────────────────────────────

static gboolean on_scroll(GtkWidget* widget, GdkEventScroll* event, gpointer data) {
    (void)widget; (void)data;
    if (g_tabs.empty()) return FALSE;
    if (event->direction == GDK_SCROLL_UP)        CurTab().scrollY -= 60.f;
    else if (event->direction == GDK_SCROLL_DOWN) CurTab().scrollY += 60.f;
    else if (event->direction == GDK_SCROLL_SMOOTH) CurTab().scrollY -= (float)event->delta_y * 30.f;
    if (CurTab().scrollY < 0) CurTab().scrollY = 0;
    gtk_widget_queue_draw(g_drawingArea);
    return TRUE;
}

static gboolean on_button_press(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    (void)widget; (void)data;
    if (event->button != 1 || g_tabs.empty()) return FALSE;
    float px = (float)event->x, py = (float)event->y;
    if (g_layoutRoot) {
        Node* input = FormState::hitTestInput(*g_layoutRoot, px, py, CurTab().scrollY, 0);
        if (input) {
            g_formState.focus(input);
            gtk_widget_set_can_focus(g_drawingArea, TRUE);
            gtk_widget_grab_focus(g_drawingArea);
            gtk_widget_queue_draw(g_drawingArea);
            return TRUE;
        }
        g_formState.blur();
        gtk_widget_queue_draw(g_drawingArea);
    }
    return FALSE;
}

static gboolean on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer data) {
    (void)widget; (void)data;
    if (!g_formState.focusedInput) return FALSE;
    guint key = event->keyval;
    if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
        std::string url = g_formState.buildFormQuery();
        if (!url.empty()) {
            g_formState.blur();
            // Resolve against current page URL.
            if (!url.empty() && url[0] == '/') {
                std::string base = CurTab().page ? CurTab().page->url : "";
                size_t scheme = base.find("://");
                if (scheme != std::string::npos) {
                    size_t slash = base.find('/', scheme + 3);
                    url = base.substr(0, slash) + url;
                }
            }
            gtk_entry_set_text(GTK_ENTRY(g_urlEntry), url.c_str());
            on_url_activate(GTK_ENTRY(g_urlEntry), nullptr);
        }
        return TRUE;
    }
    if (key == GDK_KEY_BackSpace) { g_formState.backspace(); gtk_widget_queue_draw(g_drawingArea); return TRUE; }
    if (key == GDK_KEY_Delete)    { g_formState.deleteChar(); gtk_widget_queue_draw(g_drawingArea); return TRUE; }
    if (key == GDK_KEY_Left && g_formState.cursorPos > 0) { g_formState.cursorPos--; gtk_widget_queue_draw(g_drawingArea); return TRUE; }
    if (key == GDK_KEY_Right) {
        std::string v = g_formState.getValue(g_formState.focusedInput);
        if (g_formState.cursorPos < v.size()) g_formState.cursorPos++;
        gtk_widget_queue_draw(g_drawingArea); return TRUE;
    }
    if (key == GDK_KEY_Home) { g_formState.cursorPos = 0; gtk_widget_queue_draw(g_drawingArea); return TRUE; }
    if (key == GDK_KEY_End) { g_formState.cursorPos = g_formState.getValue(g_formState.focusedInput).size(); gtk_widget_queue_draw(g_drawingArea); return TRUE; }
    if (key == GDK_KEY_Escape) { g_formState.blur(); gtk_widget_queue_draw(g_drawingArea); return TRUE; }
    // Printable character.
    guint32 uc = gdk_keyval_to_unicode(key);
    if (uc >= 32 && uc < 127) {
        g_formState.insertChar((char)uc);
        gtk_widget_queue_draw(g_drawingArea);
        return TRUE;
    }
    return FALSE;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    if (GtkSettings* settings = gtk_settings_get_default()) {
        g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme",
            G_CALLBACK(OnLinuxThemeChanged), nullptr);
        g_signal_connect(settings, "notify::gtk-theme-name",
            G_CALLBACK(OnLinuxThemeChanged), nullptr);
    }

    // Auto-update: apply staged update, then check for new one in background.
    {
        std::string exePath = "/proc/self/exe";
        char buf[4096] = {};
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) { buf[len] = 0; exePath = buf; }
        Updater::applyPendingUpdate(exePath);
        g_updater.onStatusChanged = []() {
            if (g_statusLabel)
                gtk_label_set_text(GTK_LABEL(g_statusLabel), g_updater.statusMessage.c_str());
        };
        g_updater.checkForUpdateAsync(exePath);
    }

    // Window
    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "Vertex");
    SetLinuxWindowIcon(g_window, argv[0]);
    gtk_window_set_default_size(GTK_WINDOW(g_window), 1280, 800);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    // Vertical box: toolbar + drawing area + status bar
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g_window), vbox);

    // Toolbar
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, vertex::chrome_theme::Gap);
    gtk_widget_set_margin_start(toolbar, 0);
    gtk_widget_set_margin_end(toolbar, 0);
    gtk_widget_set_margin_top(toolbar, 0);
    gtk_widget_set_margin_bottom(toolbar, 0);

    // Adwaita symbolic icons — GTK's native icon theme, matching the same
    // "use the OS's own icon system" approach as Windows' Segoe MDL2 Assets
    // toolbar glyphs. Symbolic icons recolor to the widget's CSS `color`
    // automatically, so .vertex-command's existing hover/active/disabled
    // theming (main_linux.cpp:174-192) applies unchanged.
    GtkWidget* backBtn   = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
    GtkWidget* fwdBtn    = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
    GtkWidget* reloadBtn = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
    GtkWidget* homeBtn   = gtk_button_new_from_icon_name("go-home-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
    g_signal_connect(backBtn,   "clicked", G_CALLBACK(on_back), NULL);
    g_signal_connect(fwdBtn,    "clicked", G_CALLBACK(on_forward), NULL);
    g_signal_connect(reloadBtn, "clicked", G_CALLBACK(on_reload), NULL);
    g_signal_connect(homeBtn,   "clicked", G_CALLBACK(on_home), NULL);
    std::vector<GtkWidget*> chromeButtons = { backBtn, fwdBtn, reloadBtn, homeBtn };

    g_urlEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_urlEntry), "Enter URL or search...");
    g_signal_connect(g_urlEntry, "activate", G_CALLBACK(on_url_activate), NULL);
    gtk_widget_set_hexpand(g_urlEntry, TRUE);
    g_urlBadge = gtk_label_new("H");

    gtk_box_pack_start(GTK_BOX(toolbar), backBtn,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), fwdBtn,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), reloadBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), homeBtn,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), g_urlBadge, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), g_urlEntry, TRUE,  TRUE,  0);

    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    // Drawing area
    g_drawingArea = gtk_drawing_area_new();
    gtk_widget_set_vexpand(g_drawingArea, TRUE);
    gtk_widget_set_hexpand(g_drawingArea, TRUE);
    gtk_widget_add_events(g_drawingArea,
        GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK
        | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(g_drawingArea, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(g_drawingArea, "scroll-event", G_CALLBACK(on_scroll), NULL);
    g_signal_connect(g_drawingArea, "button-press-event", G_CALLBACK(on_button_press), NULL);
    g_signal_connect(g_drawingArea, "key-press-event", G_CALLBACK(on_key_press), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_drawingArea, TRUE, TRUE, 0);

    // Status bar
    g_statusLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_statusLabel), 0);
    gtk_widget_set_margin_start(g_statusLabel, 0);
    ApplyChromeTheme(g_window, toolbar, g_statusLabel, g_urlEntry, g_urlBadge, chromeButtons);
    gtk_box_pack_start(GTK_BOX(vbox), g_statusLabel, FALSE, FALSE, 0);

    // Wire chrome callbacks.
    g_chrome.cb.repaint = []() { if (g_drawingArea) gtk_widget_queue_draw(g_drawingArea); };
    g_chrome.cb.setTitle = [](const std::string& t) { if (g_window) gtk_window_set_title(GTK_WINDOW(g_window), t.c_str()); };
    g_chrome.cb.setAddressText = [](const std::string& u) {
        if (g_urlEntry) gtk_entry_set_text(GTK_ENTRY(g_urlEntry), u.c_str());
        SetUrlBadge(u);
    };
    g_chrome.cb.setStatusText = [](const std::string& s) { if (g_statusLabel) gtk_label_set_text(GTK_LABEL(g_statusLabel), s.c_str()); };
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
        if (g_drawingArea) gtk_widget_queue_draw(g_drawingArea);
    };
    g_chrome.onNavigateRequested = platformFetch;
    g_chrome.init();

    gtk_widget_show_all(g_window);
    gtk_window_present(GTK_WINDOW(g_window));
    gtk_window_set_keep_above(GTK_WINDOW(g_window), TRUE);
    // Release keep-above after a brief delay so the window lands on top once
    g_timeout_add(500, [](gpointer data) -> gboolean {
        gtk_window_set_keep_above(GTK_WINDOW(data), FALSE);
        return G_SOURCE_REMOVE;
    }, g_window);
    g_timeout_add(16, [](gpointer) -> gboolean {
        g_chrome.pumpJs();
        return G_SOURCE_CONTINUE;
    }, nullptr);
    gtk_main();
    return 0;
}

#endif // __linux__
