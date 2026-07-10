#define _USE_MATH_DEFINES
#include "js/runtime.h"
#include <cmath>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <random>
#include <regex>
#include <cstdio>
#include <memory>
#include <unordered_map>

// â”€â”€ Helper macros â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

#define NATIVE(name) [](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue
#define ARG(i) (args.size() > (size_t)(i) ? args[i] : JsValue::undefined())
#define ARG_STR(i) ARG(i).toString()
#define ARG_NUM(i) ARG(i).toNumber()
#define ARG_INT(i) ARG(i).toInt32()

static JsValue addNative(VM& vm, JsObject* obj, const std::string& name, NativeFn fn) {
    auto* fnObj = vm.gc().newNativeFunction(fn, name);
    obj->setProp(name, JsValue::object(fnObj));
    return JsValue::object(fnObj);
}

static std::unordered_map<std::string, std::string> g_symbolRegistry;

static JsValue arrayIteratorObject(VM& vm, JsValue target, const std::string& kind) {
    JsValue targetRoot = target;
    vm.gc().addRoot(&targetRoot);
    auto* iter = vm.gc().newObject(ObjKind::Iterator);
    JsValue iterVal = JsValue::object(iter);
    vm.gc().addRoot(&iterVal);
    iter->iterTarget = target;
    iter->iterIndex = 0;
    iter->setProp("__kind", vm.str(kind));
    addNative(vm, iter, "next", NATIVE("iterator_next") {
        if (!thisVal.isObject()) return JsValue::undefined();
        JsObject* self = thisVal.asObject();
        JsValue source = self->iterTarget;
        uint32_t index = self->iterIndex;
        uint32_t len = source.isString() ? (uint32_t)source.asString()->value.size()
            : (source.isObject() ? source.asObject()->arrayLength() : 0);
        auto* result = vm.gc().newObject(ObjKind::Plain);
        if (index >= len) {
            result->setProp("done", JsValue::boolean(true));
            result->setProp("value", JsValue::undefined());
            return JsValue::object(result);
        }
        self->iterIndex++;
        std::string kind = self->getProp("__kind").toString();
        JsValue value;
        if (kind == "key") value = JsValue::integer((int32_t)index);
        else if (kind == "entry") {
            auto* pair = vm.gc().newArray();
            pair->arrayPush(JsValue::integer((int32_t)index));
            pair->arrayPush(source.isString() ? vm.str(std::string(1, source.asString()->value[index])) : source.asObject()->arrayGet(index));
            value = JsValue::object(pair);
        } else value = source.isString() ? vm.str(std::string(1, source.asString()->value[index])) : source.asObject()->arrayGet(index);
        result->setProp("done", JsValue::boolean(false));
        result->setProp("value", value);
        return JsValue::object(result);
    });
    iter->setProp("Symbol(Symbol.iterator)_registry", JsValue::object(iter));
    iter->setProp("Symbol(iterator)_registry", JsValue::object(iter));
    vm.gc().removeRoot(&iterVal);
    vm.gc().removeRoot(&targetRoot);
    return iterVal;
}

static JsObject* newArrayWithPrototype(VM& vm) {
    auto* arr = vm.gc().newArray();
    JsValue arrayCtor = vm.getGlobal("Array");
    if (arrayCtor.isObject()) {
        JsValue proto = arrayCtor.asObject()->getProp("prototype");
        if (proto.isObject()) arr->proto = proto.asObject();
    }
    return arr;
}

static JsValue cloneJsValue(VM& vm, JsValue value, int depth = 0) {
    if (!value.isObject() || depth > 32) return value;
    JsObject* source = value.asObject();
    if (source->kind == ObjKind::Array) {
        auto* arr = vm.gc().newArray();
        JsValue arrVal = JsValue::object(arr);
        vm.gc().addRoot(&arrVal);
        for (uint32_t i = 0; i < source->arrayLength(); ++i)
            arr->arrayPush(cloneJsValue(vm, source->arrayGet(i), depth + 1));
        vm.gc().removeRoot(&arrVal);
        return arrVal;
    }
    auto* out = vm.gc().newObject(ObjKind::Plain);
    JsValue outVal = JsValue::object(out);
    vm.gc().addRoot(&outVal);
    for (const auto& key : source->ownEnumKeys())
        out->setProp(key, cloneJsValue(vm, source->getProp(key), depth + 1));
    vm.gc().removeRoot(&outVal);
    return outVal;
}

// â”€â”€ Object.prototype methods â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerObject(VM& vm) {
    auto* ctor  = vm.gc().newNativeFunction(NATIVE("Object") {
        if (args.empty() || ARG(0).isNullOrUndefined())
            return JsValue::object(vm.gc().newObject(ObjKind::Plain));
        if (ARG(0).isObject()) return ARG(0);
        return JsValue::object(vm.gc().newObject(ObjKind::Plain));
    }, "Object");

    auto* proto = vm.gc().newObject(ObjKind::Plain);
    ctor->setProp("prototype", JsValue::object(proto));

    addNative(vm, ctor, "keys", NATIVE("keys") {
        auto* arr = vm.gc().newArray();
        if (ARG(0).isObject()) {
            for (auto& k : ARG(0).asObject()->ownEnumKeys())
                arr->arrayPush(vm.str(k));
        }
        return JsValue::object(arr);
    });
    addNative(vm, ctor, "values", NATIVE("values") {
        auto* arr = vm.gc().newArray();
        if (ARG(0).isObject()) {
            auto* o = ARG(0).asObject();
            for (auto& k : o->ownEnumKeys()) arr->arrayPush(o->getProp(k));
        }
        return JsValue::object(arr);
    });
    addNative(vm, ctor, "entries", NATIVE("entries") {
        auto* arr = vm.gc().newArray();
        if (ARG(0).isObject()) {
            auto* o = ARG(0).asObject();
            for (auto& k : o->ownEnumKeys()) {
                auto* pair = vm.gc().newArray();
                pair->arrayPush(vm.str(k));
                pair->arrayPush(o->getProp(k));
                arr->arrayPush(JsValue::object(pair));
            }
        }
        return JsValue::object(arr);
    });
    addNative(vm, ctor, "assign", NATIVE("assign") {
        if (args.empty() || !ARG(0).isObject()) return ARG(0);
        auto* target = ARG(0).asObject();
        for (size_t i = 1; i < args.size(); i++) {
            if (!args[i].isObject()) continue;
            auto* src = args[i].asObject();
            for (auto& k : src->ownEnumKeys()) target->setProp(k, src->getProp(k));
        }
        return ARG(0);
    });
    addNative(vm, ctor, "create", NATIVE("create") {
        auto* o = vm.gc().newObject(ObjKind::Plain);
        if (ARG(0).isObject()) o->proto = ARG(0).asObject();
        return JsValue::object(o);
    });
    addNative(vm, ctor, "defineProperty", NATIVE("defineProperty") {
        if (ARG(0).isObject() && ARG(2).isObject()) {
            auto* o = ARG(0).asObject();
            auto* desc = ARG(2).asObject();
            std::string key = ARG_STR(1);
            JsValue getter = desc->getProp("get");
            JsValue setter = desc->getProp("set");
            if (getter.isCallable() || setter.isCallable()) {
                // Store getter/setter as special properties __get_<key>__ / __set_<key>__
                if (getter.isCallable()) o->setProp("__get_" + key + "__", getter);
                if (setter.isCallable()) o->setProp("__set_" + key + "__", setter);
                // Set a sentinel so property access knows to call the getter
                o->setProp(key, JsValue::undefined());
            } else {
                JsValue val = desc->getProp("value");
                if (!val.isUndefined()) o->setProp(key, val);
            }
        }
        return ARG(0);
    });
    addNative(vm, ctor, "getOwnPropertyNames", NATIVE("getOwnPropertyNames") {
        auto* arr = vm.gc().newArray();
        if (ARG(0).isObject()) for (auto& k : ARG(0).asObject()->ownAllKeys()) arr->arrayPush(vm.str(k));
        return JsValue::object(arr);
    });
    addNative(vm, ctor, "getOwnPropertyDescriptor", NATIVE("getOwnPropertyDescriptor") {
        if (!ARG(0).isObject()) return JsValue::undefined();
        auto* o = ARG(0).asObject();
        std::string key = ARG_STR(1);
        if (!o->hasOwnProp(key)) return JsValue::undefined();
        auto* desc = vm.gc().newObject(ObjKind::Plain);
        desc->setProp("value", o->getProp(key));
        desc->setProp("writable", JsValue::boolean(true));
        desc->setProp("enumerable", JsValue::boolean(true));
        desc->setProp("configurable", JsValue::boolean(true));
        return JsValue::object(desc);
    });
    addNative(vm, ctor, "defineProperties", NATIVE("defineProperties") {
        if (!ARG(0).isObject() || !ARG(1).isObject()) return ARG(0);
        JsValue objectCtor = vm.getGlobal("Object");
        JsValue define = objectCtor.isObject() ? objectCtor.asObject()->getProp("defineProperty") : JsValue::undefined();
        for (const auto& key : ARG(1).asObject()->ownEnumKeys())
            if (define.isCallable()) vm.call(define, objectCtor, { ARG(0), vm.str(key), ARG(1).asObject()->getProp(key) });
        return ARG(0);
    });
    addNative(vm, ctor, "hasOwn", NATIVE("hasOwn") {
        return JsValue::boolean(ARG(0).isObject() && ARG(0).asObject()->hasOwnProp(ARG_STR(1)));
    });
    addNative(vm, ctor, "freeze", NATIVE("freeze") { return ARG(0); });
    addNative(vm, ctor, "seal", NATIVE("seal") { return ARG(0); });
    addNative(vm, ctor, "preventExtensions", NATIVE("preventExtensions") { return ARG(0); });
    addNative(vm, ctor, "isFrozen", NATIVE("isFrozen") { return JsValue::boolean(false); });
    addNative(vm, ctor, "isSealed", NATIVE("isSealed") { return JsValue::boolean(false); });
    addNative(vm, ctor, "isExtensible", NATIVE("isExtensible") { return JsValue::boolean(true); });
    addNative(vm, ctor, "getPrototypeOf", NATIVE("getPrototypeOf") {
        if (ARG(0).isObject() && ARG(0).asObject()->proto)
            return JsValue::object(ARG(0).asObject()->proto);
        return JsValue::null();
    });
    addNative(vm, ctor, "fromEntries", NATIVE("fromEntries") {
        auto* o = vm.gc().newObject(ObjKind::Plain);
        if (ARG(0).isObject()) {
            auto* arr = ARG(0).asObject();
            for (uint32_t i = 0; i < arr->arrayLength(); i++) {
                JsValue pair = arr->arrayGet(i);
                if (pair.isObject()) {
                    std::string key = pair.asObject()->arrayGet(0).toString();
                    o->setProp(key, pair.asObject()->arrayGet(1));
                }
            }
        }
        return JsValue::object(o);
    });
    addNative(vm, ctor, "groupBy", NATIVE("Object.groupBy") {
        auto* out = vm.gc().newObject(ObjKind::Plain);
        JsValue outVal = JsValue::object(out);
        vm.gc().addRoot(&outVal);
        if (ARG(0).isObject() && ARG(1).isCallable()) {
            auto* arr = ARG(0).asObject();
            for (uint32_t i = 0; i < arr->arrayLength(); ++i) {
                JsValue item = arr->arrayGet(i);
                std::string key = vm.call(ARG(1), JsValue::undefined(), { item, JsValue::integer((int32_t)i) }).toString();
                JsValue bucket = out->getProp(key);
                JsObject* bucketArr = nullptr;
                if (bucket.isObject() && bucket.asObject()->kind == ObjKind::Array) bucketArr = bucket.asObject();
                else {
                    bucketArr = newArrayWithPrototype(vm);
                    out->setProp(key, JsValue::object(bucketArr));
                }
                bucketArr->arrayPush(item);
            }
        }
        vm.gc().removeRoot(&outVal);
        return outVal;
    });

    addNative(vm, proto, "hasOwnProperty", NATIVE("hasOwnProperty") {
        if (!thisVal.isObject()) return JsValue::boolean(false);
        return JsValue::boolean(thisVal.asObject()->hasOwnProp(ARG_STR(0)));
    });
    addNative(vm, proto, "toString", NATIVE("toString") {
        return vm.str("[object Object]");
    });
    addNative(vm, proto, "valueOf", NATIVE("valueOf") { return thisVal; });

    vm.setGlobal("Object", JsValue::object(ctor));
}

// â”€â”€ Array â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerArray(VM& vm) {
    auto* proto = vm.gc().newObject(ObjKind::Plain);

    addNative(vm, proto, "push", NATIVE("push") {
        if (!thisVal.isObject()) return JsValue::integer(0);
        auto* arr = thisVal.asObject();
        for (auto& a : args) arr->arrayPush(a);
        return JsValue::integer((int32_t)arr->arrayLength());
    });
    addNative(vm, proto, "pop", NATIVE("pop") {
        if (!thisVal.isObject()) return JsValue::undefined();
        auto* arr = thisVal.asObject();
        uint32_t len = arr->arrayLength();
        if (len == 0) return JsValue::undefined();
        JsValue v = arr->arrayGet(len - 1);
        arr->arraySetLength(len - 1);
        return v;
    });
    addNative(vm, proto, "shift", NATIVE("shift") {
        if (!thisVal.isObject()) return JsValue::undefined();
        auto* arr = thisVal.asObject();
        if (arr->arrayLength() == 0) return JsValue::undefined();
        JsValue v = arr->arrayGet(0);
        uint32_t len = arr->arrayLength();
        for (uint32_t i = 0; i < len - 1; i++) arr->arraySet(i, arr->arrayGet(i+1));
        arr->arraySetLength(len - 1);
        return v;
    });
    addNative(vm, proto, "unshift", NATIVE("unshift") {
        if (!thisVal.isObject()) return JsValue::integer(0);
        auto* arr = thisVal.asObject();
        uint32_t n = (uint32_t)args.size(), len = arr->arrayLength();
        arr->arraySetLength(len + n);
        for (uint32_t i = len; i > 0; i--) arr->arraySet(i + n - 1, arr->arrayGet(i - 1));
        for (uint32_t i = 0; i < n; i++) arr->arraySet(i, args[i]);
        return JsValue::integer((int32_t)(len + n));
    });
    addNative(vm, proto, "splice", NATIVE("splice") {
        if (!thisVal.isObject()) return JsValue::object(vm.gc().newArray());
        auto* arr = thisVal.asObject();
        uint32_t len = arr->arrayLength();
        int start = args.size() > 0 ? ARG_INT(0) : 0;
        if (start < 0) start = std::max(0, (int)len + start);
        start = std::min(start, (int)len);
        int deleteCount = args.size() > 1 ? ARG_INT(1) : (int)len - start;
        deleteCount = std::max(0, std::min(deleteCount, (int)len - start));
        auto* removed = vm.gc().newArray();
        for (int i = 0; i < deleteCount; i++) removed->arrayPush(arr->arrayGet(start + i));
        std::vector<JsValue> insert;
        for (size_t i = 2; i < args.size(); i++) insert.push_back(args[i]);
        std::vector<JsValue> result;
        for (int i = 0; i < start; i++) result.push_back(arr->arrayGet(i));
        for (auto& v : insert) result.push_back(v);
        for (int i = start + deleteCount; i < (int)len; i++) result.push_back(arr->arrayGet(i));
        arr->arraySetLength((uint32_t)result.size());
        for (uint32_t i = 0; i < result.size(); i++) arr->arraySet(i, result[i]);
        return JsValue::object(removed);
    });
    addNative(vm, proto, "slice", NATIVE("slice") {
        if (!thisVal.isObject()) return JsValue::object(vm.gc().newArray());
        auto* arr = thisVal.asObject();
        int len = (int)arr->arrayLength();
        int start = args.size() > 0 ? ARG_INT(0) : 0;
        int end = args.size() > 1 ? ARG_INT(1) : len;
        if (start < 0) start = std::max(0, len + start);
        if (end < 0) end = std::max(0, len + end);
        start = std::min(start, len); end = std::min(end, len);
        auto* result = vm.gc().newArray();
        for (int i = start; i < end; i++) result->arrayPush(arr->arrayGet(i));
        return JsValue::object(result);
    });
    addNative(vm, proto, "indexOf", NATIVE("indexOf") {
        if (!thisVal.isObject()) return JsValue::integer(-1);
        auto* arr = thisVal.asObject();
        uint32_t len = arr->arrayLength();
        for (uint32_t i = 0; i < len; i++)
            if (arr->arrayGet(i).strictEq(ARG(0))) return JsValue::integer((int32_t)i);
        return JsValue::integer(-1);
    });
    addNative(vm, proto, "includes", NATIVE("includes") {
        if (!thisVal.isObject()) return JsValue::boolean(false);
        auto* arr = thisVal.asObject();
        for (uint32_t i = 0; i < arr->arrayLength(); i++)
            if (arr->arrayGet(i).strictEq(ARG(0))) return JsValue::boolean(true);
        return JsValue::boolean(false);
    });
    addNative(vm, proto, "join", NATIVE("join") {
        if (!thisVal.isObject()) return vm.str("");
        auto* arr = thisVal.asObject();
        std::string sep = args.empty() || ARG(0).isUndefined() ? "," : ARG_STR(0);
        std::string result;
        for (uint32_t i = 0; i < arr->arrayLength(); i++) {
            if (i > 0) result += sep;
            JsValue v = arr->arrayGet(i);
            if (!v.isNullOrUndefined()) result += v.toString();
        }
        return vm.str(result);
    });
    addNative(vm, proto, "reverse", NATIVE("reverse") {
        if (!thisVal.isObject()) return thisVal;
        auto* arr = thisVal.asObject();
        uint32_t len = arr->arrayLength();
        for (uint32_t i = 0; i < len / 2; i++) {
            JsValue tmp = arr->arrayGet(i);
            arr->arraySet(i, arr->arrayGet(len - 1 - i));
            arr->arraySet(len - 1 - i, tmp);
        }
        return thisVal;
    });
    addNative(vm, proto, "sort", NATIVE("sort") {
        if (!thisVal.isObject()) return thisVal;
        auto* arr = thisVal.asObject();
        uint32_t len = arr->arrayLength();
        std::vector<JsValue> elems;
        for (uint32_t i = 0; i < len; i++) elems.push_back(arr->arrayGet(i));
        JsValue cmpFn = ARG(0);
        std::stable_sort(elems.begin(), elems.end(), [&](const JsValue& a, const JsValue& b) {
            if (cmpFn.isCallable()) {
                try {
                    JsValue r = vm.call(cmpFn, JsValue::undefined(), {a, b});
                    return r.toNumber() < 0;
                } catch (...) {}
            }
            return a.toString() < b.toString();
        });
        for (uint32_t i = 0; i < len; i++) arr->arraySet(i, elems[i]);
        return thisVal;
    });
    addNative(vm, proto, "map", NATIVE("map") {
        if (!thisVal.isObject() || !ARG(0).isCallable()) return JsValue::object(vm.gc().newArray());
        auto* arr = thisVal.asObject();
        auto* result = vm.gc().newArray();
        for (uint32_t i = 0; i < arr->arrayLength(); i++) {
            JsValue v = vm.call(ARG(0), thisVal, {arr->arrayGet(i), JsValue::integer((int32_t)i), thisVal});
            result->arrayPush(v);
        }
        return JsValue::object(result);
    });
    addNative(vm, proto, "filter", NATIVE("filter") {
        if (!thisVal.isObject() || !ARG(0).isCallable()) return JsValue::object(vm.gc().newArray());
        auto* arr = thisVal.asObject();
        auto* result = vm.gc().newArray();
        for (uint32_t i = 0; i < arr->arrayLength(); i++) {
            JsValue v = arr->arrayGet(i);
            JsValue r = vm.call(ARG(0), thisVal, {v, JsValue::integer((int32_t)i), thisVal});
            if (r.toBool()) result->arrayPush(v);
        }
        return JsValue::object(result);
    });
    addNative(vm, proto, "reduce", NATIVE("reduce") {
        if (!thisVal.isObject() || !ARG(0).isCallable()) return JsValue::undefined();
        auto* arr = thisVal.asObject();
        uint32_t len = arr->arrayLength();
        JsValue acc;
        uint32_t start = 0;
        if (args.size() > 1) { acc = ARG(1); }
        else if (len > 0) { acc = arr->arrayGet(0); start = 1; }
        for (uint32_t i = start; i < len; i++)
            acc = vm.call(ARG(0), JsValue::undefined(), {acc, arr->arrayGet(i), JsValue::integer((int32_t)i), thisVal});
        return acc;
    });
    addNative(vm, proto, "find", NATIVE("find") {
        if (!thisVal.isObject() || !ARG(0).isCallable()) return JsValue::undefined();
        auto* arr = thisVal.asObject();
        for (uint32_t i = 0; i < arr->arrayLength(); i++) {
            JsValue v = arr->arrayGet(i);
            if (vm.call(ARG(0), thisVal, {v, JsValue::integer((int32_t)i), thisVal}).toBool()) return v;
        }
        return JsValue::undefined();
    });
    addNative(vm, proto, "findIndex", NATIVE("findIndex") {
        if (!thisVal.isObject() || !ARG(0).isCallable()) return JsValue::integer(-1);
        auto* arr = thisVal.asObject();
        for (uint32_t i = 0; i < arr->arrayLength(); i++) {
            JsValue v = arr->arrayGet(i);
            if (vm.call(ARG(0), thisVal, {v, JsValue::integer((int32_t)i), thisVal}).toBool())
                return JsValue::integer((int32_t)i);
        }
        return JsValue::integer(-1);
    });
    addNative(vm, proto, "some", NATIVE("some") {
        if (!thisVal.isObject() || !ARG(0).isCallable()) return JsValue::boolean(false);
        auto* arr = thisVal.asObject();
        for (uint32_t i = 0; i < arr->arrayLength(); i++) {
            JsValue v = arr->arrayGet(i);
            if (vm.call(ARG(0), thisVal, {v, JsValue::integer((int32_t)i), thisVal}).toBool()) return JsValue::boolean(true);
        }
        return JsValue::boolean(false);
    });
    addNative(vm, proto, "every", NATIVE("every") {
        if (!thisVal.isObject() || !ARG(0).isCallable()) return JsValue::boolean(true);
        auto* arr = thisVal.asObject();
        for (uint32_t i = 0; i < arr->arrayLength(); i++) {
            JsValue v = arr->arrayGet(i);
            if (!vm.call(ARG(0), thisVal, {v, JsValue::integer((int32_t)i), thisVal}).toBool()) return JsValue::boolean(false);
        }
        return JsValue::boolean(true);
    });
    addNative(vm, proto, "flat", NATIVE("flat") {
        auto* result = vm.gc().newArray();
        int depth = args.empty() ? 1 : ARG_INT(0);
        std::function<void(JsValue, int)> flatHelper = [&](JsValue v, int d) {
            if (d > 0 && v.isObject() && v.asObject()->kind == ObjKind::Array) {
                for (uint32_t i = 0; i < v.asObject()->arrayLength(); i++)
                    flatHelper(v.asObject()->arrayGet(i), d - 1);
            } else result->arrayPush(v);
        };
        if (thisVal.isObject()) {
            for (uint32_t i = 0; i < thisVal.asObject()->arrayLength(); i++)
                flatHelper(thisVal.asObject()->arrayGet(i), depth);
        }
        return JsValue::object(result);
    });
    addNative(vm, proto, "flatMap", NATIVE("flatMap") {
        if (!ARG(0).isCallable()) return thisVal;
        auto* result = vm.gc().newArray();
        if (thisVal.isObject()) {
            auto* arr = thisVal.asObject();
            for (uint32_t i = 0; i < arr->arrayLength(); i++) {
                JsValue v = arr->arrayGet(i);
                JsValue mapped = vm.call(ARG(0), thisVal, {v, JsValue::integer((int32_t)i), thisVal});
                if (mapped.isObject() && mapped.asObject()->kind == ObjKind::Array)
                    for (uint32_t j = 0; j < mapped.asObject()->arrayLength(); j++) result->arrayPush(mapped.asObject()->arrayGet(j));
                else result->arrayPush(mapped);
            }
        }
        return JsValue::object(result);
    });
    addNative(vm, proto, "forEach", NATIVE("forEach") {
        if (thisVal.isObject() && ARG(0).isCallable()) {
            auto* arr = thisVal.asObject();
            for (uint32_t i = 0; i < arr->arrayLength(); i++)
                vm.call(ARG(0), thisVal, {arr->arrayGet(i), JsValue::integer((int32_t)i), thisVal});
        }
        return JsValue::undefined();
    });
    addNative(vm, proto, "concat", NATIVE("concat") {
        auto* result = vm.gc().newArray();
        if (thisVal.isObject()) {
            auto* arr = thisVal.asObject();
            for (uint32_t i = 0; i < arr->arrayLength(); i++) result->arrayPush(arr->arrayGet(i));
        }
        for (auto& a : args) {
            if (a.isObject() && a.asObject()->kind == ObjKind::Array) {
                for (uint32_t i = 0; i < a.asObject()->arrayLength(); i++) result->arrayPush(a.asObject()->arrayGet(i));
            } else result->arrayPush(a);
        }
        return JsValue::object(result);
    });
    addNative(vm, proto, "toString", NATIVE("array_toString") {
        if (!thisVal.isObject()) return vm.str("");
        auto* arr = thisVal.asObject();
        std::string s;
        for (uint32_t i = 0; i < arr->arrayLength(); i++) {
            if (i > 0) s += ",";
            s += arr->arrayGet(i).toString();
        }
        return vm.str(s);
    });
    addNative(vm, proto, "fill", NATIVE("fill") {
        if (!thisVal.isObject()) return thisVal;
        auto* arr = thisVal.asObject(); int len = (int)arr->arrayLength();
        JsValue v = ARG(0); int start = args.size()>1?ARG_INT(1):0, end = args.size()>2?ARG_INT(2):len;
        if (start<0)start=std::max(0,len+start); if (end<0)end=std::max(0,len+end);
        for (int i=start;i<end&&i<len;i++) arr->arraySet(i,v);
        return thisVal;
    });
    addNative(vm, proto, "keys", NATIVE("array_keys") {
        return arrayIteratorObject(vm, thisVal, "key");
    });
    addNative(vm, proto, "values", NATIVE("array_values") {
        return arrayIteratorObject(vm, thisVal, "value");
    });
    addNative(vm, proto, "entries", NATIVE("array_entries") {
        return arrayIteratorObject(vm, thisVal, "entry");
    });
    addNative(vm, proto, "at", NATIVE("array_at") {
        if (!thisVal.isObject()) return JsValue::undefined();
        auto* arr = thisVal.asObject();
        int32_t len = (int32_t)arr->arrayLength();
        int32_t index = ARG_INT(0);
        if (index < 0) index = len + index;
        return index < 0 || index >= len ? JsValue::undefined() : arr->arrayGet((uint32_t)index);
    });
    addNative(vm, proto, "copyWithin", NATIVE("copyWithin") {
        if (!thisVal.isObject()) return thisVal;
        auto* arr = thisVal.asObject();
        int32_t len = (int32_t)arr->arrayLength();
        int32_t target = ARG_INT(0); if (target < 0) target = len + target;
        int32_t start = args.size() > 1 ? ARG_INT(1) : 0; if (start < 0) start = len + start;
        int32_t end = args.size() > 2 ? ARG_INT(2) : len; if (end < 0) end = len + end;
        target = std::max(0, std::min(target, len));
        start = std::max(0, std::min(start, len));
        end = std::max(0, std::min(end, len));
        std::vector<JsValue> values;
        for (int32_t i = start; i < end; ++i) values.push_back(arr->arrayGet((uint32_t)i));
        for (size_t i = 0; i < values.size() && target + (int32_t)i < len; ++i) arr->arraySet((uint32_t)(target + i), values[i]);
        return thisVal;
    });
    addNative(vm, proto, "findLast", NATIVE("findLast") {
        if (!thisVal.isObject() || !ARG(0).isCallable()) return JsValue::undefined();
        auto* arr = thisVal.asObject();
        for (int32_t i = (int32_t)arr->arrayLength() - 1; i >= 0; --i) {
            JsValue v = arr->arrayGet((uint32_t)i);
            if (vm.call(ARG(0), thisVal, {v, JsValue::integer(i), thisVal}).toBool()) return v;
        }
        return JsValue::undefined();
    });
    addNative(vm, proto, "findLastIndex", NATIVE("findLastIndex") {
        if (!thisVal.isObject() || !ARG(0).isCallable()) return JsValue::integer(-1);
        auto* arr = thisVal.asObject();
        for (int32_t i = (int32_t)arr->arrayLength() - 1; i >= 0; --i) {
            JsValue v = arr->arrayGet((uint32_t)i);
            if (vm.call(ARG(0), thisVal, {v, JsValue::integer(i), thisVal}).toBool()) return JsValue::integer(i);
        }
        return JsValue::integer(-1);
    });
    addNative(vm, proto, "toReversed", NATIVE("toReversed") {
        auto* out = newArrayWithPrototype(vm);
        if (thisVal.isObject()) {
            auto* arr = thisVal.asObject();
            for (int32_t i = (int32_t)arr->arrayLength() - 1; i >= 0; --i) out->arrayPush(arr->arrayGet((uint32_t)i));
        }
        return JsValue::object(out);
    });
    addNative(vm, proto, "toSorted", NATIVE("toSorted") {
        auto* out = newArrayWithPrototype(vm);
        if (thisVal.isObject()) {
            auto* arr = thisVal.asObject();
            std::vector<JsValue> elems;
            for (uint32_t i = 0; i < arr->arrayLength(); ++i) elems.push_back(arr->arrayGet(i));
            JsValue cmpFn = ARG(0);
            std::stable_sort(elems.begin(), elems.end(), [&](const JsValue& a, const JsValue& b) {
                if (cmpFn.isCallable()) {
                    try { return vm.call(cmpFn, JsValue::undefined(), { a, b }).toNumber() < 0; } catch (...) {}
                }
                return a.toString() < b.toString();
            });
            for (const auto& value : elems) out->arrayPush(value);
        }
        return JsValue::object(out);
    });
    addNative(vm, proto, "toSpliced", NATIVE("toSpliced") {
        auto* out = newArrayWithPrototype(vm);
        if (!thisVal.isObject()) return JsValue::object(out);
        auto* arr = thisVal.asObject();
        int32_t len = (int32_t)arr->arrayLength();
        int32_t start = ARG_INT(0); if (start < 0) start = len + start;
        start = std::max(0, std::min(start, len));
        int32_t deleteCount = args.size() > 1 ? ARG_INT(1) : len - start;
        deleteCount = std::max(0, std::min(deleteCount, len - start));
        for (int32_t i = 0; i < start; ++i) out->arrayPush(arr->arrayGet((uint32_t)i));
        for (size_t i = 2; i < args.size(); ++i) out->arrayPush(args[i]);
        for (int32_t i = start + deleteCount; i < len; ++i) out->arrayPush(arr->arrayGet((uint32_t)i));
        return JsValue::object(out);
    });
    addNative(vm, proto, "with", NATIVE("array_with") {
        JsValue copyFn = thisVal.isObject() ? thisVal.asObject()->getProp("slice") : JsValue::undefined();
        JsValue copied = copyFn.isCallable() ? vm.call(copyFn, thisVal, {}) : JsValue::object(vm.gc().newArray());
        vm.gc().addRoot(&copied);
        if (copied.isObject()) {
            auto* arr = copied.asObject();
            int32_t len = (int32_t)arr->arrayLength();
            int32_t index = ARG_INT(0);
            if (index < 0) index = len + index;
            if (index >= 0 && index < len) arr->arraySet((uint32_t)index, ARG(1));
        }
        vm.gc().removeRoot(&copied);
        return copied;
    });
    proto->setProp("Symbol(Symbol.iterator)_registry", proto->getProp("values"));

    // Static methods
    auto* ctor = vm.gc().newNativeFunction(NATIVE("Array") {
        auto* arr = vm.gc().newArray();
        if (args.size() == 1 && ARG(0).isInt32()) arr->arraySetLength(ARG(0).asInt32());
        else for (auto& a : args) arr->arrayPush(a);
        return JsValue::object(arr);
    }, "Array");
    ctor->setProp("prototype", JsValue::object(proto));
    addNative(vm, ctor, "isArray", NATIVE("isArray") {
        return JsValue::boolean(ARG(0).isObject() && ARG(0).asObject()->kind == ObjKind::Array);
    });
    addNative(vm, ctor, "from", NATIVE("from") {
        auto* result = vm.gc().newArray();
        JsValue src = ARG(0), mapFn = ARG(1);
        if (src.isObject()) {
            auto* o = src.asObject();
            JsValue nextFn = o->getProp("next");
            if (nextFn.isCallable()) {
                int32_t i = 0;
                while (true) {
                    JsValue step = vm.call(nextFn, src, {});
                    if (!step.isObject() || step.asObject()->getProp("done").toBool()) break;
                    JsValue v = step.asObject()->getProp("value");
                    if (mapFn.isCallable()) v = vm.call(mapFn, JsValue::undefined(), {v, JsValue::integer(i)});
                    result->arrayPush(v);
                    ++i;
                }
            } else if (o->kind == ObjKind::Array) {
                for (uint32_t i = 0; i < o->arrayLength(); i++) {
                    JsValue v = o->arrayGet(i);
                    if (mapFn.isCallable()) v = vm.call(mapFn, JsValue::undefined(), {v, JsValue::integer((int32_t)i)});
                    result->arrayPush(v);
                }
            } else {
                JsValue len = o->getProp("length");
                if (!len.isUndefined()) {
                    int n = len.toInt32();
                    for (int i = 0; i < n; i++) {
                        JsValue v = o->getProp(std::to_string(i));
                        if (mapFn.isCallable()) v = vm.call(mapFn, JsValue::undefined(), {v, JsValue::integer(i)});
                        result->arrayPush(v);
                    }
                }
            }
        } else if (src.isString()) {
            const auto& s = src.asString()->value;
            for (size_t i = 0; i < s.size(); i++) {
                JsValue v = vm.str(std::string(1, s[i]));
                if (mapFn.isCallable()) v = vm.call(mapFn, JsValue::undefined(), {v, JsValue::integer((int32_t)i)});
                result->arrayPush(v);
            }
        }
        return JsValue::object(result);
    });
    addNative(vm, ctor, "of", NATIVE("of") {
        auto* arr = vm.gc().newArray();
        for (auto& a : args) arr->arrayPush(a);
        return JsValue::object(arr);
    });
    vm.setGlobal("Array", JsValue::object(ctor));
}

// â”€â”€ String â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static std::string getStr(JsValue v) {
    if (v.isString()) return v.asString()->value;
    return v.toString();
}

static void registerString(VM& vm) {
    auto* proto = vm.gc().newObject(ObjKind::Plain);

    addNative(vm, proto, "toString", NATIVE("str_toString") { return thisVal; });
    addNative(vm, proto, "valueOf",  NATIVE("str_valueOf")  { return thisVal; });
    addNative(vm, proto, "charAt", NATIVE("charAt") {
        std::string s = getStr(thisVal); int i = ARG_INT(0);
        if (i < 0 || i >= (int)s.size()) return vm.str("");
        return vm.str(std::string(1, s[i]));
    });
    addNative(vm, proto, "charCodeAt", NATIVE("charCodeAt") {
        std::string s = getStr(thisVal); int i = ARG_INT(0);
        if (i < 0 || i >= (int)s.size()) return JsValue::number(std::nan(""));
        return JsValue::integer((int32_t)(unsigned char)s[i]);
    });
    addNative(vm, proto, "indexOf", NATIVE("str_indexOf") {
        std::string s = getStr(thisVal), sub = ARG_STR(0);
        auto pos = s.find(sub);
        return JsValue::integer(pos == std::string::npos ? -1 : (int32_t)pos);
    });
    addNative(vm, proto, "lastIndexOf", NATIVE("str_lastIndexOf") {
        std::string s = getStr(thisVal), sub = ARG_STR(0);
        auto pos = s.rfind(sub);
        return JsValue::integer(pos == std::string::npos ? -1 : (int32_t)pos);
    });
    addNative(vm, proto, "includes", NATIVE("str_includes") {
        std::string s = getStr(thisVal), sub = ARG_STR(0);
        return JsValue::boolean(s.find(sub) != std::string::npos);
    });
    addNative(vm, proto, "startsWith", NATIVE("startsWith") {
        std::string s = getStr(thisVal), sub = ARG_STR(0);
        return JsValue::boolean(s.size() >= sub.size() && s.compare(0, sub.size(), sub) == 0);
    });
    addNative(vm, proto, "endsWith", NATIVE("endsWith") {
        std::string s = getStr(thisVal), sub = ARG_STR(0);
        return JsValue::boolean(s.size() >= sub.size() && s.compare(s.size()-sub.size(), sub.size(), sub) == 0);
    });
    addNative(vm, proto, "slice", NATIVE("str_slice") {
        std::string s = getStr(thisVal); int len = (int)s.size();
        int start = args.size()>0?ARG_INT(0):0, end = args.size()>1?ARG_INT(1):len;
        if (start<0)start=std::max(0,len+start); if (end<0)end=std::max(0,len+end);
        start=std::min(start,len); end=std::min(end,len);
        return vm.str(start<end?s.substr(start,end-start):"");
    });
    addNative(vm, proto, "substring", NATIVE("substring") {
        std::string s = getStr(thisVal); int len=(int)s.size();
        int a=std::max(0,std::min(ARG_INT(0),len)), b=args.size()>1?std::max(0,std::min(ARG_INT(1),len)):len;
        if (a>b)std::swap(a,b);
        return vm.str(s.substr(a,b-a));
    });
    addNative(vm, proto, "toUpperCase", NATIVE("toUpperCase") {
        std::string s = getStr(thisVal);
        for (char& c : s) c = toupper(c);
        return vm.str(s);
    });
    addNative(vm, proto, "toLowerCase", NATIVE("toLowerCase") {
        std::string s = getStr(thisVal);
        for (char& c : s) c = tolower(c);
        return vm.str(s);
    });
    addNative(vm, proto, "trim", NATIVE("trim") {
        std::string s = getStr(thisVal);
        auto b = s.find_first_not_of(" \t\n\r\f\v");
        auto e = s.find_last_not_of(" \t\n\r\f\v");
        return vm.str(b==std::string::npos?"":s.substr(b,e-b+1));
    });
    addNative(vm, proto, "trimStart", NATIVE("trimStart") {
        std::string s = getStr(thisVal);
        auto b = s.find_first_not_of(" \t\n\r\f\v");
        return vm.str(b==std::string::npos?"":s.substr(b));
    });
    addNative(vm, proto, "trimEnd", NATIVE("trimEnd") {
        std::string s = getStr(thisVal);
        auto e = s.find_last_not_of(" \t\n\r\f\v");
        return vm.str(e==std::string::npos?"":s.substr(0,e+1));
    });
    addNative(vm, proto, "split", NATIVE("split") {
        std::string s = getStr(thisVal);
        auto* result = vm.gc().newArray();
        if (ARG(0).isUndefined()) { result->arrayPush(vm.str(s)); return JsValue::object(result); }
        std::string sep = ARG_STR(0);
        if (sep.empty()) { for (char c : s) result->arrayPush(vm.str(std::string(1,c))); return JsValue::object(result); }
        size_t pos = 0, found;
        while ((found = s.find(sep, pos)) != std::string::npos) {
            result->arrayPush(vm.str(s.substr(pos, found - pos)));
            pos = found + sep.size();
        }
        result->arrayPush(vm.str(s.substr(pos)));
        return JsValue::object(result);
    });
    addNative(vm, proto, "replace", NATIVE("replace") {
        std::string s = getStr(thisVal), from = ARG_STR(0), to;
        if (ARG(1).isCallable()) {
            auto pos = s.find(from);
            if (pos != std::string::npos) {
                JsValue replaced = vm.call(ARG(1), JsValue::undefined(), {vm.str(from), JsValue::integer((int32_t)pos), thisVal});
                to = replaced.toString();
                return vm.str(s.substr(0,pos) + to + s.substr(pos + from.size()));
            }
            return vm.str(s);
        }
        to = ARG_STR(1);
        auto pos = s.find(from);
        if (pos != std::string::npos) return vm.str(s.substr(0,pos) + to + s.substr(pos + from.size()));
        return vm.str(s);
    });
    addNative(vm, proto, "replaceAll", NATIVE("replaceAll") {
        std::string s = getStr(thisVal), from = ARG_STR(0), to = ARG_STR(1);
        if (from.empty()) return vm.str(s);
        std::string result;
        size_t pos = 0, found;
        while ((found = s.find(from, pos)) != std::string::npos) {
            result += s.substr(pos, found - pos) + to;
            pos = found + from.size();
        }
        result += s.substr(pos);
        return vm.str(result);
    });
    addNative(vm, proto, "match", NATIVE("match") {
        std::string s = getStr(thisVal);
        if (ARG(0).isObject() && ARG(0).asObject()->kind == ObjKind::RegExp) {
            std::string pattern = ARG(0).asObject()->getProp("source").toString();
            std::string flags   = ARG(0).asObject()->getProp("flags").toString();
            try {
                auto rxFlags = std::regex_constants::ECMAScript;
                if (flags.find('i') != std::string::npos) rxFlags |= std::regex_constants::icase;
                std::regex rx(pattern, rxFlags);
                bool global = (flags.find('g') != std::string::npos);
                if (global) {
                    auto* arr = vm.gc().newArray();
                    auto it = std::sregex_iterator(s.begin(), s.end(), rx);
                    for (; it != std::sregex_iterator(); ++it) arr->arrayPush(vm.str((*it)[0].str()));
                    return arr->arrayLength() > 0 ? JsValue::object(arr) : JsValue::null();
                } else {
                    std::smatch m;
                    if (!std::regex_search(s, m, rx)) return JsValue::null();
                    auto* arr = vm.gc().newArray();
                    for (size_t i = 0; i < m.size(); ++i) arr->arrayPush(vm.str(m[i].str()));
                    arr->setProp("index", JsValue::integer((int32_t)m.position(0)));
                    return JsValue::object(arr);
                }
            } catch (...) { return JsValue::null(); }
        }
        std::string pat = ARG_STR(0);
        auto pos = s.find(pat);
        if (pos == std::string::npos) return JsValue::null();
        auto* arr = vm.gc().newArray();
        arr->arrayPush(vm.str(pat));
        arr->setProp("index", JsValue::integer((int32_t)pos));
        return JsValue::object(arr);
    });
    addNative(vm, proto, "search", NATIVE("search") {
        std::string s = getStr(thisVal);
        std::string pat = ARG_STR(0);
        try {
            std::regex rx(pat, std::regex_constants::ECMAScript);
            std::smatch m;
            if (std::regex_search(s, m, rx)) return JsValue::integer((int32_t)m.position(0));
        } catch (...) {
            auto pos = s.find(pat);
            if (pos != std::string::npos) return JsValue::integer((int32_t)pos);
        }
        return JsValue::integer(-1);
    });
    addNative(vm, proto, "matchAll", NATIVE("matchAll") {
        auto* arr = vm.gc().newArray();
        std::string s = getStr(thisVal);
        if (ARG(0).isObject() && ARG(0).asObject()->kind == ObjKind::RegExp) {
            std::string pattern = ARG(0).asObject()->getProp("source").toString();
            std::string flags   = ARG(0).asObject()->getProp("flags").toString();
            try {
                auto rxFlags = std::regex_constants::ECMAScript;
                if (flags.find('i') != std::string::npos) rxFlags |= std::regex_constants::icase;
                std::regex rx(pattern, rxFlags);
                auto it = std::sregex_iterator(s.begin(), s.end(), rx);
                for (; it != std::sregex_iterator(); ++it) {
                    auto* m = vm.gc().newArray();
                    for (size_t i = 0; i < it->size(); ++i) m->arrayPush(vm.str((*it)[i].str()));
                    m->setProp("index", JsValue::integer((int32_t)it->position(0)));
                    arr->arrayPush(JsValue::object(m));
                }
            } catch (...) {}
        }
        return JsValue::object(arr);
    });
    addNative(vm, proto, "padStart", NATIVE("padStart") {
        std::string s = getStr(thisVal); int len = ARG_INT(0);
        std::string pad = args.size()>1?ARG_STR(1):" ";
        if ((int)s.size() >= len) return vm.str(s);
        std::string result;
        while ((int)(result.size() + s.size()) < len) result += pad;
        result = result.substr(0, len - s.size());
        return vm.str(result + s);
    });
    addNative(vm, proto, "padEnd", NATIVE("padEnd") {
        std::string s = getStr(thisVal); int len = ARG_INT(0);
        std::string pad = args.size()>1?ARG_STR(1):" ";
        if ((int)s.size() >= len) return vm.str(s);
        std::string result = s;
        while ((int)result.size() < len) result += pad;
        return vm.str(result.substr(0, len));
    });
    addNative(vm, proto, "repeat", NATIVE("repeat") {
        std::string s = getStr(thisVal); int n = ARG_INT(0);
        if (n <= 0) return vm.str("");
        std::string result;
        for (int i = 0; i < n; i++) result += s;
        return vm.str(result);
    });
    addNative(vm, proto, "at", NATIVE("str_at") {
        std::string s = getStr(thisVal); int i = ARG_INT(0);
        if (i < 0) i = (int)s.size() + i;
        if (i < 0 || i >= (int)s.size()) return JsValue::undefined();
        return vm.str(std::string(1, s[i]));
    });
    addNative(vm, proto, "Symbol(Symbol.iterator)_registry", NATIVE("str_iterator") {
        return arrayIteratorObject(vm, thisVal, "value");
    });

    auto* ctor = vm.gc().newNativeFunction(NATIVE("String") {
        if (args.empty()) return vm.str("");
        return vm.str(ARG_STR(0));
    }, "String");
    ctor->setProp("prototype", JsValue::object(proto));
    addNative(vm, ctor, "fromCharCode", NATIVE("fromCharCode") {
        std::string s;
        for (auto& a : args) s += (char)(int)a.toNumber();
        return vm.str(s);
    });
    vm.setGlobal("String", JsValue::object(ctor));
}

// â”€â”€ Number â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerNumber(VM& vm) {
    auto* proto = vm.gc().newObject(ObjKind::Plain);
    addNative(vm, proto, "toString", NATIVE("num_toString") {
        double v = thisVal.toNumber();
        int base = args.empty() ? 10 : ARG_INT(0);
        if (base == 10) {
            std::ostringstream ss;
            if (v == (int64_t)v) ss << (int64_t)v; else ss << v;
            return vm.str(ss.str());
        }
        if (base < 2 || base > 36) vm.throwRangeError("toString() radix must be between 2 and 36");
        if (std::isnan(v)) return vm.str("NaN");
        if (std::isinf(v)) return vm.str(v > 0 ? "Infinity" : "-Infinity");
        int64_t n = (int64_t)std::abs(v);
        std::string result;
        if (n == 0) return vm.str("0");
        while (n > 0) { result = "0123456789abcdefghijklmnopqrstuvwxyz"[n % base] + result; n /= base; }
        if (v < 0) result = "-" + result;
        return vm.str(result);
    });
    addNative(vm, proto, "toFixed", NATIVE("toFixed") {
        double v = thisVal.toNumber(); int d = ARG_INT(0);
        std::ostringstream ss;
        ss << std::fixed;
        ss.precision(d);
        ss << v;
        return vm.str(ss.str());
    });
    addNative(vm, proto, "valueOf", NATIVE("num_valueOf") { return thisVal; });

    auto* ctor = vm.gc().newNativeFunction(NATIVE("Number") {
        if (args.empty()) return JsValue::integer(0);
        return JsValue::number(ARG_NUM(0));
    }, "Number");
    ctor->setProp("prototype", JsValue::object(proto));
    ctor->setProp("isNaN",          JsValue::object(vm.gc().newNativeFunction(NATIVE("isNaN")     { return JsValue::boolean(std::isnan(ARG_NUM(0))); }, "isNaN")));
    ctor->setProp("isFinite",       JsValue::object(vm.gc().newNativeFunction(NATIVE("isFinite")  { return JsValue::boolean(std::isfinite(ARG_NUM(0))); }, "isFinite")));
    ctor->setProp("isInteger",      JsValue::object(vm.gc().newNativeFunction(NATIVE("isInteger") { double v=ARG_NUM(0); return JsValue::boolean(std::isfinite(v)&&v==(int64_t)v); }, "isInteger")));
    ctor->setProp("parseInt",       JsValue::object(vm.gc().newNativeFunction(NATIVE("parseInt")  { return JsValue::integer((int32_t)strtol(ARG_STR(0).c_str(),nullptr,args.size()>1?ARG_INT(1):10)); }, "parseInt")));
    ctor->setProp("parseFloat",     JsValue::object(vm.gc().newNativeFunction(NATIVE("parseFloat"){ return JsValue::number(strtod(ARG_STR(0).c_str(),nullptr)); }, "parseFloat")));
    ctor->setProp("MAX_SAFE_INTEGER", JsValue::number(9007199254740991.0));
    ctor->setProp("MIN_SAFE_INTEGER", JsValue::number(-9007199254740991.0));
    ctor->setProp("MAX_VALUE",       JsValue::number(1.7976931348623157e+308));
    ctor->setProp("MIN_VALUE",       JsValue::number(5e-324));
    ctor->setProp("POSITIVE_INFINITY", JsValue::number(std::numeric_limits<double>::infinity()));
    ctor->setProp("NEGATIVE_INFINITY", JsValue::number(-std::numeric_limits<double>::infinity()));
    ctor->setProp("NaN",             JsValue::number(std::nan("")));
    ctor->setProp("EPSILON",         JsValue::number(2.220446049250313e-16));
    vm.setGlobal("Number", JsValue::object(ctor));
}

// â”€â”€ Boolean â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerBoolean(VM& vm) {
    auto* ctor = vm.gc().newNativeFunction(NATIVE("Boolean") {
        return JsValue::boolean(!args.empty() && ARG(0).toBool());
    }, "Boolean");
    vm.setGlobal("Boolean", JsValue::object(ctor));
}

// â”€â”€ Math â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerMath(VM& vm) {
    auto* math = vm.gc().newObject(ObjKind::Plain);
    math->setProp("PI",      JsValue::number(M_PI));
    math->setProp("E",       JsValue::number(M_E));
    math->setProp("LN2",     JsValue::number(M_LN2));
    math->setProp("LN10",    JsValue::number(std::log(10.0)));
    math->setProp("LOG2E",   JsValue::number(M_LOG2E));
    math->setProp("LOG10E",  JsValue::number(M_LOG10E));
    math->setProp("SQRT2",   JsValue::number(M_SQRT2));

    auto addM = [&](const char* n, auto fn) { addNative(vm, math, n, fn); };
    addM("abs",   NATIVE("abs")   { return JsValue::number(std::abs(ARG_NUM(0))); });
    addM("ceil",  NATIVE("ceil")  { return JsValue::number(std::ceil(ARG_NUM(0))); });
    addM("floor", NATIVE("floor") { return JsValue::number(std::floor(ARG_NUM(0))); });
    addM("round", NATIVE("round") { return JsValue::number(std::round(ARG_NUM(0))); });
    addM("trunc", NATIVE("trunc") { return JsValue::number(std::trunc(ARG_NUM(0))); });
    addM("sqrt",  NATIVE("sqrt")  { return JsValue::number(std::sqrt(ARG_NUM(0))); });
    addM("cbrt",  NATIVE("cbrt")  { return JsValue::number(std::cbrt(ARG_NUM(0))); });
    addM("pow",   NATIVE("pow")   { return JsValue::number(std::pow(ARG_NUM(0), ARG_NUM(1))); });
    addM("log",   NATIVE("log")   { return JsValue::number(std::log(ARG_NUM(0))); });
    addM("log2",  NATIVE("log2")  { return JsValue::number(std::log2(ARG_NUM(0))); });
    addM("log10", NATIVE("log10") { return JsValue::number(std::log10(ARG_NUM(0))); });
    addM("exp",   NATIVE("exp")   { return JsValue::number(std::exp(ARG_NUM(0))); });
    addM("sin",   NATIVE("sin")   { return JsValue::number(std::sin(ARG_NUM(0))); });
    addM("cos",   NATIVE("cos")   { return JsValue::number(std::cos(ARG_NUM(0))); });
    addM("tan",   NATIVE("tan")   { return JsValue::number(std::tan(ARG_NUM(0))); });
    addM("asin",  NATIVE("asin")  { return JsValue::number(std::asin(ARG_NUM(0))); });
    addM("acos",  NATIVE("acos")  { return JsValue::number(std::acos(ARG_NUM(0))); });
    addM("atan",  NATIVE("atan")  { return JsValue::number(std::atan(ARG_NUM(0))); });
    addM("atan2", NATIVE("atan2") { return JsValue::number(std::atan2(ARG_NUM(0), ARG_NUM(1))); });
    addM("hypot", NATIVE("hypot") {
        double sum = 0; for (auto& a : args) { double v=a.toNumber(); sum+=v*v; }
        return JsValue::number(std::sqrt(sum));
    });
    addM("max", NATIVE("max") {
        if (args.empty()) return JsValue::number(-std::numeric_limits<double>::infinity());
        double m = ARG_NUM(0);
        for (size_t i=1;i<args.size();i++) m = std::max(m, args[i].toNumber());
        return JsValue::number(m);
    });
    addM("min", NATIVE("min") {
        if (args.empty()) return JsValue::number(std::numeric_limits<double>::infinity());
        double m = ARG_NUM(0);
        for (size_t i=1;i<args.size();i++) m = std::min(m, args[i].toNumber());
        return JsValue::number(m);
    });
    addM("sign",  NATIVE("sign")  { double v=ARG_NUM(0); return JsValue::number(v<0?-1:v>0?1:0); });
    addM("clz32", NATIVE("clz32") {
        uint32_t v = (uint32_t)ARG_INT(0);
        if (v == 0) return JsValue::integer(32);
        int cnt = 0; while (!(v & 0x80000000u)) { v<<=1; cnt++; } return JsValue::integer(cnt);
    });
    addM("fround", NATIVE("fround"){ return JsValue::number((float)ARG_NUM(0)); });
    addM("imul",   NATIVE("imul")  { return JsValue::integer(ARG_INT(0) * ARG_INT(1)); });
    addM("random", NATIVE("random"){
        static std::mt19937_64 rng(std::chrono::steady_clock::now().time_since_epoch().count());
        static std::uniform_real_distribution<double> dist(0.0, 1.0);
        return JsValue::number(dist(rng));
    });

    vm.setGlobal("Math", JsValue::object(math));
}

// â”€â”€ JSON â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static std::string jsonStringify(JsValue val, int indent = 0, int depth = 0) {
    if (val.isUndefined() || val.tag == JsTag::Object && val.isCallable()) return "";
    if (val.isNull()) return "null";
    if (val.isBool()) return val.asBool() ? "true" : "false";
    if (val.isInt32()) return std::to_string(val.asInt32());
    if (val.isNumber()) {
        double d = val.asDouble();
        if (std::isnan(d) || std::isinf(d)) return "null";
        std::ostringstream ss;
        if (d == (int64_t)d) ss << (int64_t)d; else { ss.precision(17); ss << d; }
        return ss.str();
    }
    if (val.isString()) {
        std::string s = val.asString()->value, result = "\"";
        for (char c : s) {
            if (c=='"') result += "\\\"";
            else if (c=='\\') result += "\\\\";
            else if (c=='\n') result += "\\n";
            else if (c=='\r') result += "\\r";
            else if (c=='\t') result += "\\t";
            else result += c;
        }
        return result + "\"";
    }
    if (val.isObject()) {
        auto* o = val.asObject();
        if (depth > 50) return "null"; // cycle guard
        if (o->kind == ObjKind::Array) {
            std::string r = "["; bool first = true;
            for (uint32_t i = 0; i < o->arrayLength(); i++) {
                if (!first) r += ","; first = false;
                std::string v = jsonStringify(o->arrayGet(i), indent, depth+1);
                r += v.empty() ? "null" : v;
            }
            return r + "]";
        }
        std::string r = "{"; bool first = true;
        for (auto& k : o->ownEnumKeys()) {
            std::string v = jsonStringify(o->getProp(k), indent, depth+1);
            if (v.empty()) continue;
            if (!first) r += ","; first = false;
            r += "\"" + k + "\":" + v;
        }
        return r + "}";
    }
    return "null";
}

static JsValue jsonParse(VM& vm, const std::string& s, size_t& pos) {
    auto skip = [&]() { while (pos<s.size() && isspace(s[pos])) pos++; };
    skip();
    if (pos >= s.size()) return JsValue::undefined();
    char c = s[pos];
    if (c == 'n') { pos+=4; return JsValue::null(); }
    if (c == 't') { pos+=4; return JsValue::boolean(true); }
    if (c == 'f') { pos+=5; return JsValue::boolean(false); }
    if (c == '"') {
        pos++; std::string res;
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\') { pos++; switch(s[pos++]) {
                case 'n':res+='\n';break; case 't':res+='\t';break;
                case 'r':res+='\r';break; case '"':res+='"';break;
                case '\\':res+='\\';break; default:break; } }
            else res += s[pos++];
        }
        pos++; return vm.str(res);
    }
    if (c == '[') {
        pos++; auto* arr = vm.gc().newArray();
        skip();
        if (pos<s.size()&&s[pos]==']'){pos++;return JsValue::object(arr);}
        while (pos<s.size()) {
            arr->arrayPush(jsonParse(vm,s,pos));
            skip();
            if (pos<s.size()&&s[pos]==',') { pos++; continue; }
            if (pos<s.size()&&s[pos]==']') { pos++; break; }
        }
        return JsValue::object(arr);
    }
    if (c == '{') {
        pos++; auto* o = vm.gc().newObject(ObjKind::Plain);
        skip();
        if (pos<s.size()&&s[pos]=='}'){pos++;return JsValue::object(o);}
        while (pos<s.size()) {
            skip();
            std::string key = jsonParse(vm,s,pos).toString();
            skip();
            if (pos<s.size()&&s[pos]==':') pos++;
            JsValue val = jsonParse(vm,s,pos);
            o->setProp(key,val); skip();
            if (pos<s.size()&&s[pos]==',') { pos++; continue; }
            if (pos<s.size()&&s[pos]=='}') { pos++; break; }
        }
        return JsValue::object(o);
    }
    // Number
    size_t start = pos;
    if (c=='-') pos++;
    while (pos<s.size()&&(isdigit(s[pos])||s[pos]=='.'||s[pos]=='e'||s[pos]=='E'||s[pos]=='+'||s[pos]=='-')) pos++;
    double d = std::stod(s.substr(start, pos-start));
    return JsValue::number(d);
}

static void registerJSON(VM& vm) {
    auto* json = vm.gc().newObject(ObjKind::Plain);
    addNative(vm, json, "stringify", NATIVE("stringify") {
        return vm.str(jsonStringify(ARG(0)));
    });
    addNative(vm, json, "parse", NATIVE("parse") {
        std::string s = ARG_STR(0);
        size_t pos = 0;
        return jsonParse(vm, s, pos);
    });
    vm.setGlobal("JSON", JsValue::object(json));
}

// â”€â”€ Promise â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerPromise(VM& vm) {
    auto* ctor = vm.gc().newNativeFunction(NATIVE("Promise") {
        auto* p = vm.gc().newPromise();
        vm.initPromiseObject(p);
        JsValue pVal = JsValue::object(p);
        if (ARG(0).isCallable()) {
            auto* resolveFn = vm.gc().newNativeFunction([p](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
                vm.settlePromiseObject(p, args.empty() ? JsValue::undefined() : args[0], false);
                return JsValue::undefined();
            }, "resolve");
            auto* rejectFn = vm.gc().newNativeFunction([p](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
                vm.settlePromiseObject(p, args.empty() ? JsValue::undefined() : args[0], true);
                return JsValue::undefined();
            }, "reject");
            try {
                vm.call(ARG(0), JsValue::undefined(), {JsValue::object(resolveFn), JsValue::object(rejectFn)});
            } catch (JsException& ex) {
                vm.rejectPromise(p, ex.val);
            }
        }
        return pVal;
    }, "Promise");
    addNative(vm, ctor, "resolve", NATIVE("Promise.resolve") { return vm.promiseResolve(ARG(0)); });
    addNative(vm, ctor, "reject",  NATIVE("Promise.reject")  { return vm.promiseReject(ARG(0)); });
    addNative(vm, ctor, "all", NATIVE("Promise.all") {
        auto* out = vm.gc().newPromise();
        vm.initPromiseObject(out);
        JsValue iterable = ARG(0);
        if (!iterable.isObject() || iterable.asObject()->kind != ObjKind::Array) {
            vm.resolvePromise(out, iterable);
            return JsValue::object(out);
        }
        auto* source = iterable.asObject();
        auto* values = vm.gc().newArray();
        uint32_t len = source->arrayLength();
        if (len == 0) {
            vm.resolvePromise(out, JsValue::object(values));
            return JsValue::object(out);
        }
        auto remaining = std::make_shared<uint32_t>(len);
        for (uint32_t i = 0; i < len; ++i) {
            values->arraySet(i, JsValue::undefined());
            JsValue item = source->arrayGet(i);
            auto fulfill = JsValue::object(vm.gc().newNativeFunction(
                [out, values, remaining, i](VM& v, JsValue, std::vector<JsValue> a) -> JsValue {
                    values->arraySet(i, a.empty() ? JsValue::undefined() : a[0]);
                    if (*remaining > 0 && --(*remaining) == 0)
                        v.resolvePromise(out, JsValue::object(values));
                    return JsValue::undefined();
                }, "Promise.all.fulfill"));
            auto reject = JsValue::object(vm.gc().newNativeFunction(
                [out](VM& v, JsValue, std::vector<JsValue> a) -> JsValue {
                    v.rejectPromise(out, a.empty() ? JsValue::undefined() : a[0]);
                    return JsValue::undefined();
                }, "Promise.all.reject"));
            if (item.isObject() && item.asObject()->kind == ObjKind::Promise)
                vm.promiseThen(item.asObject(), fulfill, reject);
            else {
                values->arraySet(i, item);
                if (*remaining > 0 && --(*remaining) == 0)
                    vm.resolvePromise(out, JsValue::object(values));
            }
        }
        return JsValue::object(out);
    });
    addNative(vm, ctor, "allSettled", NATIVE("Promise.allSettled") {
        auto* out = vm.gc().newPromise();
        vm.initPromiseObject(out);
        JsValue iterable = ARG(0);
        if (!iterable.isObject() || iterable.asObject()->kind != ObjKind::Array) {
            vm.resolvePromise(out, iterable);
            return JsValue::object(out);
        }
        auto* source = iterable.asObject();
        auto* values = vm.gc().newArray();
        uint32_t len = source->arrayLength();
        if (len == 0) {
            vm.resolvePromise(out, JsValue::object(values));
            return JsValue::object(out);
        }
        auto remaining = std::make_shared<uint32_t>(len);
        auto settleOne = [out, values, remaining](VM& v, uint32_t i, JsValue value, bool rejected) {
            auto* record = v.gc().newObject(ObjKind::Plain);
            record->setProp("status", v.str(rejected ? "rejected" : "fulfilled"));
            record->setProp(rejected ? "reason" : "value", value);
            values->arraySet(i, JsValue::object(record));
            if (*remaining > 0 && --(*remaining) == 0)
                v.resolvePromise(out, JsValue::object(values));
        };
        for (uint32_t i = 0; i < len; ++i) {
            values->arraySet(i, JsValue::undefined());
            JsValue item = source->arrayGet(i);
            auto fulfill = JsValue::object(vm.gc().newNativeFunction(
                [settleOne, i](VM& v, JsValue, std::vector<JsValue> a) -> JsValue {
                    settleOne(v, i, a.empty() ? JsValue::undefined() : a[0], false);
                    return JsValue::undefined();
                }, "Promise.allSettled.fulfill"));
            auto reject = JsValue::object(vm.gc().newNativeFunction(
                [settleOne, i](VM& v, JsValue, std::vector<JsValue> a) -> JsValue {
                    settleOne(v, i, a.empty() ? JsValue::undefined() : a[0], true);
                    return JsValue::undefined();
                }, "Promise.allSettled.reject"));
            if (item.isObject() && item.asObject()->kind == ObjKind::Promise)
                vm.promiseThen(item.asObject(), fulfill, reject);
            else
                settleOne(vm, i, item, false);
        }
        return JsValue::object(out);
    });
    addNative(vm, ctor, "race", NATIVE("Promise.race") {
        auto* out = vm.gc().newPromise();
        vm.initPromiseObject(out);
        JsValue iterable = ARG(0);
        if (!iterable.isObject() || iterable.asObject()->kind != ObjKind::Array) {
            vm.resolvePromise(out, iterable);
            return JsValue::object(out);
        }
        auto* source = iterable.asObject();
        for (uint32_t i = 0; i < source->arrayLength(); ++i) {
            JsValue item = source->arrayGet(i);
            if (item.isObject() && item.asObject()->kind == ObjKind::Promise) {
                auto fulfill = JsValue::object(vm.gc().newNativeFunction(
                    [out](VM& v, JsValue, std::vector<JsValue> a) -> JsValue {
                        v.resolvePromise(out, a.empty() ? JsValue::undefined() : a[0]);
                        return JsValue::undefined();
                    }, "Promise.race.fulfill"));
                auto reject = JsValue::object(vm.gc().newNativeFunction(
                    [out](VM& v, JsValue, std::vector<JsValue> a) -> JsValue {
                        v.rejectPromise(out, a.empty() ? JsValue::undefined() : a[0]);
                        return JsValue::undefined();
                    }, "Promise.race.reject"));
                vm.promiseThen(item.asObject(), fulfill, reject);
            } else {
                vm.resolvePromise(out, item);
                break;
            }
        }
        return JsValue::object(out);
    });
    addNative(vm, ctor, "any", NATIVE("Promise.any") {
        auto* out = vm.gc().newPromise();
        vm.initPromiseObject(out);
        JsValue iterable = ARG(0);
        if (!iterable.isObject() || iterable.asObject()->kind != ObjKind::Array) {
            vm.resolvePromise(out, iterable);
            return JsValue::object(out);
        }
        auto* source = iterable.asObject();
        uint32_t len = source->arrayLength();
        if (len == 0) {
            vm.rejectPromise(out, vm.makeError("AggregateError", "All promises were rejected"));
            return JsValue::object(out);
        }
        auto remaining = std::make_shared<uint32_t>(len);
        for (uint32_t i = 0; i < len; ++i) {
            JsValue item = source->arrayGet(i);
            if (item.isObject() && item.asObject()->kind == ObjKind::Promise) {
                auto fulfill = JsValue::object(vm.gc().newNativeFunction(
                    [out](VM& v, JsValue, std::vector<JsValue> a) -> JsValue {
                        v.resolvePromise(out, a.empty() ? JsValue::undefined() : a[0]);
                        return JsValue::undefined();
                    }, "Promise.any.fulfill"));
                auto reject = JsValue::object(vm.gc().newNativeFunction(
                    [out, remaining](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
                        if (*remaining > 0 && --(*remaining) == 0)
                            v.rejectPromise(out, v.makeError("AggregateError", "All promises were rejected"));
                        return JsValue::undefined();
                    }, "Promise.any.reject"));
                vm.promiseThen(item.asObject(), fulfill, reject);
            } else {
                vm.resolvePromise(out, item);
                break;
            }
        }
        return JsValue::object(out);
    });
    vm.setGlobal("Promise", JsValue::object(ctor));
}

// â”€â”€ console â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerConsole(VM& vm) {
    auto* console = vm.gc().newObject(ObjKind::Plain);
    auto logFn = [](const std::string& prefix) -> NativeFn {
        return [prefix](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
            std::string s = prefix;
            bool first = true;
            for (auto& a : args) { if (!first) s += " "; first = false; s += a.toString(); }
            fprintf(stderr, "%s",(s + "\n").c_str());
            return JsValue::undefined();
        };
    };
    addNative(vm, console, "log",   logFn("[JS] "));
    addNative(vm, console, "warn",  logFn("[JS WARN] "));
    addNative(vm, console, "error", logFn("[JS ERROR] "));
    addNative(vm, console, "info",  logFn("[JS INFO] "));
    addNative(vm, console, "debug", logFn("[JS DEBUG] "));
    addNative(vm, console, "assert", NATIVE("assert") {
        if (!ARG(0).toBool()) {
            std::string msg = "Assertion failed";
            if (args.size() > 1) msg += ": " + ARG_STR(1);
            fprintf(stderr, "%s",(msg + "\n").c_str());
        }
        return JsValue::undefined();
    });
    addNative(vm, console, "dir", logFn("[JS dir] "));
    addNative(vm, console, "trace", logFn("[JS trace] "));
    
    // console.table - simplified table output
    addNative(vm, console, "table", NATIVE("table") {
        if (args.empty()) return JsValue::undefined();
        fprintf(stderr, "[JS table]\n");
        if (ARG(0).isObject() && ARG(0).asObject()->kind == ObjKind::Array) {
            auto* arr = ARG(0).asObject();
            int len = arr->getProp("length").toInt32();
            for (int i = 0; i < len && i < 100; i++) {
                fprintf(stderr, "  %d: %s\n", i, arr->getProp(std::to_string(i)).toString().c_str());
            }
        } else if (ARG(0).isObject()) {
            auto* obj = ARG(0).asObject();
            for (auto& k : obj->ownEnumKeys()) {
                fprintf(stderr, "  %s: %s\n", k.c_str(), obj->getProp(k).toString().c_str());
            }
        } else {
            fprintf(stderr, "  %s\n", ARG_STR(0).c_str());
        }
        return JsValue::undefined();
    });
    
    // console.time / console.timeEnd - simple timing
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> timers;
    addNative(vm, console, "time", NATIVE("time") {
        std::string label = args.empty() ? "default" : ARG_STR(0);
        timers[label] = std::chrono::steady_clock::now();
        return JsValue::undefined();
    });
    addNative(vm, console, "timeEnd", NATIVE("timeEnd") {
        std::string label = args.empty() ? "default" : ARG_STR(0);
        auto it = timers.find(label);
        if (it != timers.end()) {
            auto duration = std::chrono::steady_clock::now() - it->second;
            double ms = std::chrono::duration<double, std::milli>(duration).count();
            fprintf(stderr, "[JS] %s: %.3fms\n", label.c_str(), ms);
            timers.erase(it);
        }
        return JsValue::undefined();
    });
    
    // console.count / console.countReset - count occurrences
    static std::unordered_map<std::string, int> counters;
    addNative(vm, console, "count", NATIVE("count") {
        std::string label = args.empty() ? "default" : ARG_STR(0);
        counters[label]++;
        fprintf(stderr, "[JS] %s: %d\n", label.c_str(), counters[label]);
        return JsValue::undefined();
    });
    addNative(vm, console, "countReset", NATIVE("countReset") {
        std::string label = args.empty() ? "default" : ARG_STR(0);
        counters[label] = 0;
        return JsValue::undefined();
    });
    
    // console.clear - clear console (just output newlines)
    addNative(vm, console, "clear", NATIVE("clear") {
        fprintf(stderr, "\n\n\n");
        return JsValue::undefined();
    });
    
    // console.group / console.groupEnd - group messages
    static int groupDepth = 0;
    addNative(vm, console, "group", NATIVE("group") {
        std::string label = args.empty() ? "" : ARG_STR(0);
        fprintf(stderr, "[JS group] %s\n", label.c_str());
        groupDepth++;
        return JsValue::undefined();
    });
    addNative(vm, console, "groupCollapsed", NATIVE("groupCollapsed") {
        std::string label = args.empty() ? "" : ARG_STR(0);
        fprintf(stderr, "[JS group] %s\n", label.c_str());
        groupDepth++;
        return JsValue::undefined();
    });
    addNative(vm, console, "groupEnd", NATIVE("groupEnd") {
        if (groupDepth > 0) groupDepth--;
        return JsValue::undefined();
    });
    
    vm.setGlobal("console", JsValue::object(console));
}

// â”€â”€ Timers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerTimers(VM& vm) {
    vm.setGlobal("setTimeout", JsValue::object(vm.gc().newNativeFunction(NATIVE("setTimeout") {
        if (!ARG(0).isCallable()) return JsValue::integer(0);
        int delay = args.size() > 1 ? std::max(0, ARG_INT(1)) : 0;
        std::vector<JsValue> cbArgs;
        for (size_t i = 2; i < args.size(); i++) cbArgs.push_back(args[i]);
        return JsValue::integer(vm.scheduleMacrotask(ARG(0), std::move(cbArgs), delay, false));
    }, "setTimeout")));

    vm.setGlobal("clearTimeout", JsValue::object(vm.gc().newNativeFunction(NATIVE("clearTimeout") {
        vm.cancelMacrotask(ARG_INT(0));
        return JsValue::undefined();
    }, "clearTimeout")));

    vm.setGlobal("setInterval", JsValue::object(vm.gc().newNativeFunction(NATIVE("setInterval") {
        if (!ARG(0).isCallable()) return JsValue::integer(0);
        int delay = args.size() > 1 ? std::max(1, ARG_INT(1)) : 1;
        std::vector<JsValue> cbArgs;
        for (size_t i = 2; i < args.size(); i++) cbArgs.push_back(args[i]);
        return JsValue::integer(vm.scheduleMacrotask(ARG(0), std::move(cbArgs), delay, true));
    }, "setInterval")));

    vm.setGlobal("clearInterval", JsValue::object(vm.gc().newNativeFunction(NATIVE("clearInterval") {
        vm.cancelMacrotask(ARG_INT(0));
        return JsValue::undefined();
    }, "clearInterval")));

    vm.setGlobal("queueMicrotask", JsValue::object(vm.gc().newNativeFunction(NATIVE("queueMicrotask") {
        if (ARG(0).isCallable()) vm.enqueueMicrotask(ARG(0), JsValue::undefined());
        return JsValue::undefined();
    }, "queueMicrotask")));
}

// â”€â”€ Global helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerGlobals(VM& vm) {
    vm.setGlobal("undefined", JsValue::undefined());
    vm.setGlobal("null",      JsValue::null());
    vm.setGlobal("NaN",       JsValue::number(std::nan("")));
    vm.setGlobal("Infinity",  JsValue::number(std::numeric_limits<double>::infinity()));
    vm.setGlobal("globalThis", JsValue::object(vm.globals()));
    vm.setGlobal("window",    JsValue::object(vm.globals()));
    vm.setGlobal("self",      JsValue::object(vm.globals()));

    vm.setGlobal("isNaN",    JsValue::object(vm.gc().newNativeFunction(NATIVE("isNaN") {
        return JsValue::boolean(std::isnan(ARG_NUM(0)));
    }, "isNaN")));
    vm.setGlobal("isFinite", JsValue::object(vm.gc().newNativeFunction(NATIVE("isFinite") {
        return JsValue::boolean(std::isfinite(ARG_NUM(0)));
    }, "isFinite")));
    vm.setGlobal("parseInt", JsValue::object(vm.gc().newNativeFunction(NATIVE("parseInt") {
        std::string s = ARG_STR(0);
        int base = args.size() > 1 ? ARG_INT(1) : 10;
        if (s.empty()) return JsValue::number(std::nan(""));
        try { return JsValue::integer((int32_t)std::stol(s, nullptr, base)); }
        catch (...) { return JsValue::number(std::nan("")); }
    }, "parseInt")));
    vm.setGlobal("parseFloat", JsValue::object(vm.gc().newNativeFunction(NATIVE("parseFloat") {
        try { return JsValue::number(std::stod(ARG_STR(0))); }
        catch (...) { return JsValue::number(std::nan("")); }
    }, "parseFloat")));
    vm.setGlobal("encodeURIComponent", JsValue::object(vm.gc().newNativeFunction(NATIVE("encodeURIComponent") {
        std::string s = ARG_STR(0), result;
        for (unsigned char c : s) {
            if (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='!'||c=='~'||c=='*'||c=='\''||c=='('||c==')') result += c;
            else { char buf[4]; snprintf(buf,4,"%%%02X",c); result += buf; }
        }
        return vm.str(result);
    }, "encodeURIComponent")));
    vm.setGlobal("decodeURIComponent", JsValue::object(vm.gc().newNativeFunction(NATIVE("decodeURIComponent") {
        std::string s = ARG_STR(0), result;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i]=='%' && i+2<s.size()) {
                char hex[3] = {s[i+1],s[i+2],0};
                result += (char)strtol(hex,nullptr,16); i+=2;
            } else result += s[i];
        }
        return vm.str(result);
    }, "decodeURIComponent")));
    vm.setGlobal("encodeURI", vm.getGlobal("encodeURIComponent"));
    vm.setGlobal("decodeURI", vm.getGlobal("decodeURIComponent"));
    vm.setGlobal("__dynamicImport", JsValue::object(vm.gc().newNativeFunction(NATIVE("dynamic_import") {
        auto* module = vm.gc().newObject(ObjKind::Plain);
        JsValue moduleVal = JsValue::object(module);
        vm.gc().addRoot(&moduleVal);
        module->setProp("default", JsValue::undefined());
        module->setProp("url", args.empty() ? vm.str("") : ARG(0));
        JsValue promise = vm.promiseResolve(moduleVal);
        vm.gc().removeRoot(&moduleVal);
        return promise;
    }, "import")));
    vm.setGlobal("structuredClone", JsValue::object(vm.gc().newNativeFunction(NATIVE("structuredClone") {
        return cloneJsValue(vm, ARG(0));
    }, "structuredClone")));

    // Symbol: represented as unique string atoms for now, with registry helpers
    // so common platform/library probes behave like the web surface.
    static int symbolCounter = 0;
    auto symbolDescription = [](const std::string& raw) -> std::string {
        const std::string prefix = "Symbol(";
        if (raw.rfind(prefix, 0) != 0) return "";
        size_t close = raw.find(')', prefix.size());
        if (close == std::string::npos) return "";
        return raw.substr(prefix.size(), close - prefix.size());
    };
    auto symbolDisplay = [symbolDescription](const std::string& raw) -> std::string {
        return "Symbol(" + symbolDescription(raw) + ")";
    };
    auto* symbolCtor = vm.gc().newNativeFunction([&](VM& v, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string desc = args.empty() || args[0].isUndefined() ? "" : args[0].toString();
        return v.str("Symbol(" + desc + ")_" + std::to_string(++symbolCounter));
    }, "Symbol");
    addNative(vm, symbolCtor, "for", [](VM& v, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        auto it = g_symbolRegistry.find(key);
        if (it == g_symbolRegistry.end())
            it = g_symbolRegistry.emplace(key, "Symbol(" + key + ")_registry").first;
        return v.str(it->second);
    });
    addNative(vm, symbolCtor, "keyFor", [](VM& v, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string raw = args.empty() ? "" : args[0].toString();
        for (const auto& [key, value] : g_symbolRegistry)
            if (value == raw) return v.str(key);
        const std::string prefix = "Symbol(";
        const std::string suffix = ")_registry";
        if (raw.rfind(prefix, 0) == 0 && raw.size() >= prefix.size() + suffix.size()
            && raw.compare(raw.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return v.str(raw.substr(prefix.size(), raw.size() - prefix.size() - suffix.size()));
        }
        return JsValue::undefined();
    });
    addNative(vm, symbolCtor, "description", [symbolDescription](VM& v, JsValue, std::vector<JsValue> args) -> JsValue {
        if (args.empty()) return JsValue::undefined();
        return v.str(symbolDescription(args[0].toString()));
    });
    addNative(vm, symbolCtor, "toString", [symbolDisplay](VM& v, JsValue, std::vector<JsValue> args) -> JsValue {
        if (args.empty()) return v.str("Symbol()");
        return v.str(symbolDisplay(args[0].toString()));
    });
    symbolCtor->setProp("iterator", vm.str("Symbol(Symbol.iterator)_registry"));
    vm.setGlobal("Symbol", JsValue::object(symbolCtor));

    // Error constructors
    auto makeErrCtor = [&](const char* name) {
        std::string n(name);
        auto* e = vm.gc().newNativeFunction([n](VM& vm2, JsValue, std::vector<JsValue> args) -> JsValue {
            return vm2.makeError(n, args.empty() ? "" : args[0].toString());
        }, name);
        vm.setGlobal(name, JsValue::object(e));
    };
    makeErrCtor("Error");
    makeErrCtor("TypeError");
    makeErrCtor("RangeError");
    makeErrCtor("ReferenceError");
    makeErrCtor("SyntaxError");
    makeErrCtor("URIError");
    makeErrCtor("EvalError");
    makeErrCtor("AggregateError");
}

// â”€â”€ Map / Set â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerMapSet(VM& vm) {
    // Map
    auto* mapCtor = vm.gc().newNativeFunction(NATIVE("Map") {
        auto* map = vm.gc().newObject(ObjKind::Map);
        auto syncSize = [map]() {
            map->setProp("size", JsValue::integer((int32_t)map->mapEntries.size()));
        };
        auto setEntry = [](JsObject* target, JsValue key, JsValue value) {
            auto& entries = target->mapEntries;
            auto it = std::find_if(entries.begin(), entries.end(),
                [&](const auto& entry) { return entry.first.strictEq(key); });
            if (it != entries.end()) it->second = value;
            else entries.push_back({ key, value });
        };
        if (ARG(0).isObject()) {
            auto* arr = ARG(0).asObject();
            for (uint32_t i = 0; i < arr->arrayLength(); i++) {
                JsValue pair = arr->arrayGet(i);
                if (pair.isObject()) {
                    setEntry(map, pair.asObject()->arrayGet(0), pair.asObject()->arrayGet(1));
                }
            }
        }
        syncSize();
        addNative(vm, map, "get", NATIVE("map_get") {
            if (!thisVal.isObject()) return JsValue::undefined();
            for (auto& entry : thisVal.asObject()->mapEntries)
                if (entry.first.strictEq(ARG(0))) return entry.second;
            return JsValue::undefined();
        });
        addNative(vm, map, "set", NATIVE("map_set") {
            if (!thisVal.isObject()) return thisVal;
            auto* target = thisVal.asObject();
            auto& entries = target->mapEntries;
            auto it = std::find_if(entries.begin(), entries.end(),
                [&](const auto& entry) { return entry.first.strictEq(ARG(0)); });
            if (it != entries.end()) it->second = ARG(1);
            else entries.push_back({ ARG(0), ARG(1) });
            target->setProp("size", JsValue::integer((int32_t)entries.size()));
            return thisVal;
        });
        addNative(vm, map, "has", NATIVE("map_has") {
            if (!thisVal.isObject()) return JsValue::boolean(false);
            for (auto& entry : thisVal.asObject()->mapEntries)
                if (entry.first.strictEq(ARG(0))) return JsValue::boolean(true);
            return JsValue::boolean(false);
        });
        addNative(vm, map, "delete", NATIVE("map_delete") {
            if (!thisVal.isObject()) return JsValue::boolean(false);
            auto* target = thisVal.asObject();
            auto& entries = target->mapEntries;
            auto before = entries.size();
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [&](const auto& entry) { return entry.first.strictEq(ARG(0)); }), entries.end());
            target->setProp("size", JsValue::integer((int32_t)entries.size()));
            return JsValue::boolean(entries.size() != before);
        });
        addNative(vm, map, "clear", NATIVE("map_clear") {
            if (thisVal.isObject()) {
                thisVal.asObject()->mapEntries.clear();
                thisVal.asObject()->setProp("size", JsValue::integer(0));
            }
            return JsValue::undefined();
        });
        addNative(vm, map, "keys", NATIVE("map_keys") {
            auto* a = vm.gc().newArray();
            if (thisVal.isObject()) for (auto& entry : thisVal.asObject()->mapEntries) a->arrayPush(entry.first);
            return JsValue::object(a);
        });
        addNative(vm, map, "values", NATIVE("map_values") {
            auto* a = vm.gc().newArray();
            if (thisVal.isObject()) for (auto& entry : thisVal.asObject()->mapEntries) a->arrayPush(entry.second);
            return JsValue::object(a);
        });
        addNative(vm, map, "entries", NATIVE("map_entries") {
            auto* a = vm.gc().newArray();
            JsValue aVal = JsValue::object(a); vm.gc().addRoot(&aVal);
            if (thisVal.isObject()) for (auto& entry : thisVal.asObject()->mapEntries) {
                auto* p = vm.gc().newArray();
                JsValue pVal = JsValue::object(p); vm.gc().addRoot(&pVal);
                p->arrayPush(entry.first);
                p->arrayPush(entry.second);
                a->arrayPush(pVal);
                vm.gc().removeRoot(&pVal);
            }
            vm.gc().removeRoot(&aVal);
            return aVal;
        });
        addNative(vm, map, "forEach", NATIVE("map_foreach") {
            if (thisVal.isObject() && ARG(0).isCallable())
                for (auto& entry : thisVal.asObject()->mapEntries)
                    vm.call(ARG(0), ARG(1), { entry.second, entry.first, thisVal });
            return JsValue::undefined();
        });
        return JsValue::object(map);
    }, "Map");
    vm.setGlobal("Map", JsValue::object(mapCtor));

    // Set
    auto* setCtor = vm.gc().newNativeFunction(NATIVE("Set") {
        auto* set = vm.gc().newObject(ObjKind::Set);
        auto addEntry = [](JsObject* target, JsValue value) {
            for (auto& entry : target->setEntries)
                if (entry.strictEq(value)) return;
            target->setEntries.push_back(value);
        };
        if (ARG(0).isObject()) {
            auto* arr = ARG(0).asObject();
            for (uint32_t i = 0; i < arr->arrayLength(); i++)
                addEntry(set, arr->arrayGet(i));
        }
        set->setProp("size", JsValue::integer((int32_t)set->setEntries.size()));
        addNative(vm, set, "add", NATIVE("set_add") {
            if (thisVal.isObject()) {
                auto* target = thisVal.asObject();
                bool exists = false;
                for (auto& entry : target->setEntries)
                    if (entry.strictEq(ARG(0))) { exists = true; break; }
                if (!exists) target->setEntries.push_back(ARG(0));
                target->setProp("size", JsValue::integer((int32_t)target->setEntries.size()));
            }
            return thisVal;
        });
        addNative(vm, set, "has", NATIVE("set_has") {
            if (!thisVal.isObject()) return JsValue::boolean(false);
            for (auto& entry : thisVal.asObject()->setEntries)
                if (entry.strictEq(ARG(0))) return JsValue::boolean(true);
            return JsValue::boolean(false);
        });
        addNative(vm, set, "delete", NATIVE("set_delete") {
            if (!thisVal.isObject()) return JsValue::boolean(false);
            auto* target = thisVal.asObject();
            auto& entries = target->setEntries;
            auto before = entries.size();
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [&](const JsValue& entry) { return entry.strictEq(ARG(0)); }), entries.end());
            target->setProp("size", JsValue::integer((int32_t)entries.size()));
            return JsValue::boolean(entries.size() != before);
        });
        addNative(vm, set, "clear", NATIVE("set_clear") {
            if (thisVal.isObject()) {
                thisVal.asObject()->setEntries.clear();
                thisVal.asObject()->setProp("size", JsValue::integer(0));
            }
            return JsValue::undefined();
        });
        addNative(vm, set, "forEach", NATIVE("set_foreach") {
            if (thisVal.isObject() && ARG(0).isCallable())
                for (auto& value : thisVal.asObject()->setEntries)
                    vm.call(ARG(0), ARG(1), { value, value, thisVal });
            return JsValue::undefined();
        });
        addNative(vm, set, "values", NATIVE("set_values") {
            auto* a = vm.gc().newArray();
            if (thisVal.isObject()) for (auto& value : thisVal.asObject()->setEntries) a->arrayPush(value);
            return JsValue::object(a);
        });
        addNative(vm, set, "keys", NATIVE("set_keys") {
            JsValue values = thisVal.isObject() ? thisVal.asObject()->getProp("values") : JsValue::undefined();
            return values.isCallable() ? vm.call(values, thisVal, {}) : JsValue::object(vm.gc().newArray());
        });
        addNative(vm, set, "entries", NATIVE("set_entries") {
            auto* a = vm.gc().newArray();
            JsValue aVal = JsValue::object(a); vm.gc().addRoot(&aVal);
            if (thisVal.isObject()) for (auto& value : thisVal.asObject()->setEntries) {
                auto* p = vm.gc().newArray();
                JsValue pVal = JsValue::object(p); vm.gc().addRoot(&pVal);
                p->arrayPush(value);
                p->arrayPush(value);
                a->arrayPush(pVal);
                vm.gc().removeRoot(&pVal);
            }
            vm.gc().removeRoot(&aVal);
            return aVal;
        });
        return JsValue::object(set);
    }, "Set");
    vm.setGlobal("Set", JsValue::object(setCtor));

    auto* weakMapCtor = vm.gc().newNativeFunction(NATIVE("WeakMap") {
        auto* map = vm.gc().newObject(ObjKind::Map);
        if (ARG(0).isObject()) {
            auto* arr = ARG(0).asObject();
            for (uint32_t i = 0; i < arr->arrayLength(); i++) {
                JsValue pair = arr->arrayGet(i);
                if (!pair.isObject()) continue;
                JsValue key = pair.asObject()->arrayGet(0);
                if (!key.isObject()) continue;
                JsValue value = pair.asObject()->arrayGet(1);
                auto it = std::find_if(map->mapEntries.begin(), map->mapEntries.end(),
                    [&](const auto& entry) { return entry.first.strictEq(key); });
                if (it != map->mapEntries.end()) it->second = value;
                else map->mapEntries.push_back({ key, value });
            }
        }
        addNative(vm, map, "get", NATIVE("weakmap_get") {
            if (!thisVal.isObject()) return JsValue::undefined();
            for (auto& entry : thisVal.asObject()->mapEntries)
                if (entry.first.strictEq(ARG(0))) return entry.second;
            return JsValue::undefined();
        });
        addNative(vm, map, "set", NATIVE("weakmap_set") {
            if (!thisVal.isObject() || !ARG(0).isObject()) return thisVal;
            auto& entries = thisVal.asObject()->mapEntries;
            auto it = std::find_if(entries.begin(), entries.end(),
                [&](const auto& entry) { return entry.first.strictEq(ARG(0)); });
            if (it != entries.end()) it->second = ARG(1);
            else entries.push_back({ ARG(0), ARG(1) });
            return thisVal;
        });
        addNative(vm, map, "has", NATIVE("weakmap_has") {
            if (!thisVal.isObject()) return JsValue::boolean(false);
            for (auto& entry : thisVal.asObject()->mapEntries)
                if (entry.first.strictEq(ARG(0))) return JsValue::boolean(true);
            return JsValue::boolean(false);
        });
        addNative(vm, map, "delete", NATIVE("weakmap_delete") {
            if (!thisVal.isObject()) return JsValue::boolean(false);
            auto& entries = thisVal.asObject()->mapEntries;
            auto before = entries.size();
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [&](const auto& entry) { return entry.first.strictEq(ARG(0)); }), entries.end());
            return JsValue::boolean(entries.size() != before);
        });
        return JsValue::object(map);
    }, "WeakMap");
    vm.setGlobal("WeakMap", JsValue::object(weakMapCtor));

    auto* weakSetCtor = vm.gc().newNativeFunction(NATIVE("WeakSet") {
        auto* set = vm.gc().newObject(ObjKind::Set);
        if (ARG(0).isObject()) {
            auto* arr = ARG(0).asObject();
            for (uint32_t i = 0; i < arr->arrayLength(); i++) {
                JsValue value = arr->arrayGet(i);
                if (!value.isObject()) continue;
                bool exists = false;
                for (auto& entry : set->setEntries) {
                    if (entry.strictEq(value)) { exists = true; break; }
                }
                if (!exists) set->setEntries.push_back(value);
            }
        }
        addNative(vm, set, "add", NATIVE("weakset_add") {
            if (!thisVal.isObject() || !ARG(0).isObject()) return thisVal;
            auto& entries = thisVal.asObject()->setEntries;
            for (auto& entry : entries)
                if (entry.strictEq(ARG(0))) return thisVal;
            entries.push_back(ARG(0));
            return thisVal;
        });
        addNative(vm, set, "has", NATIVE("weakset_has") {
            if (!thisVal.isObject()) return JsValue::boolean(false);
            for (auto& entry : thisVal.asObject()->setEntries)
                if (entry.strictEq(ARG(0))) return JsValue::boolean(true);
            return JsValue::boolean(false);
        });
        addNative(vm, set, "delete", NATIVE("weakset_delete") {
            if (!thisVal.isObject()) return JsValue::boolean(false);
            auto& entries = thisVal.asObject()->setEntries;
            auto before = entries.size();
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [&](const JsValue& entry) { return entry.strictEq(ARG(0)); }), entries.end());
            return JsValue::boolean(entries.size() != before);
        });
        return JsValue::object(set);
    }, "WeakSet");
    vm.setGlobal("WeakSet", JsValue::object(weakSetCtor));

    auto* weakRefCtor = vm.gc().newNativeFunction(NATIVE("WeakRef") {
        auto* ref = vm.gc().newObject(ObjKind::Plain);
        ref->setProp("__target__", ARG(0).isObject() ? ARG(0) : JsValue::undefined());
        addNative(vm, ref, "deref", NATIVE("weakref_deref") {
            return thisVal.isObject() ? thisVal.asObject()->getProp("__target__") : JsValue::undefined();
        });
        return JsValue::object(ref);
    }, "WeakRef");
    vm.setGlobal("WeakRef", JsValue::object(weakRefCtor));
}

// â”€â”€ Date â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static double currentTimeMs() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static bool parseIsoDateMs(const std::string& s, double& out) {
    std::smatch m;
    static const std::regex iso(R"(^(\d{4})-(\d{2})-(\d{2})(?:[Tt ](\d{2}):(\d{2})(?::(\d{2})(?:\.(\d{1,3}))?)?(?:[Zz])?)?$)");
    if (!std::regex_match(s, m, iso)) return false;
    std::tm tm{};
    tm.tm_year = std::stoi(m[1].str()) - 1900;
    tm.tm_mon = std::stoi(m[2].str()) - 1;
    tm.tm_mday = std::stoi(m[3].str());
    tm.tm_hour = m[4].matched ? std::stoi(m[4].str()) : 0;
    tm.tm_min = m[5].matched ? std::stoi(m[5].str()) : 0;
    tm.tm_sec = m[6].matched ? std::stoi(m[6].str()) : 0;
    int ms = 0;
    if (m[7].matched) {
        std::string frac = m[7].str();
        while (frac.size() < 3) frac += '0';
        ms = std::stoi(frac.substr(0, 3));
    }
#if defined(_WIN32)
    __time64_t seconds = _mkgmtime64(&tm);
#else
    time_t seconds = timegm(&tm);
#endif
    if (seconds < 0) return false;
    out = (double)seconds * 1000.0 + (double)ms;
    return true;
}

static std::string isoFromMs(double msValue) {
    int64_t totalMs = (int64_t)std::llround(msValue);
    time_t seconds = (time_t)(totalMs / 1000);
    int millis = (int)(totalMs % 1000);
    if (millis < 0) millis += 1000;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
    return buf;
}

static double dateArgToMs(const std::vector<JsValue>& args) {
    if (args.empty() || args[0].isUndefined()) return currentTimeMs();
    if (args[0].isString()) {
        double parsed = 0;
        if (parseIsoDateMs(args[0].toString(), parsed)) return parsed;
        return std::nan("");
    }
    return args[0].toNumber();
}

static void registerDate(VM& vm) {
    auto* dateCtor = vm.gc().newNativeFunction(NATIVE("Date") {
        auto* d = vm.gc().newObject(ObjKind::Plain);
        double ms = dateArgToMs(args);
        d->setProp("__time__", JsValue::number(ms));
        addNative(vm, d, "getTime",         NATIVE("getTime")     { return thisVal.isObject()?thisVal.asObject()->getProp("__time__"):JsValue::number(0); });
        addNative(vm, d, "toISOString",     NATIVE("toISOString") {
            double t = thisVal.isObject() ? thisVal.asObject()->getProp("__time__").toNumber() : 0;
            return vm.str(isoFromMs(t));
        });
        addNative(vm, d, "toLocaleDateString", NATIVE("toLocaleDateString") {
            double t = thisVal.isObject() ? thisVal.asObject()->getProp("__time__").toNumber() : 0;
            std::string iso = isoFromMs(t);
            return vm.str(iso.substr(5, 2) + "/" + iso.substr(8, 2) + "/" + iso.substr(0, 4));
        });
        addNative(vm, d, "toString",        NATIVE("date_toString"){
            double t = thisVal.isObject() ? thisVal.asObject()->getProp("__time__").toNumber() : 0;
            return vm.str(isoFromMs(t));
        });
        addNative(vm, d, "valueOf",         NATIVE("date_valueOf") { return thisVal.isObject()?thisVal.asObject()->getProp("__time__"):JsValue::number(0); });
        return JsValue::object(d);
    }, "Date");

    addNative(vm, dateCtor, "now", NATIVE("Date.now") {
        return JsValue::number(currentTimeMs());
    });
    addNative(vm, dateCtor, "parse", NATIVE("Date.parse") {
        double parsed = 0;
        return parseIsoDateMs(ARG_STR(0), parsed) ? JsValue::number(parsed) : JsValue::number(std::nan(""));
    });
    vm.setGlobal("Date", JsValue::object(dateCtor));
}

// â”€â”€ RegExp â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void registerRegExp(VM& vm) {
    auto* ctor = vm.gc().newNativeFunction(NATIVE("RegExp") {
        auto* re = vm.gc().newObject(ObjKind::RegExp);
        re->setProp("source",    ARG(0).isUndefined() ? vm.str("") : ARG(0));
        re->setProp("flags",     ARG(1).isUndefined() ? vm.str("") : ARG(1));
        re->setProp("lastIndex", JsValue::integer(0));
        re->setProp("global",    JsValue::boolean(ARG_STR(1).find('g') != std::string::npos));
        addNative(vm, re, "test", NATIVE("test") {
            if (!thisVal.isObject()) return JsValue::boolean(false);
            std::string pattern = thisVal.asObject()->getProp("source").toString();
            std::string flags   = thisVal.asObject()->getProp("flags").toString();
            std::string str = ARG_STR(0);
            try {
                auto rxFlags = std::regex_constants::ECMAScript;
                if (flags.find('i') != std::string::npos) rxFlags |= std::regex_constants::icase;
                std::regex rx(pattern, rxFlags);
                return JsValue::boolean(std::regex_search(str, rx));
            } catch (...) {
                return JsValue::boolean(str.find(pattern) != std::string::npos);
            }
        });
        addNative(vm, re, "exec", NATIVE("exec") {
            if (!thisVal.isObject()) return JsValue::null();
            std::string pattern = thisVal.asObject()->getProp("source").toString();
            std::string flags   = thisVal.asObject()->getProp("flags").toString();
            std::string str = ARG_STR(0);
            try {
                auto rxFlags = std::regex_constants::ECMAScript;
                if (flags.find('i') != std::string::npos) rxFlags |= std::regex_constants::icase;
                std::regex rx(pattern, rxFlags);
                std::smatch m;
                if (!std::regex_search(str, m, rx)) return JsValue::null();
                auto* result = vm.gc().newArray();
                for (size_t i = 0; i < m.size(); ++i) result->arrayPush(vm.str(m[i].str()));
                result->setProp("index", JsValue::integer((int32_t)m.position(0)));
                result->setProp("input", vm.str(str));
                return JsValue::object(result);
            } catch (...) {
                return JsValue::null();
            }
        });
        addNative(vm, re, "toString", NATIVE("re_toString") {
            if (!thisVal.isObject()) return vm.str("/(?:)/");
            return vm.str("/" + thisVal.asObject()->getProp("source").toString() + "/" + thisVal.asObject()->getProp("flags").toString());
        });
        return JsValue::object(re);
    }, "RegExp");
    vm.setGlobal("RegExp", JsValue::object(ctor));
}

// â”€â”€ Top-level entry â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

namespace {

struct WasmType { std::vector<uint8_t> params; std::vector<uint8_t> results; };
struct WasmFunc { uint32_t type = 0; std::vector<uint8_t> locals; std::vector<uint8_t> code; };
struct WasmModuleData { std::vector<WasmType> types; std::vector<WasmFunc> funcs; std::unordered_map<std::string, uint32_t> exports; };
static std::unordered_map<JsObject*, std::shared_ptr<WasmModuleData>> g_wasmModules;

struct WasmReader {
    const std::vector<uint8_t>& b; size_t p = 0; bool ok = true;
    bool byte(uint8_t& out) { if (p >= b.size()) { ok = false; return false; } out = b[p++]; return true; }
    bool bytes(size_t n, const uint8_t*& out) { if (p + n > b.size()) { ok = false; return false; } out = b.data() + p; p += n; return true; }
    bool u32(uint32_t& out) { out = 0; int shift = 0; for (int i = 0; i < 5; ++i) { uint8_t c = 0; if (!byte(c)) return false; out |= (uint32_t)(c & 0x7f) << shift; if (!(c & 0x80)) return true; shift += 7; } ok = false; return false; }
    bool i32(int32_t& out) { uint32_t value = 0; int shift = 0; uint8_t c = 0; for (int i = 0; i < 5; ++i) { if (!byte(c)) return false; value |= (uint32_t)(c & 0x7f) << shift; shift += 7; if (!(c & 0x80)) { if (shift < 32 && (c & 0x40)) value |= (~0u << shift); out = (int32_t)value; return true; } } ok = false; return false; }
};

static bool readWasmName(WasmReader& r, std::string& out) {
    uint32_t len = 0; const uint8_t* p = nullptr;
    if (!r.u32(len) || len > 1024 * 1024 || !r.bytes(len, p)) return false;
    out.assign((const char*)p, (size_t)len);
    return true;
}

static std::vector<uint8_t> wasmBytesFromJs(JsValue v) {
    std::vector<uint8_t> out;
    if (v.isString()) { const std::string& s = v.asString()->value; out.assign(s.begin(), s.end()); return out; }
    if (!v.isObject()) return out;
    JsObject* o = v.asObject();
    uint32_t len = o->kind == ObjKind::Array ? o->arrayLength() : (uint32_t)std::max(0, o->getProp("length").toInt32());
    if (len > 16 * 1024 * 1024) return {};
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) out.push_back((uint8_t)(o->arrayGet(i).toInt32() & 255));
    return out;
}

static std::shared_ptr<WasmModuleData> parseWasmModule(const std::vector<uint8_t>& bytes, std::string& error) {
    if (bytes.size() < 8 || bytes[0] != 0x00 || bytes[1] != 0x61 || bytes[2] != 0x73 || bytes[3] != 0x6d || bytes[4] != 1 || bytes[5] || bytes[6] || bytes[7]) { error = "bad wasm header"; return nullptr; }
    auto module = std::make_shared<WasmModuleData>();
    WasmReader r{bytes, 8, true};
    std::vector<uint32_t> funcTypes;
    while (r.p < bytes.size() && r.ok) {
        uint8_t id = 0; uint32_t size = 0;
        if (!r.byte(id) || !r.u32(size) || r.p + size > bytes.size()) break;
        size_t end = r.p + size;
        std::vector<uint8_t> sec(bytes.begin() + (ptrdiff_t)r.p, bytes.begin() + (ptrdiff_t)end);
        WasmReader s{sec, 0, true}; r.p = end;
        if (id == 1) {
            uint32_t count = 0; if (!s.u32(count) || count > 10000) { error = "bad type section"; return nullptr; }
            for (uint32_t i = 0; i < count; ++i) {
                uint8_t form = 0; uint32_t n = 0; WasmType t;
                if (!s.byte(form) || form != 0x60 || !s.u32(n) || n > 64) { error = "bad function type"; return nullptr; }
                for (uint32_t j = 0; j < n; ++j) { uint8_t vt = 0; if (!s.byte(vt) || vt != 0x7f) { error = "only i32 params supported"; return nullptr; } t.params.push_back(vt); }
                if (!s.u32(n) || n > 1) { error = "only 0/1 result supported"; return nullptr; }
                for (uint32_t j = 0; j < n; ++j) { uint8_t vt = 0; if (!s.byte(vt) || vt != 0x7f) { error = "only i32 results supported"; return nullptr; } t.results.push_back(vt); }
                module->types.push_back(std::move(t));
            }
        } else if (id == 3) {
            uint32_t count = 0; if (!s.u32(count) || count > 10000) { error = "bad function section"; return nullptr; }
            funcTypes.resize(count);
            for (uint32_t& t : funcTypes) if (!s.u32(t) || t >= module->types.size()) { error = "bad function type index"; return nullptr; }
        } else if (id == 7) {
            uint32_t count = 0; if (!s.u32(count) || count > 10000) { error = "bad export section"; return nullptr; }
            for (uint32_t i = 0; i < count; ++i) { std::string name; uint8_t kind = 0; uint32_t index = 0; if (!readWasmName(s, name) || !s.byte(kind) || !s.u32(index)) { error = "bad export"; return nullptr; } if (kind == 0) module->exports[name] = index; }
        } else if (id == 10) {
            uint32_t count = 0; if (!s.u32(count) || count != funcTypes.size()) { error = "bad code section"; return nullptr; }
            module->funcs.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t bodySize = 0; if (!s.u32(bodySize) || s.p + bodySize > sec.size()) { error = "bad code body"; return nullptr; }
                size_t bodyEnd = s.p + bodySize; uint32_t localGroups = 0; if (!s.u32(localGroups) || localGroups > 1024) { error = "bad locals"; return nullptr; }
                WasmFunc f; f.type = funcTypes[i];
                for (uint32_t g = 0; g < localGroups; ++g) { uint32_t n = 0; uint8_t vt = 0; if (!s.u32(n) || !s.byte(vt) || vt != 0x7f || f.locals.size() + n > 65536) { error = "only i32 locals supported"; return nullptr; } f.locals.insert(f.locals.end(), n, vt); }
                f.code.assign(sec.begin() + (ptrdiff_t)s.p, sec.begin() + (ptrdiff_t)bodyEnd); s.p = bodyEnd; module->funcs[i] = std::move(f);
            }
        }
        if (!s.ok) { error = "malformed wasm section"; return nullptr; }
    }
    if (!r.ok || module->types.empty() || module->funcs.empty()) { error = "incomplete wasm module"; return nullptr; }
    for (const auto& e : module->exports) if (e.second >= module->funcs.size()) { error = "bad export index"; return nullptr; }
    return module;
}

static int32_t runWasmFunction(const WasmModuleData& module, uint32_t funcIndex, const std::vector<JsValue>& args) {
    if (funcIndex >= module.funcs.size()) throw std::runtime_error("wasm bad function index");
    const WasmFunc& f = module.funcs[funcIndex]; const WasmType& t = module.types[f.type];
    std::vector<int32_t> locals; locals.reserve(t.params.size() + f.locals.size());
    for (size_t i = 0; i < t.params.size(); ++i) locals.push_back(i < args.size() ? args[i].toInt32() : 0);
    locals.insert(locals.end(), f.locals.size(), 0);
    std::vector<int32_t> stack; WasmReader r{f.code, 0, true};
    auto pop = [&]() -> int32_t { if (stack.empty()) throw std::runtime_error("wasm stack underflow"); int32_t v = stack.back(); stack.pop_back(); return v; };
    while (r.p < f.code.size()) {
        uint8_t op = 0; if (!r.byte(op)) break;
        if (op == 0x0b) break;
        if (op == 0x0f) return stack.empty() ? 0 : stack.back();
        if (op == 0x10) {
            uint32_t callee = 0;
            if (!r.u32(callee) || callee >= module.funcs.size()) throw std::runtime_error("wasm bad call");
            const WasmType& ct = module.types[module.funcs[callee].type];
            std::vector<JsValue> callArgs(ct.params.size());
            for (size_t i = ct.params.size(); i > 0; --i) callArgs[i - 1] = JsValue::integer(pop());
            int32_t ret = runWasmFunction(module, callee, callArgs);
            if (!ct.results.empty()) stack.push_back(ret);
        }
        else if (op == 0x20) { uint32_t i = 0; if (!r.u32(i) || i >= locals.size()) throw std::runtime_error("wasm bad local.get"); stack.push_back(locals[i]); }
        else if (op == 0x21) { uint32_t i = 0; if (!r.u32(i) || i >= locals.size()) throw std::runtime_error("wasm bad local.set"); locals[i] = pop(); }
        else if (op == 0x22) { uint32_t i = 0; if (!r.u32(i) || i >= locals.size()) throw std::runtime_error("wasm bad local.tee"); locals[i] = stack.empty() ? 0 : stack.back(); }
        else if (op == 0x41) { int32_t v = 0; if (!r.i32(v)) throw std::runtime_error("wasm bad i32.const"); stack.push_back(v); }
        else if (op == 0x45) stack.push_back(pop() == 0);
        else if (op == 0x46) { int32_t b = pop(), a = pop(); stack.push_back(a == b); }
        else if (op == 0x47) { int32_t b = pop(), a = pop(); stack.push_back(a != b); }
        else if (op == 0x48) { int32_t b = pop(), a = pop(); stack.push_back(a < b); }
        else if (op == 0x4a) { int32_t b = pop(), a = pop(); stack.push_back(a > b); }
        else if (op == 0x4c) { int32_t b = pop(), a = pop(); stack.push_back(a <= b); }
        else if (op == 0x4e) { int32_t b = pop(), a = pop(); stack.push_back(a >= b); }
        else if (op == 0x6a) { int32_t b = pop(), a = pop(); stack.push_back(a + b); }
        else if (op == 0x6b) { int32_t b = pop(), a = pop(); stack.push_back(a - b); }
        else if (op == 0x6c) { int32_t b = pop(), a = pop(); stack.push_back(a * b); }
        else if (op == 0x6d) { int32_t b = pop(), a = pop(); if (!b) throw std::runtime_error("wasm divide by zero"); stack.push_back(a / b); }
        else if (op == 0x6f) { int32_t b = pop(), a = pop(); if (!b) throw std::runtime_error("wasm divide by zero"); stack.push_back(a % b); }
        else if (op == 0x71) { int32_t b = pop(), a = pop(); stack.push_back(a & b); }
        else if (op == 0x72) { int32_t b = pop(), a = pop(); stack.push_back(a | b); }
        else if (op == 0x73) { int32_t b = pop(), a = pop(); stack.push_back(a ^ b); }
        else if (op == 0x74) { int32_t b = pop(), a = pop(); stack.push_back(a << (b & 31)); }
        else if (op == 0x75) { int32_t b = pop(), a = pop(); stack.push_back(a >> (b & 31)); }
        else if (op == 0x76) { int32_t b = pop(); uint32_t a = (uint32_t)pop(); stack.push_back((int32_t)(a >> (b & 31))); }
        else throw std::runtime_error("unsupported wasm opcode");
        if (stack.size() > 1024 * 1024) throw std::runtime_error("wasm stack limit");
    }
    if (!r.ok) throw std::runtime_error("truncated wasm code");
    return stack.empty() ? 0 : stack.back();
}

static JsObject* makeWasmModuleObject(VM& vm, std::shared_ptr<WasmModuleData> module) { auto* obj = vm.gc().newObject(ObjKind::Plain); g_wasmModules[obj] = std::move(module); return obj; }

static JsObject* makeWasmInstanceObject(VM& vm, std::shared_ptr<WasmModuleData> module) {
    auto* inst = vm.gc().newObject(ObjKind::Plain); JsValue instVal = JsValue::object(inst); vm.gc().addRoot(&instVal);
    auto* exports = vm.gc().newObject(ObjKind::Plain); JsValue exportsVal = JsValue::object(exports); vm.gc().addRoot(&exportsVal);
    for (const auto& it : module->exports) {
        std::string name = it.first; uint32_t index = it.second;
        auto* fn = vm.gc().newNativeFunction([module, index](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            try { return JsValue::integer(runWasmFunction(*module, index, args)); }
            catch (const std::exception& e) { vm.throwError("RuntimeError", e.what()); }
            return JsValue::undefined();
        }, name);
        exports->setProp(name, JsValue::object(fn));
    }
    inst->setProp("exports", exportsVal); vm.gc().removeRoot(&exportsVal); vm.gc().removeRoot(&instVal); return inst;
}

static void registerWebAssembly(VM& vm) {
    auto* wasm = vm.gc().newObject(ObjKind::Plain);
    auto* moduleCtor = vm.gc().newNativeFunction(NATIVE("WebAssembly.Module") {
        std::string error; auto module = parseWasmModule(wasmBytesFromJs(ARG(0)), error);
        if (!module) vm.throwError("CompileError", error);
        return JsValue::object(makeWasmModuleObject(vm, module));
    }, "Module");
    auto* instanceCtor = vm.gc().newNativeFunction(NATIVE("WebAssembly.Instance") {
        if (!ARG(0).isObject() || !g_wasmModules.count(ARG(0).asObject())) vm.throwTypeError("WebAssembly.Instance needs Module");
        return JsValue::object(makeWasmInstanceObject(vm, g_wasmModules[ARG(0).asObject()]));
    }, "Instance");
    wasm->setProp("Module", JsValue::object(moduleCtor));
    wasm->setProp("Instance", JsValue::object(instanceCtor));
    addNative(vm, wasm, "validate", NATIVE("WebAssembly.validate") { std::string error; return JsValue::boolean((bool)parseWasmModule(wasmBytesFromJs(ARG(0)), error)); });
    addNative(vm, wasm, "instantiate", NATIVE("WebAssembly.instantiate") {
        std::shared_ptr<WasmModuleData> module;
        if (ARG(0).isObject() && g_wasmModules.count(ARG(0).asObject())) module = g_wasmModules[ARG(0).asObject()];
        else { std::string error; module = parseWasmModule(wasmBytesFromJs(ARG(0)), error); if (!module) vm.throwError("CompileError", error); }
        return JsValue::object(makeWasmInstanceObject(vm, module));
    });
    vm.setGlobal("WebAssembly", JsValue::object(wasm));
}

} // namespace

void registerBuiltins(VM& vm) {
    registerGlobals(vm);
    registerObject(vm);
    registerArray(vm);
    registerString(vm);
    registerNumber(vm);
    registerBoolean(vm);
    registerMath(vm);
    registerJSON(vm);
    registerPromise(vm);
    registerConsole(vm);
    registerTimers(vm);
    registerMapSet(vm);
    registerDate(vm);
    registerRegExp(vm);
    registerWebAssembly(vm);
    
    // Crypto API - crypto.randomUUID() and crypto.getRandomValues()
    auto* crypto = vm.gc().newObject(ObjKind::Plain);
    addNative(vm, crypto, "randomUUID", NATIVE("randomUUID") {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> hex(0, 15);
        static std::uniform_int_distribution<> variant(8, 11);
        auto hexChar = [](int n) { return "0123456789abcdef"[n]; };
        std::string uuid;
        uuid.reserve(36);
        for (int i = 0; i < 8; i++) uuid += hexChar(hex(gen));
        uuid += '-';
        for (int i = 0; i < 4; i++) uuid += hexChar(hex(gen));
        uuid += '-';
        uuid += '4';
        for (int i = 0; i < 3; i++) uuid += hexChar(hex(gen));
        uuid += '-';
        uuid += hexChar(variant(gen));
        for (int i = 0; i < 3; i++) uuid += hexChar(hex(gen));
        uuid += '-';
        for (int i = 0; i < 12; i++) uuid += hexChar(hex(gen));
        return vm.str(uuid);
    });
    addNative(vm, crypto, "getRandomValues", NATIVE("getRandomValues") {
        if (!ARG(0).isObject() || ARG(0).asObject()->kind != ObjKind::Array) return ARG(0);
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> byte(0, 255);
        auto* arr = ARG(0).asObject();
        int len = arr->getProp("length").toInt32();
        for (int i = 0; i < len && i < 65536; i++)
            arr->setProp(std::to_string(i), JsValue::integer(byte(gen)));
        return ARG(0);
    });
    vm.setGlobal("crypto", JsValue::object(crypto));
}
