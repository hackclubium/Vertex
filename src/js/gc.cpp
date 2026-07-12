#include "js/gc.h"
#include "js/value.h"
#include <cassert>
#include <algorithm>
#include <sstream>
#include <chrono>

// Runaway-script wall-clock deadline (see gc.h).
long long g_jsDeadlineMs = 0;
long long JsNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── JsValue::toString ─────────────────────────────────────────────────────────
std::string JsValue::toString() const {
    switch (tag) {
    case JsTag::Undefined: return "undefined";
    case JsTag::Null:      return "null";
    case JsTag::Bool:      return u.b ? "true" : "false";
    case JsTag::Int32:     return std::to_string(u.i);
    case JsTag::Float64: {
        if (std::isnan(u.f))  return "NaN";
        if (std::isinf(u.f))  return u.f > 0 ? "Infinity" : "-Infinity";
        // Remove trailing zeros
        std::ostringstream ss;
        ss << u.f;
        return ss.str();
    }
    case JsTag::String: return u.str ? u.str->value : "";
    case JsTag::Object: {
        if (!u.obj) return "null";
        if (u.obj->kind == ObjKind::Array) {
            // [a,b,c] — guard against self-referential arrays. Without this an
            // array that (transitively) contains itself recurses forever
            // (stack overflow), and a mere depth cap turns it into an n^depth
            // blow-up. Track arrays currently being stringified and bail on
            // re-entry, matching JS behaviour (Array.toString of a cycle).
            static thread_local std::vector<const JsObject*> inProgress;
            for (const JsObject* o : inProgress) if (o == u.obj) return "";
            inProgress.push_back(u.obj);
            std::string out;
            uint32_t len = u.obj->arrayLength();
            for (uint32_t i = 0; i < len; i++) {
                if (i) out += ',';
                JsValue el = u.obj->arrayGet(i);
                if (!el.isNullOrUndefined()) out += el.toString();
            }
            inProgress.pop_back();
            return out;
        }
        if (u.obj->kind == ObjKind::Error) {
            return u.obj->errType + (u.obj->errMsg.empty() ? "" : ": " + u.obj->errMsg);
        }
        // Try toString property (basic)
        return "[object Object]";
    }
    default: return "";
    }
}

bool JsValue::isCallable() const noexcept {
    return isObject() && u.obj &&
           (u.obj->kind == ObjKind::Function || u.obj->kind == ObjKind::NativeFunction);
}

bool JsValue::looseEq(const JsValue& o) const {
    if (tag == o.tag) return strictEq(o);
    // null == undefined
    if (isNullOrUndefined() && o.isNullOrUndefined()) return true;
    if (isNullOrUndefined() || o.isNullOrUndefined()) return false;
    // number == string: coerce string to number
    if (isNumber() && o.isString()) return asDouble() == o.toNumber();
    if (isString() && o.isNumber()) return toNumber() == o.asDouble();
    // bool to number
    if (isBool()) return JsValue::number(u.b ? 1.0 : 0.0).looseEq(o);
    if (o.isBool()) return looseEq(JsValue::number(o.u.b ? 1.0 : 0.0));
    return false;
}

// ── JsObject ──────────────────────────────────────────────────────────────────
JsValue JsObject::getProp(const std::string& key) const {
    // Check own properties first
    auto it = props.find(key);
    if (it != props.end()) return it->second;
    // Array length
    if (kind == ObjKind::Array && key == "length")
        return JsValue::integer((int32_t)arrayLength());
    // Integer index for arrays/strings
    if ((kind == ObjKind::Array) && !key.empty()) {
        bool allDigit = true;
        for (char c : key) if (c < '0' || c > '9') { allDigit = false; break; }
        if (allDigit) {
            uint32_t idx = (uint32_t)std::stoul(key);
            if (idx < elements.size()) return elements[idx];
        }
    }
    // Walk prototype chain
    if (proto) return proto->getProp(key);
    return JsValue::undefined();
}

bool JsObject::hasProp(const std::string& key) const {
    if (hasOwn(key)) return true;
    if (proto) return proto->hasProp(key);
    return false;
}

bool JsObject::hasOwn(const std::string& key) const {
    if (props.count(key)) return true;
    if (kind == ObjKind::Array && key == "length") return true;
    if (kind == ObjKind::Array && !key.empty()) {
        bool allDigit = true;
        for (char c : key) if (c < '0' || c > '9') { allDigit = false; break; }
        if (allDigit) {
            uint32_t idx = (uint32_t)std::stoul(key);
            return idx < elements.size();
        }
    }
    return false;
}

void JsObject::setProp(const std::string& key, JsValue v) {
    if (kind == ObjKind::Array && key == "length") {
        uint32_t newLen = (uint32_t)v.toNumber();
        elements.resize(newLen);
        return;
    }
    if (kind == ObjKind::Array && !key.empty()) {
        bool allDigit = true;
        for (char c : key) if (c < '0' || c > '9') { allDigit = false; break; }
        if (allDigit) {
            uint32_t idx = (uint32_t)std::stoul(key);
            arraySet(idx, v);
            return;
        }
    }
    if (!props.count(key)) propOrder.push_back(key);
    props[key] = v;
}

void JsObject::deleteProp(const std::string& key) {
    props.erase(key);
    propOrder.erase(std::remove(propOrder.begin(), propOrder.end(), key), propOrder.end());
}

std::vector<std::string> JsObject::ownEnumKeys() const {
    std::vector<std::string> keys;
    if (kind == ObjKind::Array) {
        for (uint32_t i = 0; i < elements.size(); i++)
            keys.push_back(std::to_string(i));
    }
    for (auto& k : propOrder) keys.push_back(k);
    return keys;
}

uint32_t JsObject::arrayLength() const {
    auto it = props.find("length");
    if (it != props.end()) return (uint32_t)it->second.toNumber();
    return (uint32_t)elements.size();
}

JsValue JsObject::arrayGet(uint32_t i) const {
    if (i < elements.size()) return elements[i];
    return JsValue::undefined();
}

void JsObject::arraySet(uint32_t i, JsValue v) {
    if (i >= elements.size()) elements.resize(i + 1);
    elements[i] = v;
    // Keep length in sync
    uint32_t newLen = i + 1;
    auto it = props.find("length");
    if (it == props.end() || (uint32_t)it->second.toNumber() < newLen)
        props["length"] = JsValue::integer((int32_t)newLen);
}

void JsObject::arrayPush(JsValue v) {
    arraySet((uint32_t)elements.size(), v);
}

void JsObject::arraySetLength(uint32_t n) {
    elements.resize(n);
    props["length"] = JsValue::integer((int32_t)n);
}

// ── GcCell::gcMark impls ──────────────────────────────────────────────────────
void Upvalue::gcMark(GC& gc) {
    gc.markValue(closed);
}

void JsObject::gcMark(GC& gc) {
    if (proto) gc.markObject(proto);
    for (auto& [k, v] : props)    gc.markValue(v);
    for (auto& v : elements)      gc.markValue(v);
    for (auto* uv : upvalues)     if (uv) gc.markObject(reinterpret_cast<JsObject*>(uv)); // upvalues are GcCells
    gc.markValue(promResult);
    for (auto& [a,b] : promHandlers) { gc.markValue(a); gc.markValue(b); }
    for (auto& [a,b] : mapEntries)   { gc.markValue(a); gc.markValue(b); }
    for (auto& v : setEntries)       gc.markValue(v);
    gc.markValue(iterTarget);
    for (auto& [evtType, handlers] : listeners)
        for (auto& h : handlers) gc.markValue(h);
}

// ── GC ────────────────────────────────────────────────────────────────────────
GC::~GC() {
    m_strings.clear();

    GcCell* c = m_head;
    while (c) {
        GcCell* next = c->gcNext;
        delete c;
        c = next;
    }
}

JsString* GC::internString(const std::string& s) {
    auto it = m_strings.find(s);
    if (it != m_strings.end()) return it->second;
    auto* js = alloc<JsString>(s);
    m_strings[s] = js;
    return js;
}

JsString* GC::internString(std::string&& s) {
    auto it = m_strings.find(s);
    if (it != m_strings.end()) return it->second;
    auto* js = alloc<JsString>(s);
    m_strings[std::move(s)] = js;
    return js;
}

JsObject* GC::newObject(ObjKind kind) {
    auto* o = alloc<JsObject>();
    o->kind = kind;
    return o;
}

JsObject* GC::newArray() {
    auto* o = alloc<JsObject>();
    o->kind = ObjKind::Array;
    o->props["length"] = JsValue::integer(0);
    return o;
}

JsObject* GC::newFunction() {
    auto* o = alloc<JsObject>();
    o->kind = ObjKind::Function;
    return o;
}

JsObject* GC::newNativeFunction(NativeFn fn, const std::string& name, int) {
    auto* o = alloc<JsObject>();
    o->kind      = ObjKind::NativeFunction;
    o->nativeFn  = std::move(fn);
    o->funcName  = name;
    return o;
}

JsObject* GC::newError(const std::string& type, const std::string& msg) {
    auto* o = alloc<JsObject>();
    o->kind    = ObjKind::Error;
    o->errType = type;
    o->errMsg  = msg;
    // Set message property
    o->setProp("message", JsValue::string(internString(msg)));
    o->setProp("name",    JsValue::string(internString(type)));
    return o;
}

JsObject* GC::newPromise() {
    auto* o = alloc<JsObject>();
    o->kind = ObjKind::Promise;
    return o;
}

void GC::removeRoot(JsValue* p) {
    m_roots.erase(std::remove(m_roots.begin(), m_roots.end(), p), m_roots.end());
}

void GC::markValue(const JsValue& v) {
    if (v.isObject() && v.u.obj)  markObject(v.u.obj);
    if (v.isString() && v.u.str)  markString(v.u.str);
}

void GC::markString(JsString* s) {
    if (s && !s->gcMarked) s->gcMarked = true;
}

void GC::markObject(JsObject* o) {
    if (!o || o->gcMarked) return;
    o->gcMarked = true;
    o->gcMark(*this);
    // Mark upvalues (they are GcCell but not JsObject)
    for (auto* uv : o->upvalues) {
        if (uv && !uv->gcMarked) {
            uv->gcMarked = true;
            uv->gcMark(*this);
        }
    }
}

void GC::collect() {
    // Mark phase: clear all marks
    for (GcCell* c = m_head; c; c = c->gcNext) c->gcMarked = false;

    // Mark from roots
    for (auto* p : m_roots) if (p) markValue(*p);

    // Mark from temporary frame roots
    for (auto& r : m_tempRoots)
        for (size_t i = 0; i < r.count; i++) markValue(r.base[i]);

    // Sweep phase
    GcCell** pp = &m_head;
    while (*pp) {
        GcCell* c = *pp;
        if (!c->gcMarked) {
            // Remove from string interning table if it's a JsString
            if (auto* js = dynamic_cast<JsString*>(c)) {
                m_strings.erase(js->value);
            }
            *pp = c->gcNext;
            delete c;
            m_allocCount--;
        } else {
            c->gcMarked = false; // reset for next cycle
            pp = &c->gcNext;
        }
    }

    // Grow threshold
    m_gcThreshold = m_allocCount * 2 + 1024;
}
