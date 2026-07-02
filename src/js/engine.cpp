#include "js/engine.h"
#include "js/gc.h"
#include "js/vm.h"
#include "js/runtime.h"
#include "js/dom_bridge.h"
#include "js/lexer.h"
#include "js/parser.h"
#include "js/compiler.h"
#include <chrono>
#include <cstdio>

#define NATIVE(name) [](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue
#define ARG(i) (args.size() > (size_t)(i) ? args[i] : JsValue::undefined())
#define ARG_STR(i) ARG(i).toString()

static void addNative(VM& vm, JsObject* obj, const std::string& name, NativeFn fn) {
    auto* fnObj = vm.gc().newNativeFunction(std::move(fn), name);
    obj->setProp(name, JsValue::object(fnObj));
}

struct JsEngine::Impl {
    GC  gc;
    VM  vm;
    std::vector<std::unique_ptr<BytecodeFunction>> scripts;
    std::shared_ptr<Node> document;
    JsScriptBudget budget;
    JsScriptStats stats;

    explicit Impl() : gc(), vm(gc) {
        registerBuiltins(vm);
        scripts.reserve(256);
    }
};

JsEngine::JsEngine() : m_impl(std::make_unique<Impl>()) {}
JsEngine::~JsEngine() = default;

void JsEngine::setDocument(std::shared_ptr<Node> doc, std::function<void()> onRepaint,
                           const std::string& pageUrl,
                           DomBridgeCallbacks callbacks) {
    m_impl = std::make_unique<Impl>();
    m_impl->document = doc;
    registerDom(m_impl->vm, std::move(doc), std::move(onRepaint), pageUrl, std::move(callbacks));
}

bool JsEngine::runScript(const std::string& source, const std::string& filename) {
    ++m_impl->stats.scriptsAttempted;
    // Skip very large scripts — our recursive parser may stack-overflow on
    // huge minified bundles. 256KB covers most real site scripts.
    if (source.size() > m_impl->budget.maxScriptBytes) {
        ++m_impl->stats.scriptsSkippedByBudget;
        fprintf(stderr, "%s",("[JS] Skipping large script (" + std::to_string(source.size()/1024) + "KB) in " + filename + "\n").c_str());
        return false;
    }
    try {
        auto parseStart = std::chrono::steady_clock::now();
        Lexer lex(source);
        auto tokens = lex.tokenize();
        Parser parser(tokens);
        Program prog = parser.parse();
        auto parseEnd = std::chrono::steady_clock::now();
        auto bytecode = Compiler::compile(prog);
        m_impl->vm.execute(bytecode.get());
        m_impl->scripts.push_back(std::move(bytecode));
        m_impl->vm.drainMicrotasks();
        auto runEnd = std::chrono::steady_clock::now();
        m_impl->stats.parseMs +=
            std::chrono::duration<double, std::milli>(parseEnd - parseStart).count();
        m_impl->stats.compileRunMs +=
            std::chrono::duration<double, std::milli>(runEnd - parseEnd).count();
        ++m_impl->stats.scriptsExecuted;
        return true;
    } catch (ParseError& e) {
        ++m_impl->stats.parseFailures;
        std::string msg = "[JS Parse Error] " + std::string(e.what()) + "\n";
        fprintf(stderr, "%s",msg.c_str());
        return false;
    } catch (JsException& e) {
        ++m_impl->stats.runtimeFailures;
        std::string msg = "[JS Error] " + e.val.toString() + "\n";
        fprintf(stderr, "%s",msg.c_str());
        return false;
    } catch (std::exception& e) {
        ++m_impl->stats.runtimeFailures;
        std::string msg = "[JS Internal Error] " + std::string(e.what()) + "\n";
        fprintf(stderr, "%s",msg.c_str());
        return false;
    } catch (...) {
        ++m_impl->stats.runtimeFailures;
        fprintf(stderr, "%s",("[JS Unknown Error] in " + filename + "\n").c_str());
        return false;
    }
}

void JsEngine::setScriptBudget(const JsScriptBudget& budget) {
    m_impl->budget = budget;
}

JsScriptStats JsEngine::scriptStats() const {
    return m_impl->stats;
}

void JsEngine::resetScriptStats() {
    m_impl->stats = JsScriptStats{};
}

void JsEngine::dispatchClick(Node* target, int x, int y) {
    auto& vm = m_impl->vm;
    (void)x;
    (void)y;
    activateDomElement(vm, target);
    vm.drainMicrotasks();
}

void JsEngine::dispatchKeyDown(int keyCode, const std::string& key) {
    auto& vm = m_impl->vm;
    auto* ev = vm.gc().newObject(ObjKind::Plain);
    ev->setProp("type",    vm.str("keydown"));
    ev->setProp("key",     vm.str(key));
    ev->setProp("keyCode", JsValue::integer(keyCode));
    ev->setProp("which",   JsValue::integer(keyCode));
    addNative(vm, ev, "preventDefault",  NATIVE("preventDefault")  { return JsValue::undefined(); });
    addNative(vm, ev, "stopPropagation", NATIVE("stopPropagation") { return JsValue::undefined(); });
    ::dispatchWindowEvent(vm, "keydown", JsValue::object(ev));
}

void JsEngine::dispatchWindowEvent(const std::string& eventName) {
    ::dispatchWindowEvent(m_impl->vm, eventName);
}

void JsEngine::dispatchDocumentEvent(const std::string& eventName) {
    if (!m_impl->document) return;
    dispatchDomEvent(m_impl->vm, m_impl->document.get(), eventName);
    m_impl->vm.drainMicrotasks();
}

void JsEngine::runMacrotasks() {
    auto& vm = m_impl->vm;
    auto tasks = std::move(vm.macrotasks());
    vm.macrotasks().clear();
    for (auto& task : tasks) {
        if (task.id <= 0 || !task.fn.isCallable()) continue;
        try {
            vm.call(task.fn, JsValue::undefined(), task.args);
            vm.drainMicrotasks();
        } catch (...) {}
        if (task.interval)
            vm.macrotasks().push_back(task);
    }
}

bool JsEngine::hasPendingMacrotasks() const {
    return !m_impl->vm.macrotasks().empty();
}
