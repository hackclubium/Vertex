#pragma once
//
// animation.h — CSS @keyframes animation system for Vertex.
//
// Interpolates opacity/background-color/color/transform across the
// keyframe stops matched at animation start, driven by wall-clock time.
// Mirrors transition.h's structure; the platform shell's existing tick()
// (which already drives TransitionManager) also drives this.
//
#include "css/style.h"
#include "css/stylesheet.h"
#include "html/dom.h"
#include <map>
#include <chrono>
#include <cmath>
#include <algorithm>

struct AnimationEntry {
    std::vector<KeyframeStop> stops; // sorted by percent, size >= 1
    float duration;      // seconds
    float delay;         // seconds
    float iterationCount; // -1 = infinite
    int direction;        // 0=normal,1=reverse,2=alternate,3=alternate-reverse
    int fillMode;         // 0=none,1=forwards,2=backwards,3=both
    int timingFunction;   // 0=ease,1=linear,2=ease-in,3=ease-out,4=ease-in-out
    std::string name;     // identifies whether the animation-name changed
    std::chrono::steady_clock::time_point startTime;
    bool done = false;    // finished with forwards/both fill: still applied, no longer ticking
    ComputedStyle frozenStyle; // the stop to keep applying once done
};

class AnimationManager {
public:
    static AnimationManager& instance() {
        static AnimationManager mgr;
        return mgr;
    }

    // Starts (or restarts, if animation identity/timing changed) tracking for a node.
    void ensureStarted(const Node* node, const ComputedStyle& style, const Stylesheet& sheet) {
        if (!style.animationSet || style.animationName.empty() || style.animationDuration <= 0) {
            m_anims.erase(node);
            return;
        }
        auto it = m_anims.find(node);
        if (it != m_anims.end()
            && it->second.name == style.animationName
            && it->second.duration == style.animationDuration
            && it->second.delay == style.animationDelay
            && it->second.iterationCount == style.animationIterationCount
            && it->second.direction == style.animationDirection
            && it->second.fillMode == style.animationFillMode
            && it->second.timingFunction == style.animationTimingFunction) return;

        auto kf = sheet.keyframes.find(style.animationName);
        if (kf == sheet.keyframes.end() || kf->second.empty()) { m_anims.erase(node); return; }

        AnimationEntry entry;
        entry.stops = kf->second;
        entry.duration = style.animationDuration;
        entry.delay = style.animationDelay;
        entry.iterationCount = style.animationIterationCount;
        entry.direction = style.animationDirection;
        entry.fillMode = style.animationFillMode;
        entry.timingFunction = style.animationTimingFunction;
        entry.name = style.animationName;
        entry.startTime = std::chrono::steady_clock::now();
        m_anims[node] = std::move(entry);
    }

    // Applies current animation state to a style. Returns true if still animating
    // (caller should schedule a repaint).
    bool applyAnimation(const Node* node, ComputedStyle& style) {
        auto it = m_anims.find(node);
        if (it == m_anims.end()) return false;
        auto& e = it->second;
        if (e.done) { applyStop(e.frozenStyle, style); return false; }

        float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - e.startTime).count() - e.delay;
        if (elapsed < 0.f) {
            // backwards/both fill applies the first stop's values during the delay.
            if (e.fillMode == 2 || e.fillMode == 3) applyStop(e.stops.front().style, style);
            return true;
        }

        bool finite = e.iterationCount >= 0.f;
        float totalDur = finite ? e.duration * e.iterationCount : 0.f;
        bool finished = finite && elapsed >= totalDur;
        float cycleT = finished ? e.duration : std::fmod(elapsed, e.duration);
        long long iterationIndex = finished ? (long long)std::floor((double)e.iterationCount) - 1
                                             : (long long)std::floor(elapsed / e.duration);

        bool reversedThisCycle = (e.direction == 1)
            || (e.direction == 2 && (iterationIndex % 2) == 1)
            || (e.direction == 3 && (iterationIndex % 2) == 0);
        float t = cycleT / e.duration;
        if (reversedThisCycle) t = 1.f - t;

        if (finished && (e.fillMode == 0 || e.fillMode == 2)) {
            // none/backwards: nothing holds after the last iteration — leave the authored style.
            m_anims.erase(it);
            return false;
        }

        ComputedStyle frame = interpolate(e.stops, ease(t, e.timingFunction));
        applyStop(frame, style);
        if (finished) { e.frozenStyle = frame; e.done = true; return false; }
        return true;
    }

    bool hasActiveAnimations() const {
        for (const auto& [node, e] : m_anims) if (!e.done) return true;
        return false;
    }
    void clear() { m_anims.clear(); }

private:
    static float ease(float t, int fn) {
        t = std::clamp(t, 0.f, 1.f);
        switch (fn) {
            case 1: return t; // linear
            case 2: return t * t; // ease-in (approximation)
            case 3: return 1.f - (1.f - t) * (1.f - t); // ease-out
            case 4: return t < 0.5f ? 2.f * t * t : 1.f - std::pow(-2.f * t + 2.f, 2.f) / 2.f; // ease-in-out
            default: return t < 0.5f ? 2.f * t * t : 1.f - std::pow(-2.f * t + 2.f, 2.f) / 2.f; // ease (approximation)
        }
    }

    // Blends the two keyframe stops bounding percent `t*100`, mirroring
    // TransitionManager's per-property lerp.
    static ComputedStyle interpolate(const std::vector<KeyframeStop>& stops, float t) {
        float pct = t * 100.f;
        const KeyframeStop* a = &stops.front();
        const KeyframeStop* b = &stops.back();
        for (size_t i = 0; i + 1 < stops.size(); ++i) {
            if (pct >= stops[i].percent && pct <= stops[i + 1].percent) {
                a = &stops[i]; b = &stops[i + 1]; break;
            }
        }
        float span = b->percent - a->percent;
        float local = span > 0.f ? std::clamp((pct - a->percent) / span, 0.f, 1.f) : 0.f;

        ComputedStyle out = a->style;
        auto lerp = [&](float x, float y) { return x + (y - x) * local; };
        auto lerpSet = [&](float& dst, float x, float y) { dst = lerp(x, y); };
        auto bothSet = [](float x, float y) { return x > kCssNotSet + 1.f && y > kCssNotSet + 1.f; };
        auto lerpColor = [&](CssColor x, CssColor y) {
            x.r = lerp(x.r, y.r);
            x.g = lerp(x.g, y.g);
            x.b = lerp(x.b, y.b);
            x.a = lerp(x.a, y.a);
            return x;
        };
        if (a->style.opacitySet && b->style.opacitySet)
            out.opacity = lerp(a->style.opacity, b->style.opacity);
        if (a->style.fontSize > 0 && b->style.fontSize > 0) lerpSet(out.fontSize, a->style.fontSize, b->style.fontSize);
        if (a->style.lineHeight > 0 && b->style.lineHeight > 0) lerpSet(out.lineHeight, a->style.lineHeight, b->style.lineHeight);
        if (a->style.letterSpacingSet && b->style.letterSpacingSet) { out.letterSpacing = lerp(a->style.letterSpacing, b->style.letterSpacing); out.letterSpacingSet = true; }
        if (bothSet(a->style.marginTop, b->style.marginTop)) lerpSet(out.marginTop, a->style.marginTop, b->style.marginTop);
        if (bothSet(a->style.marginRight, b->style.marginRight)) lerpSet(out.marginRight, a->style.marginRight, b->style.marginRight);
        if (bothSet(a->style.marginBottom, b->style.marginBottom)) lerpSet(out.marginBottom, a->style.marginBottom, b->style.marginBottom);
        if (bothSet(a->style.marginLeft, b->style.marginLeft)) lerpSet(out.marginLeft, a->style.marginLeft, b->style.marginLeft);
        if (a->style.paddingTop >= 0 && b->style.paddingTop >= 0) lerpSet(out.paddingTop, a->style.paddingTop, b->style.paddingTop);
        if (a->style.paddingRight >= 0 && b->style.paddingRight >= 0) lerpSet(out.paddingRight, a->style.paddingRight, b->style.paddingRight);
        if (a->style.paddingBottom >= 0 && b->style.paddingBottom >= 0) lerpSet(out.paddingBottom, a->style.paddingBottom, b->style.paddingBottom);
        if (a->style.paddingLeft >= 0 && b->style.paddingLeft >= 0) lerpSet(out.paddingLeft, a->style.paddingLeft, b->style.paddingLeft);
        if (a->style.borderWidth >= 0 && b->style.borderWidth >= 0) lerpSet(out.borderWidth, a->style.borderWidth, b->style.borderWidth);
        if (a->style.borderTopWidth >= 0 && b->style.borderTopWidth >= 0) lerpSet(out.borderTopWidth, a->style.borderTopWidth, b->style.borderTopWidth);
        if (a->style.borderRightWidth >= 0 && b->style.borderRightWidth >= 0) lerpSet(out.borderRightWidth, a->style.borderRightWidth, b->style.borderRightWidth);
        if (a->style.borderBottomWidth >= 0 && b->style.borderBottomWidth >= 0) lerpSet(out.borderBottomWidth, a->style.borderBottomWidth, b->style.borderBottomWidth);
        if (a->style.borderLeftWidth >= 0 && b->style.borderLeftWidth >= 0) lerpSet(out.borderLeftWidth, a->style.borderLeftWidth, b->style.borderLeftWidth);
        if (a->style.width >= 0 && b->style.width >= 0) lerpSet(out.width, a->style.width, b->style.width);
        if (a->style.widthPercent >= 0 && b->style.widthPercent >= 0) lerpSet(out.widthPercent, a->style.widthPercent, b->style.widthPercent);
        if (a->style.height >= 0 && b->style.height >= 0) lerpSet(out.height, a->style.height, b->style.height);
        if (a->style.heightPercent >= 0 && b->style.heightPercent >= 0) lerpSet(out.heightPercent, a->style.heightPercent, b->style.heightPercent);
        if (a->style.minWidth >= 0 && b->style.minWidth >= 0) lerpSet(out.minWidth, a->style.minWidth, b->style.minWidth);
        if (a->style.maxWidth >= 0 && b->style.maxWidth >= 0) lerpSet(out.maxWidth, a->style.maxWidth, b->style.maxWidth);
        if (a->style.minHeight >= 0 && b->style.minHeight >= 0) lerpSet(out.minHeight, a->style.minHeight, b->style.minHeight);
        if (a->style.maxHeight >= 0 && b->style.maxHeight >= 0) lerpSet(out.maxHeight, a->style.maxHeight, b->style.maxHeight);
        if (a->style.topSet && b->style.topSet && a->style.topPercent == b->style.topPercent) { out.top = lerp(a->style.top, b->style.top); out.topSet = true; out.topPercent = a->style.topPercent; }
        if (a->style.rightSet && b->style.rightSet && a->style.rightPercent == b->style.rightPercent) { out.right = lerp(a->style.right, b->style.right); out.rightSet = true; out.rightPercent = a->style.rightPercent; }
        if (a->style.bottomSet && b->style.bottomSet && a->style.bottomPercent == b->style.bottomPercent) { out.bottom = lerp(a->style.bottom, b->style.bottom); out.bottomSet = true; out.bottomPercent = a->style.bottomPercent; }
        if (a->style.leftSet && b->style.leftSet && a->style.leftPercent == b->style.leftPercent) { out.left = lerp(a->style.left, b->style.left); out.leftSet = true; out.leftPercent = a->style.leftPercent; }
        if (a->style.flexGrowSet && b->style.flexGrowSet) { out.flexGrow = lerp(a->style.flexGrow, b->style.flexGrow); out.flexGrowSet = true; }
        if (a->style.flexShrinkSet && b->style.flexShrinkSet) { out.flexShrink = lerp(a->style.flexShrink, b->style.flexShrink); out.flexShrinkSet = true; }
        if (a->style.flexBasisSet && b->style.flexBasisSet) { out.flexBasis = lerp(a->style.flexBasis, b->style.flexBasis); out.flexBasisSet = true; }
        if (a->style.flexGap >= 0 && b->style.flexGap >= 0) lerpSet(out.flexGap, a->style.flexGap, b->style.flexGap);
        if (a->style.flexRowGap >= 0 && b->style.flexRowGap >= 0) lerpSet(out.flexRowGap, a->style.flexRowGap, b->style.flexRowGap);
        if (a->style.flexColumnGap >= 0 && b->style.flexColumnGap >= 0) lerpSet(out.flexColumnGap, a->style.flexColumnGap, b->style.flexColumnGap);
        if (a->style.gridGap >= 0 && b->style.gridGap >= 0) lerpSet(out.gridGap, a->style.gridGap, b->style.gridGap);
        if (a->style.gridRowGap >= 0 && b->style.gridRowGap >= 0) lerpSet(out.gridRowGap, a->style.gridRowGap, b->style.gridRowGap);
        if (a->style.gridColumnGap >= 0 && b->style.gridColumnGap >= 0) lerpSet(out.gridColumnGap, a->style.gridColumnGap, b->style.gridColumnGap);
        if (a->style.bgColor.valid && b->style.bgColor.valid) { out.bgColor = lerpColor(a->style.bgColor, b->style.bgColor); out.bgColorSet = true; }
        if (a->style.color.valid && b->style.color.valid) out.color = lerpColor(a->style.color, b->style.color);
        if (a->style.borderColor.valid && b->style.borderColor.valid) out.borderColor = lerpColor(a->style.borderColor, b->style.borderColor);
        if (a->style.borderTopColor.valid && b->style.borderTopColor.valid) out.borderTopColor = lerpColor(a->style.borderTopColor, b->style.borderTopColor);
        if (a->style.borderRightColor.valid && b->style.borderRightColor.valid) out.borderRightColor = lerpColor(a->style.borderRightColor, b->style.borderRightColor);
        if (a->style.borderBottomColor.valid && b->style.borderBottomColor.valid) out.borderBottomColor = lerpColor(a->style.borderBottomColor, b->style.borderBottomColor);
        if (a->style.borderLeftColor.valid && b->style.borderLeftColor.valid) out.borderLeftColor = lerpColor(a->style.borderLeftColor, b->style.borderLeftColor);
        if (a->style.outlineSet && b->style.outlineSet) {
            out.outlineSet = true;
            out.outlineWidth = lerp(a->style.outlineWidth, b->style.outlineWidth);
            if (a->style.outlineColor.valid && b->style.outlineColor.valid) out.outlineColor = lerpColor(a->style.outlineColor, b->style.outlineColor);
        }
        if (a->style.shadowSet && b->style.shadowSet && a->style.shadowInset == b->style.shadowInset) {
            out.shadowSet = true;
            out.shadowInset = a->style.shadowInset;
            out.shadowX = lerp(a->style.shadowX, b->style.shadowX);
            out.shadowY = lerp(a->style.shadowY, b->style.shadowY);
            out.shadowBlur = lerp(a->style.shadowBlur, b->style.shadowBlur);
            out.shadowSpread = lerp(a->style.shadowSpread, b->style.shadowSpread);
            if (a->style.shadowColor.valid && b->style.shadowColor.valid) out.shadowColor = lerpColor(a->style.shadowColor, b->style.shadowColor);
        }
        if (a->style.textShadowSet && b->style.textShadowSet) {
            out.textShadowSet = true;
            out.textShadowX = lerp(a->style.textShadowX, b->style.textShadowX);
            out.textShadowY = lerp(a->style.textShadowY, b->style.textShadowY);
            out.textShadowBlur = lerp(a->style.textShadowBlur, b->style.textShadowBlur);
            if (a->style.textShadowColor.valid && b->style.textShadowColor.valid) out.textShadowColor = lerpColor(a->style.textShadowColor, b->style.textShadowColor);
        }
        if (a->style.gradientSet && b->style.gradientSet && a->style.gradientStops.size() == b->style.gradientStops.size()) {
            out.gradientSet = true;
            out.gradientAngle = lerp(a->style.gradientAngle, b->style.gradientAngle);
            out.gradientStops = a->style.gradientStops;
            for (size_t i = 0; i < out.gradientStops.size(); ++i) {
                out.gradientStops[i].pos = lerp(a->style.gradientStops[i].pos, b->style.gradientStops[i].pos);
                if (a->style.gradientStops[i].color.valid && b->style.gradientStops[i].color.valid)
                    out.gradientStops[i].color = lerpColor(a->style.gradientStops[i].color, b->style.gradientStops[i].color);
            }
        }
        if (a->style.transformSet || b->style.transformSet) {
            out.transformSet = true;
            out.transformTx = lerp(a->style.transformTx, b->style.transformTx);
            out.transformTy = lerp(a->style.transformTy, b->style.transformTy);
            out.transformScale = lerp(a->style.transformScale, b->style.transformScale);
            out.transformRotate = lerp(a->style.transformRotate, b->style.transformRotate);
            out.transformTxPercent = a->style.transformTxPercent;
            out.transformTyPercent = a->style.transformTyPercent;
        }
        return out;
    }

    // Copies the animatable fields of a single stop onto the live style
    // (used for delay/fill-mode edge cases where there's no pair to blend).
    static void applyStop(const ComputedStyle& stop, ComputedStyle& style) {
        if (stop.opacitySet) { style.opacity = stop.opacity; style.opacitySet = true; }
        if (stop.fontSize > 0) style.fontSize = stop.fontSize;
        if (stop.lineHeight > 0) style.lineHeight = stop.lineHeight;
        if (stop.letterSpacingSet) { style.letterSpacing = stop.letterSpacing; style.letterSpacingSet = true; }
        if (stop.marginTopSet()) style.marginTop = stop.marginTop;
        if (stop.marginRightSet()) style.marginRight = stop.marginRight;
        if (stop.marginBottomSet()) style.marginBottom = stop.marginBottom;
        if (stop.marginLeftSet()) style.marginLeft = stop.marginLeft;
        if (stop.paddingTop >= 0) style.paddingTop = stop.paddingTop;
        if (stop.paddingRight >= 0) style.paddingRight = stop.paddingRight;
        if (stop.paddingBottom >= 0) style.paddingBottom = stop.paddingBottom;
        if (stop.paddingLeft >= 0) style.paddingLeft = stop.paddingLeft;
        if (stop.borderWidth >= 0) style.borderWidth = stop.borderWidth;
        if (stop.borderTopWidth >= 0) style.borderTopWidth = stop.borderTopWidth;
        if (stop.borderRightWidth >= 0) style.borderRightWidth = stop.borderRightWidth;
        if (stop.borderBottomWidth >= 0) style.borderBottomWidth = stop.borderBottomWidth;
        if (stop.borderLeftWidth >= 0) style.borderLeftWidth = stop.borderLeftWidth;
        if (stop.width >= 0) style.width = stop.width;
        if (stop.widthPercent >= 0) style.widthPercent = stop.widthPercent;
        if (stop.height >= 0) style.height = stop.height;
        if (stop.heightPercent >= 0) style.heightPercent = stop.heightPercent;
        if (stop.minWidth >= 0) style.minWidth = stop.minWidth;
        if (stop.maxWidth >= 0) style.maxWidth = stop.maxWidth;
        if (stop.minHeight >= 0) style.minHeight = stop.minHeight;
        if (stop.maxHeight >= 0) style.maxHeight = stop.maxHeight;
        if (stop.topSet) { style.top = stop.top; style.topSet = true; style.topPercent = stop.topPercent; }
        if (stop.rightSet) { style.right = stop.right; style.rightSet = true; style.rightPercent = stop.rightPercent; }
        if (stop.bottomSet) { style.bottom = stop.bottom; style.bottomSet = true; style.bottomPercent = stop.bottomPercent; }
        if (stop.leftSet) { style.left = stop.left; style.leftSet = true; style.leftPercent = stop.leftPercent; }
        if (stop.flexGrowSet) { style.flexGrow = stop.flexGrow; style.flexGrowSet = true; }
        if (stop.flexShrinkSet) { style.flexShrink = stop.flexShrink; style.flexShrinkSet = true; }
        if (stop.flexBasisSet) { style.flexBasis = stop.flexBasis; style.flexBasisSet = true; }
        if (stop.flexGap >= 0) style.flexGap = stop.flexGap;
        if (stop.flexRowGap >= 0) style.flexRowGap = stop.flexRowGap;
        if (stop.flexColumnGap >= 0) style.flexColumnGap = stop.flexColumnGap;
        if (stop.gridGap >= 0) style.gridGap = stop.gridGap;
        if (stop.gridRowGap >= 0) style.gridRowGap = stop.gridRowGap;
        if (stop.gridColumnGap >= 0) style.gridColumnGap = stop.gridColumnGap;
        if (stop.bgColor.valid) { style.bgColor = stop.bgColor; style.bgColorSet = true; }
        if (stop.color.valid) style.color = stop.color;
        if (stop.borderColor.valid) style.borderColor = stop.borderColor;
        if (stop.borderTopColor.valid) style.borderTopColor = stop.borderTopColor;
        if (stop.borderRightColor.valid) style.borderRightColor = stop.borderRightColor;
        if (stop.borderBottomColor.valid) style.borderBottomColor = stop.borderBottomColor;
        if (stop.borderLeftColor.valid) style.borderLeftColor = stop.borderLeftColor;
        if (stop.outlineSet) {
            style.outlineSet = true;
            style.outlineWidth = stop.outlineWidth;
            style.outlineColor = stop.outlineColor;
        }
        if (stop.shadowSet) {
            style.shadowSet = true;
            style.shadowX = stop.shadowX;
            style.shadowY = stop.shadowY;
            style.shadowBlur = stop.shadowBlur;
            style.shadowSpread = stop.shadowSpread;
            style.shadowColor = stop.shadowColor;
            style.shadowInset = stop.shadowInset;
        }
        if (stop.textShadowSet) {
            style.textShadowSet = true;
            style.textShadowX = stop.textShadowX;
            style.textShadowY = stop.textShadowY;
            style.textShadowBlur = stop.textShadowBlur;
            style.textShadowColor = stop.textShadowColor;
        }
        if (stop.gradientSet) {
            style.gradientSet = true;
            style.gradientAngle = stop.gradientAngle;
            style.gradientStops = stop.gradientStops;
        }
        if (stop.transformSet) {
            style.transformSet = true;
            style.transformTx = stop.transformTx;
            style.transformTy = stop.transformTy;
            style.transformScale = stop.transformScale;
            style.transformRotate = stop.transformRotate;
            style.transformTxPercent = stop.transformTxPercent;
            style.transformTyPercent = stop.transformTyPercent;
        }
    }

    std::map<const Node*, AnimationEntry> m_anims;
};
