#pragma once
#include "html/dom.h"
#include "css/stylesheet.h"
#include "layout/box.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
// WIC no longer needed — image decoding uses stb_image (cross-platform).
// #include <wincodec.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>

// HitRegion is defined in platform/platform.h (shared across all platforms).
#include "platform/platform.h"

struct TabEntry {
    std::wstring title;
    bool         active  = false;
    bool         loading = false;
    float        loadingProgress = 0.f;
};

struct RendererTimings {
    double styleMs = 0.0;
    double layoutMs = 0.0;
    double paintMs = 0.0;
    bool layoutReused = false;
};

class Renderer : public ITextMeasure {
public:
    ~Renderer();

    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height);

    // ── ITextMeasure (used by the layout engine) ──────────────────────────
    float MeasureText(const std::wstring& s, const FontKey& f) override;
    float SpaceWidth(const FontKey& f) override;
    bool  ImageIntrinsic(const std::string& url, float& w, float& h) override;
    void  RequestImage(const std::string& url) override;

    // Paint everything. Returns total document height (for scrollbar).
    float Paint(const std::shared_ptr<Node>& doc,
                float scrollY,
                const std::string& baseUrl,
                float topInset   = 0.f,
                float tabStripH  = 0.f,
                const std::vector<TabEntry>* tabs = nullptr,
                bool repaintChrome = true);

    std::string HitTest(float x, float y) const;
    const Node* HoverNodeAt(float x, float y, float scrollY, float topInset) const;
    bool LastHoverRegion(HitRegion& out) const;
    int  HitTestTab(float x, float y) const;
    bool HitTestTabClose(float x, float y, int& outIdx) const;
    bool GetAnchorY(const std::string& anchor, float& outY) const;

    void DiscardTarget();
    void ReceiveImage(const std::string& url, const std::vector<uint8_t>& bytes);

    void SetImageRequestCallback(std::function<void(std::string)> cb) {
        m_imageRequestCb = std::move(cb);
    }

    void  SetZoom(float z);
    float GetZoom() const { return m_zoom; }
    const LayoutBox* GetLayoutRoot() const { return m_layoutRoot.get(); }
    void InvalidateLayout();
    bool UsesHoverStyles() const;
    RendererTimings LastTimings() const { return m_lastTimings; }

    void SetSearchQuery(const std::wstring& q) { m_searchQuery = q; }
    const std::wstring& GetSearchQuery() const { return m_searchQuery; }
    bool FindTextY(const std::wstring& query, float currentY, bool backwards, float& outY) const;

private:
    // ── D2D / DWrite / WIC ────────────────────────────────────────────────
    ID2D1Factory*           m_factory   = nullptr;
    ID2D1HwndRenderTarget*  m_rt        = nullptr;
    IDWriteFactory*         m_dwrite    = nullptr;
    // IWICImagingFactory removed — stb_image replaces WIC for decoding.

    // ── persistent brushes ────────────────────────────────────────────────
    ID2D1SolidColorBrush*   m_textBrush  = nullptr;
    ID2D1SolidColorBrush*   m_linkBrush  = nullptr;
    ID2D1SolidColorBrush*   m_bgBrush    = nullptr;
    ID2D1SolidColorBrush*   m_hrBrush    = nullptr;
    ID2D1SolidColorBrush*   m_codeBgBrush= nullptr;  // code block bg (#f4f4f4)
    ID2D1SolidColorBrush*   m_findBrush  = nullptr;  // search highlight (#fff176)
    ID2D1SolidColorBrush*   m_quoteBrush = nullptr;  // blockquote bar (#ccc)
    ID2D1SolidColorBrush*   m_tabBgBrush = nullptr;
    ID2D1SolidColorBrush*   m_tabActBrush= nullptr;
    ID2D1SolidColorBrush*   m_tabInaBrush= nullptr;
    ID2D1SolidColorBrush*   m_tabTxtBrush= nullptr;
    ID2D1SolidColorBrush*   m_tabClsBrush= nullptr;

    IDWriteTextFormat*      m_fmtTab      = nullptr;   // tab titles (not zoom-scaled)
    IDWriteTextFormat*      m_fmtTabClose = nullptr;
    IDWriteTextFormat*      m_fmtTabPlus  = nullptr;

    float m_zoom = 1.f;

    std::wstring m_searchQuery;
    std::string  m_curBaseUrl;   // base URL for the page currently being painted

    std::map<std::string, ID2D1Bitmap*> m_images;
    std::set<std::string>               m_loadingImages;
    std::set<std::string>               m_failedImages;
    const Node*                         m_imageDocKey = nullptr;
    std::function<void(std::string)>    m_imageRequestCb;

    std::vector<ID2D1SolidColorBrush*>  m_tempBrushes;
    std::map<unsigned int, ID2D1SolidColorBrush*> m_tempBrushCache;

    HWND  m_hwnd   = nullptr;
    UINT  m_width  = 800;
    UINT  m_height = 600;

    std::vector<HitRegion> m_hits;
    mutable bool m_lastHitValid = false;
    mutable HitRegion m_lastHitRegion;
    mutable std::string m_lastHitHref;
    mutable bool m_lastHoverNodeValid = false;
    mutable HitRegion m_lastHoverNodeRegion;
    mutable const Node* m_lastHoverNode = nullptr;
    std::map<std::string, float> m_anchorY;

    struct TabHit { float x, y, w, h; int idx; bool isClose; };
    std::vector<TabHit> m_tabHits;

    // ── internal helpers ──────────────────────────────────────────────────
    bool EnsureTarget();
    void ReleaseTarget();
    void ReleaseBrushes();
    bool CreateBrushes();
    void CreateTabFont();

    void DrawTabStrip(const std::vector<TabEntry>& tabs, float h);

    // ── box-tree painter ───────────────────────────────────────────────────
    void PaintBox(const LayoutBox& box, float scrollY, float topInset, bool underFixed);
    void PaintLines(const LayoutBox& box, float scrollY, float topInset, bool underFixed);
    void PaintBoxDecorations(const LayoutBox& box, float scrollY, float topInset);
    void CollectAnchors(const LayoutBox& box);
    void CaptureLayoutBaseStyles(const LayoutBox& box);
    void ApplyPaintOnlyHoverStyles(LayoutBox& box, const Stylesheet& sheet);
    IDWriteTextFormat* FormatForKey(const FontKey& f);
    std::map<std::string, IDWriteTextFormat*> m_fmtCache;
    std::map<std::wstring, float>             m_measureCache;

    // Cached layout tree — rebuilt only when the document, size, or zoom
    // changes, so scrolling/repainting is cheap.
    Stylesheet  m_cachedSheet;
    CssColor    m_cachedPageBg;
    const Node* m_styleDocKey = nullptr;
    std::string m_styleBaseUrlKey;
    bool        m_cachedUsesHoverStyles = false;
    bool        m_cachedHoverAffectsLayout = false;

    std::unique_ptr<LayoutBox> m_layoutRoot;
    std::map<const LayoutBox*, ComputedStyle> m_layoutBaseStyles;
    const Node* m_layoutDocKey  = nullptr;
    UINT  m_layoutWKey = 0, m_layoutHKey = 0;
    float m_layoutZoomKey = 0.f;
    RendererTimings m_lastTimings;

    ID2D1SolidColorBrush* TempBrush(D2D1_COLOR_F color);
    std::string  ResolveUrl(const std::string& href, const std::string& base);

    static Stylesheet CollectStylesheet(const Node* root);
    static CssColor   FindBodyBgColor(const Node* root, const Stylesheet& sheet);
};
