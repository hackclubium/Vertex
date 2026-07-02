#pragma once
#include "js/vm.h"
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

// Mark DOM state dirty and coalesce repaint callbacks until the timer tick.
void notifyDomDirtyCoalesced(VM& vm, bool affectsLayout = true);

// Reset DOM dirty coalescing — call from platform timer tick.
void resetDomDirtyCoalesce();
