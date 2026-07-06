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
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#else
#include "platform/linux_font_registry.h"
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
        // Register entirely in-memory (mirrors AddFontMemResourceEx above) —
        // no temp file, no permanent system font-list pollution, and the
        // font is available for this process the instant this call returns.
        CFDataRef cfData = CFDataCreate(kCFAllocatorDefault,
            (const UInt8*)data.data(), (CFIndex)data.size());
        if (cfData) {
            CGDataProviderRef provider = CGDataProviderCreateWithCFData(cfData);
            if (provider) {
                CGFontRef font = CGFontCreateWithDataProvider(provider);
                if (font) {
                    CTFontManagerRegisterGraphicsFont(font, nullptr);
                    CGFontRelease(font);
                }
                CGDataProviderRelease(provider);
            }
            CFRelease(cfData);
        }
#else
        // Linux: Vertex's own font engine (platform/linux_font_registry.h)
        // has no from-memory registration API either — same as fontconfig
        // before it — so the bytes still need to land on disk, but into a
        // scratch cache directory, not the user's real ~/.local/share/fonts
        // (which would permanently pollute their system font list with
        // every web font from every page ever visited).
        // RegisterLinuxWebFont() parses the file and adds it to the
        // in-process font index directly, taking effect immediately.
        std::filesystem::path dir =
            std::filesystem::path(getenv("HOME") ? getenv("HOME") : "/tmp") / ".cache/vertex-fonts";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::path path = dir / ("vertex_" + std::to_string(std::hash<std::string>{}(family)) + ".ttf");
        FILE* f = fopen(path.string().c_str(), "wb");
        if (f) {
            fwrite(data.data(), 1, data.size(), f);
            fclose(f);
            RegisterLinuxWebFont(path.string());
        }
#endif
    }
};
