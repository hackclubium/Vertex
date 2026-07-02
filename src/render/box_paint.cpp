// box_paint.cpp — paints the layout box tree produced by the layout engine,
// and implements the ITextMeasure interface the engine uses for measurement.
#include "render/renderer.h"
#include "layout/layout_engine.h"
#include "render/transition.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <functional>
#include <cctype>
#include <cmath>

static D2D1_COLOR_F ToD2Dc(const CssColor& c) { return { c.r, c.g, c.b, c.a }; }
static constexpr size_t kMaxMeasuredTextChars = 16 * 1024;

static std::wstring NodeTextContentWide(const Node* n) {
    if (!n) return {};
    if (n->type == NodeType::Text) {
        std::wstring out;
        out.reserve(n->text.size());
        for (unsigned char c : n->text) out += (wchar_t)c;
        return out;
    }
    std::wstring out;
    for (const auto& child : n->children) out += NodeTextContentWide(child.get());
    return out;
}

static bool FormControlHasSpriteDescendant(const Node* n) {
    if (!n) return false;
    if (n->type == NodeType::Element) {
        std::string cls = n->attr("class");
        if (cls.find("sprite") != std::string::npos || cls.find("svg-") != std::string::npos)
            return true;
    }
    for (const auto& child : n->children)
        if (FormControlHasSpriteDescendant(child.get())) return true;
    return false;
}

static std::string FontCacheKey(const FontKey& f) {
    std::string k = std::to_string((int)(f.size * 4));
    k += f.bold ? "b" : "-";
    k += f.italic ? "i" : "-";
    k += f.mono ? "m" : "-";
    k += f.family;
    return k;
}

// ─── ITextMeasure ─────────────────────────────────────────────────────────────

IDWriteTextFormat* Renderer::FormatForKey(const FontKey& f) {
    std::string key = FontCacheKey(f);
    auto it = m_fmtCache.find(key);
    if (it != m_fmtCache.end()) return it->second;
    if (!m_dwrite) return nullptr;

    std::wstring fam;
    if (!f.family.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, f.family.c_str(), -1, nullptr, 0);
        if (n > 0) { fam.resize(n - 1); MultiByteToWideChar(CP_UTF8, 0, f.family.c_str(), -1, fam.data(), n); }
    }
    if (fam.empty()) fam = f.mono ? L"Consolas" : L"Segoe UI";

    IDWriteTextFormat* fmt = nullptr;
    m_dwrite->CreateTextFormat(
        fam.c_str(), nullptr,
        f.bold   ? DWRITE_FONT_WEIGHT_BOLD  : DWRITE_FONT_WEIGHT_NORMAL,
        f.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, std::max(1.f, f.size), L"en-us", &fmt);
    if (fmt) fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_fmtCache[key] = fmt;
    return fmt;
}

float Renderer::MeasureText(const std::wstring& s, const FontKey& f) {
    if (s.empty() || !m_dwrite) return 0.f;
    if (s.size() > kMaxMeasuredTextChars)
        return MeasureText(s.substr(0, kMaxMeasuredTextChars), f);
    const std::string fontKey = FontCacheKey(f);
    std::wstring ck(fontKey.begin(), fontKey.end());
    ck.reserve(ck.size() + 1 + s.size());
    ck += L"\x1";
    ck += s;
    auto cit = m_measureCache.find(ck);
    if (cit != m_measureCache.end()) return cit->second;

    auto* fmt = FormatForKey(f);
    if (!fmt) return (float)s.size() * f.size * 0.5f;
    IDWriteTextLayout* lay = nullptr;
    float w = (float)s.size() * f.size * 0.5f;
    if (SUCCEEDED(m_dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 100000.f, 100000.f, &lay)) && lay) {
        DWRITE_TEXT_METRICS m{}; lay->GetMetrics(&m);
        w = m.widthIncludingTrailingWhitespace > 0 ? m.widthIncludingTrailingWhitespace : m.width;
        lay->Release();
    }
    m_measureCache[ck] = w;
    return w;
}

float Renderer::SpaceWidth(const FontKey& f) {
    return MeasureText(L" ", f) * 0.0f + MeasureText(L"x x", f) - 2.f * MeasureText(L"x", f);
}

bool Renderer::ImageIntrinsic(const std::string& url, float& w, float& h) {
    auto it = m_images.find(url);
    if (it != m_images.end() && it->second) {
        D2D1_SIZE_F sz = it->second->GetSize();
        w = sz.width; h = sz.height;
        return w > 0 && h > 0;
    }
    return false;
}

void Renderer::RequestImage(const std::string& url) {
    if (url.empty() || m_loadingImages.count(url) || m_failedImages.count(url)) return;
    m_loadingImages.insert(url);
    if (m_imageRequestCb) m_imageRequestCb(url);
}

// ─── anchors ──────────────────────────────────────────────────────────────────

void Renderer::CollectAnchors(const LayoutBox& box) {
    if (box.node && box.node->type == NodeType::Element) {
        std::string id = box.node->attr("id");
        std::string nm = box.node->attr("name");
        if (!id.empty() && !m_anchorY.count(id)) m_anchorY[id] = box.y;
        if (!nm.empty() && !m_anchorY.count(nm)) m_anchorY[nm] = box.y;
    }
    for (auto& k : box.kids) CollectAnchors(*k);
}

// ─── decorations (background + border + replaced image) ──────────────────────

void Renderer::PaintBoxDecorations(const LayoutBox& box, float scrollY, float topInset) {
    // Apply CSS transition interpolation to the style before painting.
    ComputedStyle transStyle = box.style;
    if (box.node) TransitionManager::instance().applyTransition(box.node, transStyle);
    const ComputedStyle& s = transStyle;
    float sx = box.x;
    float sy = box.y - scrollY + topInset;
    float bw = box.borderBoxW();
    float bh = box.borderBoxH();
    if (sy + bh < topInset || sy > (float)m_height) {
        // still need replaced image? cull entirely
        if (box.kind != BoxKind::Replaced) return;
        if (box.kind == BoxKind::Replaced && (sy + bh < topInset || sy > (float)m_height)) return;
    }

    // Outline.
    if (s.outlineSet && s.outlineWidth > 0 && m_rt) {
        float ow = s.outlineWidth;
        D2D1_COLOR_F oc = s.outlineColor.valid ? ToD2Dc(s.outlineColor) : D2D1::ColorF(0, 0, 0);
        if (auto* b = TempBrush(oc))
            m_rt->DrawRectangle(D2D1::RectF(sx-ow/2, sy-ow/2, sx+bw+ow/2, sy+bh+ow/2), b, ow);
    }

    // Box shadow.
    if (s.shadowSet && s.shadowColor.valid && s.shadowColor.a > 0.01f && !s.shadowInset && m_rt) {
        float blur = s.shadowBlur;
        float spread = s.shadowSpread;
        float shx = sx + s.shadowX - spread;
        float shy = sy + s.shadowY - spread;
        float shw = bw + spread * 2;
        float shh = bh + spread * 2;
        if (blur < 1.f) {
            if (auto* b = TempBrush(D2D1::ColorF(s.shadowColor.r, s.shadowColor.g, s.shadowColor.b, s.shadowColor.a)))
                m_rt->FillRectangle(D2D1::RectF(shx, shy, shx+shw, shy+shh), b);
        } else {
            int steps = std::min((int)(blur / 2) + 1, 8);
            for (int i = steps; i >= 0; --i) {
                float f = (float)i / (float)steps;
                float expand = blur * f;
                float a = s.shadowColor.a * (1.f - f) * 0.5f;
                if (auto* b = TempBrush(D2D1::ColorF(s.shadowColor.r, s.shadowColor.g, s.shadowColor.b, a)))
                    m_rt->FillRectangle(D2D1::RectF(shx-expand, shy-expand,
                        shx+shw+expand, shy+shh+expand), b);
            }
        }
    }

    // Background color.
    if (s.bgColor.valid && s.bgColor.a > 0.001f) {
        if (auto* b = TempBrush(ToD2Dc(s.bgColor)))
            m_rt->FillRectangle(D2D1::RectF(sx, sy, sx + bw, sy + bh), b);
    }
    // Linear gradient (D2D native).
    if (s.gradientSet && s.gradientStops.size() >= 2 && bw > 0 && bh > 0 && m_rt) {
        float rad = s.gradientAngle * 3.14159265f / 180.f;
        float dx = std::sin(rad), dy = -std::cos(rad);
        float halfW = bw / 2, halfH = bh / 2;
        float gradLen = std::abs(dx * bw) + std::abs(dy * bh);
        D2D1_POINT_2F p0 = { sx + halfW - dx * gradLen / 2, sy + halfH - dy * gradLen / 2 };
        D2D1_POINT_2F p1 = { sx + halfW + dx * gradLen / 2, sy + halfH + dy * gradLen / 2 };
        std::vector<D2D1_GRADIENT_STOP> gstops;
        for (auto& gs : s.gradientStops)
            gstops.push_back({ gs.pos, D2D1::ColorF(gs.color.r, gs.color.g, gs.color.b, gs.color.a) });
        ID2D1GradientStopCollection* coll = nullptr;
        if (SUCCEEDED(m_rt->CreateGradientStopCollection(gstops.data(), (UINT32)gstops.size(), &coll)) && coll) {
            ID2D1LinearGradientBrush* gb = nullptr;
            if (SUCCEEDED(m_rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(p0, p1), coll, &gb)) && gb) {
                m_rt->FillRectangle(D2D1::RectF(sx, sy, sx + bw, sy + bh), gb);
                gb->Release();
            }
            coll->Release();
        }
    }
    // Background image (skip no-repeat fixed: anchored to viewport origin, invisible here).
    if (!s.backgroundImage.empty() && !(s.bgNoRepeat && s.bgFixed)) {
        std::string url = ResolveUrl(s.backgroundImage, m_curBaseUrl);
        auto it = m_images.find(url);
        if (it != m_images.end() && it->second) {
            D2D1_SIZE_F isz = it->second->GetSize();
            float iw = isz.width, ih = isz.height;
            if (iw > 0 && ih > 0 && bw > 0 && bh > 0) {
                // 1) Tile size from background-size.
                float tw = iw, th = ih;
                if (s.bgSizeMode == 1) {                 // cover
                    float sc = std::max(bw / iw, bh / ih); tw = iw * sc; th = ih * sc;
                } else if (s.bgSizeMode == 2) {          // contain
                    float sc = std::min(bw / iw, bh / ih); tw = iw * sc; th = ih * sc;
                } else if (s.bgSizeMode == 3) {          // explicit length/%
                    float w = s.bgSizeWPct ? bw * (s.bgSizeW / 100.f)
                            : (s.bgSizeW >= 0 ? s.bgSizeW * m_zoom : -1.f);
                    float h = s.bgSizeHPct ? bh * (s.bgSizeH / 100.f)
                            : (s.bgSizeH >= 0 ? s.bgSizeH * m_zoom : -1.f);
                    if (w < 0 && h < 0) { w = iw; h = ih; }
                    else if (w < 0)     { w = iw * (h / ih); }
                    else if (h < 0)     { h = ih * (w / iw); }
                    tw = w; th = h;
                }
                // 2) Offset from background-position (percentages align the P%
                //    point of the tile with the P% point of the box).
                float ox = s.bgPosXPct ? (bw - tw) * (s.bgPosX / 100.f) : s.bgPosX * m_zoom;
                float oy = s.bgPosYPct ? (bh - th) * (s.bgPosY / 100.f) : s.bgPosY * m_zoom;

                D2D1_RECT_F clip = D2D1::RectF(sx, sy, sx + bw, sy + bh);
                m_rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                int rep = s.bgRepeatSet ? s.bgRepeat : (s.bgNoRepeat ? 3 : 0);
                if (rep == 3) {
                    m_rt->DrawBitmap(it->second,
                        D2D1::RectF(sx + ox, sy + oy, sx + ox + tw, sy + oy + th));
                } else {
                    bool repX = (rep == 0 || rep == 1);
                    bool repY = (rep == 0 || rep == 2);
                    float startX = sx + ox, startY = sy + oy;
                    if (repX) while (startX > sx) startX -= tw;
                    if (repY) while (startY > sy) startY -= th;
                    if (tw < 1.f) tw = 1.f;
                    if (th < 1.f) th = 1.f;
                    for (float y = startY; y < sy + bh; y += th) {
                        for (float x = startX; x < sx + bw; x += tw) {
                            m_rt->DrawBitmap(it->second, D2D1::RectF(x, y, x + tw, y + th));
                            if (!repX) break;
                        }
                        if (!repY) break;
                    }
                }
                m_rt->PopAxisAlignedClip();
            }
        } else {
            const_cast<Renderer*>(this)->RequestImage(url);
        }
    }

    // Replaced image content.
    if (box.kind == BoxKind::Replaced && !box.replacedUrl.empty()) {
        auto it = m_images.find(box.replacedUrl);
        if (it != m_images.end() && it->second) {
            float cx = box.contentX();
            float cy = box.contentY() - scrollY + topInset;
            float bw2 = box.contentW, bh2 = box.contentH;
            D2D1_SIZE_F isz = it->second->GetSize();
            float iw = isz.width, ih = isz.height;
            int fit = box.style.objectFit;
            if (fit != 0 && iw > 0 && ih > 0 && bw2 > 0 && bh2 > 0) {
                // contain/cover/none/scale-down: preserve aspect ratio, center,
                // and clip to the content box (cover/none may overflow).
                float dw = bw2, dh = bh2;
                if (fit == 1 || fit == 4) {               // contain / scale-down
                    float sc = std::min(bw2 / iw, bh2 / ih);
                    if (fit == 4) sc = std::min(sc, 1.f); // scale-down never enlarges
                    dw = iw * sc; dh = ih * sc;
                } else if (fit == 2) {                    // cover
                    float sc = std::max(bw2 / iw, bh2 / ih);
                    dw = iw * sc; dh = ih * sc;
                } else if (fit == 3) {                    // none: native size
                    dw = iw; dh = ih;
                }
                float ox = cx + (bw2 - dw) / 2.f;
                float oy = cy + (bh2 - dh) / 2.f;
                bool clip = (dw > bw2 + 0.5f || dh > bh2 + 0.5f);
                if (clip) m_rt->PushAxisAlignedClip(
                    D2D1::RectF(cx, cy, cx + bw2, cy + bh2), D2D1_ANTIALIAS_MODE_ALIASED);
                m_rt->DrawBitmap(it->second, D2D1::RectF(ox, oy, ox + dw, oy + dh));
                if (clip) m_rt->PopAxisAlignedClip();
            } else {
                m_rt->DrawBitmap(it->second, D2D1::RectF(cx, cy, cx + bw2, cy + bh2));
            }
        }
    }

    // Form controls: draw simple native-ish chrome for atomic controls that
    // are not backed by an image resource.
    if (box.kind == BoxKind::Replaced && box.node && box.replacedUrl.empty()) {
        const std::string& tag = box.node->tagName;
        if (tag == "input" || tag == "textarea" || tag == "select" || tag == "button") {
            float cx = box.contentX();
            float cy = box.contentY() - scrollY + topInset;
            float cw = box.contentW;
            float ch = box.contentH;
            bool authorPaintedChrome = s.bgColor.valid || s.borderColor.valid
                || box.borderTop > 0.01f || box.borderRight > 0.01f
                || box.borderBottom > 0.01f || box.borderLeft > 0.01f;
            bool drawNativeChrome = !authorPaintedChrome;
            if (drawNativeChrome) {
                auto* fill = TempBrush(tag == "button"
                    ? D2D1::ColorF(0.94f, 0.94f, 0.94f, 1.f)
                    : D2D1::ColorF(1.f, 1.f, 1.f, 1.f));
                auto* border = TempBrush(D2D1::ColorF(0.62f, 0.62f, 0.62f, 1.f));
                if (fill) m_rt->FillRectangle(D2D1::RectF(cx, cy, cx + cw, cy + ch), fill);
                if (border) m_rt->DrawRectangle(D2D1::RectF(cx, cy, cx + cw, cy + ch), border, 1.f);
            }

            std::wstring label;
            if ((tag == "button" && !FormControlHasSpriteDescendant(box.node)) || tag == "select")
                label = NodeTextContentWide(box.node);
            if (label.empty() && tag == "input") {
                std::string ph = box.node->attr("placeholder");
                for (unsigned char c : ph) label += (wchar_t)c;
            }
            if (!label.empty()) {
                FontKey fk;
                fk.size = std::clamp((s.fontSize > 0 ? s.fontSize : 14.f) * m_zoom, 1.f, 32.f);
                fk.bold = s.bold;
                fk.italic = s.italic;
                fk.family = s.fontFamily;
                if (auto* fmt = FormatForKey(fk)) {
                    auto* textBrush = TempBrush(s.color.valid ? ToD2Dc(s.color)
                        : (tag == "input"
                            ? D2D1::ColorF(0.45f, 0.45f, 0.45f, 1.f)
                            : D2D1::ColorF(0.08f, 0.08f, 0.08f, 1.f)));
                    if (textBrush) {
                        float tx = tag == "button" ? 8.f : 6.f;
                        m_rt->DrawText(label.c_str(), (UINT32)label.size(), fmt,
                            D2D1::RectF(cx + tx, cy + 2.f, cx + cw - 8.f, cy + ch - 2.f),
                            textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
            }
            if (tag == "select" && drawNativeChrome) {
                auto* arrow = TempBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f, 1.f));
                if (arrow) {
                    float ax = cx + cw - 15.f;
                    float ay = cy + ch * 0.5f - 2.f;
                    m_rt->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(ax + 4.f, ay + 4.f), arrow, 1.5f);
                    m_rt->DrawLine(D2D1::Point2F(ax + 4.f, ay + 4.f), D2D1::Point2F(ax + 8.f, ay), arrow, 1.5f);
                }
            }
        }
    }

    // Borders.
    auto side = [&](const CssColor& c) -> ID2D1SolidColorBrush* {
        if (c.valid) return TempBrush(ToD2Dc(c));
        if (s.borderColor.valid) return TempBrush(ToD2Dc(s.borderColor));
        if (s.color.valid) return TempBrush(ToD2Dc(s.color));
        return m_hrBrush;
    };
    float ex = sx + bw, ey = sy + bh;
    int sideCount = (box.borderTop > 0) + (box.borderBottom > 0) + (box.borderLeft > 0) + (box.borderRight > 0);
    bool degenerate = (box.contentW <= 1.f || box.contentH <= 1.f) && sideCount >= 2;

    if (degenerate && m_factory) {
        // CSS borders meet at 45° miters. When the content box is ~zero, the
        // borders form triangles instead of strips — this is how border tricks
        // draw triangles/arrows (Acid2's nose, CSS tooltips, dropdown carets).
        D2D1_POINT_2F tl = {sx, sy}, tr = {ex, sy}, br = {ex, ey}, bl = {sx, ey};
        D2D1_POINT_2F ci = {sx + box.borderLeft, sy + box.borderTop};
        D2D1_POINT_2F co = {ex - box.borderRight, ey - box.borderBottom};
        auto tri = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, ID2D1SolidColorBrush* br2) {
            if (!br2) return;
            ID2D1PathGeometry* geo = nullptr; ID2D1GeometrySink* sink = nullptr;
            if (FAILED(m_factory->CreatePathGeometry(&geo)) || !geo) return;
            if (SUCCEEDED(geo->Open(&sink)) && sink) {
                sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
                D2D1_POINT_2F pts[2] = { b, c };
                sink->AddLines(pts, 2);
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close(); sink->Release();
                m_rt->FillGeometry(geo, br2);
            }
            geo->Release();
        };
        if (box.borderTop > 0)    tri(tl, tr, ci, side(s.borderTopColor));
        if (box.borderRight > 0)  tri(tr, br, co, side(s.borderRightColor));
        if (box.borderBottom > 0) tri(br, bl, co, side(s.borderBottomColor));
        if (box.borderLeft > 0)   tri(bl, tl, ci, side(s.borderLeftColor));
    } else {
        if (box.borderTop > 0)    if (auto* b = side(s.borderTopColor))    m_rt->FillRectangle(D2D1::RectF(sx, sy, ex, sy + box.borderTop), b);
        if (box.borderBottom > 0) if (auto* b = side(s.borderBottomColor)) m_rt->FillRectangle(D2D1::RectF(sx, ey - box.borderBottom, ex, ey), b);
        if (box.borderLeft > 0)   if (auto* b = side(s.borderLeftColor))   m_rt->FillRectangle(D2D1::RectF(sx, sy, sx + box.borderLeft, ey), b);
        if (box.borderRight > 0)  if (auto* b = side(s.borderRightColor))  m_rt->FillRectangle(D2D1::RectF(ex - box.borderRight, sy, ex, ey), b);
    }

    // List-item marker.
    if (box.kind == BoxKind::ListItem && !(s.listStyleSet && s.listStyleNone)) {
        FontKey f; f.size = std::max(8.f, (s.fontSize > 0 ? s.fontSize : 16.f) * m_zoom);
        auto* fmt = FormatForKey(f);
        if (fmt && m_textBrush) {
            float mx = box.contentX() - 16.f;
            float my = box.contentY() - scrollY + topInset;
            m_rt->DrawText(L"•", 1, fmt, D2D1::RectF(mx, my, mx + 14.f, my + f.size * 1.4f),
                           m_textBrush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
}

// ─── inline line painting ─────────────────────────────────────────────────────

bool Renderer::FindTextY(const std::wstring& query, float currentY, bool backwards, float& outY) const {
    if (!m_layoutRoot || query.empty()) return false;
    std::wstring needle = query;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);

    std::vector<float> hits;
    std::function<void(const LayoutBox&)> collect = [&](const LayoutBox& box) {
        for (const auto& line : box.lines) {
            for (const auto& frag : line.frags) {
                if (frag.text.empty()) continue;
                std::wstring hay = frag.text;
                std::transform(hay.begin(), hay.end(), hay.begin(), ::towlower);
                if (hay.find(needle) != std::wstring::npos) hits.push_back(frag.y);
            }
        }
        for (const auto& kid : box.kids)
            if (kid) collect(*kid);
    };
    collect(*m_layoutRoot);
    if (hits.empty()) return false;
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end(), [](float a, float b) {
        return std::fabs(a - b) < 0.5f;
    }), hits.end());

    if (backwards) {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
            if (*it < currentY - 2.f) { outY = *it; return true; }
        }
        outY = hits.back();
        return true;
    }
    for (float y : hits) {
        if (y > currentY + 2.f) { outY = y; return true; }
    }
    outY = hits.front();
    return true;
}

void Renderer::PaintLines(const LayoutBox& box, float scrollY, float topInset, bool underFixed) {
    for (const auto& line : box.lines) {
        float lineY = line.y - (underFixed ? 0.f : scrollY) + topInset;
        if (lineY + line.h < topInset || lineY > (float)m_height)
            continue;
        for (const auto& frag : line.frags) {
            if (!frag.src) continue;
            if (frag.src->kind == BoxKind::InlineBlock || frag.src->kind == BoxKind::Replaced) {
                PaintBox(*frag.src, scrollY, topInset, underFixed);
                continue;
            }
            // Text fragment.
            ComputedStyle transStyle = frag.src->style;
            if (frag.src->node) TransitionManager::instance().applyTransition(frag.src->node, transStyle);
            const ComputedStyle& fs = transStyle;
            if (fs.visibilitySet && fs.visibilityHidden) continue;
            float sy = frag.y - (underFixed ? 0.f : scrollY) + topInset;
            if (sy + frag.h < topInset || sy > (float)m_height) {
                if (!frag.src->href.empty())
                    m_hits.push_back({ frag.x, sy, frag.w, frag.h, frag.src->href });
                continue;
            }
            FontKey f;
            f.size = std::clamp((fs.fontSize > 0 ? fs.fontSize : 16.f) * m_zoom, 1.f, 40.f);
            f.bold = fs.bold; f.italic = fs.italic;
            f.family = fs.fontFamily;
            std::string fl; for (char c : fs.fontFamily) fl += (char)std::tolower((unsigned char)c);
            f.mono = (fl.find("mono") != std::string::npos || fl.find("consol") != std::string::npos || fl.find("courier") != std::string::npos);
            auto* fmt = FormatForKey(f);
            if (!fmt) continue;
            ID2D1SolidColorBrush* brush = fs.color.valid ? TempBrush(ToD2Dc(fs.color))
                                        : (!frag.src->href.empty() ? m_linkBrush : m_textBrush);
            bool needsLayoutObject =
                   (!fs.noUnderline && (fs.underline || !frag.src->href.empty()))
                || fs.lineThrough
                || (!m_searchQuery.empty() && m_findBrush)
                || (box.style.textOverflow == 1 && box.style.overflowHidden)
                || (fs.textShadowSet && fs.textShadowColor.valid && fs.textShadowColor.a > 0);
            if (!needsLayoutObject) {
                m_rt->DrawText(frag.text.c_str(), (UINT32)frag.text.size(), fmt,
                    D2D1::RectF(frag.x, sy, frag.x + frag.w + 4.f, sy + frag.h * 2.f + 4.f),
                    brush ? brush : m_textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                continue;
            }
            IDWriteTextLayout* lay = nullptr;
            if (SUCCEEDED(m_dwrite->CreateTextLayout(frag.text.c_str(), (UINT32)frag.text.size(),
                    fmt, frag.w + 4.f, frag.h * 2.f + 4.f, &lay)) && lay) {
                DWRITE_TEXT_RANGE all{ 0, (UINT32)frag.text.size() };
                if (!fs.noUnderline && (fs.underline || !frag.src->href.empty()))
                    lay->SetUnderline(TRUE, all);
                if (fs.lineThrough)
                    lay->SetStrikethrough(TRUE, all);
                if (!m_searchQuery.empty() && m_findBrush) {
                    std::wstring low = frag.text, q = m_searchQuery;
                    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
                    std::transform(q.begin(), q.end(), q.begin(), ::towlower);
                    if (low.find(q) != std::wstring::npos)
                        m_rt->FillRectangle(D2D1::RectF(frag.x - 1, sy, frag.x + frag.w + 1, sy + frag.h), m_findBrush);
                }
                // text-overflow: ellipsis — set DWRITE trimming if the parent clips.
                if (box.style.textOverflow == 1 && box.style.overflowHidden) {
                    DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    IDWriteInlineObject* ellipsis = nullptr;
                    m_dwrite->CreateEllipsisTrimmingSign(fmt, &ellipsis);
                    if (ellipsis) { lay->SetTrimming(&trim, ellipsis); ellipsis->Release(); }
                    lay->SetMaxWidth(box.contentW);
                }
                // text-shadow: draw shadow copy first.
                if (fs.textShadowSet && fs.textShadowColor.valid && fs.textShadowColor.a > 0) {
                    if (auto* sb = TempBrush(ToD2Dc(fs.textShadowColor)))
                        m_rt->DrawTextLayout(D2D1::Point2F(frag.x + fs.textShadowX, sy + fs.textShadowY), lay, sb);
                }
                m_rt->DrawTextLayout(D2D1::Point2F(frag.x, sy), lay, brush ? brush : m_textBrush);
                lay->Release();
            }
            if (!frag.src->href.empty())
                m_hits.push_back({ frag.x, sy, frag.w, frag.h, frag.src->href });
        }
    }
}

// ─── stacking-order box paint ─────────────────────────────────────────────────

void Renderer::PaintBox(const LayoutBox& box, float scrollY, float topInset, bool underFixed) {
    static thread_local int depth = 0;
    if (depth > 600) return;
    struct G { int& d; G(int& x):d(x){++d;} ~G(){--d;} } g(depth);
    if (box.style.isDisplayNone()) return;
    bool fixed = underFixed || box.style.positionMode == 3;
    float effScroll = fixed ? 0.f : scrollY;

    bool hidden = (box.style.visibilitySet && box.style.visibilityHidden)
                || (box.style.opacitySet && box.style.opacity < 0.01f);

    if (!hidden && !box.href.empty()
        && box.kind != BoxKind::Text && box.kind != BoxKind::Inline
        && box.kind != BoxKind::Break) {
        float hx = box.x;
        float hy = box.y - effScroll + topInset;
        float hw = box.borderBoxW();
        float hh = box.borderBoxH();
        if (hw > 0 && hh > 0)
            m_hits.push_back({ hx, hy, hw, hh, box.href });
    }

    // CSS transform: apply a D2D matrix around this box's center.
    D2D1_MATRIX_3X2_F oldTransform;
    bool hasTransform = box.style.transformSet && m_rt &&
        (box.style.transformTx != 0 || box.style.transformTy != 0
         || box.style.transformScale != 1 || box.style.transformRotate != 0);
    if (hasTransform) {
        m_rt->GetTransform(&oldTransform);
        float cx = box.x + box.borderBoxW() / 2;
        float cy = box.y - effScroll + topInset + box.borderBoxH() / 2;
        float tx = box.style.transformTxPercent
            ? box.borderBoxW() * (box.style.transformTx / 100.f)
            : box.style.transformTx * m_zoom;
        float ty = box.style.transformTyPercent
            ? box.borderBoxH() * (box.style.transformTy / 100.f)
            : box.style.transformTy * m_zoom;
        auto mat = D2D1::Matrix3x2F::Translation(-cx, -cy)
                 * D2D1::Matrix3x2F::Scale(box.style.transformScale, box.style.transformScale)
                 * D2D1::Matrix3x2F::Rotation(box.style.transformRotate)
                 * D2D1::Matrix3x2F::Translation(cx + tx, cy + ty);
        m_rt->SetTransform(mat * oldTransform);
    }

    // 1. This box's own background / borders / replaced content / marker.
    if (!hidden && box.kind != BoxKind::Text && box.kind != BoxKind::Inline
        && box.kind != BoxKind::Break)
        PaintBoxDecorations(box, effScroll, topInset);

    // overflow:hidden/auto/scroll clips all children to the border box.
    bool clipped = false;
    float scrollYBefore = scrollY;
    extern std::map<const Node*, float> g_elementScrollY;
    if (!hidden && m_rt) {
        bool shouldClip = box.style.overflowHidden;
        if (shouldClip) {
            float cx = box.x;
            float cy = box.y - effScroll + topInset;
            float cw = box.borderBoxW();
            float ch = box.borderBoxH();
            if (cw > 0 && ch > 0) {
                m_rt->PushAxisAlignedClip(
                    D2D1::RectF(cx, cy, cx + cw, cy + ch),
                    D2D1_ANTIALIAS_MODE_ALIASED);
                clipped = true;
                // Apply per-element scroll for overflow:auto/scroll.
                if (box.style.overflowMode >= 2 && box.node) {
                    auto it = g_elementScrollY.find(box.node);
                    if (it != g_elementScrollY.end())
                        scrollY += it->second;
                }
            }
        }
    }

    // 2. Children, in CSS2 stacking order (simplified):
    //    negative z-index positioned → in-flow → floats → z>=0 positioned.
    bool simpleInFlowChildren = true;
    for (auto& kptr : box.kids) {
        const LayoutBox* k = kptr.get();
        if (k->isOutOfFlow() || k->isFloat() || k->style.positionMode == 1
            || k->style.zIndexSet) {
            simpleInFlowChildren = false;
            break;
        }
    }
    if (simpleInFlowChildren) {
        float screenY = box.y - effScroll + topInset;
        bool offscreenSimpleSubtree =
               !fixed
            && !hasTransform
            && box.kind != BoxKind::Inline
            && box.kind != BoxKind::Text
            && (screenY + box.borderBoxH() < topInset || screenY > (float)m_height);
        if (offscreenSimpleSubtree) {
            if (clipped) {
                m_rt->PopAxisAlignedClip();
                scrollY = scrollYBefore;
            }
            return;
        }
        if (box.establishesInline) {
            if (!hidden) PaintLines(box, effScroll, topInset, fixed);
        } else {
            for (auto& kptr : box.kids) PaintBox(*kptr, scrollY, topInset, fixed);
        }
        if (clipped) {
            m_rt->PopAxisAlignedClip();
            scrollY = scrollYBefore;
        }
        if (hasTransform) m_rt->SetTransform(oldTransform);
        return;
    }

    std::vector<const LayoutBox*> negZ, inflow, floats, posZ;
    for (auto& kptr : box.kids) {
        const LayoutBox* k = kptr.get();
        if (k->isOutOfFlow()) {
            if (k->style.zIndexSet && k->style.zIndex < 0) negZ.push_back(k);
            else posZ.push_back(k);
        } else if (k->isFloat()) {
            floats.push_back(k);
        } else if (k->style.positionMode == 1) {
            // relative: in normal flow but paints with positioned descendants on top
            posZ.push_back(k);
        } else {
            inflow.push_back(k);
        }
    }
    auto byZ = [](const LayoutBox* a, const LayoutBox* b) {
        int za = a->style.zIndexSet ? a->style.zIndex : 0;
        int zb = b->style.zIndexSet ? b->style.zIndex : 0;
        return za < zb;
    };
    std::stable_sort(negZ.begin(), negZ.end(), byZ);
    std::stable_sort(posZ.begin(), posZ.end(), byZ);

    for (auto* k : negZ)   PaintBox(*k, scrollY, topInset, fixed);

    // In-flow content of THIS box.
    if (box.establishesInline) {
        if (!hidden) PaintLines(box, effScroll, topInset, fixed);
    } else {
        for (auto* k : inflow) PaintBox(*k, scrollY, topInset, fixed);
    }
    for (auto* k : floats) PaintBox(*k, scrollY, topInset, fixed);
    for (auto* k : posZ)   PaintBox(*k, scrollY, topInset, fixed);

    if (clipped) {
        m_rt->PopAxisAlignedClip();
        scrollY = scrollYBefore;
    }
    if (hasTransform) m_rt->SetTransform(oldTransform);
}
