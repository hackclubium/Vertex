#pragma once
#include "html/dom.h"
#include "js/dom_bridge.h"
#include <memory>
#include <functional>
#include <string>

struct JsScriptBudget {
    size_t maxScriptBytes = 1024 * 1024;
};

struct JsScriptStats {
    size_t scriptsAttempted = 0;
    size_t scriptsExecuted = 0;
    size_t scriptsSkippedByBudget = 0;
    size_t parseFailures = 0;
    size_t runtimeFailures = 0;
    double parseMs = 0.0;
    double compileRunMs = 0.0;
};

// Top-level JS engine facade.
// Owns the GC, VM, and built-in registrations.
class JsEngine {
public:
    JsEngine();
    ~JsEngine();

    // Attach a DOM document so JS can access document/window.
    void setDocument(std::shared_ptr<Node> doc, std::function<void()> onRepaint,
                     const std::string& pageUrl = "",
                     DomBridgeCallbacks callbacks = {});

    // Execute a JS source string (parses, compiles, runs).
    // Returns false and logs on parse/runtime error.
    bool runScript(const std::string& source, const std::string& filename = "<script>");
    void setScriptBudget(const JsScriptBudget& budget);
    JsScriptStats scriptStats() const;
    void resetScriptStats();

    // Dispatch a click event to a DOM node.
    void dispatchClick(Node* target, int x, int y);

    // Dispatch a keydown event.
    void dispatchKeyDown(int keyCode, const std::string& key);
    void dispatchWindowEvent(const std::string& eventName);
    void dispatchDocumentEvent(const std::string& eventName);

    // Run pending macrotasks (called by Win32 WM_TIMER).
    void runMacrotasks();
    bool hasPendingMacrotasks() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
