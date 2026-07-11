#pragma once
//
// chrome.h — Shared browser chrome state and commands.
//
// This is the "brain" of the browser UI. It owns the tab strip, navigation
// state, address bar, find bar, status bar, zoom, and update status. It does
// NOT render web pages — that's the engine's job. It does NOT create native
// windows — that's the platform adapter's job.
//
// Each platform adapter (Win32, macOS, Linux) reads from ChromeState to know
// what to display, and sends ChromeCommands to mutate it. This keeps the
// browser's visible shell consistent across platforms while each OS only
// handles native window/input/render plumbing.
//
// Architecture:
//   BrowserCore (tabs/pages/navigation) ← already exists
//   BrowserChrome (this file) ← UI state + commands
//   Platform adapter (main_*.cpp) ← native window + input
//   Engine (layout/paint) ← page rendering
//

#include "platform/browser_core.h"
#include "platform/form_state.h"
#include "platform/updater.h"
#include "js/engine.h"
#include "js/dom_bridge.h"
#include "network/resource_cache.h"
#include "network/text_decode.h"
#include "network/websocket.h"
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <algorithm>

// ── Pending page scripts ────────────────────────────────────────────────────
// A <script> tag queued to run on the timer tick rather than synchronously
// during page-load, so a slow/huge script can't block the UI thread. Ported
// from the Windows adapter's WM_PAGE_READY/WM_TIMER handling (src/main.cpp)
// into this shared header so every platform adapter gets the same behavior
// instead of reimplementing it per OS.
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
static constexpr size_t kMaxWebSocketEventsPerTimerTick = 16;

// ── Chrome layout constants ─────────────────────────────────────────────────

struct ChromeLayout {
    float tabStripH   = 36.f;
    float toolbarH    = 44.f;
    float statusBarH  = 22.f;
    float findBarH    = 28.f;
    float buttonW     = 32.f;
    int   buttonCount = 5;  // back, forward, reload, stop, home

    float topInset() const { return tabStripH + toolbarH; }
    float contentY() const { return topInset(); }
    float contentH(float windowH) const {
        return windowH - topInset() - statusBarH;
    }
};

// ── Chrome state ────────────────────────────────────────────────────────────

struct ChromeState {
    // Tab strip
    std::vector<Tab> tabs;
    int activeTab = 0;

    // Address bar
    std::string addressText;
    bool addressFocused = false;

    // Find bar
    bool findVisible = false;
    std::string findQuery;

    // Status bar
    std::string statusText;
    std::string hoverUrl;

    // Loading
    bool loading = false;

    // Zoom
    float zoom = 1.f;

    // Update
    Updater updater;

    // Page interaction
    FormState form;
    JsEngine js;
    const Node* hoverNode = nullptr;
    std::deque<PendingPageScript> pendingScripts;

    // Layout
    ChromeLayout layout;

    // Convenience
    Tab& curTab() {
        if (tabs.empty()) {
            tabs.emplace_back();
            tabs[0].page = std::make_shared<Page>();
            tabs[0].page->url = "vertex://home";
            tabs[0].page->dom = ParseHtml(HomePageHtml());
        }
        if (activeTab < 0 || activeTab >= (int)tabs.size()) activeTab = 0;
        return tabs[activeTab];
    }

    std::string title() const {
        if (tabs.empty()) return "Vertex";
        const Tab& t = tabs[activeTab < (int)tabs.size() ? activeTab : 0];
        return t.title.empty() ? "Vertex" : t.title + " \xE2\x80\x94 Vertex";
    }

    std::string displayStatus() const {
        if (!hoverUrl.empty()) return hoverUrl;
        if (!updater.statusMessage.empty()) return updater.statusMessage;
        return statusText;
    }
};

// ── Chrome commands ─────────────────────────────────────────────────────────
// These are the actions the platform adapter can trigger. Each one mutates
// ChromeState and optionally calls platform callbacks (repaint, set title, etc.)

enum class ChromeCmd {
    Navigate,
    Back,
    Forward,
    Reload,
    Stop,
    Home,
    NewTab,
    CloseTab,
    SwitchTab,
    FocusAddress,
    SubmitAddress,
    ShowFind,
    HideFind,
    SetFindQuery,
    ZoomIn,
    ZoomOut,
    ZoomReset,
    ApplyUpdate,
};

// ── Platform callbacks ──────────────────────────────────────────────────────
// The chrome layer calls these when the platform needs to update its UI.
// Each platform adapter sets these to its own native implementations.

struct ChromeCallbacks {
    std::function<void()> repaint;         // invalidate the window
    std::function<void(const std::string&)> setTitle;
    std::function<void(const std::string&)> setAddressText;
    std::function<void(const std::string&)> setStatusText;
    std::function<void(bool)> setFindVisible;
    std::function<void()> focusAddress;
    std::function<void()> focusFind;
    // Platform-specific DOM hooks that need the platform's own layout tree /
    // rendering backend — forwarded as-is into DomBridgeCallbacks by
    // onPageReady(). Unset is fine (matches DomBridgeCallbacks' own
    // no-op-safe defaults).
    std::function<void(Node* target)> scrollIntoView;
    std::function<ICanvasSurface*(Node*)> getCanvasSurface;
    std::function<bool(Node*)> mediaPlay;
    std::function<void(Node*)> mediaPause;
    std::function<void(Node*, double)> mediaSetCurrentTime;
    std::function<double(Node*)> mediaCurrentTime;
    std::function<double(Node*)> mediaDuration;
    std::function<void(Node*, double)> mediaSetVolume;
    std::function<double(Node*)> mediaVolume;
    std::function<void(Node*, bool)> mediaSetMuted;
    std::function<bool(Node*)> mediaMuted;
    std::function<bool(Node*)> mediaPaused;
};

// ── Chrome controller ───────────────────────────────────────────────────────
// Processes commands and mutates ChromeState. This is the single place where
// "what happens when the user clicks Back" is defined.

class BrowserChrome {
public:
    ChromeState state;
    ChromeCallbacks cb;

    BrowserChrome() : alive_(std::make_shared<bool>(true)) {}
    ~BrowserChrome() { if (alive_) *alive_ = false; }

    void init() {
        state.tabs.emplace_back();
        state.tabs[0].page = std::make_shared<Page>();
        state.tabs[0].page->url = "vertex://home";
        state.tabs[0].page->dom = ParseHtml(HomePageHtml());
        state.tabs[0].title = "Vertex";
        updateTitle();
    }

    void navigate(const std::string& rawUrl) {
        navigate(state.activeTab, rawUrl, true);
    }

    void navigate(int tabIdx, const std::string& rawUrl, bool pushHistory) {
        if (tabIdx < 0 || tabIdx >= (int)state.tabs.size()) return;
        Tab& tab = state.tabs[tabIdx];

        std::string url = rawUrl;
        if (url == "vertex://home") {
            tab.page = std::make_shared<Page>();
            tab.page->url = url;
            tab.page->dom = ParseHtml(HomePageHtml());
            tab.title = "Vertex";
            tab.url = url;
            tab.loading = false;
            state.loading = false;
            tab.scrollY = 0;
            if (pushHistory) pushToHistory(tab, url);
            updateTitle();
            if (cb.setAddressText) cb.setAddressText(url);
            if (cb.repaint) cb.repaint();
            return;
        }

        // Normalize URL
        if (LooksLikeUrl(url)) {
            if (url.find("://") == std::string::npos) url = "https://" + url;
        } else {
            url = "https://duckduckgo.com/html/?q=" + UrlEncodeQuery(url);
        }

        tab.url = url;
        tab.title = "Loading...";
        tab.loading = true;
        tab.scrollY = 0;
        state.loading = true;
        if (pushHistory) pushToHistory(tab, url);
        updateTitle();
        if (cb.setAddressText) cb.setAddressText(url);
        if (cb.repaint) cb.repaint();

        // Emit navigate-requested intent. The platform adapter owns the fetch
        // thread and posts the result back on the UI thread via onPageReady().
        // This avoids lifetime/thread bugs from detached threads in the chrome.
        if (onNavigateRequested) onNavigateRequested(tabIdx, url);
    }

    void onPageReady(int tabIdx, Page* page) {
        if (tabIdx < 0 || tabIdx >= (int)state.tabs.size()) { delete page; return; }
        Tab& tab = state.tabs[tabIdx];
        tab.page = std::shared_ptr<Page>(page);
        tab.loading = false;
        state.loading = false;
        state.hoverNode = nullptr;
        state.form.blur();
        state.form.values.clear();

        // Extract title
        if (tab.page->dom) {
            std::function<std::string(const Node*)> findTitle = [&](const Node* n) -> std::string {
                if (n->tagName == "title")
                    for (auto& c : n->children) if (c->type == NodeType::Text) return c->text;
                for (auto& c : n->children) { auto t = findTitle(c.get()); if (!t.empty()) return t; }
                return "";
            };
            std::string t = findTitle(tab.page->dom.get());
            if (!t.empty()) tab.title = t;
        }
        updateTitle();

        runPageScripts(tabIdx);

        if (cb.repaint) cb.repaint();
    }

    // Called periodically (~16ms) by the platform adapter's own timer
    // mechanism (Win32 SetTimer / GTK g_timeout_add / Cocoa NSTimer) to pump
    // pending page scripts, JS macrotasks, and async resource completions.
    // Mirrors src/main.cpp's WM_TIMER handler, kept here so every platform
    // adapter gets identical behavior instead of reimplementing it. Returns
    // true if there's still pending work worth ticking again for.
    bool pumpJs() {
        resetDomDirtyCoalesce();
        if (DrainResourceCompletions(kMaxResourceCompletionsPerTimerTick) > 0) {
            if (cb.repaint) cb.repaint();
        }
        DrainWebSocketEvents(kMaxWebSocketEventsPerTimerTick);
        runPendingPageScripts();
        try {
            state.js.runMacrotasks(kMaxMacrotasksPerTimerTick);
        } catch (...) {
        }
        return !state.pendingScripts.empty() || state.js.hasPendingMacrotasks()
            || HasPendingResourceCompletions() || HasOpenWebSockets();
    }

    void back() {
        Tab& tab = state.curTab();
        if (tab.histIdx > 0)
            navigate(state.activeTab, tab.history[--tab.histIdx], false);
    }

    void forward() {
        Tab& tab = state.curTab();
        if (tab.histIdx + 1 < (int)tab.history.size())
            navigate(state.activeTab, tab.history[++tab.histIdx], false);
    }

    void reload() {
        if (!state.curTab().loading)
            navigate(state.activeTab, state.curTab().url, false);
    }

    void home() { navigate("vertex://home"); }

    int newTab(const std::string& url = "vertex://home") {
        state.tabs.emplace_back();
        int idx = (int)state.tabs.size() - 1;
        state.activeTab = idx;
        navigate(idx, url, true);
        return idx;
    }

    void closeTab(int idx) {
        if ((int)state.tabs.size() <= 1) {
            navigate("vertex://home");
            return;
        }
        if (idx >= 0 && idx < (int)state.tabs.size())
            state.tabs.erase(state.tabs.begin() + idx);
        if (state.activeTab >= (int)state.tabs.size())
            state.activeTab = (int)state.tabs.size() - 1;
        updateTitle();
        if (cb.repaint) cb.repaint();
    }

    void switchTab(int idx) {
        if (idx >= 0 && idx < (int)state.tabs.size()) {
            state.activeTab = idx;
            updateTitle();
            if (cb.setAddressText) cb.setAddressText(state.curTab().url);
            if (cb.repaint) cb.repaint();
        }
    }

    void zoomIn() { state.zoom = std::min(3.f, state.zoom + 0.1f); if (cb.repaint) cb.repaint(); }
    void zoomOut() { state.zoom = std::max(0.5f, state.zoom - 0.1f); if (cb.repaint) cb.repaint(); }
    void zoomReset() { state.zoom = 1.f; if (cb.repaint) cb.repaint(); }

    // Platform callbacks for navigation:
    // onNavigateRequested: platform fetches the URL and calls onPageReady() when done.
    // This keeps thread ownership in the platform layer, not the chrome.
    std::function<void(int tabIdx, const std::string& url)> onNavigateRequested;

private:
    void pushToHistory(Tab& tab, const std::string& url) {
        TabPushHistory(tab, url);
    }

    void updateTitle() {
        if (cb.setTitle) cb.setTitle(state.title());
    }

    // ── JS execution (ported from src/main.cpp's WM_PAGE_READY/WM_TIMER) ────

    void clearPendingScriptsForTab(int tabIdx) {
        auto& q = state.pendingScripts;
        q.erase(std::remove_if(q.begin(), q.end(),
            [tabIdx](const PendingPageScript& job) { return job.tabIdx == tabIdx; }),
            q.end());
    }

    bool pendingPageScriptStillCurrent(const PendingPageScript& job) const {
        return job.tabIdx >= 0
            && job.tabIdx < (int)state.tabs.size()
            && state.tabs[job.tabIdx].page
            && state.tabs[job.tabIdx].page->url == job.pageUrl;
    }

    static bool pendingPageScriptWaitingForFetch(const PendingPageScript& job) {
        return job.fetchBeforeRun && job.source.empty() && !job.filename.empty();
    }

    void requeueFetchedPageScript(PendingPageScript job, FetchResult res) {
        if (!pendingPageScriptStillCurrent(job)) return;
        if (res.success && !res.body.empty())
            job.source = DecodeTextToUtf8(res.body, res.contentType);
        job.fetchBeforeRun = false;
        state.pendingScripts.push_back(std::move(job));
    }

    void runPendingPageScripts() {
        size_t ran = 0;
        while (!state.pendingScripts.empty() && ran < kMaxScriptsPerTimerTick) {
            PendingPageScript job = std::move(state.pendingScripts.front());
            state.pendingScripts.pop_front();
            if (!pendingPageScriptStillCurrent(job)) continue;
            try {
                if (job.dispatchLoadEvents) {
                    state.js.dispatchDocumentEvent("DOMContentLoaded");
                    state.js.dispatchWindowEvent("load");
                } else if (pendingPageScriptWaitingForFetch(job)) {
                    auto aliveFlag = alive_;
                    FetchResourceAsync(job.filename, 1024 * 1024, ResourceKind::Script,
                        [this, aliveFlag, job](FetchResult res) mutable {
                            if (!aliveFlag || !*aliveFlag) return;
                            requeueFetchedPageScript(std::move(job), std::move(res));
                        });
                } else {
                    std::string source = std::move(job.source);
                    if (!source.empty())
                        state.js.runScript(source, job.filename);
                }
            } catch (...) {
            }
            ++ran;
        }
    }

    // Walks the loaded DOM for <script> tags, registers the DOM/window
    // globals with the JS engine, and queues blocking/deferred scripts plus
    // a final DOMContentLoaded/load dispatch — same shape as the Windows
    // adapter's WM_PAGE_READY script-setup block (src/main.cpp).
    void runPageScripts(int tabIdx) {
        if (tabIdx < 0 || tabIdx >= (int)state.tabs.size()) return;
        Tab& tab = state.tabs[tabIdx];
        if (!tab.page || !tab.page->dom) return;
        try {
            auto repaint = [this]() { if (cb.repaint) cb.repaint(); };
            DomBridgeCallbacks callbacks;
            callbacks.repaintOnly = repaint;
            callbacks.navigate = [this, tabIdx](const std::string& url, bool replace) {
                if (tabIdx != state.activeTab) return;
                navigate(state.activeTab, url, !replace);
            };
            callbacks.scrollTo = [this, tabIdx](float, float y) {
                if (tabIdx != state.activeTab || tabIdx >= (int)state.tabs.size()) return;
                state.tabs[tabIdx].scrollY = y;
                if (cb.repaint) cb.repaint();
            };
            callbacks.scrollBy = [this, tabIdx](float, float dy) {
                if (tabIdx != state.activeTab || tabIdx >= (int)state.tabs.size()) return;
                state.tabs[tabIdx].scrollY += dy;
                if (cb.repaint) cb.repaint();
            };
            callbacks.scrollIntoView = cb.scrollIntoView;
            callbacks.getCanvasSurface = cb.getCanvasSurface;
            callbacks.mediaPlay = cb.mediaPlay;
            callbacks.mediaPause = cb.mediaPause;
            callbacks.mediaSetCurrentTime = cb.mediaSetCurrentTime;
            callbacks.mediaCurrentTime = cb.mediaCurrentTime;
            callbacks.mediaDuration = cb.mediaDuration;
            callbacks.mediaSetVolume = cb.mediaSetVolume;
            callbacks.mediaVolume = cb.mediaVolume;
            callbacks.mediaSetMuted = cb.mediaSetMuted;
            callbacks.mediaMuted = cb.mediaMuted;
            callbacks.mediaPaused = cb.mediaPaused;

            state.js.setDocument(tab.page->dom, repaint, tab.page->url, std::move(callbacks));

            struct ScriptEntry { std::string source; std::string filename; bool fetchBeforeRun = false; };
            std::vector<ScriptEntry> deferred;
            std::vector<ScriptEntry> blocking;
            const std::string pageUrl = tab.page->url;
            clearPendingScriptsForTab(tabIdx);
            std::vector<const Node*> stack;
            stack.push_back(tab.page->dom.get());
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
                        filename = ResolveUrlAgainstBase(srcUrl, pageUrl);
                    } else {
                        for (auto& c : n->children)
                            if (c->type == NodeType::Text) source += c->text;
                    }
                    const bool fetchBeforeRun = !srcUrl.empty() && preloadedFilename.empty() && !skip;
                    totalScriptBytes += source.size();
                    if (!skip && (!source.empty() || fetchBeforeRun) && scriptCount < 192
                        && totalScriptBytes <= 2 * 1024 * 1024) {
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
                state.pendingScripts.push_back({ tabIdx, pageUrl, std::move(script.source),
                    std::move(script.filename), false, script.fetchBeforeRun });
            for (auto& script : deferred)
                state.pendingScripts.push_back({ tabIdx, pageUrl, std::move(script.source),
                    std::move(script.filename), false, script.fetchBeforeRun });
            state.pendingScripts.push_back({ tabIdx, pageUrl, {}, "__vertex_load_events__", true, false });
        } catch (...) {
            // Page script setup failed; continue without page scripts.
        }
    }

    std::shared_ptr<bool> alive_;
};
