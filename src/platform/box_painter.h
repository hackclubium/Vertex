#pragma once
//
// box_painter.h — cross-platform box-tree painter.
//
// Paints a LayoutBox tree through IPlatformRenderer, with no direct D2D/CG/Cairo
// calls. Used by all platform shells.
//
#include "platform/platform.h"
#include "platform/form_state.h"
#include "render/svg.h"
#include "layout/box.h"
#include "css/style.h"
#include "network/url.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <cmath>

// Per-element scroll offsets for overflow:auto/scroll containers.
struct ScrollableRegion {
    const Node* node = nullptr;
    float x, y, w, h;          // screen coords of the scroll container
    float contentH;             // total content height
    float scrollY = 0;          // current scroll offset within the container
};

struct PaintState {
    IPlatformRenderer* r = nullptr;
    float scrollY = 0;
    float topInset = 0;
    float zoom = 1.f;
    std::string baseUrl;
    std::map<std::string, PlatBitmap>* images = nullptr;
    // <canvas> elements' backing drawing surfaces, keyed by Node* (not URL —
    // canvas content is per-element mutable state, not a content-addressed
    // network resource like `images`). See the "__canvas__" replacedUrl
    // marker branch in PaintBoxDecorations below.
    std::map<const Node*, PlatBitmap>* canvasSurfaces = nullptr;
    std::vector<HitRegion>* hits = nullptr;
    std::map<std::string, PlatFont>* fontCache = nullptr;
    FormState* form = nullptr;
    std::vector<ScrollableRegion>* scrollables = nullptr;
    std::map<const Node*, float>* elementScrollY = nullptr;
    std::function<void(const LayoutBox&, float, float, float, float)> mediaRect;
};

struct PaintYExtent {
    float minY = 0.f;
    float maxY = 0.f;
    bool safe = true;
};

inline PaintYExtent ComputePaintYExtent(const LayoutBox& root) {
    PaintYExtent extent;
    bool initialized = false;
    std::vector<const LayoutBox*> stack;
    stack.push_back(&root);

    while (!stack.empty()) {
        const LayoutBox* box = stack.back();
        stack.pop_back();
        if (!box || box->style.isDisplayNone()) continue;

        if (box->style.positionMode == 3 || box->style.positionMode == 4 || box->style.transformSet) {
            extent.safe = false;
            return extent;
        }

        auto includeY = [&](float top, float bottom) {
            if (!std::isfinite(top) || !std::isfinite(bottom)) return;
            if (bottom < top) std::swap(top, bottom);
            if (!initialized) {
                extent.minY = top;
                extent.maxY = bottom;
                initialized = true;
            } else {
                extent.minY = std::min(extent.minY, top);
                extent.maxY = std::max(extent.maxY, bottom);
            }
        };

        includeY(box->y, box->y + box->borderBoxH());
        for (const auto& line : box->lines) {
            includeY(line.y, line.y + line.h);
            for (const auto& frag : line.frags)
                includeY(frag.y, frag.y + frag.h);
        }
        for (const auto& kid : box->kids)
            stack.push_back(kid.get());
    }

    if (!initialized) {
        extent.minY = root.y;
        extent.maxY = root.y + root.borderBoxH();
    }
    return extent;
}

inline bool CanCullOffscreenPaintSubtree(const LayoutBox& box,
                                         float scrollY,
                                         float topInset,
                                         float viewportH) {
    if (box.style.positionMode == 3 || box.style.positionMode == 4 || box.style.transformSet) return false;
    if (box.kind == BoxKind::Inline || box.kind == BoxKind::Text || box.kind == BoxKind::Break)
        return false;

    const float boxTop = box.y - scrollY + topInset;
    const float boxBottom = boxTop + box.borderBoxH();
    if (boxBottom >= topInset && boxTop <= viewportH) return false;

    PaintYExtent extent = ComputePaintYExtent(box);
    if (!extent.safe) return false;
    const float screenMin = extent.minY - scrollY + topInset;
    const float screenMax = extent.maxY - scrollY + topInset;
    return screenMax < topInset || screenMin > viewportH;
}

inline PlatColor ToPlatColor(const CssColor& c) { return { c.r, c.g, c.b, c.a }; }

inline PlatFont GetOrCreateFont(PaintState& ps, const FontKey& f) {
    std::string key = std::to_string((int)(f.size * 4))
        + (f.bold ? "b" : "-") + (f.italic ? "i" : "-") + (f.mono ? "m" : "-") + f.family;
    auto it = ps.fontCache->find(key);
    if (it != ps.fontCache->end()) return it->second;
    PlatFont pf = ps.r->CreateFont(f.size, f.bold, f.italic, f.mono, f.family);
    (*ps.fontCache)[key] = pf;
    return pf;
}

inline std::wstring NodeTextContentWide(const Node* n) {
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

// ── paint functions ──────────────────────────────────────────────────────────

inline void PaintBoxDecorations(PaintState& ps, const LayoutBox& box) {
    const ComputedStyle& s = box.style;
    float sx = box.x;
    float sy = box.y - ps.scrollY + ps.topInset;
    float bw = box.borderBoxW();
    float bh = box.borderBoxH();

    if (sy + bh < ps.topInset || sy > (float)ps.r->Height()) return;

    // Box shadow (painted before background so it appears behind).
    if (s.shadowSet && s.shadowColor.valid && s.shadowColor.a > 0.01f && !s.shadowInset) {
        PlatColor sc = ToPlatColor(s.shadowColor);
        float blur = s.shadowBlur;
        float spread = s.shadowSpread;
        float shx = sx + s.shadowX - spread;
        float shy = sy + s.shadowY - spread;
        float shw = bw + spread * 2;
        float shh = bh + spread * 2;
        if (blur < 1.f) {
            ps.r->FillRect(shx, shy, shw, shh, sc);
        } else {
            // Approximate blur with concentric fading rectangles.
            int steps = std::min((int)(blur / 2) + 1, 8);
            for (int i = steps; i >= 0; --i) {
                float f = (float)i / (float)steps;
                float expand = blur * f;
                PlatColor fc = sc;
                fc.a = sc.a * (1.f - f) * 0.5f;
                ps.r->FillRect(shx - expand, shy - expand,
                              shw + expand * 2, shh + expand * 2, fc);
            }
        }
    }

    // Background color
    if (s.bgColor.valid && s.bgColor.a > 0.001f) {
        ps.r->FillRect(sx, sy, bw, bh, ToPlatColor(s.bgColor));
    }
    // Linear gradient
    if (s.gradientSet && s.gradientStops.size() >= 2 && bw > 0 && bh > 0) {
        float rad = s.gradientAngle * 3.14159265f / 180.f;
        float dx = std::sin(rad), dy = -std::cos(rad);
        float halfW = bw / 2, halfH = bh / 2;
        float gradLen = std::abs(dx * bw) + std::abs(dy * bh);
        if (gradLen < 1) gradLen = 1;
        float cx = sx + halfW, cy = sy + halfH;
        int steps = (int)std::max(bw, bh);
        if (steps > 400) steps = 400;
        float stripW = (dx != 0 && std::abs(dy) < 0.001f) ? bw / steps : bw;
        float stripH = (dy != 0 && std::abs(dx) < 0.001f) ? bh / steps : bh;
        bool horizontal = std::abs(dx) > std::abs(dy);
        for (int i = 0; i < steps; ++i) {
            float t;
            float rx, ry, rw, rh;
            if (horizontal) {
                rw = bw / steps + 1; rh = bh;
                rx = sx + i * (bw / steps); ry = sy;
                float px = rx + rw / 2 - cx, py = 0;
                t = (px * dx + py * dy) / (gradLen / 2) * 0.5f + 0.5f;
            } else {
                rw = bw; rh = bh / steps + 1;
                rx = sx; ry = sy + i * (bh / steps);
                float px = 0, py = ry + rh / 2 - cy;
                t = (px * dx + py * dy) / (gradLen / 2) * 0.5f + 0.5f;
            }
            t = std::max(0.f, std::min(1.f, t));
            // Find the two stops surrounding t.
            size_t si = 0;
            for (size_t j = 1; j < s.gradientStops.size(); ++j)
                if (s.gradientStops[j].pos <= t) si = j;
            size_t ei = std::min(si + 1, s.gradientStops.size() - 1);
            float range = s.gradientStops[ei].pos - s.gradientStops[si].pos;
            float frac = range > 0.001f ? (t - s.gradientStops[si].pos) / range : 0.f;
            frac = std::max(0.f, std::min(1.f, frac));
            auto& c0 = s.gradientStops[si].color;
            auto& c1 = s.gradientStops[ei].color;
            PlatColor gc = {
                c0.r + (c1.r - c0.r) * frac,
                c0.g + (c1.g - c0.g) * frac,
                c0.b + (c1.b - c0.b) * frac,
                c0.a + (c1.a - c0.a) * frac
            };
            ps.r->FillRect(rx, ry, rw, rh, gc);
        }
    }

    // Background image
    if (!s.backgroundImage.empty() && !(s.bgNoRepeat && s.bgFixed) && ps.images) {
        std::string url = ResolveUrlAgainstBase(s.backgroundImage, ps.baseUrl);
        auto it = ps.images->find(url);
        if (it != ps.images->end() && it->second) {
            int rep = s.bgRepeatSet ? s.bgRepeat : (s.bgNoRepeat ? 3 : 0);
            float tw = bw, th = bh;
            float ox = s.bgPosXPct ? (bw - tw) * (s.bgPosX / 100.f) : s.bgPosX;
            float oy = s.bgPosYPct ? (bh - th) * (s.bgPosY / 100.f) : s.bgPosY;
            bool repX = (rep == 0 || rep == 1);
            bool repY = (rep == 0 || rep == 2);
            if (rep == 3) {
                ps.r->PushClip(sx, sy, bw, bh);
                ps.r->DrawBitmap(it->second, sx + ox, sy + oy, tw, th);
                ps.r->PopClip();
            } else {
                ps.r->DrawBitmapTiled(it->second, sx, sy, bw, bh, tw, th, ox, oy, repX, repY);
            }
        }
    }

    // Replaced image or inline SVG
    if (box.kind == BoxKind::Replaced && !box.replacedUrl.empty()) {
        float cx = box.contentX();
        float cy = box.contentY() - ps.scrollY + ps.topInset;
        if (box.replacedUrl == "__svg__" && box.node) {
            // Render inline SVG to a bitmap on the fly.
            int svgMax = std::min(1024, (int)std::max(box.contentW, box.contentH));
            auto svgBmp = svg::renderSvg(box.node, std::max(16, svgMax));
            if (svgBmp.width > 0 && svgBmp.height > 0) {
                PlatBitmap bmp = ps.r->CreateBitmap(svgBmp.width, svgBmp.height, svgBmp.pixels.data());
                if (bmp) {
                    ps.r->DrawBitmap(bmp, cx, cy, box.contentW, box.contentH);
                    ps.r->ReleaseBitmap(bmp);
                }
            }
        } else if (box.replacedUrl == "__canvas__" && box.node && ps.canvasSurfaces) {
            // <canvas>'s backing surface is owned/persisted by the platform
            // adapter (not per-frame like inline SVG above) — content must
            // survive across repaints since JS draws into it once, not every
            // frame. Composited exactly like a decoded <img> bitmap.
            auto it = ps.canvasSurfaces->find(box.node);
            if (it != ps.canvasSurfaces->end() && it->second) {
                ps.r->DrawBitmap(it->second, cx, cy, box.contentW, box.contentH);
            }
        } else if (ps.images) {
            auto it = ps.images->find(box.replacedUrl);
            if (it != ps.images->end() && it->second) {
                ps.r->DrawBitmap(it->second, cx, cy, box.contentW, box.contentH);
            }
        }
    }

    if (box.kind == BoxKind::Replaced && box.node
        && (box.node->tagName == "video" || box.node->tagName == "audio")) {
        float cx = box.contentX();
        float cy = box.contentY() - ps.scrollY + ps.topInset;
        ps.r->FillRect(cx, cy, box.contentW, box.contentH,
            box.node->tagName == "video" ? PlatColor{0.02f, 0.02f, 0.025f, 1.f}
                                         : PlatColor{0.94f, 0.94f, 0.94f, 1.f});
        ps.r->DrawRect(cx, cy, box.contentW, box.contentH, PlatColor{0.35f, 0.35f, 0.38f, 1.f}, 1.f);
        if (ps.mediaRect) ps.mediaRect(box, cx, cy, box.contentW, box.contentH);
    }

    // Borders
    auto borderColor = [&](const CssColor& side) -> PlatColor {
        if (side.valid) return ToPlatColor(side);
        if (s.borderColor.valid) return ToPlatColor(s.borderColor);
        if (s.color.valid) return ToPlatColor(s.color);
        return { 0.7f, 0.7f, 0.7f, 1.f };
    };
    if (box.borderTop > 0)
        ps.r->FillRect(sx, sy, bw, box.borderTop, borderColor(s.borderTopColor));
    if (box.borderBottom > 0)
        ps.r->FillRect(sx, sy + bh - box.borderBottom, bw, box.borderBottom, borderColor(s.borderBottomColor));
    if (box.borderLeft > 0)
        ps.r->FillRect(sx, sy, box.borderLeft, bh, borderColor(s.borderLeftColor));
    if (box.borderRight > 0)
        ps.r->FillRect(sx + bw - box.borderRight, sy, box.borderRight, bh, borderColor(s.borderRightColor));

    // Outline (drawn outside the border box)
    if (s.outlineSet && s.outlineWidth > 0) {
        PlatColor oc = s.outlineColor.valid ? ToPlatColor(s.outlineColor) : PlatColor{0,0,0,1};
        float ow = s.outlineWidth;
        ps.r->DrawRect(sx - ow, sy - ow, bw + ow*2, bh + ow*2, oc, ow);
    }

    // List marker
    if (box.kind == BoxKind::ListItem) {
        FontKey fk;
        fk.size = std::max(1.f, (s.fontSize > 0 ? s.fontSize : 16.f));
        fk.bold = s.bold;
        PlatFont font = GetOrCreateFont(ps, fk);
        float markerY = box.contentY() - ps.scrollY + ps.topInset;
        PlatColor tc = s.color.valid ? ToPlatColor(s.color) : PlatColor{0,0,0,1};
        ps.r->DrawText(L"•", box.contentX() - 16.f, markerY, 16.f, 24.f, font, tc);
    }

    // Form controls: draw the control chrome + typed text.
    if (box.kind == BoxKind::Replaced && box.node && ps.form) {
        const std::string& tag = box.node->tagName;
        if (tag == "input" || tag == "textarea" || tag == "button" || tag == "select") {
            float cx = box.contentX();
            float cy = box.contentY() - ps.scrollY + ps.topInset;
            float cw = box.contentW;
            float ch = box.contentH;
            bool focused = (ps.form->focusedInput == box.node);
            bool authorPaintedChrome = s.bgColor.valid || s.borderColor.valid
                || box.borderTop > 0.01f || box.borderRight > 0.01f
                || box.borderBottom > 0.01f || box.borderLeft > 0.01f;
            bool drawNativeChrome = !authorPaintedChrome;
            if (drawNativeChrome) {
                ps.r->FillRect(cx, cy, cw, ch,
                    tag == "button" ? PlatColor{0.94f,0.94f,0.94f,1.f}
                                    : PlatColor{1.f,1.f,1.f,1.f});
                PlatColor border = focused ? PlatColor{0.2f,0.4f,0.8f,1} : PlatColor{0.7f,0.7f,0.7f,1};
                ps.r->DrawRect(cx, cy, cw, ch, border, focused ? 2.f : 1.f);
            }
            if (tag == "button" || tag == "select") {
                FontKey fk; fk.size = std::max(1.f, (s.fontSize > 0 ? s.fontSize : 14.f));
                fk.bold = s.bold; fk.italic = s.italic; fk.family = s.fontFamily;
                PlatFont font = GetOrCreateFont(ps, fk);
                std::wstring label = NodeTextContentWide(box.node);
                if (!label.empty())
                    ps.r->DrawText(label, cx + 8, cy + 2, cw - 16, ch - 4, font, {0.08f,0.08f,0.08f,1});
                if (tag == "select" && drawNativeChrome) {
                    float ax = cx + cw - 15.f;
                    float ay = cy + ch * 0.5f - 2.f;
                    ps.r->DrawLine(ax, ay, ax + 4.f, ay + 4.f, {0.2f,0.2f,0.2f,1}, 1.5f);
                    ps.r->DrawLine(ax + 4.f, ay + 4.f, ax + 8.f, ay, {0.2f,0.2f,0.2f,1}, 1.5f);
                }
                return;
            }
            // Draw the value text.
            std::string val = ps.form->getValue(const_cast<Node*>(box.node));
            if (!val.empty()) {
                FontKey fk; fk.size = std::max(1.f, (s.fontSize > 0 ? s.fontSize : 14.f));
                PlatFont font = GetOrCreateFont(ps, fk);
                std::wstring wval; for (char c : val) wval += (wchar_t)(unsigned char)c;
                ps.r->DrawText(wval, cx + 4, cy + 2, cw - 8, ch - 4, font, {0,0,0,1});
                // Draw cursor if focused.
                if (focused && ps.form->cursorVisible) {
                    float cursorX = cx + 4 + ps.r->MeasureText(wval.substr(0, ps.form->cursorPos), font);
                    ps.r->FillRect(cursorX, cy + 3, 1.f, ch - 6, {0,0,0,1});
                }
            } else {
                // Placeholder.
                std::string ph = box.node->attr("placeholder");
                if (!ph.empty()) {
                    FontKey fk; fk.size = std::max(1.f, (s.fontSize > 0 ? s.fontSize : 14.f));
                    PlatFont font = GetOrCreateFont(ps, fk);
                    std::wstring wph; for (char c : ph) wph += (wchar_t)(unsigned char)c;
                    ps.r->DrawText(wph, cx + 4, cy + 2, cw - 8, ch - 4, font, {0.6f,0.6f,0.6f,1});
                }
                if (focused && ps.form->cursorVisible) {
                    ps.r->FillRect(cx + 4, cy + 3, 1.f, ch - 6, {0,0,0,1});
                }
            }
            // Register a hit region so clicks can focus this input.
            if (ps.hits) {
                ps.hits->push_back({ cx, cy, cw, ch, "__input__", false, "" });
            }
        }
    }
}

inline void PaintLines(PaintState& ps, const LayoutBox& box) {
    for (const auto& line : box.lines) {
        float lineY = line.y - ps.scrollY + ps.topInset;
        if (lineY + line.h < ps.topInset || lineY > (float)ps.r->Height())
            continue;
        for (const auto& frag : line.frags) {
            if (!frag.src) continue;
            if (frag.src->kind == BoxKind::InlineBlock || frag.src->kind == BoxKind::Replaced) {
                // Paint the atomic box recursively
                extern void PaintBoxTree(PaintState& ps, const LayoutBox& box);
                PaintBoxTree(ps, *frag.src);
                continue;
            }
            const ComputedStyle& fs = frag.src->style;
            if (fs.visibilitySet && fs.visibilityHidden) continue;
            if (fs.opacitySet && fs.opacity < 0.01f) continue;
            float sy = frag.y - ps.scrollY + ps.topInset;
            if (sy + frag.h < ps.topInset || sy > (float)ps.r->Height()) {
                continue; // offscreen text/link fragments do not need paint or hit regions
            }
            FontKey fk;
            fk.size = std::clamp((fs.fontSize > 0 ? fs.fontSize : 16.f), 1.f, 40.f);
            fk.bold = fs.bold; fk.italic = fs.italic; fk.family = fs.fontFamily;
            std::string fl; for (char c : fs.fontFamily) fl += (char)std::tolower((unsigned char)c);
            fk.mono = (fl.find("mono") != std::string::npos || fl.find("consol") != std::string::npos
                    || fl.find("courier") != std::string::npos);
            PlatFont font = GetOrCreateFont(ps, fk);
            PlatColor color = fs.color.valid ? ToPlatColor(fs.color)
                            : (!frag.src->href.empty() ? PlatColor{0.1f,0.1f,0.8f,1} : PlatColor{0,0,0,1});
            bool underline = !fs.noUnderline && (fs.underline || !frag.src->href.empty());
            // text-shadow
            if (fs.textShadowSet && fs.textShadowColor.valid && fs.textShadowColor.a > 0)
                ps.r->DrawText(frag.text, frag.x + fs.textShadowX, sy + fs.textShadowY,
                               frag.w + 4.f, frag.h * 2.f + 4.f, font, ToPlatColor(fs.textShadowColor));
            ps.r->DrawText(frag.text, frag.x, sy, frag.w + 4.f, frag.h * 2.f + 4.f,
                           font, color, underline);
            if (fs.lineThrough) {
                // Strike line at roughly the text's x-height midpoint.
                float thick = std::max(1.f, fk.size / 14.f);
                float strikeY = sy + frag.h * 0.5f - thick * 0.5f;
                ps.r->FillRect(frag.x, strikeY, frag.w, thick, color);
            }
            if (ps.hits && !frag.src->href.empty())
                ps.hits->push_back({ frag.x, sy, frag.w, frag.h,
                    frag.src->href, frag.src->download, frag.src->downloadName });
        }
    }
}

inline void PaintBoxTree(PaintState& ps, const LayoutBox& box) {
    static thread_local int depth = 0;
    if (depth > 600) return;
    struct G { int& d; G(int& x):d(x){++d;} ~G(){--d;} } g(depth);
    if (box.style.isDisplayNone()) return;

    float authoredOpacity = box.style.opacitySet ? box.style.opacity : 1.f;
    if (box.style.filterSet) authoredOpacity *= box.style.filterOpacity;
    authoredOpacity = std::clamp(authoredOpacity, 0.f, 1.f);
    bool hidden = (box.style.visibilitySet && box.style.visibilityHidden)
               || authoredOpacity < 0.01f;

    if (CanCullOffscreenPaintSubtree(box, ps.scrollY, ps.topInset, (float)ps.r->Height()))
        return;

    if (!hidden && ps.hits && !box.href.empty()
        && box.kind != BoxKind::Text && box.kind != BoxKind::Inline
        && box.kind != BoxKind::Break) {
        float hitScrollY = box.style.positionMode == 3 ? 0.f : ps.scrollY;
        float hx = box.x;
        float hy = box.y - hitScrollY + ps.topInset;
        float hw = box.borderBoxW();
        float hh = box.borderBoxH();
        bool hitVisible = hy + hh >= ps.topInset && hy <= (float)ps.r->Height();
        if (hitVisible && hw > 0 && hh > 0)
            ps.hits->push_back({ hx, hy, hw, hh, box.href, box.download, box.downloadName });
    }

    // CSS transform: for the cross-platform painter we apply translate by
    // temporarily shifting scrollY/topInset (which offsets all paint coords).
    // Scale and rotate would need native matrix support per-renderer.
    float savedScrollY = ps.scrollY;
    float savedTopInset = ps.topInset;
    float tx = box.style.transformTxPercent
        ? box.borderBoxW() * (box.style.transformTx / 100.f)
        : box.style.transformTx;
    float ty = box.style.transformTyPercent
        ? box.borderBoxH() * (box.style.transformTy / 100.f)
        : box.style.transformTy;
    if (box.style.transformSet) {
        ps.topInset += ty;
        // Horizontal translate: shift all x coords by adjusting the box's
        // paint origin. We cast away const to apply the offset temporarily.
        const_cast<LayoutBox&>(box).x += tx;
    }

    bool cssClipped = false;
    if (!hidden && box.style.clipRectSet) {
        float effScroll = box.style.positionMode == 3 ? 0.f : ps.scrollY;
        float clipX = box.x + box.style.clipLeft;
        float clipY = box.y - effScroll + ps.topInset + box.style.clipTop;
        float clipW = box.style.clipRight - box.style.clipLeft;
        float clipH = box.style.clipBottom - box.style.clipTop;
        if (clipW <= 0.f || clipH <= 0.f) hidden = true;
        else { ps.r->PushClip(clipX, clipY, clipW, clipH); cssClipped = true; }
    }

    // Decorations
    if (!hidden && box.kind != BoxKind::Text && box.kind != BoxKind::Inline && box.kind != BoxKind::Break)
        PaintBoxDecorations(ps, box);

    // overflow:hidden/auto/scroll clip + scroll offset
    bool clipped = false;
    float savedScrollForOverflow = ps.scrollY;
    if (box.style.overflowHidden && !hidden) {
        float effScroll = box.style.positionMode == 3 ? 0.f : ps.scrollY;
        if (box.style.positionMode == 4 && box.style.topSet) {
            float minY = ps.topInset + box.style.top * ps.zoom;
            float natural = box.y - ps.scrollY + ps.topInset;
            if (natural < minY) effScroll = box.y + ps.topInset - minY;
        }
        float cx = box.x, cy = box.y - effScroll + ps.topInset;
        float cw = box.borderBoxW(), ch = box.borderBoxH();
        if (cw > 0 && ch > 0) {
            ps.r->PushClip(cx, cy, cw, ch);
            clipped = true;
            // For auto/scroll, apply the element's own scroll offset.
            if (box.style.overflowMode >= 2 && box.node && ps.elementScrollY) {
                auto it = ps.elementScrollY->find(box.node);
                float elScroll = (it != ps.elementScrollY->end()) ? it->second : 0.f;
                ps.scrollY += elScroll;
                // Register as a scrollable region so the shell can route wheel events.
                if (ps.scrollables) {
                    ps.scrollables->push_back({
                        box.node, cx, cy, cw, ch,
                        box.contentH + box.padTop + box.padBottom,
                        elScroll
                    });
                }
            }
        }
    }

    // Children (simplified stacking: in-flow, then floats, then positioned)
    bool simpleInFlowChildren = true;
    for (auto& k : box.kids) {
        if (k->isOutOfFlow() || k->isFloat() || k->style.positionMode == 1 || k->style.positionMode == 4
            || k->style.zIndexSet) {
            simpleInFlowChildren = false;
            break;
        }
    }
    if (simpleInFlowChildren) {
        float screenY = box.y - ps.scrollY + ps.topInset;
        bool offscreenSimpleSubtree =
               box.style.positionMode != 3
            && !box.style.transformSet
            && box.kind != BoxKind::Inline
            && box.kind != BoxKind::Text
            && (screenY + box.borderBoxH() < ps.topInset || screenY > (float)ps.r->Height());
        if (offscreenSimpleSubtree) {
            if (clipped) {
                ps.r->PopClip();
                ps.scrollY = savedScrollForOverflow;
            }
            if (cssClipped) ps.r->PopClip();
            return;
        }
        // A PushClip above must always be matched by PopClip before this
        // function returns, or the clip stack stays corrupted for every
        // frame painted afterward — one bad subtree could then blank out
        // every page painted from then on. Cairo's per-frame-fresh cairo_t
        // limits the blast radius to a single frame today (unlike D2D's
        // long-lived render target, where the equivalent bug was real), but
        // this stays correct if that ever changes (e.g. longer-lived canvas
        // contexts).
        try {
            if (box.establishesInline) {
                if (!hidden) PaintLines(ps, box);
            } else {
                for (auto& k : box.kids) PaintBoxTree(ps, *k);
            }
        } catch (...) {
            if (clipped) { ps.r->PopClip(); ps.scrollY = savedScrollForOverflow; }
            if (cssClipped) ps.r->PopClip();
            throw;
        }
        if (clipped) {
            ps.r->PopClip();
            ps.scrollY = savedScrollForOverflow;
        }
        if (cssClipped) ps.r->PopClip();
        if (box.style.transformSet) {
            const_cast<LayoutBox&>(box).x -= tx;
            ps.scrollY = savedScrollY;
            ps.topInset = savedTopInset;
        }
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
        } else if (k->style.positionMode == 1 || k->style.positionMode == 4) {
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

    // Same exception-safety rationale as the simple-children path above.
    try {
        for (auto* k : negZ)   PaintBoxTree(ps, *k);
        if (box.establishesInline) {
            if (!hidden) PaintLines(ps, box);
        } else {
            for (auto* k : inflow) PaintBoxTree(ps, *k);
        }
        for (auto* k : floats) PaintBoxTree(ps, *k);
        for (auto* k : posZ)   PaintBoxTree(ps, *k);
    } catch (...) {
        if (clipped) { ps.r->PopClip(); ps.scrollY = savedScrollForOverflow; }
        if (cssClipped) ps.r->PopClip();
        throw;
    }

    if (clipped) {
        ps.r->PopClip();
        ps.scrollY = savedScrollForOverflow;
    }
    if (cssClipped) ps.r->PopClip();

    // Restore transform offsets.
    if (box.style.transformSet) {
        const_cast<LayoutBox&>(box).x -= tx;
        ps.scrollY = savedScrollY;
        ps.topInset = savedTopInset;
    }
}
