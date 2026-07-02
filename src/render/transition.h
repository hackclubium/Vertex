#pragma once
//
// transition.h — CSS transition animation system for Vertex.
//
// Tracks style changes on elements and interpolates numeric values
// (opacity, colors) over the transition duration. The platform shell
// calls tick() on each frame to advance animations and repaint.
//
#include "css/style.h"
#include "html/dom.h"
#include <map>
#include <chrono>
#include <cmath>

struct TransitionEntry {
    float startOpacity, endOpacity;
    CssColor startBgColor, endBgColor;
    CssColor startColor, endColor;
    float duration; // seconds
    std::chrono::steady_clock::time_point startTime;
};

class TransitionManager {
public:
    static TransitionManager& instance() {
        static TransitionManager mgr;
        return mgr;
    }

    // Called when a style changes on an element. If the element has a
    // transition, start animating from the old values to the new ones.
    void onStyleChange(const Node* node, const ComputedStyle& oldStyle, const ComputedStyle& newStyle) {
        if (!newStyle.transitionSet || newStyle.transitionDuration <= 0) return;
        // Check if any animatable property actually changed.
        bool changed = false;
        if (oldStyle.opacitySet && newStyle.opacitySet && oldStyle.opacity != newStyle.opacity) changed = true;
        if (oldStyle.bgColorSet && newStyle.bgColorSet) {
            if (oldStyle.bgColor.r != newStyle.bgColor.r || oldStyle.bgColor.g != newStyle.bgColor.g ||
                oldStyle.bgColor.b != newStyle.bgColor.b) changed = true;
        }
        if (oldStyle.color.valid && newStyle.color.valid) {
            if (oldStyle.color.r != newStyle.color.r || oldStyle.color.g != newStyle.color.g ||
                oldStyle.color.b != newStyle.color.b) changed = true;
        }
        if (!changed) return;

        TransitionEntry entry;
        entry.startOpacity = oldStyle.opacitySet ? oldStyle.opacity : 1.f;
        entry.endOpacity = newStyle.opacitySet ? newStyle.opacity : 1.f;
        entry.startBgColor = oldStyle.bgColor;
        entry.endBgColor = newStyle.bgColor;
        entry.startColor = oldStyle.color;
        entry.endColor = newStyle.color;
        entry.duration = newStyle.transitionDuration;
        entry.startTime = std::chrono::steady_clock::now();
        m_transitions[node] = entry;
    }

    // Apply current transition state to a style. Returns true if the
    // element is still animating (caller should schedule a repaint).
    bool applyTransition(const Node* node, ComputedStyle& style) {
        auto it = m_transitions.find(node);
        if (it == m_transitions.end()) return false;

        auto& e = it->second;
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - e.startTime).count();
        float t = std::min(1.f, elapsed / e.duration);

        // Ease-out curve.
        float eased = 1.f - (1.f - t) * (1.f - t);

        // Interpolate opacity.
        if (style.opacitySet)
            style.opacity = e.startOpacity + (e.endOpacity - e.startOpacity) * eased;

        // Interpolate background color.
        if (style.bgColorSet && e.startBgColor.valid && e.endBgColor.valid) {
            style.bgColor.r = e.startBgColor.r + (e.endBgColor.r - e.startBgColor.r) * eased;
            style.bgColor.g = e.startBgColor.g + (e.endBgColor.g - e.startBgColor.g) * eased;
            style.bgColor.b = e.startBgColor.b + (e.endBgColor.b - e.startBgColor.b) * eased;
        }

        // Interpolate text color.
        if (style.color.valid && e.startColor.valid && e.endColor.valid) {
            style.color.r = e.startColor.r + (e.endColor.r - e.startColor.r) * eased;
            style.color.g = e.startColor.g + (e.endColor.g - e.startColor.g) * eased;
            style.color.b = e.startColor.b + (e.endColor.b - e.startColor.b) * eased;
        }

        if (t >= 1.f) { m_transitions.erase(it); return false; }
        return true;
    }

    // Returns true if any transitions are active (caller should keep repainting).
    bool hasActiveTransitions() const { return !m_transitions.empty(); }

    void clear() { m_transitions.clear(); }

private:
    std::map<const Node*, TransitionEntry> m_transitions;
};
