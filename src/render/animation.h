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
        if (a->style.opacitySet && b->style.opacitySet)
            out.opacity = lerp(a->style.opacity, b->style.opacity);
        if (a->style.bgColor.valid && b->style.bgColor.valid) {
            out.bgColor.r = lerp(a->style.bgColor.r, b->style.bgColor.r);
            out.bgColor.g = lerp(a->style.bgColor.g, b->style.bgColor.g);
            out.bgColor.b = lerp(a->style.bgColor.b, b->style.bgColor.b);
            out.bgColor.a = lerp(a->style.bgColor.a, b->style.bgColor.a);
        }
        if (a->style.color.valid && b->style.color.valid) {
            out.color.r = lerp(a->style.color.r, b->style.color.r);
            out.color.g = lerp(a->style.color.g, b->style.color.g);
            out.color.b = lerp(a->style.color.b, b->style.color.b);
            out.color.a = lerp(a->style.color.a, b->style.color.a);
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
        if (stop.bgColor.valid) style.bgColor = stop.bgColor;
        if (stop.color.valid) style.color = stop.color;
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
