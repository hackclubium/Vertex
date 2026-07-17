#include "js/vm.h"
#include "js/dom_bridge.h"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <sstream>
#include <stdexcept>
#include <chrono>

static long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Maximum wall-clock time a single top-level script run may take.
static constexpr long long kScriptBudgetMs = 4000;

VM::VM(GC& gc) : m_gc(gc) {
    m_globals = m_gc.newObject(ObjKind::Plain);
    m_globalsRoot = JsValue::object(m_globals);
    m_gc.addRoot(&m_globalsRoot);
    m_gc.markExtraRoots = [this](GC& gc) {
        for (const auto& task : m_macrotasks) {
            gc.markValue(task.fn);
            for (const auto& arg : task.args) gc.markValue(arg);
        }
        for (const auto& task : m_microtasks) {
            gc.markValue(task.fn);
            gc.markValue(task.arg);
        }
        markDomEventListenerRoots(gc);
    };
    m_stack.reserve(1024);
    m_frames.reserve(256);
}

VM::~VM() {
    m_gc.markExtraRoots = nullptr;
    m_gc.removeRoot(&m_globalsRoot);
}

// ── Constant resolution ────────────────────────────────────────────────────────

JsValue VM::resolveConst(BytecodeFunction* fn, uint16_t idx) {
    if (idx >= fn->consts.size()) return JsValue::undefined();
    // String constants are stored as null placeholders with a parallel marker,
    // so an actual null constant and an empty string literal stay distinct.
    // Re-intern on every load instead of caching the resolved JsString* back
    // into consts[idx]: BytecodeFunction::consts outlives any single call and
    // is not a GC root, so a cached pointer would dangle once the collector
    // actually frees unreachable strings.
    if (idx < fn->constStrings.size()
        && idx < fn->constIsString.size() && fn->constIsString[idx])
        return JsValue::string(m_gc.internString(fn->constStrings[idx]));
    return fn->consts[idx];
}

// ── Global access ─────────────────────────────────────────────────────────────

void VM::setGlobal(const std::string& name, JsValue val) {
    m_globals->setProp(name, val);
}

JsValue VM::getGlobal(const std::string& name) {
    return m_globals->getProp(name);
}

// ── Error helpers ─────────────────────────────────────────────────────────────

JsValue VM::makeError(const std::string& type, const std::string& msg) {
    return JsValue::object(m_gc.newError(type, msg));
}

// ── Property access ───────────────────────────────────────────────────────────

JsValue VM::getProp(JsValue obj, const std::string& key) {
    if (obj.isObject()) {
        auto* o = obj.asObject();
        // Live DOM reads (className/id/value/…) win over the stored snapshot.
        if (o->domNode && onDomPropGet) {
            JsValue out;
            if (onDomPropGet(o, key, out)) return out;
        }
        // Check for getter defined via Object.defineProperty
        JsValue getter = o->getProp("__get_" + key + "__");
        if (getter.isCallable()) {
            try { return call(getter, obj, {}); } catch (...) { return JsValue::undefined(); }
        }
        // Check own + proto chain via JsObject::getProp
        JsValue v = o->getProp(key);
        if (!v.isUndefined()) return v;
        // Check prototype methods for arrays/strings/etc
        return v;
    }
    if (obj.isString()) {
        JsString* strObj = obj.asString();
        if (!strObj) return JsValue::undefined();
        // String properties
        if (key == "length") return JsValue::integer((int32_t)strObj->value.size());
        // Check String.prototype
        JsValue strProto = m_globals->getProp("String");
        if (strProto.isObject()) {
            JsValue sp = strProto.asObject()->getProp("prototype");
            if (sp.isObject()) {
                JsValue method = sp.asObject()->getProp(key);
                if (!method.isUndefined()) return method;
            }
        }
        // Integer index
        bool allDigit = !key.empty();
        for (char c : key) if (c < '0' || c > '9') { allDigit = false; break; }
        if (allDigit) {
            uint32_t idx = (uint32_t)std::stoul(key);
            const auto& s = strObj->value;
            if (idx < s.size()) return str(std::string(1, s[idx]));
        }
        return JsValue::undefined();
    }
    if (obj.isNumber()) {
        JsValue numProto = m_globals->getProp("Number");
        if (numProto.isObject()) {
            JsValue np = numProto.asObject()->getProp("prototype");
            if (np.isObject()) return np.asObject()->getProp(key);
        }
    }
    return JsValue::undefined();
}

JsValue VM::getProp(JsValue obj, JsValue key) {
    if (key.isString()) {
        JsString* s = key.asString();
        if (!s) return JsValue::undefined();
        return getProp(obj, s->value);
    }
    if (key.isInt32()) {
        if (obj.isObject()) {
            auto* o = obj.asObject();
            if (!o) return JsValue::undefined();
            if (o->kind == ObjKind::Array) return o->arrayGet((uint32_t)key.asInt32());
            return o->getProp(std::to_string(key.asInt32()));
        }
        if (obj.isString()) {
            JsString* strObj = obj.asString();
            if (strObj) {
                uint32_t idx = (uint32_t)key.asInt32();
                const auto& s = strObj->value;
                if (idx < s.size()) return str(std::string(1, s[idx]));
            }
        }
    }
    return getProp(obj, key.toString());
}

void VM::setProp(JsValue obj, const std::string& key, JsValue val) {
    if (obj.isObject()) {
        auto* o = obj.asObject();
        // Check for setter defined via Object.defineProperty.
        JsValue setter = o->getProp("__set_" + key + "__");
        if (setter.isCallable()) {
            try { call(setter, obj, {val}); } catch (...) {}
            return;
        }
        o->setProp(key, val);
        if (o->domNode && onDomPropSet) onDomPropSet(o, key, val);
    }
}

void VM::setProp(JsValue obj, JsValue key, JsValue val) {
    if (key.isString()) {
        JsString* s = key.asString();
        if (!s) return;
        setProp(obj, s->value, val);
    } else if (key.isInt32()) {
        if (obj.isObject()) {
            auto* o = obj.asObject();
            if (!o) return;
            if (o->kind == ObjKind::Array) o->arraySet((uint32_t)key.asInt32(), val);
            else o->setProp(std::to_string(key.asInt32()), val);
        }
    } else setProp(obj, key.toString(), val);
}

// ── Arithmetic ────────────────────────────────────────────────────────────────

JsValue VM::add(JsValue a, JsValue b) {
    // If either is string, concatenate
    if (a.isString() || b.isString())
        return str(a.toString() + b.toString());
    // If both int32, fast path
    if (a.isInt32() && b.isInt32()) {
        int64_t res = (int64_t)a.asInt32() + b.asInt32();
        if (res >= INT32_MIN && res <= INT32_MAX) return JsValue::integer((int32_t)res);
        return JsValue::number((double)res);
    }
    return JsValue::number(a.toNumber() + b.toNumber());
}

// ── Upvalue management ────────────────────────────────────────────────────────

Upvalue* VM::captureUpvalue(JsValue* slot) {
    for (auto* uv : m_openUpvalues)
        if (uv->slot == slot) return uv;
    auto* uv = m_gc.alloc<Upvalue>();
    uv->slot = slot; uv->isOpen = true;
    m_openUpvalues.push_back(uv);
    return uv;
}

void VM::closeUpvalues(JsValue* minSlot) {
    for (auto it = m_openUpvalues.begin(); it != m_openUpvalues.end(); ) {
        auto* uv = *it;
        if (uv->slot >= minSlot) { uv->close(); it = m_openUpvalues.erase(it); }
        else ++it;
    }
}

void VM::closeFunctionUpvalues(JsValue fn) {
    if (!fn.isObject()) return;
    JsObject* obj = fn.asObject();
    for (size_t i = 0; i < obj->upvalues.size(); ++i) {
        auto* uv = obj->upvalues[i];
        if (!uv || !uv->isOpen) continue;
        if (!uv->get().isObject() && obj->bcFn && i < obj->bcFn->upvalDescs.size()) {
            JsValue global = getGlobal(obj->bcFn->upvalDescs[i].name);
            if (global.isObject()) uv->set(global);
        }
        uv->close();
        m_openUpvalues.erase(std::remove(m_openUpvalues.begin(), m_openUpvalues.end(), uv), m_openUpvalues.end());
    }
}

void VM::closeAllOpenUpvalues() {
    for (auto* uv : m_openUpvalues)
        if (uv && uv->isOpen) uv->close();
    m_openUpvalues.clear();
}

// ── Iterator protocol ─────────────────────────────────────────────────────────

JsObject* VM::getIterator(JsValue val) {
    // Create a simple index-based iterator object
    auto* iter = m_gc.newObject(ObjKind::Iterator);
    iter->iterIndex  = 0;
    iter->iterTarget = val;
    return iter;
}

JsValue VM::iteratorNext(JsObject* iter) {
    JsValue target = iter->iterTarget;
    uint32_t idx = iter->iterIndex;
    uint32_t len = 0;
    if (target.isString()) len = (uint32_t)target.asString()->value.size();
    else if (target.isObject()) {
        auto* o = target.asObject();
        if (o->kind == ObjKind::Array) len = o->arrayLength();
        else len = 0;
    }
    auto* result = m_gc.newObject(ObjKind::Plain);
    if (idx >= len) {
        result->setProp("done",  JsValue::boolean(true));
        result->setProp("value", JsValue::undefined());
    } else {
        iter->iterIndex++;
        JsValue val;
        if (target.isString()) val = str(std::string(1, target.asString()->value[idx]));
        else if (target.isObject()) val = target.asObject()->arrayGet(idx);
        result->setProp("done",  JsValue::boolean(false));
        result->setProp("value", val);
    }
    return JsValue::object(result);
}

// ── Promise helpers ───────────────────────────────────────────────────────────

static void settlePromiseWith(VM& vm, JsObject* target, JsValue value, bool rejected);

static void attachPromiseMethods(VM& vm, JsObject* p) {
    if (!p) return;
    p->setProp("then", JsValue::object(vm.gc().newNativeFunction(
        [](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
            auto* next = vm.gc().newPromise();
            attachPromiseMethods(vm, next);
            JsValue onFulfilled = args.size() > 0 ? args[0] : JsValue::undefined();
            JsValue onRejected = args.size() > 1 ? args[1] : JsValue::undefined();
            JsValue fulfill = JsValue::object(vm.gc().newNativeFunction(
                [next, onFulfilled](VM& vm, JsValue, std::vector<JsValue> cbArgs) -> JsValue {
                    JsValue value = cbArgs.empty() ? JsValue::undefined() : cbArgs[0];
                    if (!onFulfilled.isCallable()) {
                        vm.resolvePromise(next, value);
                        return JsValue::undefined();
                    }
                    try {
                        settlePromiseWith(vm, next, vm.call(onFulfilled, JsValue::undefined(), { value }), false);
                    } catch (JsException& ex) {
                        vm.rejectPromise(next, ex.val);
                    }
                    return JsValue::undefined();
                }, "promise_then_fulfill"));
            JsValue reject = JsValue::object(vm.gc().newNativeFunction(
                [next, onRejected](VM& vm, JsValue, std::vector<JsValue> cbArgs) -> JsValue {
                    JsValue reason = cbArgs.empty() ? JsValue::undefined() : cbArgs[0];
                    if (!onRejected.isCallable()) {
                        vm.rejectPromise(next, reason);
                        return JsValue::undefined();
                    }
                    try {
                        settlePromiseWith(vm, next, vm.call(onRejected, JsValue::undefined(), { reason }), false);
                    } catch (JsException& ex) {
                        vm.rejectPromise(next, ex.val);
                    }
                    return JsValue::undefined();
                }, "promise_then_reject"));
            if (thisVal.isObject() && thisVal.asObject()->kind == ObjKind::Promise)
                vm.promiseThen(thisVal.asObject(), fulfill, reject);
            else
                vm.rejectPromise(next, vm.makeTypeError("then called on non-promise"));
            return JsValue::object(next);
        }, "then")));
    p->setProp("catch", JsValue::object(vm.gc().newNativeFunction(
        [](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
            JsValue then = thisVal.isObject() ? thisVal.asObject()->getProp("then") : JsValue::undefined();
            if (then.isCallable()) return vm.call(then, thisVal, { JsValue::undefined(), args.empty() ? JsValue::undefined() : args[0] });
            return JsValue::undefined();
        }, "catch")));
    p->setProp("finally", JsValue::object(vm.gc().newNativeFunction(
        [](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
            JsValue then = thisVal.isObject() ? thisVal.asObject()->getProp("then") : JsValue::undefined();
            if (then.isCallable()) {
                JsValue cb = args.empty() ? JsValue::undefined() : args[0];
                return vm.call(then, thisVal, { cb, cb });
            }
            return JsValue::undefined();
        }, "finally")));
}

static void settlePromiseWith(VM& vm, JsObject* target, JsValue value, bool rejected) {
    if (!target) return;
    if (rejected) {
        vm.rejectPromise(target, value);
        return;
    }
    if (value.isObject() && value.asObject()->kind == ObjKind::Promise) {
        JsValue resolve = JsValue::object(vm.gc().newNativeFunction(
            [target](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
                vm.resolvePromise(target, args.empty() ? JsValue::undefined() : args[0]);
                return JsValue::undefined();
            }, "promise_adopt_resolve"));
        JsValue reject = JsValue::object(vm.gc().newNativeFunction(
            [target](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
                vm.rejectPromise(target, args.empty() ? JsValue::undefined() : args[0]);
                return JsValue::undefined();
            }, "promise_adopt_reject"));
        vm.promiseThen(value.asObject(), resolve, reject);
        return;
    }
    vm.resolvePromise(target, value);
}

void VM::initPromiseObject(JsObject* p) {
    attachPromiseMethods(*this, p);
}

void VM::settlePromiseObject(JsObject* p, JsValue value, bool rejected) {
    settlePromiseWith(*this, p, value, rejected);
}

JsValue VM::promiseResolve(JsValue val) {
    auto* p = m_gc.newPromise();
    attachPromiseMethods(*this, p);
    p->promState  = JsObject::PS_Fulfilled;
    p->promResult = val;
    return JsValue::object(p);
}

JsValue VM::promiseReject(JsValue reason) {
    auto* p = m_gc.newPromise();
    attachPromiseMethods(*this, p);
    p->promState  = JsObject::PS_Rejected;
    p->promResult = reason;
    return JsValue::object(p);
}

void VM::resolvePromise(JsObject* p, JsValue val) {
    if (p->promState != JsObject::PS_Pending) return;
    p->promState  = JsObject::PS_Fulfilled;
    p->promResult = val;
    for (auto& [onF, onR] : p->promHandlers)
        if (onF.isCallable()) enqueueMicrotask(onF, val);
    p->promHandlers.clear();
}

void VM::rejectPromise(JsObject* p, JsValue reason) {
    if (p->promState != JsObject::PS_Pending) return;
    p->promState  = JsObject::PS_Rejected;
    p->promResult = reason;
    for (auto& [onF, onR] : p->promHandlers)
        if (onR.isCallable()) enqueueMicrotask(onR, reason);
    p->promHandlers.clear();
}

void VM::promiseThen(JsObject* p, JsValue onFulfilled, JsValue onRejected) {
    if (p->promState == JsObject::PS_Fulfilled) {
        if (onFulfilled.isCallable()) enqueueMicrotask(onFulfilled, p->promResult);
    } else if (p->promState == JsObject::PS_Rejected) {
        if (onRejected.isCallable()) enqueueMicrotask(onRejected, p->promResult);
    } else {
        p->promHandlers.push_back({onFulfilled, onRejected});
    }
}

void VM::enqueueMicrotask(JsValue fn, JsValue arg) {
    m_microtasks.push_back({fn, arg});
}

void VM::drainMicrotasks() {
    // Drain all pending microtasks (may enqueue more).
    while (!m_microtasks.empty()) {
        auto task = m_microtasks.front();
        m_microtasks.erase(m_microtasks.begin());
        try { call(task.fn, JsValue::undefined(), {task.arg}); } catch (...) {}
    }
}

int VM::scheduleMacrotask(JsValue fn, std::vector<JsValue> args, int delay, bool interval) {
    if (!fn.isCallable()) return 0;
    closeFunctionUpvalues(fn);
    Macrotask task;
    task.fn = fn;
    task.args = std::move(args);
    task.delay = std::max(0, delay);
    task.intervalDelay = task.delay;
    task.interval = interval;
    task.id = m_nextMacrotaskId++;
    if (m_nextMacrotaskId <= 0) m_nextMacrotaskId = 1;
    m_macrotasks.push_back(std::move(task));
    return m_macrotasks.back().id;
}

bool VM::cancelMacrotask(int id) {
    if (id <= 0) return false;
    size_t before = m_macrotasks.size();
    m_macrotasks.erase(std::remove_if(m_macrotasks.begin(), m_macrotasks.end(),
        [id](const Macrotask& task) { return task.id == id; }), m_macrotasks.end());
    return m_macrotasks.size() != before;
}

// ── Function call dispatch ────────────────────────────────────────────────────

JsValue VM::call(JsValue fnVal, JsValue thisVal, std::vector<JsValue> args) {
    if (!fnVal.isCallable()) throwTypeError("not a function");
    auto* fn = fnVal.asObject();
    bool top = (m_executeDepth == 0);
    if (top) { m_deadlineMs = NowMs() + kScriptBudgetMs; g_jsDeadlineMs = m_deadlineMs; }
    ++m_executeDepth;
    try {
        JsValue result = callFunction(fn, thisVal, args, false);
        --m_executeDepth;
        if (top) { m_deadlineMs = 0; g_jsDeadlineMs = 0; }
        return result;
    } catch (const JsException&) {
        --m_executeDepth;
        if (top) { m_deadlineMs = 0; g_jsDeadlineMs = 0; }
        throw;
    } catch (const std::exception&) {
        --m_executeDepth;
        if (top) { m_deadlineMs = 0; g_jsDeadlineMs = 0; }
        throw;
    }
}

JsValue VM::callNew(JsValue ctorVal, std::vector<JsValue> args) {
    if (!ctorVal.isCallable()) throwTypeError("not a constructor");
    auto* ctor = ctorVal.asObject();
    // Create new object with ctor.prototype as proto
    auto* newObj = m_gc.newObject(ObjKind::Plain);
    JsValue proto = ctor->getProp("prototype");
    if (proto.isObject()) newObj->proto = proto.asObject();
    JsValue result = callFunction(ctor, JsValue::object(newObj), args, true);
    // If ctor returned an object, use it; otherwise use newObj
    if (result.isObject()) return result;
    return JsValue::object(newObj);
}

JsValue VM::callFunction(JsObject* fn, JsValue thisVal, std::vector<JsValue>& args, bool isCtor) {
    if (fn->kind == ObjKind::NativeFunction) {
        if (!fn->nativeFn) throwTypeError("invalid native function");
        return fn->nativeFn(*this, thisVal, args);
    }
    if (fn->kind == ObjKind::Function && fn->bcFn) {
        if (fn->isArrow) {
            JsValue lexicalThis = fn->getProp("__lexicalThis__");
            if (!lexicalThis.isUndefined()) thisVal = lexicalThis;
        } else if (thisVal.isNullOrUndefined()) {
            thisVal = JsValue::object(m_globals);
        }
        if (fn->isGenerator) {
            auto* gen = m_gc.newObject(ObjKind::Iterator);
            auto done = std::make_shared<bool>(false);
            BytecodeFunction* bc = fn->bcFn;
            auto closure = fn->upvalues;
            gen->setProp("next", JsValue::object(m_gc.newNativeFunction(
                [done, bc, closure](VM& vm, JsValue, std::vector<JsValue>) mutable -> JsValue {
                    auto* result = vm.gc().newObject(ObjKind::Plain);
                    if (*done) {
                        result->setProp("value", JsValue::undefined());
                        result->setProp("done", JsValue::boolean(true));
                        return JsValue::object(result);
                    }
                    std::vector<JsValue> noArgs;
                    JsValue value = vm.callBytecode(bc, JsValue::undefined(), noArgs, false, closure);
                    *done = true;
                    result->setProp("value", value);
                    result->setProp("done", JsValue::boolean(false));
                    return JsValue::object(result);
                }, "generator_next")));
            return JsValue::object(gen);
        }
        return callBytecode(fn->bcFn, thisVal, args, isCtor, fn->upvalues);
    }
    throwTypeError("not a function");
}

JsValue VM::callBytecode(BytecodeFunction* bc, JsValue thisVal,
                          std::vector<JsValue>& args, bool isCtor,
                          std::vector<Upvalue*>& closureUpvals) {
    // Grow stack for this frame
    int regBase = (int)m_stack.size();
    int regCount = bc->regCount + 64; // headroom for under-counted temporaries in complex expressions
    m_stack.resize(regBase + regCount);

    // Copy args into params
    for (int i = 0; i < (int)bc->paramCount && i < (int)args.size(); i++)
        m_stack[regBase + i] = args[i];
    // Rest param
    if (bc->hasRest) {
        auto* restArr = m_gc.newArray();
        for (int i = (int)bc->paramCount - 1; i < (int)args.size(); i++)
            restArr->arrayPush(args[i]);
        // rest goes in last param slot
        if (bc->paramCount > 0) m_stack[regBase + bc->paramCount - 1] = JsValue::object(restArr);
    }

    CallFrame frame;
    frame.fn       = bc;
    frame.pc       = 0;
    frame.regBase  = regBase;
    frame.thisVal  = thisVal;
    frame.isCtor   = isCtor;

    // Set up upvalues for this closure
    for (auto& desc : bc->upvalDescs) {
        Upvalue* uv;
        if (desc.inStack) {
            // Capture from enclosing frame's stack
            if (!m_frames.empty()) {
                int encBase = m_frames.back().regBase;
                uv = captureUpvalue(&m_stack[encBase + desc.idx]);
            } else {
                uv = m_gc.alloc<Upvalue>();
                uv->closed = JsValue::undefined(); uv->isOpen = false;
            }
        } else {
            // From enclosing closure's upvalue list
            if (desc.idx < closureUpvals.size()) uv = closureUpvals[desc.idx];
            else { uv = m_gc.alloc<Upvalue>(); uv->closed = JsValue::undefined(); uv->isOpen = false; }
        }
        frame.upvalues.push_back(uv);
    }

    // Set __this__ in globals for arrow functions (they capture outer this)
    if (bc->isArrow) {
        // Arrow uses enclosing this: don't change global __this__
    } else {
        m_globals->setProp("__this__", thisVal);
    }

    m_gc.pushTempRoots(m_stack.data() + regBase, regCount);
    m_frames.push_back(frame);
    JsValue result;
    try {
        result = runFrame(m_frames.back());
    } catch (const JsException&) {
        m_frames.pop_back();
        closeUpvalues(m_stack.data() + regBase);
        m_stack.resize(regBase);
        m_gc.popTempRoots();
        throw;
    } catch (const std::exception&) {
        m_frames.pop_back();
        closeUpvalues(m_stack.data() + regBase);
        m_stack.resize(regBase);
        m_gc.popTempRoots();
        throw;
    }
    m_frames.pop_back();
    closeUpvalues(m_stack.data() + regBase);
    m_stack.resize(regBase);
    m_gc.popTempRoots();
    return result;
}

// ── Main execution loop ───────────────────────────────────────────────────────

JsValue VM::runFrame(CallFrame& frame) {
    BytecodeFunction* fn = frame.fn;
    auto& code = fn->code;

    auto REG = [&](int r) -> JsValue& {
        return m_stack[frame.regBase + r];
    };

    // Try handler stack: pairs of (catch_pc, finally_pc=-1)
    struct TryEntry { int catchPc; };
    std::vector<TryEntry> tryStack;
    JsValue caughtException;

    while (frame.pc < (int)code.size()) {
        // Runaway-script guard: check the wall-clock deadline periodically
        // (check every 4096 instructions instead of 65536 for faster timeout response)
        if ((++m_instrCount & 0xFFF) == 0 && m_deadlineMs && NowMs() > m_deadlineMs)
            throw std::runtime_error("Script execution exceeded time limit");
        const Instruction& ins = code[frame.pc++];
        int ln = fn->lines.empty() ? 0 : fn->lines[frame.pc-1];

        try {
        switch ((Opcode)ins.op) {

        case OP_LOAD_CONST:
            REG(ins.a) = resolveConst(fn, ins.bc());
            break;
        case OP_LOAD_UNDEF:
            REG(ins.a) = JsValue::undefined();
            break;
        case OP_LOAD_NULL:
            REG(ins.a) = JsValue::null();
            break;
        case OP_LOAD_TRUE:
            REG(ins.a) = JsValue::boolean(true);
            break;
        case OP_LOAD_FALSE:
            REG(ins.a) = JsValue::boolean(false);
            break;
        case OP_LOAD_INT:
            REG(ins.a) = JsValue::integer((int32_t)ins.sbc());
            break;
        case OP_MOVE:
            REG(ins.a) = REG(ins.b);
            break;

        case OP_GET_GLOBAL: {
            JsValue key = resolveConst(fn, ins.bc());
            std::string k = key.isString() ? key.asString()->value : fn->constStrings[ins.bc()];
            REG(ins.a) = m_globals->getProp(k);
            break;
        }
        case OP_SET_GLOBAL: {
            JsValue key = resolveConst(fn, ins.bc());
            std::string k = key.isString() ? key.asString()->value : fn->constStrings[ins.bc()];
            m_globals->setProp(k, REG(ins.a));
            break;
        }
        case OP_DEL_GLOBAL: {
            JsValue key = resolveConst(fn, ins.bc());
            std::string k = key.isString() ? key.asString()->value : fn->constStrings[ins.bc()];
            m_globals->deleteProp(k);
            REG(ins.a) = JsValue::boolean(true);
            break;
        }

        case OP_GET_UPVAL:
            if (ins.b < frame.upvalues.size()) {
                REG(ins.a) = frame.upvalues[ins.b]->get();
                if (!REG(ins.a).isObject() && ins.b < frame.fn->upvalDescs.size()) {
                    JsValue global = getGlobal(frame.fn->upvalDescs[ins.b].name);
                    if (global.isObject()) REG(ins.a) = global;
                }
            } else REG(ins.a) = JsValue::undefined();
            break;
        case OP_SET_UPVAL:
            if (ins.b < frame.upvalues.size()) frame.upvalues[ins.b]->set(REG(ins.a));
            break;
        case OP_CLOSE_UPVAL:
            closeUpvalues(&m_stack[frame.regBase + ins.a]);
            break;

        case OP_GET_PROP: {
            JsValue obj = REG(ins.b), key = REG(ins.c);
            REG(ins.a) = getProp(obj, key);
            break;
        }
        case OP_SET_PROP: {
            JsValue obj = REG(ins.a), key = REG(ins.b), val = REG(ins.c);
            setProp(obj, key, val);
            break;
        }
        case OP_GET_PROP_S: {
            JsValue key = resolveConst(fn, ins.bc());
            std::string k = key.isString() ? key.asString()->value : fn->constStrings[ins.bc()];
            REG(ins.a) = getProp(REG(ins.b), k);
            break;
        }
        case OP_SET_PROP_S: {
            // ins.a=obj, ins.bc=name_idx, next: ins.a=val
            JsValue key = resolveConst(fn, ins.bc());
            std::string k = key.isString() ? key.asString()->value : fn->constStrings[ins.bc()];
            // Next instruction carries the value in its .a field
            if (frame.pc < (int)code.size()) {
                const Instruction& next = code[frame.pc++];
                setProp(REG(ins.a), k, REG(next.a));
            }
            break;
        }
        case OP_DEL_PROP: {
            JsValue obj = REG(ins.b), key = REG(ins.c);
            if (obj.isObject()) obj.asObject()->deleteProp(key.toString());
            REG(ins.a) = JsValue::boolean(true);
            break;
        }
        case OP_GET_THIS:
            REG(ins.a) = frame.thisVal;
            break;
        case OP_DELETE: {
            JsValue obj = REG(ins.b), key = REG(ins.c);
            if (obj.isObject()) obj.asObject()->deleteProp(key.toString());
            REG(ins.a) = JsValue::boolean(true);
            break;
        }

        case OP_NEW_ARRAY: {
            auto* arr = m_gc.newArray();
            JsValue arrayCtor = m_globals->getProp("Array");
            if (arrayCtor.isObject()) {
                JsValue proto = arrayCtor.asObject()->getProp("prototype");
                if (proto.isObject()) arr->proto = proto.asObject();
            }
            REG(ins.a) = JsValue::object(arr);
            break;
        }
        case OP_NEW_OBJECT: {
            auto* o = m_gc.newObject(ObjKind::Plain);
            REG(ins.a) = JsValue::object(o);
            break;
        }
        case OP_ARRAY_PUSH: {
            JsValue arr = REG(ins.a);
            if (arr.isObject()) arr.asObject()->arrayPush(REG(ins.b));
            break;
        }
        case OP_SPREAD_CALL: {
            JsValue arr = REG(ins.a), spread = REG(ins.b);
            if (spread.isObject() && spread.asObject()->kind == ObjKind::Array) {
                auto* sa = spread.asObject();
                for (uint32_t i = 0; i < sa->arrayLength(); i++)
                    if (arr.isObject()) arr.asObject()->arrayPush(sa->arrayGet(i));
            }
            break;
        }

        // Arithmetic
        case OP_ADD: REG(ins.a) = add(REG(ins.b), REG(ins.c)); break;
        case OP_SUB: REG(ins.a) = JsValue::number(REG(ins.b).toNumber() - REG(ins.c).toNumber()); break;
        case OP_MUL: REG(ins.a) = JsValue::number(REG(ins.b).toNumber() * REG(ins.c).toNumber()); break;
        case OP_DIV: REG(ins.a) = JsValue::number(REG(ins.b).toNumber() / REG(ins.c).toNumber()); break;
        case OP_MOD: {
            double a = REG(ins.b).toNumber(), b = REG(ins.c).toNumber();
            REG(ins.a) = JsValue::number(std::fmod(a, b));
            break;
        }
        case OP_POW: REG(ins.a) = JsValue::number(std::pow(REG(ins.b).toNumber(), REG(ins.c).toNumber())); break;
        case OP_NEG: REG(ins.a) = JsValue::number(-REG(ins.b).toNumber()); break;
        case OP_PLUS:REG(ins.a) = JsValue::number(REG(ins.b).toNumber()); break;
        case OP_INC: {
            JsValue& v = REG(ins.a);
            v = v.isInt32() ? JsValue::integer(v.asInt32() + 1) : JsValue::number(v.toNumber() + 1);
            break;
        }
        case OP_DEC: {
            JsValue& v = REG(ins.a);
            v = v.isInt32() ? JsValue::integer(v.asInt32() - 1) : JsValue::number(v.toNumber() - 1);
            break;
        }

        // Bitwise
        case OP_BAND: REG(ins.a) = JsValue::integer(REG(ins.b).toInt32() & REG(ins.c).toInt32()); break;
        case OP_BOR:  REG(ins.a) = JsValue::integer(REG(ins.b).toInt32() | REG(ins.c).toInt32()); break;
        case OP_BXOR: REG(ins.a) = JsValue::integer(REG(ins.b).toInt32() ^ REG(ins.c).toInt32()); break;
        case OP_BNOT: REG(ins.a) = JsValue::integer(~REG(ins.b).toInt32()); break;
        case OP_SHL:  REG(ins.a) = JsValue::integer(REG(ins.b).toInt32() << (REG(ins.c).toUint32() & 31)); break;
        case OP_SHR:  REG(ins.a) = JsValue::integer(REG(ins.b).toInt32() >> (REG(ins.c).toUint32() & 31)); break;
        case OP_USHR: REG(ins.a) = JsValue::integer((int32_t)(REG(ins.b).toUint32() >> (REG(ins.c).toUint32() & 31))); break;

        // Comparison
        case OP_EQ:  REG(ins.a) = JsValue::boolean(REG(ins.b).looseEq(REG(ins.c)));  break;
        case OP_NEQ: REG(ins.a) = JsValue::boolean(!REG(ins.b).looseEq(REG(ins.c))); break;
        case OP_SEQ: REG(ins.a) = JsValue::boolean(REG(ins.b).strictEq(REG(ins.c))); break;
        case OP_SNEQ:REG(ins.a) = JsValue::boolean(!REG(ins.b).strictEq(REG(ins.c)));break;
        case OP_LT:  REG(ins.a) = JsValue::boolean(REG(ins.b).toNumber() < REG(ins.c).toNumber()); break;
        case OP_LTE: REG(ins.a) = JsValue::boolean(REG(ins.b).toNumber() <= REG(ins.c).toNumber()); break;
        case OP_GT:  REG(ins.a) = JsValue::boolean(REG(ins.b).toNumber() > REG(ins.c).toNumber()); break;
        case OP_GTE: REG(ins.a) = JsValue::boolean(REG(ins.b).toNumber() >= REG(ins.c).toNumber()); break;
        case OP_INSTANCEOF: {
            JsValue obj = REG(ins.b), ctor = REG(ins.c);
            bool result = false;
            if (obj.isObject() && ctor.isObject()) {
                JsValue proto = ctor.asObject()->getProp("prototype");
                JsObject* o = obj.asObject()->proto;
                while (o) { if (o == proto.asObject()) { result = true; break; } o = o->proto; }
            }
            REG(ins.a) = JsValue::boolean(result);
            break;
        }
        case OP_IN: {
            JsValue key = REG(ins.b), obj = REG(ins.c);
            bool result = false;
            if (obj.isObject()) result = obj.asObject()->hasProp(key.toString());
            REG(ins.a) = JsValue::boolean(result);
            break;
        }

        // Logical
        case OP_NOT:    REG(ins.a) = JsValue::boolean(!REG(ins.b).toBool()); break;
        case OP_TYPEOF: {
            JsValue v = REG(ins.b);
            const char* t;
            switch (v.tag) {
            case JsTag::Undefined: t = "undefined"; break;
            case JsTag::Null:      t = "object"; break;
            case JsTag::Bool:      t = "boolean"; break;
            case JsTag::Int32: case JsTag::Float64: t = "number"; break;
            case JsTag::String:    t = "string"; break;
            case JsTag::Object:    t = v.isCallable() ? "function" : "object"; break;
            default:               t = "undefined";
            }
            REG(ins.a) = str(t);
            break;
        }

        // Jumps
        case OP_JUMP:
            frame.pc += ins.sbc();
            break;
        case OP_JUMP_TRUE:
            if (REG(ins.a).toBool()) frame.pc += ins.sbc();
            break;
        case OP_JUMP_FALSE:
            if (!REG(ins.a).toBool()) frame.pc += ins.sbc();
            break;
        case OP_JUMP_NULLISH:
            if (!REG(ins.a).isNullOrUndefined()) frame.pc += ins.sbc();
            break;
        case OP_JUMP_TRUE_POP:
            if (REG(ins.a).toBool()) frame.pc += ins.sbc();
            break;
        case OP_JUMP_FALSE_POP:
            if (!REG(ins.a).toBool()) frame.pc += ins.sbc();
            break;

        // Functions
        case OP_MAKE_FUNC: {
            uint16_t fnIdx = ins.bc();
            if (fnIdx >= fn->innerFns.size()) { REG(ins.a) = JsValue::undefined(); break; }
            BytecodeFunction* innerFn = fn->innerFns[fnIdx].get();
            auto* fnObj = m_gc.newFunction();
            fnObj->bcFn     = innerFn;
            fnObj->funcName = innerFn->name;
            fnObj->isArrow  = innerFn->isArrow;
            fnObj->isAsync  = innerFn->isAsync;
            fnObj->isGenerator = innerFn->isGenerator;
            // Set up upvalues using current frame
            fnObj->upvalues.clear();
            for (auto& desc : innerFn->upvalDescs) {
                Upvalue* uv;
                if (desc.inStack) {
                    uv = captureUpvalue(&m_stack[frame.regBase + desc.idx]);
                } else if (desc.idx < frame.upvalues.size()) {
                    uv = frame.upvalues[desc.idx];
                } else {
                    uv = m_gc.alloc<Upvalue>(); uv->closed = JsValue::undefined(); uv->isOpen = false;
                }
                fnObj->upvalues.push_back(uv);
            }
            // Create .prototype
            auto* proto = m_gc.newObject(ObjKind::Plain);
            proto->setProp("constructor", JsValue::object(fnObj));
            fnObj->setProp("prototype", JsValue::object(proto));
            fnObj->setProp("name", str(innerFn->name));
            fnObj->setProp("length", JsValue::integer(innerFn->paramCount));
            if (innerFn->isArrow) fnObj->setProp("__lexicalThis__", frame.thisVal);
            REG(ins.a) = JsValue::object(fnObj);
            break;
        }

        case OP_CALL: {
            JsValue thisV = REG(ins.b);
            JsValue fnV   = REG(ins.c);
            // Next instruction carries argc
            int argc = 0;
            uint16_t calleeNameIdx = 0;
            if (frame.pc < (int)code.size()) {
                const Instruction& next = code[frame.pc++];
                argc = next.a;
                calleeNameIdx = next.bc();
            }
            // Args are in registers fn_reg+1 .. fn_reg+argc
            int fnReg = ins.c;
            std::vector<JsValue> args;
            for (int i = 1; i <= argc; i++) args.push_back(REG(fnReg + i));
            if (!fnV.isCallable()) {
                if (calleeNameIdx > 0) {
                    uint16_t idx = (uint16_t)(calleeNameIdx - 1);
                    std::string name = idx < fn->constStrings.size() ? fn->constStrings[idx] : "<unknown>";
                    std::string recv = thisV.toString();
                    int callPc = frame.pc - 2;
                    uint32_t line = callPc >= 0 && (size_t)callPc < fn->lines.size() ? fn->lines[(size_t)callPc] : 0;
                    throwTypeError("not a function: " + name + " is " + fnV.toString() + " on " + recv + " in " + fn->name + " line " + std::to_string(line));
                }
                throwTypeError("not a function: " + fnV.toString());
            }
            REG(ins.a) = callFunction(fnV.asObject(), thisV, args, false);
            break;
        }
        case OP_NEW: {
            JsValue fnV = REG(ins.b);
            int argc = ins.c;
            std::vector<JsValue> args;
            for (int i = 1; i <= argc; i++) args.push_back(REG(ins.b + i));
            REG(ins.a) = callNew(fnV, args);
            break;
        }

        case OP_RETURN:
            return REG(ins.a);
        case OP_RETURN_UNDEF:
            return frame.isCtor ? frame.thisVal : JsValue::undefined();

        // Exceptions
        case OP_THROW: {
            JsValue ex = REG(ins.a);
            throw JsException(ex);
        }
        case OP_ENTER_TRY: {
            int catchPc = frame.pc + ins.sbc();
            tryStack.push_back({catchPc});
            break;
        }
        case OP_EXIT_TRY:
            if (!tryStack.empty()) tryStack.pop_back();
            break;
        case OP_CATCH_LOAD:
            REG(ins.a) = caughtException;
            break;

        // Iteration
        case OP_FOR_IN_INIT: {
            JsValue obj = REG(ins.b);
            auto* iter = m_gc.newObject(ObjKind::Iterator);
            // Collect enumerable keys
            auto* keysArr = m_gc.newArray();
            if (obj.isObject()) {
                for (auto& k : obj.asObject()->ownEnumKeys())
                    keysArr->arrayPush(str(k));
            }
            iter->iterTarget = JsValue::object(keysArr);
            iter->iterIndex = 0;
            REG(ins.a) = JsValue::object(iter);
            break;
        }
        case OP_FOR_IN_NEXT: {
            JsValue iterV = REG(ins.c);
            if (!iterV.isObject()) { REG(ins.b) = JsValue::boolean(true); break; }
            auto* iter = iterV.asObject();
            auto* keys = iter->iterTarget.isObject() ? iter->iterTarget.asObject() : nullptr;
            if (!keys || iter->iterIndex >= keys->arrayLength()) {
                REG(ins.b) = JsValue::boolean(true);
            } else {
                REG(ins.a) = keys->arrayGet(iter->iterIndex++);
                REG(ins.b) = JsValue::boolean(false);
            }
            break;
        }
        case OP_FOR_OF_INIT: {
            JsValue obj = REG(ins.b);
            auto* iter = getIterator(obj);
            REG(ins.a) = JsValue::object(iter);
            break;
        }
        case OP_FOR_OF_NEXT: {
            JsValue iterV = REG(ins.c);
            if (!iterV.isObject()) { REG(ins.b) = JsValue::boolean(true); break; }
            JsValue result = iteratorNext(iterV.asObject());
            JsValue done = getProp(result, "done");
            REG(ins.b) = done;
            if (!done.toBool()) REG(ins.a) = getProp(result, "value");
            break;
        }

        case OP_AWAIT: {
            // Simplified: just return the value (no real async suspend in synchronous context)
            JsValue promise = REG(ins.b);
            if (promise.isObject() && promise.asObject()->kind == ObjKind::Promise) {
                auto* p = promise.asObject();
                if (p->promState == JsObject::PS_Fulfilled) REG(ins.a) = p->promResult;
                else if (p->promState == JsObject::PS_Rejected) throw JsException(p->promResult);
                else REG(ins.a) = JsValue::undefined(); // pending
            } else {
                REG(ins.a) = promise;
            }
            break;
        }
        case OP_YIELD:
            // The lightweight generator wrapper treats this return value as
            // the first yielded value.
            return REG(ins.a);

        case OP_DEBUGGER:
        case OP_NOP:
        case OP_VOID_OP:
        default:
            break;
        } // end switch

        } catch (JsException& ex) {
            // Unwind to nearest try handler
            if (!tryStack.empty()) {
                caughtException = ex.val;
                TryEntry te = tryStack.back();
                tryStack.pop_back();
                frame.pc = te.catchPc;
                continue;
            }
            throw; // propagate
        }
    } // end while

    return frame.isCtor ? frame.thisVal : JsValue::undefined();
}

JsValue VM::execute(BytecodeFunction* fn, JsValue thisVal) {
    if (thisVal.isUndefined()) thisVal = JsValue::object(m_globals);
    // Start the runaway-script deadline at the outermost execution only. The
    // deadline is global so native code (GC allocation, DOM/string churn) can
    // also bail out — not just the bytecode loop.
    bool top = (m_executeDepth == 0);
    if (top) { m_deadlineMs = NowMs() + kScriptBudgetMs; g_jsDeadlineMs = m_deadlineMs; }
    ++m_executeDepth;
    std::vector<JsValue> noArgs;
    std::vector<Upvalue*> noUpvals;
    JsValue r;
    try { r = callBytecode(fn, thisVal, noArgs, false, noUpvals); }
    catch (...) {
        --m_executeDepth;
        if (top) { m_deadlineMs = 0; g_jsDeadlineMs = 0; }
        throw;
    }
    --m_executeDepth;
    if (top) { m_deadlineMs = 0; g_jsDeadlineMs = 0; }
    return r;
}
