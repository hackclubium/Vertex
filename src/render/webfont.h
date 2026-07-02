#pragma once
//
// webfont.h — download and install @font-face web fonts.
//
// Downloads font files from URLs found in @font-face rules and registers
// them with the OS so the platform's text renderer (DirectWrite, CoreText,
// Pango) picks them up automatically. No font parsing needed.
//
#include "css/stylesheet.h"
#include "network/resource_cache.h"
#include "network/url.h"
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

class WebFontLoader {
public:
    static WebFontLoader& instance() {
        static WebFontLoader loader;
        return loader;
    }

    // Load all @font-face fonts from a stylesheet. Non-blocking: fonts that
    // are already loaded are skipped, new ones are fetched in background.
    void loadFonts(const Stylesheet& sheet, const std::string& baseUrl,
                   std::function<void()> onLoaded = nullptr) {
        for (auto& ff : sheet.fontFaces) {
            std::string resolved = ResolveUrlAgainstBase(ff.srcUrl, baseUrl);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_loaded.count(resolved)) continue;
                m_loaded.insert(resolved);
            }
            // Fetch and install in background.
            std::thread([this, resolved, ff, onLoaded]() {
                auto res = FetchResourceCached(resolved, 2 * 1024 * 1024, ResourceKind::Font);
                if (!res.success || res.body.size() < 100) return;
                installFont(res.body, ff.family);
                if (onLoaded) onLoaded();
            }).detach();
        }
    }

    ~WebFontLoader() {
#ifdef _WIN32
        for (HANDLE h : m_handles)
            RemoveFontMemResourceEx(h);
#endif
    }

private:
    std::mutex m_mutex;
    std::set<std::string> m_loaded;
#ifdef _WIN32
    std::vector<HANDLE> m_handles;
#endif

    void installFont(const std::string& data, const std::string& family) {
#ifdef _WIN32
        DWORD numFonts = 0;
        HANDLE h = AddFontMemResourceEx((void*)data.data(), (DWORD)data.size(), NULL, &numFonts);
        if (h) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_handles.push_back(h);
        }
#elif defined(__APPLE__)
        // macOS: write to temp file, register with CTFontManagerRegisterFontsForURL.
        // Simplified: write to /tmp and hope CoreText picks it up.
        std::string path = "/tmp/vertex_font_" + std::to_string(std::hash<std::string>{}(family)) + ".ttf";
        FILE* f = fopen(path.c_str(), "wb");
        if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
#else
        // Linux: write to ~/.local/share/fonts/ for fontconfig to find.
        std::string dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.local/share/fonts";
        std::string cmd = "mkdir -p " + dir;
        system(cmd.c_str());
        std::string path = dir + "/vertex_" + std::to_string(std::hash<std::string>{}(family)) + ".ttf";
        FILE* f = fopen(path.c_str(), "wb");
        if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
        system("fc-cache -f 2>/dev/null &");
#endif
    }
};
