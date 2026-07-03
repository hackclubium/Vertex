#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#pragma comment(lib, "comctl32.lib")

#include "network/resource_cache.h"
#include "network/url.h"
#include "html/parser.h"
#include "html/resources.h"
#include "network/text_decode.h"
#include "layout/scroll.h"
#include "render/renderer.h"
#include "platform/form_state.h"
#include "platform/resource.h"
#include "platform/updater.h"
#include "platform/chrome.h"
#include "platform/chrome_theme.h"
#include "platform/box_painter.h"
#include "js/engine.h"
#include "js/dom_bridge.h"

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <deque>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <condition_variable>

static Semaphore g_imageFetchGate(6);

// ─── control IDs ─────────────────────────────────────────────────────────────
enum : int { IDC_BACK = 101, IDC_FWRD, IDC_REFR, IDC_STOP, IDC_HOME, IDC_URL, IDC_FIND };

// ─── custom messages ──────────────────────────────────────────────────────────
constexpr UINT WM_PAGE_READY  = WM_USER + 1;
constexpr UINT WM_IMAGE_READY = WM_USER + 2;
constexpr UINT WM_NEWTAB_NAVIGATE = WM_USER + 3;

// ─── globals ─────────────────────────────────────────────────────────────────
static HWND     g_hwnd;
static HWND     g_hwndBack, g_hwndFwrd, g_hwndRefr, g_hwndStop, g_hwndHome, g_hwndUrl;
static HWND     g_hwndUrlBadge;
static HWND     g_hwndStatus;
static HWND     g_hwndFind;
static bool     g_findVisible = false;
static Renderer g_renderer;
const Node* g_hoverNode = nullptr;
std::map<const Node*, float> g_elementScrollY;
static std::vector<ScrollableRegion> g_scrollables;

// BrowserChrome owns tabs, form state, JS engine, updater.
static BrowserChrome g_chrome;
static auto& g_tabs      = g_chrome.state.tabs;
static auto& g_activeTab = g_chrome.state.activeTab;
static auto& g_formState = g_chrome.state.form;
static auto& g_updater   = g_chrome.state.updater;
static auto& g_js        = g_chrome.state.js;

struct PendingPageScript {
    int tabIdx = -1;
    std::string pageUrl;
    std::string source;
    std::string filename;
    bool dispatchLoadEvents = false;
    bool fetchBeforeRun = false;
};

static constexpr size_t kMaxScriptsPerTimerTick = 2;
static constexpr size_t kMaxResourceCompletionsPerTimerTick = 8;
static constexpr size_t kMaxMacrotasksPerTimerTick = 8;
static std::deque<PendingPageScript> g_pendingPageScripts;

static HCURSOR  g_cursorArrow, g_cursorHand;
static HFONT    g_uiFont = nullptr;
static HFONT    g_urlFont = nullptr;
static HBRUSH   g_toolbarBrush = nullptr;
static HBRUSH   g_statusBrush = nullptr;
static HBRUSH   g_editBrush = nullptr;
static HBRUSH   g_windowBrush = nullptr;

static constexpr COLORREF ToColorRef(vertex::chrome_theme::Rgb c) {
    return RGB(c.r, c.g, c.b);
}

static constexpr COLORREF kChromeInk     = ToColorRef(vertex::chrome_theme::Ink);
static constexpr COLORREF kChromePanel   = ToColorRef(vertex::chrome_theme::Panel);
static constexpr COLORREF kChromeRail    = ToColorRef(vertex::chrome_theme::Rail);
static constexpr COLORREF kChromeActive  = ToColorRef(vertex::chrome_theme::Active);
static constexpr COLORREF kChromeHover   = ToColorRef(vertex::chrome_theme::Hover);
static constexpr COLORREF kChromePressed = ToColorRef(vertex::chrome_theme::Pressed);
static constexpr COLORREF kChromeDisabled = ToColorRef(vertex::chrome_theme::Disabled);
static constexpr COLORREF kChromeAccent  = ToColorRef(vertex::chrome_theme::Accent);
static constexpr COLORREF kChromeAccentSoft = ToColorRef(vertex::chrome_theme::AccentSoft);
static constexpr COLORREF kChromeQuiet   = ToColorRef(vertex::chrome_theme::Quiet);
static constexpr COLORREF kChromeDisabledText = ToColorRef(vertex::chrome_theme::DisabledText);
static constexpr COLORREF kChromeLine    = ToColorRef(vertex::chrome_theme::Line);

// ─── layout constants ─────────────────────────────────────────────────────────
// Layout constants from ChromeLayout (shared with chrome.h).
constexpr int TAB_H     = vertex::chrome_theme::TabHeight;
constexpr int TOOLBAR_H = vertex::chrome_theme::ToolbarHeight;
constexpr int STATUS_H  = vertex::chrome_theme::StatusHeight;
constexpr int FIND_H    = 34;   // find bar height
constexpr int TOP_INSET = TAB_H + TOOLBAR_H;  // total above content
constexpr int BTN_W     = vertex::chrome_theme::ButtonWidth;
constexpr int BTN_H     = vertex::chrome_theme::ButtonHeight;
constexpr int MARGIN    = vertex::chrome_theme::Margin;
constexpr int GAP       = vertex::chrome_theme::Gap;
constexpr int CORNER_R  = vertex::chrome_theme::CornerRadius;
constexpr int URL_BADGE_W = 28;
static HWND g_hoverChromeButton = nullptr;

// ─── active tab helpers ───────────────────────────────────────────────────────
static Tab& CurTab() { return g_tabs[g_activeTab]; }

// ─── string helpers ───────────────────────────────────────────────────────────
static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring UrlBadgeText(const std::string& url) {
    if (url.rfind("vertex://", 0) == 0 || url.rfind("felix://", 0) == 0) return L"H";
    if (url.rfind("https://", 0) == 0) return L"S";
    if (url.rfind("http://", 0) == 0) return L"i";
    return L"?";
}

static void SetUrlBadge(const std::string& url) {
    if (g_hwndUrlBadge)
        SetWindowTextW(g_hwndUrlBadge, UrlBadgeText(url).c_str());
}

static void SetUrlBar(const std::string& url) {
    SetWindowTextW(g_hwndUrl, ToWide(url).c_str());
    SetUrlBadge(url);
}
static void SetUrlBarForTab(const Tab& tab) {
    SetUrlBar(tab.displayUrl.empty() ? tab.url : tab.displayUrl);
}
static void SetStatus(const std::string& s) {
    // Show updater status when not hovering a link.
    std::string effective = (s.empty() && !g_updater.statusMessage.empty())
        ? g_updater.statusMessage
        : s;
    static std::string lastStatus;
    if (effective == lastStatus) return;
    lastStatus = effective;
    SetWindowTextW(g_hwndStatus, ToWide(effective).c_str());
}

static void UpdatePerfStatusMaybe() {
    if (getenv("VERTEX_PERF") == nullptr) return;
    const RendererTimings render = g_renderer.LastTimings();
    const ResourceCacheStats resources = ResourceCache::instance().stats();
    const JsScriptStats js = g_js.scriptStats();
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
        "style %.1fms layout %.1fms%s paint %.1fms | js parse %.1fms run %.1fms %zu/%zu | fetch %.1fms req %llu hit %llu net %llu %.1fMB",
        render.styleMs,
        render.layoutMs,
        render.layoutReused ? " cached" : "",
        render.paintMs,
        js.parseMs,
        js.compileRunMs,
        js.scriptsExecuted,
        js.scriptsAttempted,
        resources.fetchMs,
        static_cast<unsigned long long>(resources.requests),
        static_cast<unsigned long long>(resources.cacheHits),
        static_cast<unsigned long long>(resources.networkFetches),
        resources.bytesFetched / (1024.0 * 1024.0));
    SetStatus(buffer);
}
static void SetBrowserCursor(HCURSOR cursor) {
    static HCURSOR lastCursor = nullptr;
    if (cursor == lastCursor) return;
    lastCursor = cursor;
    SetCursor(cursor);
}
static void UpdateTitle() {
    std::wstring t = ToWide(CurTab().title);
    if (t.empty()) t = L"New Tab";
    SetWindowTextW(g_hwnd, (t + L" \x2014 Vertex").c_str());
}

static bool AnyTabLoading() {
    for (const auto& tab : g_tabs)
        if (tab.loading) return true;
    return false;
}

// ─── scrollbar ───────────────────────────────────────────────────────────────
static int ViewportH() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    return rc.bottom - rc.top - TOP_INSET - STATUS_H;
}
static void UpdateScrollbar() {
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin  = 0;
    si.nMax  = (int)CurTab().docHeight;
    si.nPage = (UINT)std::max(0, ViewportH());
    si.nPos  = (int)CurTab().scrollY;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
}
static void ClampScroll() {
    float maxY = std::max(0.f, CurTab().docHeight - (float)ViewportH());
    CurTab().scrollY = std::max(0.f, std::min(CurTab().scrollY, maxY));
}

static RECT ContentPaintRect() {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    rc.top = TOP_INSET;
    rc.bottom = std::max(rc.top, rc.bottom - STATUS_H - (g_findVisible ? FIND_H : 0));
    return rc;
}

static void InvalidateContent() {
    if (!g_hwnd) return;
    RECT rc = ContentPaintRect();
    InvalidateRect(g_hwnd, &rc, FALSE);
}

static RECT HoverRegionToClientRect(const HitRegion& region) {
    RECT content = ContentPaintRect();
    RECT rc{};
    rc.left = (LONG)(region.x - 3.f);
    rc.top = (LONG)(region.y - CurTab().scrollY + (float)TOP_INSET - 3.f);
    rc.right = (LONG)(region.x + region.w + 4.f);
    rc.bottom = (LONG)(region.y + region.h - CurTab().scrollY + (float)TOP_INSET + 4.f);
    rc.left = std::max(content.left, rc.left);
    rc.top = std::max(content.top, rc.top);
    rc.right = std::min(content.right, rc.right);
    rc.bottom = std::min(content.bottom, rc.bottom);
    return rc;
}

static void InvalidateHoverRegions(const HitRegion* oldRegion, const HitRegion* newRegion) {
    if (!g_hwnd) return;
    if (!oldRegion || !newRegion) {
        InvalidateContent();
        return;
    }
    RECT oldRect = HoverRegionToClientRect(*oldRegion);
    RECT newRect = HoverRegionToClientRect(*newRegion);
    RECT dirty{};
    if (UnionRect(&dirty, &oldRect, &newRect) && dirty.right > dirty.left && dirty.bottom > dirty.top)
        InvalidateRect(g_hwnd, &dirty, FALSE);
    else
        InvalidateContent();
}

static void ClearPendingPageScriptsForTab(int tabIdx) {
    g_pendingPageScripts.erase(
        std::remove_if(g_pendingPageScripts.begin(), g_pendingPageScripts.end(),
            [tabIdx](const PendingPageScript& job) { return job.tabIdx == tabIdx; }),
        g_pendingPageScripts.end());
}

static bool PendingPageScriptStillCurrent(const PendingPageScript& job) {
    return job.tabIdx >= 0
        && job.tabIdx < (int)g_tabs.size()
        && g_tabs[job.tabIdx].page
        && g_tabs[job.tabIdx].page->url == job.pageUrl;
}

static bool PendingPageScriptWaitingForFetch(const PendingPageScript& job) {
    return job.fetchBeforeRun && job.source.empty() && !job.filename.empty();
}

static void RequeueFetchedPageScript(HWND hwnd, PendingPageScript job, FetchResult res) {
    if (!PendingPageScriptStillCurrent(job)) return;
    if (res.success && !res.body.empty())
        job.source = DecodeTextToUtf8(res.body, res.contentType);
    job.fetchBeforeRun = false;
    g_pendingPageScripts.push_back(std::move(job));
    SetTimer(hwnd, 1, 16, NULL);
}

static void RunPendingPageScripts(HWND hwnd) {
    size_t ran = 0;
    while (!g_pendingPageScripts.empty() && ran < kMaxScriptsPerTimerTick) {
        PendingPageScript job = std::move(g_pendingPageScripts.front());
        g_pendingPageScripts.pop_front();
        if (!PendingPageScriptStillCurrent(job)) continue;
        try {
            if (job.dispatchLoadEvents) {
                g_js.dispatchDocumentEvent("DOMContentLoaded");
                g_js.dispatchWindowEvent("load");
            } else if (PendingPageScriptWaitingForFetch(job)) {
                FetchResourceAsync(job.filename, 1024 * 1024, ResourceKind::Script,
                    [hwnd, job = std::move(job)](FetchResult res) mutable {
                        RequeueFetchedPageScript(hwnd, std::move(job), std::move(res));
                    });
            } else {
                std::string source = std::move(job.source);
                if (!source.empty())
                    g_js.runScript(source, job.filename);
            }
        } catch (...) {
            OutputDebugStringA("[JS] Pending page script failed; continuing\n");
        }
        ++ran;
    }
    if (!g_pendingPageScripts.empty())
        SetTimer(hwnd, 1, 16, NULL);
}

// Home page HTML comes from HomePageHtml() in browser_core.h.

// ─── title extraction ────────────────────────────────────────────────────────
static std::string ExtractTitle(const Node* root) {
    if (!root) return {};
    std::function<std::string(const Node*)> find = [&](const Node* n) -> std::string {
        if (!n) return {};
        if (n->type == NodeType::Element && n->tagName == "title") {
            std::string t;
            for (auto& c : n->children)
                if (c->type == NodeType::Text) t += c->text;
            while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
            while (!t.empty() && isspace((unsigned char)t.back()))  t.pop_back();
            return t;
        }
        for (auto& c : n->children) {
            auto r = find(c.get());
            if (!r.empty()) return r;
        }
        return {};
    };
    return find(root);
}

// ─── address-bar input: URL vs. search query ──────────────────────────────────
// UrlEncodeQuery, LooksLikeUrl, TabPushHistory, UrlFragment, UrlWithoutFragment
// now come from browser_core.h via chrome.h.

// ─── navigation ──────────────────────────────────────────────────────────────
static void Navigate(int tabIdx, const std::string& rawUrl, bool pushHistory = true);
static void Navigate(const std::string& rawUrl, bool push = true) {
    Navigate(g_activeTab, rawUrl, push);
}

static void Navigate(int tabIdx, const std::string& rawUrl, bool pushHistory) {
    if (tabIdx < 0 || tabIdx >= (int)g_tabs.size()) return;
    Tab& tab = g_tabs[tabIdx];
    if (tab.loading) return;
    ClearPendingPageScriptsForTab(tabIdx);

    std::string url = rawUrl;
    tab.displayUrl.clear();

    // ── built-in: home ──────────────────────────────────────────────────
    if (url.empty() || url == "vertex://home" || url == "felix://home") {
        url = "vertex://home";
        tab.page.reset(new Page{ url, ParseHtml(HomePageHtml()), {} });
        tab.url     = url;
        tab.title   = "Vertex";
        tab.loading = false;
        tab.scrollY = 0.f;
        tab.pendingFragment.clear();
        tab.fragmentScrollPending = false;
        if (pushHistory) TabPushHistory(tab, url);
        if (tabIdx == g_activeTab) {
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            SetUrlBar(url);
            UpdateTitle();
            g_renderer.InvalidateLayout();
            UpdateScrollbar();
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
        return;
    }

    // ── built-in: history ──────────────────────────────────────────────
    if (url == "vertex://history" || url == "felix://history") {
        url = "vertex://history";
        std::string html = "<html><body><h1>History</h1>";
        bool any = false;
        for (int i = (int)tab.history.size() - 1; i >= 0; i--) {
            const auto& h = tab.history[i];
            if (h == "vertex://history") continue;
            html += "<p><a href=\"" + h + "\">" + h + "</a></p>";
            any = true;
        }
        if (!any) html += "<p>No history yet.</p>";
        html += "</body></html>";
        tab.page.reset(new Page{ url, ParseHtml(html), {} });
        tab.url     = url;
        tab.title   = "History";
        tab.loading = false;
        tab.scrollY = 0.f;
        tab.pendingFragment.clear();
        tab.fragmentScrollPending = false;
        if (pushHistory) TabPushHistory(tab, url);
        if (tabIdx == g_activeTab) {
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            SetUrlBar(url);
            UpdateTitle();
            g_renderer.InvalidateLayout();
            UpdateScrollbar();
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
        return;
    }

    // If it's a URL, ensure it has a scheme; otherwise treat it as a search
    // query and route it to Bing's server-rendered results page.
    std::string displayUrl = url;
    if (LooksLikeUrl(url)) {
        if (url.find("://") == std::string::npos)
            url = "https://" + url;
        displayUrl = url;
    } else {
        displayUrl = url;
        url = "https://www.bing.com/search?q=" + UrlEncodeQuery(url);
    }

    tab.loading    = true;
    tab.url        = url;
    tab.displayUrl = displayUrl;
    tab.title      = "Loading…";
    tab.pendingFragment = UrlFragment(url);
    tab.fragmentScrollPending = !tab.pendingFragment.empty();
    if (pushHistory) TabPushHistory(tab, url);

    if (tabIdx == g_activeTab) {
        EnableWindow(g_hwndStop, TRUE);
        EnableWindow(g_hwndRefr, FALSE);
        SetUrlBar(displayUrl);
        UpdateTitle();
        SetTimer(g_hwnd, 1, 16, NULL);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }

    HWND hwnd = g_hwnd;
    std::string fetchUrl = UrlWithoutFragment(url);
    std::thread([hwnd, url, fetchUrl, tabIdx]() {
        auto* p = new Page;
        p->url   = url;
        try {
            auto res = FetchResourceCached(fetchUrl, 12 * 1024 * 1024, ResourceKind::Document);
            if (res.success) {
                p->dom = ParseHtml(DecodeTextToUtf8(res.body, res.contentType, true));
                if (!res.finalUrl.empty() && res.finalUrl != url)
                    p->url = res.finalUrl;
                LoadExternalStylesheets(p->dom, p->url);
                LoadExternalScriptSources(p->dom, p->url);
            } else {
                p->error = res.error;
            }
        } catch (...) {
            p->dom.reset();
            p->error = "Failed to load page (internal error).";
        }
        auto* pm = new PageMsg{ tabIdx, p };
        PostMessageW(hwnd, WM_PAGE_READY, 0, (LPARAM)pm);
    }).detach();
}

static void GoBack() {
    Tab& tab = CurTab();
    if (tab.histIdx > 0) Navigate(g_activeTab, tab.history[--tab.histIdx], false);
}
static void GoForward() {
    Tab& tab = CurTab();
    if (tab.histIdx + 1 < (int)tab.history.size())
        Navigate(g_activeTab, tab.history[++tab.histIdx], false);
}

// ─── tab management ───────────────────────────────────────────────────────────
static void NewTab(const std::string& url = "vertex://home") {
    g_tabs.emplace_back();
    int idx = (int)g_tabs.size() - 1;
    g_activeTab = idx;
    InvalidateRect(g_hwnd, NULL, FALSE);
    Navigate(idx, url);
}

static void CloseTab(int idx) {
    if (g_tabs.size() <= 1) {
        Navigate("vertex://home");
        return;
    }
    g_tabs.erase(g_tabs.begin() + idx);
    if (g_activeTab >= (int)g_tabs.size())
        g_activeTab = (int)g_tabs.size() - 1;
    SetUrlBarForTab(CurTab());
    UpdateTitle();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void SwitchTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    g_activeTab = idx;
    const Tab& tab = CurTab();
    SetUrlBarForTab(tab);
    UpdateTitle();
    if (tab.loading) {
        EnableWindow(g_hwndStop, TRUE);
        EnableWindow(g_hwndRefr, FALSE);
    } else {
        EnableWindow(g_hwndStop, FALSE);
        EnableWindow(g_hwndRefr, TRUE);
    }
    ClampScroll();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ─── control layout ──────────────────────────────────────────────────────────
static void LayoutControls() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int btnY = TAB_H + (TOOLBAR_H - BTN_H) / 2;
    int x    = MARGIN;
    SetWindowPos(g_hwndBack, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    x += BTN_W + GAP;
    SetWindowPos(g_hwndFwrd, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    x += BTN_W + GAP + 4;
    SetWindowPos(g_hwndRefr, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    x += BTN_W + GAP;
    SetWindowPos(g_hwndStop, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    x += BTN_W + GAP + 4;
    SetWindowPos(g_hwndHome, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    int urlX = x + BTN_W + MARGIN + 2;
    int urlW = w - urlX - MARGIN;
    SetWindowPos(g_hwndUrlBadge, NULL, urlX + 5, btnY + 4, URL_BADGE_W - 6, BTN_H - 8, SWP_NOZORDER);
    SetWindowPos(g_hwndUrl,    NULL, urlX + URL_BADGE_W, btnY, urlW - URL_BADGE_W - 6, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndStatus, NULL, 0, h - STATUS_H, w, STATUS_H, SWP_NOZORDER);
    SetWindowPos(g_hwndFind,   NULL, 0, h - STATUS_H - FIND_H, w, FIND_H, SWP_NOZORDER);
}

static void DrawChromeButton(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focus = (dis->itemState & ODS_FOCUS) != 0;
    bool hover = (dis->hwndItem == g_hoverChromeButton);

    COLORREF fill = disabled ? kChromeDisabled
        : pressed ? kChromePressed
        : hover ? kChromeHover
        : kChromeActive;
    COLORREF stroke = focus ? kChromeAccent : kChromeLine;
    COLORREF text = disabled ? kChromeDisabledText : kChromeInk;

    HBRUSH bg = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, focus ? 2 : 1, stroke);
    HGDIOBJ oldBrush = SelectObject(dc, bg);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, CORNER_R, CORNER_R);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(bg);

    wchar_t label[16] = {};
    GetWindowTextW(dis->hwndItem, label, 16);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    HFONT oldFont = g_uiFont ? (HFONT)SelectObject(dc, g_uiFont) : nullptr;
    if (pressed) OffsetRect(&r, 0, 1);
    DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    if (oldFont) SelectObject(dc, oldFont);
}

static void InvalidateControlFrame(HWND control) {
    if (!g_hwnd || !control) return;
    RECT r{};
    GetWindowRect(control, &r);
    MapWindowPoints(NULL, g_hwnd, reinterpret_cast<POINT*>(&r), 2);
    InflateRect(&r, 4, 4);
    InvalidateRect(g_hwnd, &r, FALSE);
}

static void DrawEditFrame(HDC dc, HWND edit) {
    if (!edit) return;
    RECT r{};
    GetWindowRect(edit, &r);
    MapWindowPoints(NULL, g_hwnd, reinterpret_cast<POINT*>(&r), 2);
    InflateRect(&r, 2, 2);
    bool focused = (GetFocus() == edit);

    HBRUSH bg = CreateSolidBrush(focused ? kChromeActive : kChromeHover);
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, focused ? kChromeAccent : kChromeLine);
    HGDIOBJ oldBrush = SelectObject(dc, bg);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (focused) {
        RECT halo = r;
        InflateRect(&halo, 2, 2);
        HPEN haloPen = CreatePen(PS_SOLID, 1, kChromeAccentSoft);
        HGDIOBJ oldHaloPen = SelectObject(dc, haloPen);
        HGDIOBJ oldHaloBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, halo.left, halo.top, halo.right, halo.bottom, CORNER_R + 4, CORNER_R + 4);
        SelectObject(dc, oldHaloBrush);
        SelectObject(dc, oldHaloPen);
        DeleteObject(haloPen);
    }
    RoundRect(dc, r.left, r.top, r.right, r.bottom, CORNER_R + 2, CORNER_R + 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(bg);
}

static void DrawUrlFrame(HDC dc) {
    if (!g_hwndUrl || !g_hwndUrlBadge) return;
    RECT r{};
    GetWindowRect(g_hwndUrlBadge, &r);
    RECT edit{};
    GetWindowRect(g_hwndUrl, &edit);
    if (edit.left < r.left) r.left = edit.left;
    if (edit.top < r.top) r.top = edit.top;
    if (edit.right > r.right) r.right = edit.right;
    if (edit.bottom > r.bottom) r.bottom = edit.bottom;
    MapWindowPoints(NULL, g_hwnd, reinterpret_cast<POINT*>(&r), 2);
    InflateRect(&r, 3, 2);
    bool focused = (GetFocus() == g_hwndUrl);

    HBRUSH bg = CreateSolidBrush(focused ? kChromeActive : kChromeHover);
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, focused ? kChromeAccent : kChromeLine);
    HGDIOBJ oldBrush = SelectObject(dc, bg);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (focused) {
        RECT halo = r;
        InflateRect(&halo, 2, 2);
        HPEN haloPen = CreatePen(PS_SOLID, 1, kChromeAccentSoft);
        HGDIOBJ oldHaloPen = SelectObject(dc, haloPen);
        HGDIOBJ oldHaloBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, halo.left, halo.top, halo.right, halo.bottom, CORNER_R + 4, CORNER_R + 4);
        SelectObject(dc, oldHaloBrush);
        SelectObject(dc, oldHaloPen);
        DeleteObject(haloPen);
    }
    RoundRect(dc, r.left, r.top, r.right, r.bottom, CORNER_R + 2, CORNER_R + 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(bg);
}

static void DrawLoadingAccent(HDC dc) {
    if (!AnyTabLoading()) return;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    int w = rc.right - rc.left;
    if (w <= 0) return;
    float phase = (float)((GetTickCount() % 1400) / 1400.0);
    int segW = std::max(96, w / 5);
    int x = (int)((w + segW) * phase) - segW;
    RECT rail{ 0, TOP_INSET - 3, w, TOP_INSET };
    HBRUSH railBrush = CreateSolidBrush(kChromeAccentSoft);
    FillRect(dc, &rail, railBrush);
    DeleteObject(railBrush);
    RECT seg{ x, TOP_INSET - 3, std::min(w, x + segW), TOP_INSET };
    if (seg.right > 0) {
        if (seg.left < 0) seg.left = 0;
        HBRUSH accentBrush = CreateSolidBrush(kChromeAccent);
        FillRect(dc, &seg, accentBrush);
        DeleteObject(accentBrush);
    }
}

LRESULT CALLBACK ChromeButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && g_hoverChromeButton != hwnd) {
        HWND old = g_hoverChromeButton;
        g_hoverChromeButton = hwnd;
        if (old) InvalidateRect(old, NULL, FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
    } else if (msg == WM_MOUSELEAVE && g_hoverChromeButton == hwnd) {
        g_hoverChromeButton = nullptr;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ─── URL bar subclass ─────────────────────────────────────────────────────────
LRESULT CALLBACK UrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        wchar_t buf[2048] = {};
        GetWindowTextW(hwnd, buf, 2048);
        Navigate(ToUtf8(buf));
        return 0;
    }
    if (msg == WM_CHAR && wp == '\r') return 0;
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
        InvalidateControlFrame(hwnd);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ─── find bar helpers ─────────────────────────────────────────────────────────
static void ShowFind(bool show) {
    g_findVisible = show;
    ShowWindow(g_hwndFind, show ? SW_SHOW : SW_HIDE);
    if (show) {
        SetFocus(g_hwndFind);
        SendMessageW(g_hwndFind, EM_SETSEL, 0, -1);
    } else {
        g_renderer.SetSearchQuery(L"");
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    // Re-layout status bar (find bar sits above it)
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    SetWindowPos(g_hwndStatus, NULL, 0, h - STATUS_H, w, STATUS_H, SWP_NOZORDER);
    SetWindowPos(g_hwndFind,   NULL, 0, h - STATUS_H - FIND_H, w, FIND_H, SWP_NOZORDER);
}

static void FindNextInPage(bool backwards) {
    if (!g_findVisible) ShowFind(true);
    wchar_t buf[512] = {};
    GetWindowTextW(g_hwndFind, buf, 512);
    std::wstring query = buf;
    if (query.empty()) return;
    g_renderer.SetSearchQuery(query);
    float hitY = 0.f;
    if (g_renderer.FindTextY(query, CurTab().scrollY, backwards, hitY)) {
        CurTab().scrollY = std::max(0.f, hitY - 24.f);
        ClampScroll();
        UpdateScrollbar();
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

LRESULT CALLBACK FindProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                           UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { ShowFind(false); return 0; }
        if (wp == VK_RETURN) { return 0; }
    }
    if (msg == WM_CHAR || (msg == WM_KEYDOWN && wp != VK_ESCAPE)) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        // Update search query from current text
        wchar_t buf[512] = {};
        GetWindowTextW(hwnd, buf, 512);
        g_renderer.SetSearchQuery(buf);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return r;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
        InvalidateControlFrame(hwnd);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ─── build tab entries for renderer ──────────────────────────────────────────
static std::vector<TabEntry> BuildTabEntries() {
    std::vector<TabEntry> entries;
    entries.reserve(g_tabs.size());
    for (int i = 0; i < (int)g_tabs.size(); i++) {
        TabEntry e;
        e.title   = ToWide(g_tabs[i].title.empty() ? "New Tab" : g_tabs[i].title);
        e.active  = (i == g_activeTab);
        e.loading = g_tabs[i].loading;
        e.loadingProgress = (float)((GetTickCount() % 1200) / 1200.0);
        entries.push_back(std::move(e));
    }
    return entries;
}

// ─── WndProc ─────────────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_cursorArrow = LoadCursor(NULL, IDC_ARROW);
        g_cursorHand  = LoadCursor(NULL, IDC_HAND);

        HINSTANCE hi = GetModuleHandleW(NULL);
        auto btn = [&](LPCWSTR t, int id) {
            return CreateWindowW(L"BUTTON", t,
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
                0,0,0,0, hwnd, (HMENU)(intptr_t)id, hi, NULL);
        };
        g_hwndBack = btn(L"\x2190", IDC_BACK);
        g_hwndFwrd = btn(L"\x2192", IDC_FWRD);
        g_hwndRefr = btn(L"\x21BB", IDC_REFR);
        g_hwndStop = btn(L"\x2715", IDC_STOP);
        g_hwndHome = btn(L"\x2302", IDC_HOME);
        SetWindowSubclass(g_hwndBack, ChromeButtonProc, 11, 0);
        SetWindowSubclass(g_hwndFwrd, ChromeButtonProc, 12, 0);
        SetWindowSubclass(g_hwndRefr, ChromeButtonProc, 13, 0);
        SetWindowSubclass(g_hwndStop, ChromeButtonProc, 14, 0);
        SetWindowSubclass(g_hwndHome, ChromeButtonProc, 15, 0);

        g_hwndUrlBadge = CreateWindowW(L"STATIC", L"H",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_CENTER | SS_CENTERIMAGE,
            0,0,0,0, hwnd, NULL, hi, NULL);
        g_hwndUrl = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_URL, hi, NULL);
        g_hwndStatus = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
            0,0,0,0, hwnd, NULL, hi, NULL);

        g_hwndFind = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_FIND, hi, NULL);

        EnableWindow(g_hwndStop, FALSE);
        SetWindowSubclass(g_hwndUrl,  UrlProc,  1, 0);
        SetWindowSubclass(g_hwndFind, FindProc, 2, 0);

        g_uiFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_urlFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_toolbarBrush = CreateSolidBrush(kChromePanel);
        g_statusBrush = CreateSolidBrush(kChromeRail);
        g_editBrush = CreateSolidBrush(kChromeActive);
        SendMessageW(g_hwndUrl, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
        SendMessageW(g_hwndFind, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
        if (g_uiFont) {
            SendMessageW(g_hwndBack, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(g_hwndFwrd, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(g_hwndRefr, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(g_hwndStop, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(g_hwndHome, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(g_hwndUrlBadge, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(g_hwndStatus, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        }
        if (g_urlFont) {
            SendMessageW(g_hwndUrl, WM_SETFONT, (WPARAM)g_urlFont, TRUE);
            SendMessageW(g_hwndFind, WM_SETFONT, (WPARAM)g_urlFont, TRUE);
        }

        g_renderer.SetImageRequestCallback([hwnd](std::string url) {
            FetchResourceAsync(url, 32 * 1024 * 1024, ResourceKind::Image,
                [hwnd, url](FetchResult res) {
                auto* m = new ImageMsg;
                m->url = url;
                if (res.success && !res.body.empty()) {
                    m->bytes = std::vector<uint8_t>(res.body.begin(), res.body.end());
                }
                PostMessageW(hwnd, WM_IMAGE_READY, 0, (LPARAM)m);
            });
            SetTimer(hwnd, 1, 16, NULL);
        });

        g_renderer.Init(hwnd);

        // Start with one tab
        g_tabs.emplace_back();
        Navigate(std::string("vertex://home"));
        return 0;
    }

    case WM_SIZE: {
        UINT w = LOWORD(lp), h = HIWORD(lp);
        LayoutControls();
        g_renderer.Resize(w, h);
        ClampScroll();
        UpdateScrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        bool repaintChrome = ps.rcPaint.top < TOP_INSET;

        auto tabs = BuildTabEntries();
        Tab& cur = CurTab();
        bool repaintForFragment = false;
        if (cur.page && cur.page->dom) {
            CurTab().docHeight = g_renderer.Paint(
                cur.page->dom, cur.scrollY, cur.page->url,
                (float)TOP_INSET, (float)TAB_H, &tabs, repaintChrome);
            if (cur.fragmentScrollPending) {
                cur.fragmentScrollPending = false;
                float anchorY = 0.f;
                if (!cur.pendingFragment.empty()
                    && g_renderer.GetAnchorY(cur.pendingFragment, anchorY)) {
                    cur.docHeight = FragmentReachableDocumentHeight(
                        cur.docHeight, anchorY, (float)ViewportH());
                    cur.scrollY = std::max(0.f, anchorY - 16.f);
                    ClampScroll();
                    repaintForFragment = true;
                }
            }
        } else {
            g_renderer.Paint(nullptr, 0.f, {},
                (float)TOP_INSET, (float)TAB_H, &tabs, repaintChrome);
        }
        if (repaintChrome) {
            DrawUrlFrame(ps.hdc);
            DrawLoadingAccent(ps.hdc);
        }
        if (g_findVisible && ps.rcPaint.bottom >= ContentPaintRect().bottom)
            DrawEditFrame(ps.hdc, g_hwndFind);
        UpdateScrollbar();
        UpdatePerfStatusMaybe();
        EndPaint(hwnd, &ps);
        if (repaintForFragment) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && dis->CtlType == ODT_BUTTON) {
            DrawChromeButton(dis);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, kChromePanel);
        return (LRESULT)g_toolbarBrush;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, kChromeInk);
        SetBkColor(dc, kChromeActive);
        return (LRESULT)g_editBrush;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == g_hwndUrlBadge) {
            SetTextColor(dc, kChromeAccent);
            SetBkColor(dc, kChromeHover);
            return (LRESULT)g_editBrush;
        }
        SetTextColor(dc, kChromeQuiet);
        SetBkColor(dc, kChromeRail);
        return (LRESULT)g_statusBrush;
    }

    case WM_PAGE_READY: {
        auto* pm = reinterpret_cast<PageMsg*>(lp);
        int idx  = pm->tabIdx;
        Page* p  = pm->page;
        delete pm;

        if (idx >= 0 && idx < (int)g_tabs.size()) {
            Tab& tab   = g_tabs[idx];
            tab.page.reset(p);
            tab.scrollY = 0.f;
            tab.loading = false;
            if (p->dom) {
                tab.url = p->url;
                std::string title = ExtractTitle(p->dom.get());
                tab.title = title.empty() ? p->url : title;
            } else {
                std::string html = "<html><body><h2>Error</h2><p>"
                    + p->error + "</p></body></html>";
                tab.page->dom = ParseHtml(html);
                tab.title = "Error";
            }
        }

        if (idx == g_activeTab) {
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            SetUrlBarForTab(CurTab());
            UpdateTitle();
            g_renderer.InvalidateLayout();
            ClampScroll();
            UpdateScrollbar();

        }

        // Run <script> tags in the loaded page.
        // Pass 1: inline scripts and non-defer/async external scripts (blocking).
        // Pass 2: deferred scripts (after DOM is ready).
        if (idx >= 0 && idx < (int)g_tabs.size() && g_tabs[idx].page && g_tabs[idx].page->dom) {
            try {
                auto repaint = []() {
                    g_renderer.InvalidateLayout();
                    InvalidateContent();
                };
                DomBridgeCallbacks callbacks;
                callbacks.repaintOnly = []() {
                    InvalidateContent();
                };
                callbacks.navigate = [idx](const std::string& url, bool replace) {
                    if (idx != g_activeTab) return;
                    Navigate(g_activeTab, url, !replace);
                };
                callbacks.scrollTo = [idx](float, float y) {
                    if (idx != g_activeTab) return;
                    CurTab().scrollY = y;
                    ClampScroll();
                    UpdateScrollbar();
                    InvalidateContent();
                };
                callbacks.scrollBy = [idx](float, float dy) {
                    if (idx != g_activeTab) return;
                    CurTab().scrollY += dy;
                    ClampScroll();
                    UpdateScrollbar();
                    InvalidateContent();
                };
                callbacks.scrollIntoView = [idx](Node* target) {
                    if (idx != g_activeTab || !target) return;
                    const LayoutBox* root = g_renderer.GetLayoutRoot();
                    if (!root) return;
                    std::function<bool(const LayoutBox*, float&)> findBox =
                        [&](const LayoutBox* box, float& y) -> bool {
                            if (!box) return false;
                            if (box->node == target) {
                                y = box->y;
                                return true;
                            }
                            for (const auto& child : box->kids)
                                if (findBox(child.get(), y)) return true;
                            for (const auto& line : box->lines) {
                                for (const auto& frag : line.frags) {
                                    if (frag.src && frag.src->node == target) {
                                        y = frag.y;
                                        return true;
                                    }
                                }
                            }
                            return false;
                        };
                    float y = 0.f;
                    if (!findBox(root, y)) return;
                    CurTab().scrollY = std::max(0.f, y - 16.f);
                    ClampScroll();
                    UpdateScrollbar();
                    InvalidateContent();
                };
                g_js.setDocument(g_tabs[idx].page->dom, repaint, g_tabs[idx].page->url, std::move(callbacks));
                struct ScriptEntry { std::string source; std::string filename; bool fetchBeforeRun = false; };
                std::vector<ScriptEntry> deferred;
                std::vector<ScriptEntry> blocking;
                const std::string pageUrl = g_tabs[idx].page->url;
                ClearPendingPageScriptsForTab(idx);
                std::vector<const Node*> stack;
                stack.push_back(g_tabs[idx].page->dom.get());
                size_t scriptCount = 0;
                size_t totalScriptBytes = 0;
                while (!stack.empty()) {
                    const Node* n = stack.back();
                    stack.pop_back();
                    if (!n) continue;
                    if (n->type == NodeType::Element && n->tagName == "script") {
                        std::string type = n->attr("type");
                        bool skip = (!type.empty() && type != "text/javascript"
                                     && type != "application/javascript" && type != "module");
                        bool isDefer = !n->attr("defer").empty() || !n->attr("async").empty();
                        std::string source;
                        std::string srcUrl = n->attr("src");
                        std::string filename = "inline";
                        std::string preloadedFilename = n->attr("__vertex_script_filename");
                        if (!preloadedFilename.empty() && !skip) {
                            filename = preloadedFilename;
                            for (auto& c : n->children)
                                if (c->type == NodeType::Text) source += c->text;
                        } else if (!srcUrl.empty() && !skip) {
                            std::string resolved = ResolveUrlAgainstBase(srcUrl, g_tabs[idx].page->url);
                            filename = resolved;
                        } else {
                            for (auto& c : n->children)
                                if (c->type == NodeType::Text) source += c->text;
                        }
                        const bool fetchBeforeRun = !srcUrl.empty() && preloadedFilename.empty() && !skip;
                        totalScriptBytes += source.size();
                        if (!skip && (!source.empty() || fetchBeforeRun) && scriptCount < 192 && totalScriptBytes <= 2 * 1024 * 1024) {
                            if (isDefer && !srcUrl.empty())
                                deferred.push_back({ source, filename, fetchBeforeRun });
                            else
                                blocking.push_back({ source, filename, fetchBeforeRun });
                        }
                        ++scriptCount;
                    }
                    for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                        stack.push_back(it->get());
                }
                for (auto& script : blocking)
                    g_pendingPageScripts.push_back({ idx, pageUrl, std::move(script.source), std::move(script.filename), false, script.fetchBeforeRun });
                for (auto& script : deferred)
                    g_pendingPageScripts.push_back({ idx, pageUrl, std::move(script.source), std::move(script.filename), false, script.fetchBeforeRun });
                g_pendingPageScripts.push_back({ idx, pageUrl, {}, "__vertex_load_events__", true, false });
                // Set up timer for macrotasks / setTimeout
                SetTimer(hwnd, 1, 16, NULL);
            } catch (...) {
                OutputDebugStringA("[JS] Page script setup failed; continuing without page scripts\n");
            }
        }

        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_IMAGE_READY: {
        auto* m = reinterpret_cast<ImageMsg*>(lp);
        g_renderer.ReceiveImage(m->url, m->bytes);
        delete m;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP:     CurTab().scrollY -= 30.f;                    break;
        case SB_LINEDOWN:   CurTab().scrollY += 30.f;                    break;
        case SB_PAGEUP:     CurTab().scrollY -= (float)si.nPage;         break;
        case SB_PAGEDOWN:   CurTab().scrollY += (float)si.nPage;         break;
        case SB_THUMBTRACK: CurTab().scrollY  = (float)si.nTrackPos;     break;
        }
        ClampScroll();
        UpdateScrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        float delta = GET_WHEEL_DELTA_WPARAM(wp) * 0.5f;
        // Check if cursor is inside a scrollable container.
        bool scrolledElement = false;
        for (auto& sr : g_scrollables) {
            if (pt.x >= sr.x && pt.x <= sr.x + sr.w && pt.y >= sr.y && pt.y <= sr.y + sr.h) {
                float maxScroll = std::max(0.f, sr.contentH - sr.h);
                float& elScroll = g_elementScrollY[sr.node];
                elScroll = std::max(0.f, std::min(elScroll - delta, maxScroll));
                scrolledElement = true;
                break;
            }
        }
        if (!scrolledElement) {
            CurTab().scrollY -= delta;
            ClampScroll();
            UpdateScrollbar();
        }
        InvalidateContent();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        // Check tab strip
        if (py < TAB_H) {
            int closeIdx = -1;
            if (g_renderer.HitTestTabClose((float)px, (float)py, closeIdx)) {
                CloseTab(closeIdx);
            } else {
                int tidx = g_renderer.HitTestTab((float)px, (float)py);
                if (tidx == -1) {
                    NewTab();
                } else if (tidx >= 0) {
                    SwitchTab(tidx);
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (py >= TOP_INSET) {
            // Check if an input was clicked.
            if (g_renderer.GetLayoutRoot()) {
                Node* input = FormState::hitTestInput(*g_renderer.GetLayoutRoot(),
                    (float)px, (float)py, CurTab().scrollY, (float)TOP_INSET);
                if (input) {
                    g_formState.focus(input);
                    InvalidateContent();
                    return 0;
                }
                g_formState.blur();
            }
            std::string href = g_renderer.HitTest((float)px, (float)py);
            if (!href.empty()) Navigate(href);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (py >= TOP_INSET) {
            std::string href = g_renderer.HitTest((float)px, (float)py);
            HMENU menu = CreatePopupMenu();
            if (!href.empty()) {
                AppendMenuW(menu, MF_STRING, 9001, L"Open Link");
                AppendMenuW(menu, MF_STRING, 9002, L"Open Link in New Tab");
                AppendMenuW(menu, MF_STRING, 9003, L"Copy Link");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            }
            AppendMenuW(menu, MF_STRING, 9010, L"Back");
            AppendMenuW(menu, MF_STRING, 9011, L"Forward");
            AppendMenuW(menu, MF_STRING, 9012, L"Reload");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, 9020, L"Add Bookmark");
            AppendMenuW(menu, MF_STRING, 9021, L"View Bookmarks");
            POINT pt = {px, py};
            ClientToScreen(hwnd, &pt);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
            switch (cmd) {
                case 9001: Navigate(href); break;
                case 9002: NewTab(href); break;
                case 9003: {
                    if (OpenClipboard(hwnd)) {
                        EmptyClipboard();
                        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, href.size() + 1);
                        if (h) { memcpy(GlobalLock(h), href.c_str(), href.size() + 1); GlobalUnlock(h);
                            SetClipboardData(CF_TEXT, h); }
                        CloseClipboard();
                    }
                    break;
                }
                case 9010: GoBack(); break;
                case 9011: GoForward(); break;
                case 9012: if (!CurTab().loading) Navigate(CurTab().url, false); break;
                case 9020: {
                    // Add current page to bookmarks file.
                    std::string bmPath;
                    char appdata[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK)
                        bmPath = std::string(appdata) + "\\Vertex\\bookmarks.txt";
                    if (!bmPath.empty()) {
                        CreateDirectoryA((std::string(appdata) + "\\Vertex").c_str(), NULL);
                        FILE* f = fopen(bmPath.c_str(), "a");
                        if (f) { fprintf(f, "%s|%s\n", CurTab().url.c_str(), CurTab().title.c_str()); fclose(f); }
                    }
                    break;
                }
                case 9021: {
                    // Load bookmarks as a simple page.
                    std::string bmPath;
                    char appdata[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK)
                        bmPath = std::string(appdata) + "\\Vertex\\bookmarks.txt";
                    std::string html = "<html><head><title>Bookmarks</title></head><body><h1>Bookmarks</h1><ul>";
                    if (!bmPath.empty()) {
                        FILE* f = fopen(bmPath.c_str(), "r");
                        if (f) {
                            char line[4096];
                            while (fgets(line, sizeof(line), f)) {
                                std::string l(line); while (!l.empty() && (l.back()=='\n'||l.back()=='\r')) l.pop_back();
                                size_t pipe = l.find('|');
                                std::string url = pipe != std::string::npos ? l.substr(0, pipe) : l;
                                std::string title = pipe != std::string::npos ? l.substr(pipe + 1) : url;
                                html += "<li><a href=\"" + url + "\">" + title + "</a></li>";
                            }
                            fclose(f);
                        }
                    }
                    html += "</ul></body></html>";
                    Tab& tab = CurTab();
                    tab.page = std::make_shared<Page>();
                    tab.page->url = "vertex://bookmarks";
                    tab.page->dom = ParseHtml(html);
                    tab.title = "Bookmarks";
                    tab.url = "vertex://bookmarks";
                    g_renderer.InvalidateLayout();
                    InvalidateRect(hwnd, NULL, FALSE);
                    UpdateTitle();
                    break;
                }
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (py >= TOP_INSET) {
            std::string href = g_renderer.HitTest((float)px, (float)py);
            SetBrowserCursor(href.empty() ? g_cursorArrow : g_cursorHand);
            SetStatus(href);
            // Track :hover node for CSS hover styles (throttled to ~30Hz).
            if (g_renderer.GetLayoutRoot() && g_renderer.UsesHoverStyles()) {
                static DWORD lastHoverTick = 0;
                DWORD now = GetTickCount();
                if (now - lastHoverTick >= 33) { // ~30Hz
                    HitRegion oldHoverRegion{};
                    bool hadOldHoverRegion = g_renderer.LastHoverRegion(oldHoverRegion);
                    const Node* hover = g_renderer.HoverNodeAt(
                        (float)px, (float)py, CurTab().scrollY, (float)TOP_INSET);
                    lastHoverTick = now;
                    if (hover != g_hoverNode) {
                        HitRegion newHoverRegion{};
                        bool hasNewHoverRegion = g_renderer.LastHoverRegion(newHoverRegion);
                        g_hoverNode = hover;
                        // Only invalidate paint, not layout — hover mostly affects
                        // colors/opacity, not geometry. Full layout rebuild is expensive.
                        InvalidateHoverRegions(
                            hadOldHoverRegion ? &oldHoverRegion : nullptr,
                            hasNewHoverRegion ? &newHoverRegion : nullptr);
                    }
                }
            } else if (g_hoverNode) {
                g_hoverNode = nullptr;
                InvalidateContent();
            }
        } else {
            SetBrowserCursor(g_cursorArrow);
            SetStatus({});
            if (g_hoverNode) {
                g_hoverNode = nullptr;
                InvalidateContent();
            }
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BACK: GoBack();    break;
        case IDC_FWRD: GoForward(); break;
        case IDC_HOME: Navigate("vertex://home"); break;
        case IDC_REFR:
            if (CurTab().page) Navigate(CurTab().url, false);
            break;
        case IDC_STOP:
            CurTab().loading = false;
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        return 0;

    case WM_TIMER:
        resetDomDirtyCoalesce(); // Allow next batch of DOM mutations to trigger repaint.
        if (DrainResourceCompletions(kMaxResourceCompletionsPerTimerTick) > 0) {
            InvalidateContent();
        }
        RunPendingPageScripts(hwnd);
        try {
            g_js.runMacrotasks(kMaxMacrotasksPerTimerTick);
        } catch (...) {
            OutputDebugStringA("[JS] Macrotask pump failed; timer stopped\n");
            KillTimer(hwnd, 1);
        }
        if (AnyTabLoading()) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            rc.bottom = TOP_INSET;
            InvalidateRect(hwnd, &rc, FALSE);
        } else if (g_pendingPageScripts.empty() && !g_js.hasPendingMacrotasks() && !HasPendingResourceCompletions()) {
            KillTimer(hwnd, 1);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        if (g_uiFont) { DeleteObject(g_uiFont); g_uiFont = nullptr; }
        if (g_urlFont) { DeleteObject(g_urlFont); g_urlFont = nullptr; }
        if (g_toolbarBrush) { DeleteObject(g_toolbarBrush); g_toolbarBrush = nullptr; }
        if (g_statusBrush) { DeleteObject(g_statusBrush); g_statusBrush = nullptr; }
        if (g_editBrush) { DeleteObject(g_editBrush); g_editBrush = nullptr; }
        if (g_windowBrush) { DeleteObject(g_windowBrush); g_windowBrush = nullptr; }
        PostQuitMessage(0);
        return 0;
    }

    // Form input keyboard handling: route chars to focused input.
    if (g_formState.focusedInput) {
        if (msg == WM_CHAR) {
            if (wp == '\r') {
                // Enter in a form input: submit the form via GET.
                std::string url = g_formState.buildFormQuery();
                if (!url.empty()) {
                    g_formState.blur();
                    Navigate(url);
                }
            } else if (wp == '\b') {
                g_formState.backspace();
                InvalidateContent();
            } else if (wp >= 32) {
                g_formState.insertChar((char)wp);
                InvalidateContent();
            }
            return 0;
        }
        if (msg == WM_KEYDOWN) {
            if (wp == VK_LEFT && g_formState.cursorPos > 0) {
                g_formState.cursorPos--;
                InvalidateContent();
                return 0;
            }
            if (wp == VK_RIGHT) {
                std::string v = g_formState.getValue(g_formState.focusedInput);
                if (g_formState.cursorPos < v.size()) g_formState.cursorPos++;
                InvalidateContent();
                return 0;
            }
            if (wp == VK_DELETE) {
                g_formState.deleteChar();
                InvalidateContent();
                return 0;
            }
            if (wp == VK_HOME) { g_formState.cursorPos = 0; InvalidateContent(); return 0; }
            if (wp == VK_END) {
                g_formState.cursorPos = g_formState.getValue(g_formState.focusedInput).size();
                InvalidateContent(); return 0;
            }
            if (wp == VK_ESCAPE) { g_formState.blur(); InvalidateContent(); return 0; }
        }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── entry point ─────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Auto-update: apply a previously downloaded update, then check for new ones.
    char exeBuf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
    std::string exePath(exeBuf);
    Updater::applyPendingUpdate(exePath);
    g_updater.onStatusChanged = []() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); };
    g_updater.checkForUpdateAsync(exePath);

    WNDCLASSEXW wc   = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"VertexBrowser";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    g_windowBrush = CreateSolidBrush(kChromePanel);
    wc.hbrBackground = g_windowBrush;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_VERTEX_APP));
    wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_VERTEX_APP),
                                         IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0,
        L"VertexBrowser", L"Vertex",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 900,
        NULL, NULL, hInst, NULL);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            bool ctrl    = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt     = (GetKeyState(VK_MENU)    & 0x8000) != 0;
            bool shift   = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool handled = false;

            if (ctrl) {
                if (msg.wParam == 'L') {
                    SetFocus(g_hwndUrl);
                    SendMessageW(g_hwndUrl, EM_SETSEL, 0, -1);
                    handled = true;
                } else if (msg.wParam == 'T') {
                    NewTab();
                    handled = true;
                } else if (msg.wParam == 'W') {
                    CloseTab(g_activeTab);
                    handled = true;
                } else if (msg.wParam == VK_TAB) {
                    int next = (g_activeTab + (shift ? -1 : 1) + (int)g_tabs.size())
                             % (int)g_tabs.size();
                    SwitchTab(next);
                    handled = true;
                } else if (msg.wParam >= '1' && msg.wParam <= '9') {
                    SwitchTab((int)(msg.wParam - '1'));
                    handled = true;
                } else if (msg.wParam == 'R' || msg.wParam == VK_F5) {
                    if (!CurTab().loading) Navigate(CurTab().url, false);
                    handled = true;
                } else if (msg.wParam == 'H') {
                    Navigate("vertex://history");
                    handled = true;
                } else if (msg.wParam == 'F') {
                    ShowFind(!g_findVisible);
                    handled = true;
                } else if (msg.wParam == 'G') {
                    FindNextInPage(shift);
                    handled = true;
                } else if (msg.wParam == VK_OEM_PLUS || msg.wParam == '=') {
                    g_renderer.SetZoom(g_renderer.GetZoom() + 0.1f);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                } else if (msg.wParam == VK_OEM_MINUS) {
                    g_renderer.SetZoom(g_renderer.GetZoom() - 0.1f);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                } else if (msg.wParam == '0') {
                    g_renderer.SetZoom(1.f);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                }
            }

            if (!handled && msg.wParam == VK_F5) {
                if (!CurTab().loading) Navigate(CurTab().url, false);
                handled = true;
            }
            if (!handled && msg.wParam == VK_F12 && g_updater.updateAvailable) {
                char exeBuf[MAX_PATH] = {};
                GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
                Updater::restartToUpdate(exeBuf);
                handled = true;
            }
            if (!handled && alt) {
                if (msg.wParam == VK_LEFT)  { GoBack();    handled = true; }
                if (msg.wParam == VK_RIGHT) { GoForward(); handled = true; }
            }
            if (!handled && msg.wParam == VK_ESCAPE) {
                if (g_findVisible) {
                    ShowFind(false);
                    handled = true;
                } else if (CurTab().loading) {
                    CurTab().loading = false;
                    CurTab().title   = CurTab().url;
                    EnableWindow(g_hwndStop, FALSE);
                    EnableWindow(g_hwndRefr, TRUE);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                }
            }

            if (handled) continue;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
