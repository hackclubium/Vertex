#pragma once
//
// plat_text_measure.h — ITextMeasure backed by IPlatformRenderer.
//
// Used on macOS/Linux where DirectWrite isn't available. The layout engine
// calls MeasureText/SpaceWidth; this delegates to the platform renderer.
//
#include "platform/platform.h"
#include "layout/box.h"
#include <map>
#include <set>
#include <string>

class PlatTextMeasure : public ITextMeasure {
public:
    explicit PlatTextMeasure(IPlatformRenderer* r) : m_r(r) {}

    float MeasureText(const std::wstring& s, const FontKey& f) override {
        if (!m_r || s.empty()) return 0;
        PlatFont pf = GetFont(f);
        return pf ? m_r->MeasureText(s, pf) : (float)s.size() * f.size * 0.5f;
    }
    float SpaceWidth(const FontKey& f) override {
        PlatFont pf = GetFont(f);
        return pf ? m_r->SpaceWidth(pf) : f.size * 0.3f;
    }
    bool ImageIntrinsic(const std::string& url, float& w, float& h) override {
        // Images are tracked externally; this just checks the loaded set.
        auto it = loadedImages.find(url);
        if (it != loadedImages.end()) { w = it->second.first; h = it->second.second; return true; }
        return false;
    }
    void RequestImage(const std::string& url) override {
        if (requestedImages.count(url)) return;
        requestedImages.insert(url);
        if (onImageRequest) onImageRequest(url);
    }

    // Public so the shell can populate them.
    std::map<std::string, std::pair<float,float>> loadedImages;  // url → (w, h)
    std::set<std::string> requestedImages;
    std::function<void(std::string)> onImageRequest;

private:
    IPlatformRenderer* m_r;
    std::map<std::string, PlatFont> m_fonts;

    PlatFont GetFont(const FontKey& f) {
        std::string key = std::to_string((int)(f.size * 4))
            + (f.bold ? "b" : "-") + (f.italic ? "i" : "-") + (f.mono ? "m" : "-") + f.family;
        auto it = m_fonts.find(key);
        if (it != m_fonts.end()) return it->second;
        PlatFont pf = m_r->CreateFont(f.size, f.bold, f.italic, f.mono, f.family);
        m_fonts[key] = pf;
        return pf;
    }
};
