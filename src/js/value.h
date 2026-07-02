#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <cstdint>
#include <cmath>
#include <limits>

// ── forward declarations ──────────────────────────────────────────────────────
struct JsObject;
struct JsString;
struct GC;
struct VM;
struct BytecodeFunction;

// ── JsTag ─────────────────────────────────────────────────────────────────────
enum class JsTag : uint8_t {
    Undefined = 0, Null, Bool, Int32, Float64, String, Object
};

// ── JsValue ───────────────────────────────────────────────────────────────────
struct JsValue {
    JsTag tag = JsTag::Undefined;
    union { bool b; int32_t i; double f; JsString* str; JsObject* obj; } u{};

    static JsValue undefined() noexcept { return {}; }
    static JsValue null()      noexcept { JsValue v; v.tag = JsTag::Null; return v; }
    static JsValue boolean(bool b) noexcept {
        JsValue v; v.tag = JsTag::Bool; v.u.b = b; return v;
    }
    static JsValue integer(int32_t i) noexcept {
        JsValue v; v.tag = JsTag::Int32; v.u.i = i; return v;
    }
    static JsValue number(double f) noexcept {
        if (!std::isnan(f) && !std::isinf(f)) {
            double truncated = std::trunc(f);
            if (truncated >= (double)INT32_MIN && truncated <= (double)INT32_MAX
                && f == truncated && !(f == 0.0 && std::signbit(f)))
                return integer((int32_t)truncated);
        }
        JsValue v; v.tag = JsTag::Float64; v.u.f = f; return v;
    }
    static JsValue string(JsString* s) noexcept {
        JsValue v; v.tag = JsTag::String; v.u.str = s; return v;
    }
    static JsValue object(JsObject* o) noexcept {
        JsValue v; v.tag = JsTag::Object; v.u.obj = o; return v;
    }

    bool isUndefined()       const noexcept { return tag == JsTag::Undefined; }
    bool isNull()            const noexcept { return tag == JsTag::Null; }
    bool isBool()            const noexcept { return tag == JsTag::Bool; }
    bool isInt32()           const noexcept { return tag == JsTag::Int32; }
    bool isFloat64()         const noexcept { return tag == JsTag::Float64; }
    bool isNumber()          const noexcept { return tag == JsTag::Int32 || tag == JsTag::Float64; }
    bool isString()          const noexcept { return tag == JsTag::String; }
    bool isObject()          const noexcept { return tag == JsTag::Object; }
    bool isNullOrUndefined() const noexcept { return tag == JsTag::Undefined || tag == JsTag::Null; }
    bool isCallable()        const noexcept;

    double      toNumber()  const noexcept;
    bool        toBool()    const noexcept;
    std::string toString()  const;
    int32_t     toInt32()   const noexcept;
    uint32_t    toUint32()  const noexcept;

    JsObject*  asObject() const noexcept { return isObject() ? u.obj  : nullptr; }
    JsString*  asString() const noexcept { return isString() ? u.str  : nullptr; }
    double     asDouble() const noexcept { return isInt32()  ? (double)u.i : u.f; }
    int32_t    asInt32()  const noexcept { return u.i; }
    bool       asBool()   const noexcept { return u.b; }

    bool strictEq(const JsValue& o) const noexcept;
    bool looseEq (const JsValue& o) const;
};

// ── GcCell ────────────────────────────────────────────────────────────────────
struct GcCell {
    bool     gcMarked = false;
    GcCell*  gcNext   = nullptr;
    virtual ~GcCell() = default;
    virtual void gcMark(GC&) = 0;
};

// ── JsString (interned) ───────────────────────────────────────────────────────
struct JsString : GcCell {
    std::string value;
    explicit JsString(std::string s) : value(std::move(s)) {}
    void gcMark(GC&) override {}
};

// ── Upvalue ───────────────────────────────────────────────────────────────────
struct Upvalue : GcCell {
    JsValue* slot   = nullptr; // points into VM value stack while open
    JsValue  closed;
    bool     isOpen = true;

    JsValue get()     const { return isOpen ? *slot : closed; }
    void    set(JsValue v)  { if (isOpen) *slot = v; else closed = v; }
    void    close()         { if (isOpen && slot) { closed = *slot; slot = nullptr; isOpen = false; } }
    void gcMark(GC& gc) override;
};

// ── ObjKind / NativeFn ────────────────────────────────────────────────────────
enum class ObjKind : uint8_t {
    Plain, Array, Function, NativeFunction,
    Error, Promise, RegExp, Date, Map, Set,
    DomWrapper, Arguments, Iterator
};

using NativeFn = std::function<JsValue(VM&, JsValue, std::vector<JsValue>)>;

// ── JsObject ──────────────────────────────────────────────────────────────────
struct JsObject : GcCell {
    ObjKind   kind  = ObjKind::Plain;
    JsObject* proto = nullptr;

    // Properties (insertion-ordered)
    std::vector<std::string>              propOrder;
    std::unordered_map<std::string,JsValue> props;

    // Array elements fast path
    std::vector<JsValue> elements;

    // Function data
    BytecodeFunction*    bcFn      = nullptr; // non-owning, lifetime managed by compiler
    NativeFn             nativeFn;            // stored by value (std::function)
    std::vector<Upvalue*> upvalues;
    std::string          funcName;
    bool                 isArrow        = false;
    bool                 isAsync        = false;
    bool                 isGenerator    = false;
    bool                 isConstructor  = true;

    // Promise state
    enum { PS_Pending, PS_Fulfilled, PS_Rejected } promState = PS_Pending;
    JsValue              promResult;
    std::vector<std::pair<JsValue,JsValue>> promHandlers; // {onFulfilled, onRejected}

    // Error
    std::string errType, errMsg;

    // RegExp
    std::string reSource, reFlags;

    // Map/Set storage keeps JS value identity for object keys.
    std::vector<std::pair<JsValue,JsValue>> mapEntries;
    std::vector<JsValue>                    setEntries;

    // Iterator state (for for..of on arrays/strings)
    uint32_t iterIndex = 0;
    JsValue  iterTarget;

    // DOM wrapper
    void* domNode = nullptr;

    // Event listeners
    std::unordered_map<std::string, std::vector<JsValue>> listeners;

    // Property access
    JsValue  getProp   (const std::string& key) const;
    void     setProp   (const std::string& key, JsValue v);
    bool     hasProp      (const std::string& key) const;
    bool     hasOwn       (const std::string& key) const;
    bool     hasOwnProp   (const std::string& key) const { return hasOwn(key); }
    void     deleteProp   (const std::string& key);
    std::vector<std::string> ownEnumKeys() const;
    std::vector<std::string> ownAllKeys()  const { return ownEnumKeys(); }

    // Array helpers
    uint32_t arrayLength() const;
    JsValue  arrayGet(uint32_t i) const;
    void     arraySet(uint32_t i, JsValue v);
    void     arrayPush(JsValue v);
    void     arraySetLength(uint32_t n);

    void gcMark(GC& gc) override;
};

// ── JsException ───────────────────────────────────────────────────────────────
struct JsException {
    JsValue val;
    explicit JsException(JsValue v) : val(v) {}
};

// ── inline impls ──────────────────────────────────────────────────────────────
inline bool JsValue::toBool() const noexcept {
    switch (tag) {
    case JsTag::Undefined: return false;
    case JsTag::Null:      return false;
    case JsTag::Bool:      return u.b;
    case JsTag::Int32:     return u.i != 0;
    case JsTag::Float64:   return u.f != 0.0 && (u.f == u.f);
    case JsTag::String:    return u.str && !u.str->value.empty();
    case JsTag::Object:    return true;
    default:               return false;
    }
}

inline double JsValue::toNumber() const noexcept {
    switch (tag) {
    case JsTag::Undefined: return std::numeric_limits<double>::quiet_NaN();
    case JsTag::Null:      return 0.0;
    case JsTag::Bool:      return u.b ? 1.0 : 0.0;
    case JsTag::Int32:     return (double)u.i;
    case JsTag::Float64:   return u.f;
    case JsTag::String: {
        if (!u.str || u.str->value.empty()) return 0.0;
        const char* s = u.str->value.c_str();
        char* end;
        double d = std::strtod(s, &end);
        while (*end == ' ' || *end == '\t' || *end == '\n') end++;
        return (*end == '\0') ? d : std::numeric_limits<double>::quiet_NaN();
    }
    case JsTag::Object:    return std::numeric_limits<double>::quiet_NaN();
    default:               return 0.0;
    }
}

inline int32_t JsValue::toInt32() const noexcept {
    if (isInt32()) return u.i;
    double n = toNumber();
    if (!std::isfinite(n) || n == 0.0) return 0;
    return (int32_t)(int64_t)n;
}

inline uint32_t JsValue::toUint32() const noexcept {
    return (uint32_t)toInt32();
}

inline bool JsValue::strictEq(const JsValue& o) const noexcept {
    if (tag != o.tag) {
        if (isNumber() && o.isNumber()) return asDouble() == o.asDouble();
        return false;
    }
    switch (tag) {
    case JsTag::Undefined: return true;
    case JsTag::Null:      return true;
    case JsTag::Bool:      return u.b == o.u.b;
    case JsTag::Int32:     return u.i == o.u.i;
    case JsTag::Float64:   return u.f == o.u.f;
    case JsTag::String:
        return u.str == o.u.str ||
               (u.str && o.u.str && u.str->value == o.u.str->value);
    case JsTag::Object:    return u.obj == o.u.obj;
    default:               return false;
    }
}
