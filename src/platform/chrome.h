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
#include <string>
#include <vector>
#include <functional>

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
};

// ── Chrome controller ───────────────────────────────────────────────────────
// Processes commands and mutates ChromeState. This is the single place where
// "what happens when the user clicks Back" is defined.

class BrowserChrome {
public:
    ChromeState state;
    ChromeCallbacks cb;

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
            url = "https://www.bing.com/search?q=" + UrlEncodeQuery(url);
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
        if (cb.repaint) cb.repaint();
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
};
