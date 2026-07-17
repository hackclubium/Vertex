#pragma once
#include "js/vm.h"
#include "js/canvas_surface.h"
#include "html/dom.h"
#include <memory>
#include <functional>
#include <string>

struct DomBridgeCallbacks {
    std::function<void(const std::string& url, bool replace)> navigate;
    std::function<void(float x, float y)> scrollTo;
    std::function<void(float dx, float dy)> scrollBy;
    std::function<void(Node* target)> scrollIntoView;
    std::function<void()> repaintOnly;
    // Returns (creating if needed) the drawing surface backing a <canvas>
    // element's 2D context. Unset (nullptr) on platforms without a canvas
    // backend yet — canvas draw calls then safely no-op.
    std::function<ICanvasSurface*(Node*)> getCanvasSurface;
    std::function<bool(Node*)> mediaPlay;
    std::function<void(Node*)> mediaPause;
    std::function<void(Node*, double)> mediaSetCurrentTime;
    std::function<double(Node*)> mediaCurrentTime;
    std::function<double(Node*)> mediaDuration;
    std::function<void(Node*, double)> mediaSetVolume;
    std::function<double(Node*)> mediaVolume;
    std::function<void(Node*, bool)> mediaSetMuted;
    std::function<bool(Node*)> mediaMuted;
    std::function<bool(Node*)> mediaPaused;
};

// Wraps a Node* into a JsObject (Element/Text/Document).
JsValue wrapNode(VM& vm, std::shared_ptr<Node> node);

// Bootstrap the document object and window globals.
// pageUrl is the current page's URL (populates window.location).
void registerDom(VM& vm, std::shared_ptr<Node> document,
                 std::function<void()> onRepaint,
                 const std::string& pageUrl = "",
                 DomBridgeCallbacks callbacks = {});

// Retrieve the Node* from a DOM wrapper JsObject (nullptr if not a DOM object).
Node* unwrapNode(JsValue val);

// Dispatch an event (e.g. "click") on a DOM node, calling registered listeners.
// Returns false when a cancelable event was prevented.
bool dispatchDomEvent(VM& vm, Node* target, const std::string& eventName);
bool activateDomElement(VM& vm, Node* target);
void dispatchWindowEvent(VM& vm, const std::string& eventName, JsValue eventValue = JsValue::undefined());
void markDomEventListenerRoots(GC& gc);

// Mark DOM state dirty and coalesce repaint callbacks until the timer tick.
void notifyDomDirtyCoalesced(VM& vm, bool affectsLayout = true);

// Reset DOM dirty coalescing — call from platform timer tick.
void resetDomDirtyCoalesce();

// Canvas 2D support (implemented in dom_bridge.cpp, consumed by canvas_bridge.cpp
// since the registered DomBridgeCallbacks are file-local to dom_bridge.cpp).
ICanvasSurface* GetCanvasSurfaceForNode(Node* n);
void MarkCanvasDirty(VM& vm, Node* n);
