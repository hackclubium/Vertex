#ifdef __APPLE__
//
// main_macos.mm — macOS application shell for Vertex browser.
//
// Creates an NSWindow with a toolbar (back/forward/reload/URL bar), a custom
// NSView for rendering, and drives the browser core (tabs, navigation, etc.).
//
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#include "platform/platform.h"
#include "platform/chrome.h"
#include "platform/chrome_theme.h"
#include "platform/box_painter.h"
#include "platform/plat_text_measure.h"
#include "network/resource_cache.h"
#include "layout/layout_engine.h"
#include "css/stylesheet.h"
#include "js/dom_bridge.h"
#include "render/canvas_coregraphics.h"
#include "render/webfont.h"
#include <map>
#include <memory>

// ── forward declarations ─────────────────────────────────────────────────────

@class VertexView;
@class VertexWindowDelegate;

// ── globals ──────────────────────────────────────────────────────────────────

static NSWindow* g_window;
static NSTextField* g_urlField;
static NSTextField* g_urlBadge;
static NSTextField* g_statusField;
static VertexView* g_view;

static BrowserChrome g_chrome;
static auto& g_tabs      = g_chrome.state.tabs;
static auto& g_activeTab = g_chrome.state.activeTab;
static auto& g_js        = g_chrome.state.js;
static auto& g_formState = g_chrome.state.form;
static auto& g_updater   = g_chrome.state.updater;
static Semaphore g_imageFetchGate(6);

static std::unique_ptr<IPlatformRenderer> g_renderer;
static std::unique_ptr<PlatTextMeasure> g_measure;
static std::unique_ptr<LayoutBox> g_layoutRoot;
static std::map<std::string, PlatBitmap> g_images;
static std::map<std::string, PlatFont> g_fontCache;

// Persistent hit regions for link-click detection (populated each paint).
static std::vector<HitRegion> g_hits;

static std::string HitTestLink(float x, float y) {
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
        if (x >= it->x && x <= it->x + it->w
         && y >= it->y && y <= it->y + it->h)
            return UnwrapBingRedirect(it->href);
    return {};
}

// <canvas> 2D backend. Owns each canvas element's CoreGraphics bitmap
// context (content persists across repaints, unlike PaintState which is
// rebuilt per frame); g_canvasBitmaps mirrors a fresh CGImageRef snapshot
// for each, rebuilt right before painting since CGImageRef is immutable
// (unlike Linux's cairo_surface_t*, which can be composited live) — the old
// snapshots must be released each time or they'd leak.
static std::map<const Node*, std::unique_ptr<CoreGraphicsCanvasSurface>> g_canvasSurfaces;
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
    auto surface = std::make_unique<CoreGraphicsCanvasSurface>(w, h,
        [](const std::string& url) -> CGImageRef {
            auto imgIt = g_images.find(url);
            return imgIt != g_images.end() ? (CGImageRef)imgIt->second : nullptr;
        });
    ICanvasSurface* raw = surface.get();
    g_canvasSurfaces[n] = std::move(surface);
    return raw;
}

static Tab& CurTab() { return g_tabs[g_activeTab]; }

static NSColor* ThemeColor(vertex::chrome_theme::Rgb c, CGFloat alpha = 1.0) {
    return [NSColor colorWithCalibratedRed:(CGFloat)c.r / 255.0
                                     green:(CGFloat)c.g / 255.0
                                      blue:(CGFloat)c.b / 255.0
                                     alpha:alpha];
}

static bool IsMacDarkAppearance() {
    NSAppearance* appearance = [NSApp effectiveAppearance];
    NSString* match = [appearance bestMatchFromAppearancesWithNames:@[
        NSAppearanceNameAqua,
        NSAppearanceNameDarkAqua
    ]];
    return [match isEqualToString:NSAppearanceNameDarkAqua];
}

static void ApplyThemedApplicationIcon() {
    NSString* fileName = IsMacDarkAppearance() ? @"vertex_icon_light.icns" : @"vertex_icon.icns";
    NSString* baseName = [fileName stringByDeletingPathExtension];
    NSURL* iconUrl = [[NSBundle mainBundle] URLForResource:baseName withExtension:@"icns"];
    NSImage* icon = iconUrl ? [[NSImage alloc] initWithContentsOfURL:iconUrl] : nil;
    if (icon)
        [NSApp setApplicationIconImage:icon];
}

static NSString* UrlBadgeText(const std::string& url) {
    if (url.rfind("vertex://", 0) == 0 || url.rfind("felix://", 0) == 0) return @"H";
    if (url.rfind("https://", 0) == 0) return @"S";
    if (url.rfind("http://", 0) == 0) return @"i";
    return @"?";
}

static void SetUrlBadge(const std::string& url) {
    if (g_urlBadge)
        [g_urlBadge setStringValue:UrlBadgeText(url)];
}

static void StyleToolbarButton(NSButton* button) {
    if (!button) return;
    using namespace vertex::chrome_theme;
    [button setBordered:NO];
    [button setWantsLayer:YES];
    button.layer.backgroundColor = [ThemeColor(Active) CGColor];
    button.layer.borderColor = [ThemeColor(Line) CGColor];
    button.layer.borderWidth = 1.0;
    button.layer.cornerRadius = CornerRadius;
    [button setFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]];
    [button setTranslatesAutoresizingMaskIntoConstraints:NO];
    [[button widthAnchor] constraintEqualToConstant:ButtonWidth].active = YES;
    [[button heightAnchor] constraintEqualToConstant:ButtonHeight].active = YES;
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

// ── VertexView (custom NSView for rendering) ──────────────────────────────────

@interface VertexView : NSView
@end

@implementation VertexView

- (BOOL)isFlipped { return YES; }  // top-left origin like Windows

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    NSRect bounds = [self bounds];
    int w = (int)bounds.size.width, h = (int)bounds.size.height;

    if (!g_renderer) {
        g_renderer = CreatePlatformRenderer();
        g_renderer->Init((__bridge void*)self);
        g_measure = std::make_unique<PlatTextMeasure>(g_renderer.get());
    }
    g_renderer->Resize(w, h);
    g_renderer->BeginFrame();
    g_renderer->Clear({1, 1, 1, 1});

    if (g_tabs.empty() || !CurTab().page || !CurTab().page->dom) {
        PlatFont font = g_renderer->CreateFont(16, false, false, false, "");
        g_renderer->DrawText(CurTab().loading ? L"Loading..." : L"Navigate to a URL",
                             20, 20, 800, 30, font, {0.5f, 0.5f, 0.5f, 1});
        g_renderer->ReleaseFont(font);
        g_renderer->EndFrame();
        return;
    }

    Tab& tab = CurTab();
    try {
        Stylesheet sheet = CollectCSS(tab.page->dom.get());
        if (!sheet.fontFaces.empty()) {
            // loadFonts() dedupes by resolved URL internally, so calling it
            // every paint (this function has no doc-level style cache to
            // gate it on, unlike Windows' Renderer) is cheap after the first
            // frame. The download runs on a background thread; hop back to
            // the main queue before touching g_view.
            WebFontLoader::instance().loadFonts(sheet, tab.page->url, []() {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (g_view) [g_view setNeedsDisplay:YES];
                });
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
            g_hits.clear();
            PaintState ps;
            ps.r = g_renderer.get();
            ps.scrollY = tab.scrollY;
            ps.topInset = 0;
            ps.baseUrl = tab.page->url;
            ps.images = &g_images;
            // Rebuild from fresh snapshots each frame — CGImageRef is
            // immutable, so last frame's images must be released before
            // creating new ones, unlike Linux's live cairo_surface_t* cache.
            for (auto& [node, bmp] : g_canvasBitmaps)
                if (bmp) CGImageRelease((CGImageRef)bmp);
            g_canvasBitmaps.clear();
            for (auto& [node, surface] : g_canvasSurfaces)
                g_canvasBitmaps[node] = surface->CreateSnapshot();
            ps.canvasSurfaces = &g_canvasBitmaps;
            ps.hits = &g_hits;
            ps.fontCache = &g_fontCache;
            ps.form = &g_formState;
            PaintBoxTree(ps, *g_layoutRoot);
            tab.docHeight = g_layoutRoot->contentH + 32.f;
        }
    } catch (...) { /* keep the browser alive */ }
    g_renderer->EndFrame();
}

- (BOOL)acceptsFirstResponder { return YES; }

- (void)mouseDown:(NSEvent*)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    if (g_layoutRoot && !g_tabs.empty()) {
        Node* input = FormState::hitTestInput(*g_layoutRoot, (float)pt.x, (float)pt.y, CurTab().scrollY, 0);
        if (input) {
            g_formState.focus(input);
            [self setNeedsDisplay:YES];
            return;
        }
        g_formState.blur();
        std::string href = HitTestLink((float)pt.x, (float)pt.y);
        if (!href.empty()) {
            g_chrome.navigate(href);
            return;
        }
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseMoved:(NSEvent*)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    std::string href;
    if (g_layoutRoot && !g_tabs.empty()) {
        href = HitTestLink((float)pt.x, (float)pt.y);
    }
    if (g_statusField) {
        NSString* str = [NSString stringWithUTF8String:href.c_str()];
        if (![str isEqualToString:[g_statusField stringValue]])
            [g_statusField setStringValue:str];
    }
    if (!href.empty())
        [[NSCursor pointingHandCursor] set];
    else
        [[NSCursor arrowCursor] set];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    NSArray* areas = [self trackingAreas];
    for (NSTrackingArea* area in areas)
        [self removeTrackingArea:area];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:[self bounds]
                                                          options:(NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect)
                                                            owner:self
                                                         userInfo:nil]];
}

- (void)keyDown:(NSEvent*)event {
    if (!g_formState.focusedInput) { [super keyDown:event]; return; }
    unsigned short kc = [event keyCode];
    if (kc == 36) { // Return
        std::string url = g_formState.buildFormQuery();
        if (!url.empty()) {
            g_formState.blur();
            if (!url.empty() && url[0] == '/') {
                std::string base = CurTab().page ? CurTab().page->url : "";
                size_t scheme = base.find("://");
                if (scheme != std::string::npos) {
                    size_t slash = base.find('/', scheme + 3);
                    url = base.substr(0, slash) + url;
                }
            }
            g_chrome.navigate(url);
        }
        return;
    }
    if (kc == 51) { g_formState.backspace(); [self setNeedsDisplay:YES]; return; } // Backspace
    if (kc == 117) { g_formState.deleteChar(); [self setNeedsDisplay:YES]; return; } // Delete
    if (kc == 123 && g_formState.cursorPos > 0) { g_formState.cursorPos--; [self setNeedsDisplay:YES]; return; } // Left
    if (kc == 124) { // Right
        std::string v = g_formState.getValue(g_formState.focusedInput);
        if (g_formState.cursorPos < v.size()) g_formState.cursorPos++;
        [self setNeedsDisplay:YES]; return;
    }
    if (kc == 53) { g_formState.blur(); [self setNeedsDisplay:YES]; return; } // Escape
    NSString* chars = [event characters];
    if ([chars length] > 0) {
        unichar uc = [chars characterAtIndex:0];
        if (uc >= 32 && uc < 127) {
            g_formState.insertChar((char)uc);
            [self setNeedsDisplay:YES];
            return;
        }
    }
    [super keyDown:event];
}

- (void)scrollWheel:(NSEvent*)event {
    if (g_tabs.empty()) return;
    CurTab().scrollY -= (float)[event scrollingDeltaY] * 3.f;
    if (CurTab().scrollY < 0) CurTab().scrollY = 0;
    [self setNeedsDisplay:YES];
}

@end

// ── VertexWindowDelegate ──────────────────────────────────────────────────────

@interface VertexWindowDelegate : NSObject <NSWindowDelegate, NSTextFieldDelegate>
@end

@implementation VertexWindowDelegate

- (void)windowWillClose:(NSNotification*)notification {
    [NSApp terminate:nil];
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    NSTextField* field = [notification object];
    if (field == g_urlField) {
        std::string url = [[field stringValue] UTF8String];
        g_chrome.navigate(url);
        [g_view setNeedsDisplay:YES];
    }
}

@end

// ── toolbar actions ──────────────────────────────────────────────────────────

@interface ToolbarTarget : NSObject
- (void)goBack:(id)sender;
- (void)goForward:(id)sender;
- (void)reload:(id)sender;
- (void)goHome:(id)sender;
@end

@implementation ToolbarTarget

- (void)goBack:(id)sender {
    g_chrome.back();
    [g_view setNeedsDisplay:YES];
}

- (void)goForward:(id)sender {
    g_chrome.forward();
    [g_view setNeedsDisplay:YES];
}

- (void)reload:(id)sender {
    g_chrome.reload();
    [g_view setNeedsDisplay:YES];
}

- (void)goHome:(id)sender {
    g_chrome.home();
    [g_view setNeedsDisplay:YES];
}

@end

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        ApplyThemedApplicationIcon();
        [[NSDistributedNotificationCenter defaultCenter]
            addObserverForName:@"AppleInterfaceThemeChangedNotification"
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                        ApplyThemedApplicationIcon();
                    }];

        // Auto-update.
        {
            std::string exePath = [[[NSBundle mainBundle] executablePath] UTF8String];
            Updater::applyPendingUpdate(exePath);
            g_updater.onStatusChanged = []() {
                if (g_statusField)
                    [g_statusField setStringValue:[NSString stringWithUTF8String:g_updater.statusMessage.c_str()]];
            };
            g_updater.checkForUpdateAsync(exePath);
        }

        // Create main menu (required for Cmd+Q to work)
        NSMenu* menubar = [[NSMenu alloc] init];
        NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:appMenuItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit Vertex" action:@selector(terminate:)
                   keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];
        [NSApp setMainMenu:menubar];

        // Window
        NSRect frame = NSMakeRect(100, 100, 1280, 800);
        g_window = [[NSWindow alloc]
            initWithContentRect:frame
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
            backing:NSBackingStoreBuffered
            defer:NO];
        [g_window setTitle:@"Vertex"];
        [g_window setAcceptsMouseMovedEvents:YES];

        VertexWindowDelegate* delegate = [[VertexWindowDelegate alloc] init];
        [g_window setDelegate:delegate];

        // Content view layout
        NSView* contentView = [g_window contentView];

        // Toolbar area (URL bar + buttons)
        static ToolbarTarget* toolbarTarget = [[ToolbarTarget alloc] init];

        NSButton* backBtn = [NSButton buttonWithTitle:@"←" target:toolbarTarget action:@selector(goBack:)];
        NSButton* fwdBtn  = [NSButton buttonWithTitle:@"→" target:toolbarTarget action:@selector(goForward:)];
        NSButton* reloadBtn = [NSButton buttonWithTitle:@"↻" target:toolbarTarget action:@selector(reload:)];
        NSButton* homeBtn = [NSButton buttonWithTitle:@"⌂" target:toolbarTarget action:@selector(goHome:)];

        StyleToolbarButton(backBtn);
        StyleToolbarButton(fwdBtn);
        StyleToolbarButton(reloadBtn);
        StyleToolbarButton(homeBtn);

        g_urlField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 800, vertex::chrome_theme::ButtonHeight)];
        [g_urlField setPlaceholderString:@"Enter URL or search..."];
        [g_urlField setDelegate:delegate];
        [g_urlField setFont:[NSFont systemFontOfSize:14]];
        [g_urlField setTextColor:ThemeColor(vertex::chrome_theme::Ink)];
        [g_urlField setBackgroundColor:ThemeColor(vertex::chrome_theme::Active)];
        [g_urlField setBezeled:YES];
        [g_urlField setFocusRingType:NSFocusRingTypeExterior];
        [g_urlField setTranslatesAutoresizingMaskIntoConstraints:NO];

        g_urlBadge = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 24, vertex::chrome_theme::ButtonHeight)];
        [g_urlBadge setStringValue:@"H"];
        [g_urlBadge setBezeled:NO];
        [g_urlBadge setEditable:NO];
        [g_urlBadge setSelectable:NO];
        [g_urlBadge setDrawsBackground:NO];
        [g_urlBadge setAlignment:NSTextAlignmentCenter];
        [g_urlBadge setFont:[NSFont systemFontOfSize:13 weight:NSFontWeightBold]];
        [g_urlBadge setTextColor:ThemeColor(vertex::chrome_theme::Accent)];
        [g_urlBadge setTranslatesAutoresizingMaskIntoConstraints:NO];
        [[g_urlBadge widthAnchor] constraintEqualToConstant:24].active = YES;
        [[g_urlBadge heightAnchor] constraintEqualToConstant:vertex::chrome_theme::ButtonHeight].active = YES;

        NSStackView* toolbar = [NSStackView stackViewWithViews:@[backBtn, fwdBtn, reloadBtn, homeBtn, g_urlBadge, g_urlField]];
        [toolbar setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
        [toolbar setSpacing:vertex::chrome_theme::Gap];
        [toolbar setEdgeInsets:NSEdgeInsetsMake(
            vertex::chrome_theme::Margin,
            vertex::chrome_theme::Margin,
            vertex::chrome_theme::Margin,
            vertex::chrome_theme::Margin)];
        [toolbar setWantsLayer:YES];
        [toolbar.layer setBackgroundColor:[ThemeColor(vertex::chrome_theme::Panel) CGColor]];
        [toolbar setTranslatesAutoresizingMaskIntoConstraints:NO];
        [g_urlField setContentHuggingPriority:NSLayoutPriorityDefaultLow
                               forOrientation:NSLayoutConstraintOrientationHorizontal];
        [g_urlField setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                             forOrientation:NSLayoutConstraintOrientationHorizontal];

        // Browser view
        g_view = [[VertexView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 760)];
        [g_view setTranslatesAutoresizingMaskIntoConstraints:NO];

        // Status bar
        g_statusField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 1280, vertex::chrome_theme::StatusHeight)];
        [g_statusField setBezeled:NO];
        [g_statusField setEditable:NO];
        [g_statusField setSelectable:NO];
        [g_statusField setDrawsBackground:YES];
        [g_statusField setBackgroundColor:ThemeColor(vertex::chrome_theme::Rail)];
        [g_statusField setFont:[NSFont systemFontOfSize:11]];
        [g_statusField setTextColor:ThemeColor(vertex::chrome_theme::Quiet)];
        [g_statusField setAlignment:NSTextAlignmentLeft];
        [[g_statusField cell] setLineBreakMode:NSLineBreakByTruncatingTail];
        [g_statusField setTranslatesAutoresizingMaskIntoConstraints:NO];

        [contentView addSubview:toolbar];
        [contentView addSubview:g_view];
        [contentView addSubview:g_statusField];

        // Auto Layout constraints
        NSDictionary* views = NSDictionaryOfVariableBindings(toolbar, g_view, g_statusField);
        NSDictionary* metrics = @{
            @"toolbarH": @(vertex::chrome_theme::ToolbarHeight),
            @"statusH": @(vertex::chrome_theme::StatusHeight)
        };
        [contentView addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"H:|[toolbar]|"
            options:0 metrics:nil views:views]];
        [contentView addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"H:|[g_view]|"
            options:0 metrics:nil views:views]];
        [contentView addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"H:|[g_statusField]|"
            options:0 metrics:nil views:views]];
        [contentView addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"V:|[toolbar(==toolbarH)][g_view][g_statusField(==statusH)]|"
            options:0 metrics:metrics views:views]];

        // Wire chrome callbacks and init.
        g_chrome.cb.repaint = []() { if (g_view) [g_view setNeedsDisplay:YES]; };
        g_chrome.cb.setTitle = [](const std::string& t) {
            if (g_window) [g_window setTitle:[NSString stringWithUTF8String:t.c_str()]];
        };
        g_chrome.cb.setAddressText = [](const std::string& u) {
            if (g_urlField) [g_urlField setStringValue:[NSString stringWithUTF8String:u.c_str()]];
            SetUrlBadge(u);
        };
        g_chrome.cb.setStatusText = [](const std::string& s) {
            if (g_statusField) [g_statusField setStringValue:[NSString stringWithUTF8String:s.c_str()]];
        };
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
            if (g_view) [g_view setNeedsDisplay:YES];
        };
        g_chrome.onNavigateRequested = [](int tabIdx, const std::string& url) {
            // macOS platform-owned fetch.
            dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                auto res = FetchResourceCached(url, 12 * 1024 * 1024, ResourceKind::Document);
                auto* page = new Page();
                page->url = url;
                if (res.success && !res.body.empty()) {
                    page->dom = ParseHtml(DecodeTextToUtf8(res.body, res.contentType, true));
                    LoadExternalStylesheets(page->dom, page->url);
                } else {
                    page->error = res.error;
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    g_chrome.onPageReady(tabIdx, page);
                    [g_view setNeedsDisplay:YES];
                });
            });
        };
        g_chrome.init();
        [NSTimer scheduledTimerWithTimeInterval:0.016 repeats:YES block:^(NSTimer*) {
            g_chrome.pumpJs();
        }];

        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}

#endif // __APPLE__
