#pragma once
#include "js/value.h"
#include "js/gc.h"
#include "js/compiler.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

// ── CallFrame ─────────────────────────────────────────────────────────────────
struct CallFrame {
    BytecodeFunction*   fn       = nullptr;
    int                 pc       = 0;         // index into fn->code
    int                 regBase  = 0;         // base index into m_stack
    JsValue             thisVal;
    std::vector<Upvalue*> upvalues; // upvalues for this frame's closure
    bool                isCtor   = false;
};

// ── VM ────────────────────────────────────────────────────────────────────────
class VM {
public:
    explicit VM(GC& gc);
    ~VM();

    // Load a compiled program and execute it.
    JsValue execute(BytecodeFunction* fn, JsValue thisVal = JsValue::undefined());

    // Call a JS function with given this and args.
    JsValue call(JsValue fn, JsValue thisVal, std::vector<JsValue> args);
    JsValue callNew(JsValue ctor, std::vector<JsValue> args);
    void closeFunctionUpvalues(JsValue fn);
    void closeAllOpenUpvalues();

    // Access the global object.
    JsObject* globals() { return m_globals; }

    // Set a global variable.
    void setGlobal(const std::string& name, JsValue val);
    JsValue getGlobal(const std::string& name);

    // GC access for built-ins.
    GC& gc() { return m_gc; }
    std::weak_ptr<bool> lifetimeToken() const { return m_lifetimeToken; }

    // String interning helpers.
    JsValue str(const std::string& s)  { return JsValue::string(m_gc.internString(s)); }
    JsValue str(std::string&& s)       { return JsValue::string(m_gc.internString(std::move(s))); }

    // Create JS error.
    JsValue makeError(const std::string& type, const std::string& msg);
    JsValue makeTypeError(const std::string& msg)     { return makeError("TypeError", msg); }
    JsValue makeRangeError(const std::string& msg)    { return makeError("RangeError", msg); }
    JsValue makeReferenceError(const std::string& msg){ return makeError("ReferenceError", msg); }

    // Throw a JS error from native code.
    [[noreturn]] void throwError(const std::string& type, const std::string& msg) {
        throw JsException(makeError(type, msg));
    }
    [[noreturn]] void throwTypeError(const std::string& msg)  { throwError("TypeError",  msg); }
    [[noreturn]] void throwRangeError(const std::string& msg) { throwError("RangeError", msg); }

    // Async/Promise support.
    JsValue promiseResolve(JsValue val);
    JsValue promiseReject(JsValue reason);
    void    initPromiseObject(JsObject* p);
    void    settlePromiseObject(JsObject* p, JsValue value, bool rejected = false);
    void    promiseThen(JsObject* p, JsValue onFulfilled, JsValue onRejected);
    void    resolvePromise(JsObject* p, JsValue val);
    void    rejectPromise(JsObject* p, JsValue reason);

    // Microtask queue (Promise .then callbacks).
    struct Microtask { JsValue fn; JsValue arg; };
    void     enqueueMicrotask(JsValue fn, JsValue arg);
    void     drainMicrotasks();

    // Macrotask queue (setTimeout / requestAnimationFrame callbacks).
    struct Macrotask {
        JsValue fn;
        std::vector<JsValue> args;
        int delay = 0;
        int intervalDelay = 0;
        int id = 0;
        bool interval = false;
    };
    std::vector<Macrotask>& macrotasks() { return m_macrotasks; }
    const std::vector<Macrotask>& macrotasks() const { return m_macrotasks; }
    int scheduleMacrotask(JsValue fn, std::vector<JsValue> args, int delay, bool interval = false);
    bool cancelMacrotask(int id);

    // DOM dirty flag (set when JS modifies the DOM).
    bool domDirty = false;

    // Callback to request animation frame / repaint.
    std::function<void()> onDomDirty;
    std::function<void()> onDomPaintDirty;

    // Reflect a JS property write onto the backing DOM node (installed by the
    // DOM bridge). Called for writes to any object with a domNode so that
    // el.className / id / innerHTML / textContent / value / style mutate the
    // real DOM, not just the wrapper's JS property.
    std::function<void(JsObject*, const std::string&, JsValue)> onDomPropSet;

    // Live-read a DOM property from the backing node (className, id, value, …).
    // Returns true and fills `out` when it handles the key; false to fall back
    // to the wrapper's stored property. Keeps getters in sync after mutations.
    std::function<bool(JsObject*, const std::string&, JsValue&)> onDomPropGet;

private:
    GC&      m_gc;
    JsObject* m_globals;
    // Keeps m_globals (and everything reachable from it: console, navigator,
    // document, prototypes, ...) alive across collections. GET_GLOBAL/
    // SET_GLOBAL read/write m_globals directly as a raw pointer, never via a
    // register, so without this the GC has no way to see it's reachable.
    JsValue  m_globalsRoot;

    // Value stack: all frames' registers live here.
    std::vector<JsValue> m_stack;
    // Call stack
    std::vector<CallFrame> m_frames;

    // Open upvalue list (linked by stackSlot).
    std::vector<Upvalue*> m_openUpvalues;

    std::vector<Microtask> m_microtasks;
    std::vector<Macrotask> m_macrotasks;
    int m_nextMacrotaskId = 1;
    std::shared_ptr<bool> m_lifetimeToken = std::make_shared<bool>(true);

    // Runaway-script guard: a top-level execution is aborted once it exceeds a
    // wall-clock deadline, so an infinite loop / pathological script can't hang
    // the browser. m_deadlineMs is a steady_clock millisecond timestamp.
    long long m_deadlineMs = 0;
    uint64_t  m_instrCount = 0;
    int       m_executeDepth = 0;

    // Per-function constant string cache (const idx → JsString*).
    std::unordered_map<std::string, JsString*> m_stringCache;

    // Main execution loop for one frame.
    JsValue runFrame(CallFrame& frame);

    // Set up a new call frame and run it.
    JsValue callFunction(JsObject* fn, JsValue thisVal, std::vector<JsValue>& args, bool isCtor);
    JsValue callBytecode(BytecodeFunction* bc, JsValue thisVal,
                         std::vector<JsValue>& args, bool isCtor,
                         std::vector<Upvalue*>& closureUpvals);

    // Resolve a constant: handle string constants that need interning.
    JsValue resolveConst(BytecodeFunction* fn, uint16_t idx);

    // Property operations.
    JsValue  getProp(JsValue obj, const std::string& key);
    JsValue  getProp(JsValue obj, JsValue key);
    void     setProp(JsValue obj, const std::string& key, JsValue val);
    void     setProp(JsValue obj, JsValue key, JsValue val);

    // Arithmetic helpers.
    JsValue  add(JsValue a, JsValue b);
    JsValue  compare(JsValue a, JsValue b, const char* op);

    // Upvalue management.
    Upvalue* captureUpvalue(JsValue* slot);
    void     closeUpvalues(JsValue* minSlot);

    // Iterator protocol.
    JsObject* getIterator(JsValue val);
    JsValue   iteratorNext(JsObject* iter);
};
