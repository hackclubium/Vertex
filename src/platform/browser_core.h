#pragma once
//
// browser_core.h — platform-independent browser logic.
//
// Contains: Tab management, navigation, history, URL encoding, home page HTML,
// image fetch throttling. Used by all platform shells (Win32, macOS, Linux).
//
#include "network/fetcher.h"
#include "network/resource_cache.h"
#include "network/url.h"
#include "network/text_decode.h"
#include "html/parser.h"
#include "html/resources.h"
#include "js/engine.h"
#include "layout/box.h"

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <cctype>

// ── semaphore (C++17 compat) ─────────────────────────────────────────────────

class Semaphore {
public:
    explicit Semaphore(int count) : m_count(count) {}
    void acquire() {
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv.wait(lk, [&] { return m_count > 0; });
        --m_count;
    }
    void release() {
        { std::lock_guard<std::mutex> lk(m_mu); ++m_count; }
        m_cv.notify_one();
    }
private:
    std::mutex m_mu;
    std::condition_variable m_cv;
    int m_count;
};

// ── data types ───────────────────────────────────────────────────────────────

struct Page {
    std::string           url;
    std::shared_ptr<Node> dom;
    std::string           error;
};

struct ImageMsg {
    std::string          url;
    std::vector<uint8_t> bytes;
};

struct PageMsg {
    int   tabIdx;
    Page* page;
};

struct Tab {
    std::string           url        = "vertex://home";
    std::string           displayUrl;
    std::string           title      = "Vertex";
    std::shared_ptr<Page> page;
    float                 scrollY    = 0.f;
    float                 docHeight  = 600.f;
    bool                  loading    = false;
    std::string           pendingFragment;
    bool                  fragmentScrollPending = false;
    std::vector<std::string> history;
    int                   histIdx    = -1;
};

// ── URL helpers ──────────────────────────────────────────────────────────────

inline std::string UrlEncodeQuery(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else {
            char hex[4]; snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

inline bool LooksLikeUrl(const std::string& s) {
    if (s.empty()) return false;
    if (s.find("://") != std::string::npos) return true;
    if (s.size() > 1 && s[0] == '/' && s[1] == '/') return true;
    if (s.rfind("vertex://", 0) == 0 || s.rfind("about:", 0) == 0) return true;
    if (s.find(' ') != std::string::npos) return false;
    // localhost, localhost:3000
    if (s == "localhost" || s.rfind("localhost:", 0) == 0) return true;
    // 127.0.0.1, 127.0.0.1:8080
    if (s.rfind("127.", 0) == 0) return true;
    // [::1]
    if (s.rfind("[", 0) == 0) return true;
    // IP addresses (digits and dots only, no TLD needed)
    bool allDigitsDots = true;
    for (char c : s) if (!std::isdigit((unsigned char)c) && c != '.' && c != ':') { allDigitsDots = false; break; }
    if (allDigitsDots && s.find('.') != std::string::npos) return true;
    // Has a dot → probably a domain (example.com, en.wikipedia.org)
    size_t dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot == s.size() - 1) return false;
    return true;
}

inline std::string UrlFragment(const std::string& url) {
    size_t hash = url.find('#');
    if (hash == std::string::npos || hash + 1 >= url.size()) return {};
    return url.substr(hash + 1);
}

inline std::string UrlWithoutFragment(const std::string& url) {
    size_t hash = url.find('#');
    return hash == std::string::npos ? url : url.substr(0, hash);
}

inline void TabPushHistory(Tab& tab, const std::string& url) {
    if (tab.histIdx >= 0 && tab.histIdx < (int)tab.history.size())
        tab.history.erase(tab.history.begin() + tab.histIdx + 1, tab.history.end());
    tab.history.push_back(url);
    tab.histIdx = (int)tab.history.size() - 1;
}

// ── home page ────────────────────────────────────────────────────────────────

inline const std::string& HomePageHtml() {
    static const std::string html = R"html(<!DOCTYPE html>
<html>
<head><title>Vertex</title>
<style>
body {
    font-family: 'Segoe UI', system-ui, sans-serif;
    background: #f2e8d6;
    color: #211c16;
    margin: 0;
    padding: 36px 0 52px;
}
.w { width: 720px; margin-left: auto; margin-right: auto; }
.brand { text-align: center; margin-bottom: 22px; }
.mark {
    display: block; background: #315cf6; color: white;
    border-radius: 6px; padding: 10px 0; font-weight: 800;
    font-size: 14px; width: 104px; margin-left: auto; margin-right: auto;
}
.title {
    color: #211c16; text-align: center; font-size: 42px; font-weight: 800;
    padding-top: 22px; padding-bottom: 10px;
}
.sub { text-align: center; color: #665847; font-size: 15px; padding-bottom: 8px; }
.search {
    background: #fffaf1; border: 1px solid #d5c4a6; border-radius: 8px;
    padding: 18px 20px; margin: 24px 0 20px; color: #65553f;
    font-size: 16px; box-shadow: 0 8px 22px rgba(63, 45, 20, 0.08);
}
.search strong { color: #211c16; }
.hint {
    display: block; color: #8b7960; font-size: 13px;
    padding-top: 5px;
}
.section-title {
    color: #665847; font-size: 12px; font-weight: 800;
    padding-top: 18px; padding-bottom: 10px; text-transform: uppercase;
}
.links { padding: 0; }
.links a {
    display: block; background: #fffaf1; border: 1px solid #d8c8ac;
    border-radius: 7px; padding: 16px 18px; margin: 9px 0;
    text-decoration: none; color: #315cf6; font-size: 15px; font-weight: 800;
}
.links span {
    display: block; color: #74664f; font-size: 12px; font-weight: 500;
    padding-top: 4px;
}
.shortcuts {
    background: #e6d6ba; border: 1px solid #d1bd99;
    border-radius: 8px; padding: 14px 18px; margin-top: 20px;
}
.key { display: block; padding: 5px 0; color: #594b38; font-size: 13px; }
.ft { padding-top: 24px; text-align: center; }
.ft p { font-size: 12px; color: #74664f; }
.tag { color: #315cf6; font-size: 12px; font-weight: 800; }
</style>
</head>
<body>
<div class="w">
<div class="brand">
<span class="mark">VERTEX</span>
<div class="title">Start browsing</div>
<div class="sub">A from-scratch browser engine with its own HTML, CSS, JS, layout, and renderer.</div>
</div>
<div class="search"><strong>Ctrl+L</strong> - search or enter a URL<span class="hint">Try a site, open a quick link, or jump into history.</span></div>
<div class="section-title">Quick links</div>
<div class="links">
<a href="https://www.wikipedia.org/">Wikipedia<span>www.wikipedia.org</span></a>
<a href="https://news.ycombinator.com">Hacker News<span>news.ycombinator.com</span></a>
<a href="https://lite.cnn.com">CNN Lite<span>lite.cnn.com</span></a>
<a href="vertex://history">History<span>vertex://history</span></a>
<a href="vertex://bookmarks">Bookmarks<span>vertex://bookmarks</span></a>
<a href="vertex://downloads">Downloads<span>vertex://downloads</span></a>
<a href="vertex://settings">Settings<span>vertex://settings</span></a>
</div>
<div class="shortcuts">
<div class="section-title" style="margin-top:0;">Shortcuts</div>
<span class="key">Ctrl+L - address bar</span>
<span class="key">Ctrl+T / W - new / close tab</span>
<span class="key">Ctrl+R - reload</span>
<span class="key">Ctrl+F - find in page</span>
<span class="key">Ctrl + + / - - zoom</span>
<span class="key">Alt+Left/Right - back / forward</span>
</div>
<div class="ft">
<p><span class="tag">cross-platform</span></p>
<p style="margin-top:6px;">No Chromium. No WebView. No shortcuts.</p>
</div>
</div>
</body>
</html>)html";
    return html;
}

// ── shared helpers for page loading ──────────────────────────────────────────

inline std::string LowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

inline bool AttrContainsToken(const std::string& value, const std::string& token) {
    std::string lower = LowerAscii(value);
    size_t start = 0;
    while (start < lower.size()) {
        while (start < lower.size() && std::isspace((unsigned char)lower[start])) ++start;
        size_t end = start;
        while (end < lower.size() && !std::isspace((unsigned char)lower[end])) ++end;
        if (lower.substr(start, end - start) == token) return true;
        start = end;
    }
    return false;
}

inline bool StylesheetMediaApplies(const std::string& media) {
    std::string lower = LowerAscii(media);
    if (lower.empty()) return true;
    return lower.find("all") != std::string::npos
        || lower.find("screen") != std::string::npos
        || lower.find("projection") != std::string::npos;
}

inline Node* FindFirstElement(Node* root, const std::string& tag) {
    if (!root) return nullptr;
    std::vector<Node*> stack{ root };
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        if (n->type == NodeType::Element && n->tagName == tag) return n;
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return nullptr;
}

// Resolve @import url(...) inside a CSS string and return the combined CSS.
inline std::string ResolveImports(const std::string& css, const std::string& baseUrl,
                                   int& loaded, size_t& loadedBytes, int depth = 0) {
    if (depth > 3) return css;  // prevent infinite @import chains
    std::string result;
    size_t pos = 0;
    while (pos < css.size()) {
        // Look for @import
        size_t importAt = css.find("@import", pos);
        if (importAt == std::string::npos) { result += css.substr(pos); break; }
        result += css.substr(pos, importAt - pos);
        size_t urlStart = css.find_first_of("\"'(", importAt + 7);
        if (urlStart == std::string::npos) { result += css.substr(importAt); break; }
        std::string importUrl;
        size_t urlEnd;
        if (css[urlStart] == '(') {
            urlEnd = css.find(')', urlStart + 1);
            if (urlEnd == std::string::npos) { result += css.substr(importAt); break; }
            importUrl = css.substr(urlStart + 1, urlEnd - urlStart - 1);
            urlEnd = css.find(';', urlEnd);
        } else {
            char q = css[urlStart];
            urlEnd = css.find(q, urlStart + 1);
            if (urlEnd == std::string::npos) { result += css.substr(importAt); break; }
            importUrl = css.substr(urlStart + 1, urlEnd - urlStart - 1);
            urlEnd = css.find(';', urlEnd);
        }
        // Strip url() wrapper and quotes
        if (importUrl.rfind("url(", 0) == 0) importUrl = importUrl.substr(4);
        while (!importUrl.empty() && (importUrl.back() == ')' || importUrl.back() == '"' || importUrl.back() == '\''))
            importUrl.pop_back();
        while (!importUrl.empty() && (importUrl.front() == '"' || importUrl.front() == '\''))
            importUrl.erase(importUrl.begin());
        // Fetch the imported stylesheet.
        if (!importUrl.empty() && loaded < 64 && loadedBytes < 4 * 1024 * 1024) {
            std::string resolved = ResolveUrlAgainstBase(importUrl, baseUrl);
            auto res = FetchResourceCached(resolved, 1024 * 1024, ResourceKind::Stylesheet);
            if (res.success && !res.body.empty()) {
                std::string importedCss = DecodeTextToUtf8(res.body, res.contentType);
                loadedBytes += importedCss.size();
                ++loaded;
                result += ResolveImports(importedCss, resolved, loaded, loadedBytes, depth + 1);
            }
        }
        pos = (urlEnd != std::string::npos) ? urlEnd + 1 : css.size();
    }
    return result;
}

inline void LoadExternalStylesheets(const std::shared_ptr<Node>& dom, const std::string& pageUrl) {
    if (!dom) return;
    Node* attach = FindFirstElement(dom.get(), "head");
    if (!attach) attach = dom.get();
    std::vector<Node*> stack{ dom.get() };
    int loaded = 0;
    size_t loadedBytes = 0;
    while (!stack.empty() && loaded < 64 && loadedBytes < 4 * 1024 * 1024) {
        Node* n = stack.back();
        stack.pop_back();
        if (n->type == NodeType::Element && n->tagName == "link"
            && AttrContainsToken(n->attr("rel"), "stylesheet")
            && StylesheetMediaApplies(n->attr("media"))) {
            std::string href = ResolveUrlAgainstBase(n->attr("href"), pageUrl);
            auto res = FetchResourceCached(href, 1024 * 1024, ResourceKind::Stylesheet);
            if (res.success && !res.body.empty()) {
                std::string css = DecodeTextToUtf8(res.body, res.contentType);
                loadedBytes += css.size();
                if (loadedBytes <= 4 * 1024 * 1024) {
                    // Resolve @import directives inside this stylesheet.
                    css = ResolveImports(css, href, loaded, loadedBytes);
                    auto style = Node::makeElement("style");
                    style->appendChild(Node::makeText(css));
                    attach->appendChild(style);
                    ++loaded;
                }
            }
        }
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
            stack.push_back(it->get());
    }
}
