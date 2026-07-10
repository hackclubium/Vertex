#include "fixture.h"

#include "js/compiler.h"
#include "js/engine.h"
#include "js/gc.h"
#include "js/lexer.h"
#include "js/parser.h"
#include "js/runtime.h"
#include "js/vm.h"
#include "js/dom_bridge.h"
#include "html/parser.h"
#include "network/resource_cache.h"

#include <chrono>
#include <sstream>
#include <string>
#include <thread>

static std::string ValueForSnapshot(const JsValue& value) {
    std::ostringstream out;
    if (value.isNumber()) out << "number: " << value.toString() << "\n";
    else if (value.isString()) out << "string: " << value.toString() << "\n";
    else if (value.isBool()) out << "boolean: " << value.toString() << "\n";
    else if (value.isNull()) out << "null\n";
    else if (value.isUndefined()) out << "undefined\n";
    else if (value.isObject()) out << "object: " << value.toString() << "\n";
    return out.str();
}

static std::string RunJsSourceSnapshot(const std::string& source) {
    GC gc;
    VM vm(gc);
    registerBuiltins(vm);

    Lexer lexer(source);
    Parser parser(lexer.tokenize());
    auto program = parser.parse();
    auto bytecode = Compiler::compile(program);
    vm.execute(bytecode.get());
    vm.drainMicrotasks();
    return ValueForSnapshot(vm.getGlobal("__result"));
}

static void ExpectJsResult(
    const std::string& name,
    const std::string& source,
    const std::string& expected,
    TestResult& result) {
    try {
        ExpectEqual("js/" + name, RunJsSourceSnapshot(source), expected, result);
    } catch (const ParseError& error) {
        ExpectEqual("js/" + name, std::string("parse error: ") + error.what() + "\n", expected, result);
    } catch (const JsException& error) {
        ExpectEqual("js/" + name, std::string("runtime error: ") + error.val.toString() + "\n", expected, result);
    } catch (const std::exception& error) {
        ExpectEqual("js/" + name, std::string("internal error: ") + error.what() + "\n", expected, result);
    }
}

static std::string RunEngineDomRegistrationSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><p id=\"target\">Hello</p></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript("var el = document.getElementById('target');\n", "dom-registration");
    return ok ? "registered\n" : "script failed\n";
}

static std::string RunEngineDeepDomRegistrationSnapshot() {
    std::string html = "<html><body>";
    for (int i = 0; i < 1600; ++i) {
        html += "<div>";
    }
    html += "leaf";
    for (int i = 0; i < 1600; ++i) {
        html += "</div>";
    }
    html += "</body></html>";

    JsEngine engine;
    auto dom = ParseHtml(html);
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript("var body = document.body;\n", "deep-dom-registration");
    return ok ? "registered\n" : "script failed\n";
}

static Node* FindByTag(Node* n, const std::string& tag) {
    if (!n) return nullptr;
    if (n->tagName == tag) return n;
    for (auto& c : n->children)
        if (Node* r = FindByTag(c.get(), tag)) return r;
    return nullptr;
}

static Node* FindById(Node* n, const std::string& id) {
    if (!n) return nullptr;
    if (n->attr("id") == id) return n;
    for (auto& c : n->children)
        if (Node* r = FindById(c.get(), id)) return r;
    return nullptr;
}

// Run a script against a tiny DOM, then read back the <p> node's attributes to
// prove that className / classList / style / textContent writes mutate the real
// DOM (not just the JS wrapper).
static std::string RunDomReflectionSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><p id=\"t\" class=\"a\">Hi</p></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var el = document.getElementById('t');\n"
        "el.classList.add('b');\n"
        "el.classList.remove('a');\n"
        "el.className += ' c';\n"
        "el.style.display = 'none';\n"
        "el.textContent = 'Bye';\n", "reflection");
    if (!ok) return "script failed\n";
    Node* p = FindByTag(dom.get(), "p");
    if (!p) return "no p\n";
    std::string text;
    for (auto& c : p->children) if (c->type == NodeType::Text) text += c->text;
    return "class=" + p->attr("class") + " style=" + p->attr("style")
         + " text=" + text + "\n";
}

// Regression: HTML's <script> raw-text extraction used to run ordinary
// whitespace normalization on script source (newlines/tabs -> spaces, runs
// collapsed) same as any other text node. With no '\n' left in the string, a
// `//` line comment had nothing to stop at and silently swallowed the rest
// of the script. Reproduces the real browser's script-loading path: extract
// a <script> node's text the same way src/platform/chrome.h's
// runPageScripts() does, then run it through the engine.
static std::string RunScriptCommentSurvivesHtmlExtractionSnapshot() {
    auto dom = ParseHtml(
        "<html><body><p id=\"d\">before</p><script>\n"
        "// this is a comment\n"
        "document.getElementById('d').textContent = 'AFTER-COMMENT';\n"
        "</script></body></html>");
    JsEngine engine;
    engine.setDocument(dom, []() {});
    Node* script = FindByTag(dom.get(), "script");
    if (!script) return "no script node\n";
    std::string source;
    for (auto& c : script->children) if (c->type == NodeType::Text) source += c->text;
    bool ok = engine.runScript(source, "extracted");
    if (!ok) return "script failed\n";
    Node* p = FindByTag(dom.get(), "p");
    std::string text;
    for (auto& c : p->children) if (c->type == NodeType::Text) text += c->text;
    return "text=" + text + "\n";
}

// Exercises the canvas 2D JS surface with no ICanvasSurface wired up (the
// default test harness leaves DomBridgeCallbacks::getCanvasSurface unset) —
// proves the API shape is right and that draw calls no-op safely rather than
// crashing when there's no backing renderer, which is the real situation for
// every non-Windows build target right now.
static std::string RunCanvasContext2DSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body>"
        "<canvas id=\"plain\"></canvas>"
        "<canvas id=\"sized\" width=\"640\" height=\"480\"></canvas>"
        "</body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var plain = document.getElementById('plain');\n"
        "var sized = document.getElementById('sized');\n"
        "var ctx = plain.getContext('2d');\n"
        "var methods = ['fillRect','strokeRect','clearRect','beginPath','closePath',\n"
        "  'moveTo','lineTo','rect','arc','bezierCurveTo','quadraticCurveTo',\n"
        "  'fill','stroke','save','restore','translate','scale','rotate','drawImage'];\n"
        "var allFns = methods.every(function(m) { return typeof ctx[m] === 'function'; });\n"
        "var webgl = plain.getContext('webgl');\n"
        "ctx.fillStyle = 'red';\n"
        "ctx.beginPath(); ctx.moveTo(0,0); ctx.lineTo(10,10); ctx.fill(); ctx.stroke();\n"
        "ctx.fillRect(1,2,3,4); ctx.save(); ctx.translate(1,1); ctx.restore();\n"
        "plain.setAttribute('data-result',\n"
        "  allFns + ':' + (webgl === null) + ':' + plain.width + 'x' + plain.height\n"
        "  + ':' + sized.width + 'x' + sized.height);\n",
        "canvas-context-2d");
    if (!ok) return "script failed\n";
    Node* plain = FindById(dom.get(), "plain");
    return plain ? plain->attr("data-result") + "\n" : "missing plain\n";
}

// Structural shape only — a real open/message/close round trip is covered
// by network_tests.cpp's raw-socket integration tests, which have a real
// server to talk to. The URL here (a normally-closed local port) fails fast
// in the background and is never drained, so this stays a fast, offline,
// deterministic unit test.
static std::string RunWebSocketShapeSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var ws = new WebSocket('ws://127.0.0.1:9/');\n"
        "var methods = ['send', 'close'].every(function(m) { return typeof ws[m] === 'function'; });\n"
        "var constants = ws.CONNECTING === 0 && ws.OPEN === 1 && ws.CLOSING === 2 && ws.CLOSED === 3;\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result',\n"
        "  methods + ':' + constants + ':' + ws.readyState + ':' + ws.url);\n",
        "websocket-shape");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

// canvas.width/height writes must reflect back onto the attribute (and, per
// spec, would reset the backing bitmap — verified separately via the pixel
// probe since no ICanvasSurface exists in this harness).
static std::string RunCanvasResizeSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><canvas id=\"c\"></canvas></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var c = document.getElementById('c');\n"
        "c.width = 500;\n"
        "c.height = 400;\n",
        "canvas-resize");
    if (!ok) return "script failed\n";
    Node* c = FindById(dom.get(), "c");
    if (!c) return "missing c\n";
    return c->attr("width") + "x" + c->attr("height") + "\n";
}

static std::string RunDomCssomGeometrySnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body><div id=\"box\" style=\"position:absolute; left:12px; top:7px; width:123px; height:45px; display:block; color:red\">Hi</div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var el = document.getElementById('box');\n"
        "var r = el.getBoundingClientRect();\n"
        "var cs = getComputedStyle(el);\n"
        "el.setAttribute('data-result', el.offsetWidth + 'x' + el.offsetHeight + ':' + r.left + ',' + r.top + ':' + cs.getPropertyValue('width') + ':' + cs.display);\n",
        "cssom-geometry");
    if (!ok) return "script failed\n";
    Node* box = FindById(dom.get(), "box");
    return box ? box->attr("data-result") + "\n" : "missing box\n";
}

static std::string RunDomObserverSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"box\" style=\"width:10px; height:20px\"></div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var el = document.getElementById('box');\n"
        "globalThis.mutations = 0; globalThis.intersections = 0; globalThis.resizes = 0;\n"
        "var mo = new MutationObserver(function(records){ globalThis.mutations += records.length; });\n"
        "mo.observe(el, { attributes: true });\n"
        "el.setAttribute('data-x', '1');\n"
        "new IntersectionObserver(function(records){ globalThis.intersections = records.length; }).observe(el);\n"
        "new ResizeObserver(function(records){ globalThis.resizes = records.length; }).observe(el);\n"
        "el.setAttribute('data-before', globalThis.mutations + ':' + globalThis.intersections + ':' + globalThis.resizes);\n",
        "observers");
    if (!ok) return "script failed\n";
    engine.runMacrotasks();
    ok = engine.runScript(
        "var el = document.getElementById('box');\n"
        "el.setAttribute('data-result', el.getAttribute('data-before') + '->' + globalThis.mutations + ':' + globalThis.intersections + ':' + globalThis.resizes);\n",
        "observers-after");
    if (!ok) return "script failed\n";
    Node* box = FindById(dom.get(), "box");
    return box ? box->attr("data-result") + "\n" : "missing box\n";
}

static std::string RunObserverLifecycleSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"box\" style=\"width:10px; height:20px\"></div><div id=\"other\"></div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var box = document.getElementById('box');\n"
        "var other = document.getElementById('other');\n"
        "globalThis.ioLog = 'io:';\n"
        "globalThis.io = new IntersectionObserver(function(records, obs) { globalThis.ioLog += records.length + ':' + records[0].target.id + ':' + records[0].isIntersecting + ':' + (obs === globalThis.io) + ';'; });\n"
        "io.observe(box);\n"
        "io.observe(box);\n"
        "io.unobserve(box);\n"
        "io.observe(other);\n"
        "io.disconnect();\n"
        "io.observe(box);\n"
        "globalThis.ioRecords = io.takeRecords();\n"
        "globalThis.roLog = 'ro:';\n"
        "globalThis.ro = new ResizeObserver(function(records, obs) { globalThis.roLog += records.length + ':' + records[0].target.id + ':' + records[0].contentRect.width + 'x' + records[0].contentRect.height + ':' + (obs === globalThis.ro) + ';'; });\n"
        "ro.observe(box);\n"
        "ro.observe(box);\n"
        "ro.unobserve(box);\n"
        "ro.disconnect();\n"
        "document.body.setAttribute('data-before', globalThis.ioLog + '|take=' + globalThis.ioRecords.length + '|' + globalThis.roLog);\n",
        "observer-lifecycle");
    if (!ok) return "script failed\n";
    engine.runMacrotasks();
    ok = engine.runScript(
        "document.body.setAttribute('data-result', globalThis.ioLog + '|take=' + globalThis.ioRecords.length + '|' + globalThis.roLog);\n",
        "observer-lifecycle-after");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunObserverOptionsSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"box\" style=\"width:10px; height:20px\"></div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var box = document.getElementById('box');\n"
        "globalThis.ioResult = 'missing';\n"
        "var io = new IntersectionObserver(function(records) {\n"
        "  globalThis.ioResult = this.thresholds.join(',') + ':' + this.rootMargin + ':' + records[0].intersectionRatio + ':' + !!records[0].boundingClientRect;\n"
        "}, { rootMargin: '1px 2px', threshold: [0, 0.5, 1] });\n"
        "io.observe(box);\n"
        "globalThis.roResult = 'missing';\n"
        "var ro = new ResizeObserver(function(records) {\n"
        "  globalThis.roResult = records[0].contentBoxSize.length + ':' + records[0].borderBoxSize.length + ':' + records[0].devicePixelContentBoxSize.length;\n"
        "});\n"
        "ro.observe(box);\n"
        "document.body.setAttribute('data-before', globalThis.ioResult + '|' + globalThis.roResult);\n",
        "observer-options");
    if (!ok) return "script failed\n";
    engine.runMacrotasks();
    ok = engine.runScript(
        "document.body.setAttribute('data-result', globalThis.ioResult + '|' + globalThis.roResult);\n",
        "observer-options-after");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunMacrotaskBudgetSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.count = 0;\n"
        "for (var i = 0; i < 5; i = i + 1) setTimeout(function(){ globalThis.count = globalThis.count + 1; }, 0);\n",
        "macrotask-budget-setup");
    if (!ok) return "script failed\n";

    engine.runMacrotasks(2);
    ok = engine.runScript(
        "document.body.setAttribute('data-first', String(globalThis.count));\n",
        "macrotask-budget-first");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    std::string first = body ? body->attr("data-first") : "missing";
    std::string pending = engine.hasPendingMacrotasks() ? "pending" : "empty";

    while (engine.hasPendingMacrotasks())
        engine.runMacrotasks(2);
    ok = engine.runScript(
        "document.body.setAttribute('data-final', String(globalThis.count));\n",
        "macrotask-budget-final");
    if (!ok) return "script failed\n";
    std::string final = body ? body->attr("data-final") : "missing";
    std::string remaining = engine.hasPendingMacrotasks() ? "pending" : "empty";
    return "first=" + first + ":" + pending + " final=" + final + ":" + remaining + "\n";
}

static std::string RunWindowLifecycleSurfaceSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "var popup = open('/wiki/Popup', 'docs');\n"
        "var before = closed + ':' + (opener === null) + ':' + popup.name + ':' + popup.opener.location.href;\n"
        "popup.close();\n"
        "close();\n"
        "focus(); blur(); print();\n"
        "document.body.setAttribute('data-result', before + '|' + popup.closed + ':' + closed);\n",
        "window-lifecycle-surface");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomDirtyCoalescingSnapshot() {
    JsEngine engine;
    int repaintCount = 0;
    auto dom = ParseHtml("<html><body><div id=\"box\"></div></body></html>");
    resetDomDirtyCoalesce();
    engine.setDocument(dom, [&]() { repaintCount++; });
    bool ok = engine.runScript(
        "var el = document.getElementById('box');\n"
        "el.firstCustomProperty = 1;\n"
        "el.secondCustomProperty = 2;\n",
        "dom-dirty-coalesce");
    resetDomDirtyCoalesce();
    if (!ok) return "script failed\n";
    return std::to_string(repaintCount) + "\n";
}

static std::string RunDomPaintOnlyDirtySnapshot() {
    JsEngine engine;
    int layoutDirty = 0;
    int paintDirty = 0;
    auto dom = ParseHtml("<html><body><div id=\"box\"></div></body></html>");
    DomBridgeCallbacks callbacks;
    callbacks.repaintOnly = [&]() { paintDirty++; };
    resetDomDirtyCoalesce();
    engine.setDocument(dom, [&]() { layoutDirty++; }, "", callbacks);
    bool ok = engine.runScript(
        "var el = document.getElementById('box');\n"
        "el.style.color = 'red';\n",
        "dom-paint-only-dirty");
    if (!ok) return "script failed\n";
    resetDomDirtyCoalesce();
    ok = engine.runScript(
        "var el = document.getElementById('box');\n"
        "el.style.width = '20px';\n",
        "dom-layout-dirty");
    if (!ok) return "script failed\n";
    return "layout=" + std::to_string(layoutDirty)
        + " paint=" + std::to_string(paintDirty) + "\n";
}

static std::string RunDomSelectorCompatibilitySnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body class=\"skin-vector client-js\">"
        "<main id=\"content\" class=\"mw-body\">"
        "<div class=\"mw-parser-output\">"
        "<p id=\"lead\" class=\"mw-empty-elt\">Lead</p>"
        "<section class=\"vector-dropdown\">"
        "<input id=\"toc-toggle\" class=\"vector-dropdown-checkbox\" type=\"checkbox\" checked>"
        "<div class=\"vector-dropdown-content\" data-mw=\"toc\">"
        "<a class=\"mw-list-item selected\" href=\"#History\">History</a>"
        "</div>"
        "</section>"
        "<ul id=\"toc-list\">"
        "<li class=\"tocitem featured\">Intro</li>"
        "<li class=\"tocitem hidden\">History</li>"
        "<li class=\"tocitem\">Refs</li>"
        "</ul>"
        "<aside id=\"sister\"><a class=\"mw-link\" href=\"/wiki/Wikibooks\">Books</a></aside>"
        "<aside><em id=\"solo\">Only</em></aside>"
        "</div>"
        "</main>"
        "</body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var out = document.querySelector('body.skin-vector') ? 'body' : 'missing';\n"
        "out = out + '|' + document.querySelector('#content .vector-dropdown-content[data-mw=\"toc\"] a.selected').textContent;\n"
        "out = out + '|' + document.querySelectorAll('main > div.mw-parser-output, input:checked').length;\n"
        "var toggle = document.querySelector('input.vector-dropdown-checkbox:checked');\n"
        "out = out + '|' + (toggle && toggle.matches('input:is(.vector-dropdown-checkbox, .other):not([disabled])') ? 'match' : 'nomatch');\n"
        "out = out + '|' + document.querySelector('[href^=\"#Hist\"]').getAttribute('href');\n"
        "out = out + '|' + document.querySelectorAll('#toc-list > li:nth-child(2n+1)').length;\n"
        "out = out + '|' + document.querySelector('#toc-list > li:nth-of-type(2)').textContent;\n"
        "out = out + '|' + document.querySelector('#toc-list > li:last-of-type').textContent;\n"
        "out = out + '|' + (document.getElementById('solo').matches('em:only-of-type') ? 'only' : 'not-only');\n"
        "out = out + '|' + document.querySelector('aside:has(a[href^=\"/wiki/\"])').id;\n"
        "out = out + '|' + document.querySelectorAll('#toc-list > li:nth-child(2n+1 of .tocitem)').length;\n"
        "out = out + '|' + document.querySelectorAll('#toc-list > li.tocitem:not(.hidden, .featured)').length;\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', out);\n",
        "selector-compat");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomHistoryLocationSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {}, "https://en.wikipedia.org/wiki/Wikipedia?oldid=1#top");
    bool ok = engine.runScript(
        "history.pushState(null, '', '/wiki/Vertex#History');\n"
        "var a = location.href + '|' + location.pathname + '|' + location.hash + '|' + history.length;\n"
        "history.replaceState(null, '', '?search=browser');\n"
        "var b = location.href + '|' + location.search + '|' + history.length;\n"
        "location.assign('https://www.wikipedia.org/');\n"
        "var c = location.href + '|' + location.hostname + '|' + history.length;\n"
        "history.back();\n"
        "var d = location.href + '|' + history.length;\n"
        "history.forward();\n"
        "var e = location.href + '|' + history.length;\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', a + '\\n' + b + '\\n' + c + '\\n' + d + '\\n' + e);\n",
        "history-location");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomScrollApiSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body>"
        "<div id=\"top\" style=\"height:50px\"></div>"
        "<div id=\"target\" style=\"position:absolute; top:240px; height:20px\"></div>"
        "</body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "scrollTo(0, 120);\n"
        "var a = scrollY + ':' + pageYOffset;\n"
        "scrollBy(0, 35);\n"
        "var b = scrollY + ':' + pageYOffset;\n"
        "document.getElementById('target').scrollIntoView();\n"
        "var c = scrollY + ':' + pageYOffset;\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', a + '|' + b + '|' + c);\n",
        "scroll-apis");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunWindowEventListenerSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.windowKeyCount = 0;\n"
        "globalThis.onWindowKey = function(event) { globalThis.windowKeyCount = globalThis.windowKeyCount + event.keyCode; };\n"
        "addEventListener('keydown', globalThis.onWindowKey);\n"
        "removeEventListener('keydown', function noop() {});\n",
        "window-listener-setup");
    if (!ok) return "script failed\n";
    engine.dispatchKeyDown(7, "g");
    ok = engine.runScript("removeEventListener('keydown', globalThis.onWindowKey);\n", "window-listener-remove");
    if (!ok) return "script failed\n";
    engine.dispatchKeyDown(11, "k");
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', String(globalThis.windowKeyCount));\n",
        "window-listener-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunStartupEventListenerSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.startupEvents = 'events:';\n"
        "document.addEventListener('DOMContentLoaded', function(event) { globalThis.startupEvents = globalThis.startupEvents + event.type; });\n"
        "addEventListener('load', function(event) { globalThis.startupEvents = globalThis.startupEvents + '|' + event.type; });\n",
        "startup-listener-setup");
    if (!ok) return "script failed\n";
    engine.dispatchDocumentEvent("DOMContentLoaded");
    engine.dispatchWindowEvent("load");
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.startupEvents);\n",
        "startup-listener-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunTimerCancellationSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.fired = 0;\n"
        "var timeoutId = setTimeout(function(){ globalThis.fired = globalThis.fired + 100; }, 0);\n"
        "clearTimeout(timeoutId);\n"
        "setTimeout(function(){ globalThis.fired = globalThis.fired + 2; }, 0);\n"
        "var frameId = requestAnimationFrame(function(){ globalThis.fired = globalThis.fired + 1000; });\n"
        "cancelAnimationFrame(frameId);\n"
        "requestAnimationFrame(function(ts){ globalThis.fired = globalThis.fired + (ts >= 0 ? 3 : 3000); });\n",
        "timer-cancellation-setup");
    if (!ok) return "script failed\n";
    engine.runMacrotasks();
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', String(globalThis.fired));\n",
        "timer-cancellation-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunFetchPromiseShapeSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.fetched = 'pending';\n"
        "fetch('data:text/plain,hello').then(function(response) {\n"
        "  globalThis.fetched = response.ok + ':' + response.status + ':' + response.url;\n"
        "  return response.text();\n"
        "}).then(function(text) { globalThis.fetched = globalThis.fetched + ':' + text; });\n",
        "fetch-promise-shape");
    if (!ok) return "script failed\n";
    std::string immediate = "missing body";
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-immediate', globalThis.fetched);\n",
        "fetch-promise-immediate");
    if (!ok) return "script failed\n";
    if (Node* body = FindByTag(dom.get(), "body"))
        immediate = body->attr("data-immediate");
    for (int i = 0; i < 200 && HasPendingResourceCompletions(); ++i) {
        DrainResourceCompletions();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    DrainResourceCompletions();
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.fetched);\n",
        "fetch-promise-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? immediate + "|" + body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunFetchAbortSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.abortOut = 'pending';\n"
        "var controller = new AbortController();\n"
        "controller.abort();\n"
        "globalThis.abortSignal = controller.signal;\n"
        "fetch('data:text/plain,hello', { signal: controller.signal }).then(function() {\n"
        "  globalThis.abortOut = 'resolved';\n"
        "}, function(error) {\n"
        "  globalThis.abortOut = error.name + ':' + globalThis.abortSignal.aborted;\n"
        "});\n",
        "fetch-abort");
    if (!ok) return "script failed\n";
    ok = engine.runScript(
        "document.body.setAttribute('data-result', globalThis.abortOut);\n",
        "fetch-abort-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunFetchHeadersWindowOpenSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "var h = new Headers({ 'Content-Type': 'text/plain' });\n"
        "h.append('X-Test', 'a');\n"
        "h.append('x-test', 'b');\n"
        "var hBefore = h.get('content-type') + ':' + h.get('X-Test') + ':' + h.has('missing');\n"
        "h.set('x-test', 'c');\n"
        "h.delete('content-type');\n"
        "var hAfter = h.has('content-type') + ':' + h.get('x-test');\n"
        "globalThis.fetchCompat = 'pending';\n"
        "fetch('data:application/json,{\"ok\":true}').then(function(response) {\n"
        "  var copy = response.clone();\n"
        "  globalThis.lastResponse = response;\n"
        "  globalThis.copyType = copy.headers.get('content-type');\n"
        "  globalThis.fetchCompat = response.ok + ':' + response.status + ':' + response.statusText + ':' + response.type + ':' + response.headers.get('content-type') + ':' + response.bodyUsed;\n"
        "  return response.text();\n"
        "}).then(function(text) { globalThis.fetchCompat = globalThis.fetchCompat + ':' + globalThis.lastResponse.bodyUsed + ':' + globalThis.copyType + ':' + text; });\n"
        "var popup = window.open('/wiki/Popup', '_blank');\n"
        "var popupHref = popup.location.href;\n"
        "var openerHref = popup.opener.location.href;\n"
        "var popupLog = popupHref + ':' + openerHref + ':' + popup.closed;\n"
        "popup.focus();\n"
        "popup.blur();\n"
        "popup.close();\n"
        "var closedAfter = popup.closed;\n"
        "popupLog = popupLog + ':' + closedAfter;\n"
        "document.body.setAttribute('data-static', hBefore + '|' + hAfter + '|' + popupLog);\n",
        "fetch-headers-window-open");
    if (!ok) return "script failed\n";
    for (int i = 0; i < 200 && HasPendingResourceCompletions(); ++i) {
        DrainResourceCompletions();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    DrainResourceCompletions();
    ok = engine.runScript(
        "document.body.setAttribute('data-result', document.body.getAttribute('data-static') + '|' + globalThis.fetchCompat);\n",
        "fetch-headers-window-open-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunRequestResponseConstructorSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "var req = new Request('/api/data', { method: 'post', headers: { 'X-A': '1' }, body: 'payload' });\n"
        "var res = new Response('ok', { status: 201, statusText: 'Created', headers: { 'Content-Type': 'text/plain' } });\n"
        "globalThis.ctorResponse = res;\n"
        "globalThis.ctorCompat = req.url + ':' + req.method + ':' + req.headers.get('x-a') + ':' + req.bodyUsed + '|';\n"
        "globalThis.ctorCompat = globalThis.ctorCompat + res.ok + ':' + res.status + ':' + res.statusText + ':' + res.headers.get('content-type') + ':' + res.bodyUsed;\n"
        "res.text().then(function(text) { globalThis.ctorCompat = globalThis.ctorCompat + ':' + globalThis.ctorResponse.bodyUsed + ':' + text; });\n",
        "request-response-constructors");
    if (!ok) return "script failed\n";
    ok = engine.runScript(
        "document.body.setAttribute('data-result', globalThis.ctorCompat);\n",
        "request-response-constructors-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunUrlSearchParamsFormDataSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "var params = new URLSearchParams({ q: 'hello world', empty: '', plus: 'a+b' });\n"
        "params.append('q', 'second');\n"
        "var before = params.get('q') + ':' + params.getAll('q').join('|') + ':' + params.has('empty') + ':' + params.toString();\n"
        "params.sort();\n"
        "var sorted = params.toString();\n"
        "var copied = new URLSearchParams(params);\n"
        "copied.delete('empty');\n"
        "var fd = new FormData();\n"
        "fd.append('title', 'Vertex');\n"
        "fd.append('title', 'Browser');\n"
        "fd.set('action', 'edit');\n"
        "fd.set('title', 'Final');\n"
        "var fdLog = fd.get('title') + ':' + fd.getAll('title').join('|') + ':' + fd.has('action');\n"
        "fd.delete('action');\n"
        "fdLog = fdLog + ':' + fd.has('action');\n"
        "document.body.setAttribute('data-result', before + '|' + sorted + '|' + copied.toString() + '|' + fdLog);\n",
        "urlsearchparams-formdata");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunFetchBodyInitAndEventSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><button id=\"btn\"></button></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "var fd = new FormData();\n"
        "fd.append('title', 'Vertex');\n"
        "fd.append('space value', 'a b');\n"
        "var req = new Request('/submit', { method: 'post', body: fd });\n"
        "globalThis.blobText = 'pending';\n"
        "globalThis.plainText = 'pending';\n"
        "var res = new Response(fd, { headers: { 'Content-Type': 'text/custom' } });\n"
        "res.blob().then(function(blob) { globalThis.blobText = blob.size + ':' + blob.type; });\n"
        "res.text().then(function(text) { globalThis.plainText = text; });\n"
        "var btn = document.getElementById('btn');\n"
        "var eventLog = '';\n"
        "btn.addEventListener('keydown', function(e) { eventLog = e.type + ':' + e.key + ':' + e.code + ':' + e.bubbles + ':' + e.cancelable; });\n"
        "btn.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', code: 'Enter', bubbles: true, cancelable: true }));\n"
        "globalThis.req = req;\n"
        "globalThis.eventLog = eventLog;\n"
        "fetch-body-event");
    if (!ok) return "script failed\n";
    ok = engine.runScript(
        "document.body.setAttribute('data-result', globalThis.req.method + ':' + globalThis.req.headers.get('content-type') + ':' + globalThis.req.bodyUsed + '|' + globalThis.eventLog + '|' + globalThis.blobText + '|' + globalThis.plainText);\n",
        "fetch-body-event-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunRangeSelectionSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><article id=\"a\"><p id=\"p\">Alpha <b id=\"b\">Beta</b> Gamma</p></article></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "var p = document.getElementById('p');\n"
        "var b = document.getElementById('b');\n"
        "var range = document.createRange();\n"
        "range.selectNodeContents(p);\n"
        "var all = range.toString();\n"
        "range.selectNode(b);\n"
        "var frag = range.cloneContents();\n"
        "var contextual = range.createContextualFragment('<span id=\"made\">Made</span>');\n"
        "var sel = getSelection();\n"
        "sel.removeAllRanges();\n"
        "sel.addRange(range);\n"
        "document.body.setAttribute('data-result', all + '|' + range.toString() + '|' + frag.textContent + '|' + contextual.firstElementChild.id + ':' + contextual.textContent + '|' + sel.rangeCount + ':' + sel.toString() + ':' + (sel.getRangeAt(0) === range));\n",
        "range-selection");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunBlobFileReaderSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "var blob = new Blob(['Hello', ' ', 'Vertex'], { type: 'text/plain' });\n"
        "globalThis.blobResult = blob.size + ':' + blob.type + ':';\n"
        "blob.text().then(function(text) { globalThis.blobResult += text; });\n"
        "var reader = new FileReader();\n"
        "globalThis.readerLog = '';\n"
        "reader.onload = function(e) { globalThis.readerLog += 'load:' + reader.result + ':' + e.type + ';'; };\n"
        "reader.onloadend = function(e) { globalThis.readerLog += 'end:' + reader.readyState + ':' + e.type + ';'; };\n"
        "reader.readAsText(blob);\n"
        "globalThis.req = new Request('/blob', { method: 'post', body: blob });\n"
        "var res = new Response(blob);\n"
        "res.text().then(function(text) { document.body.setAttribute('data-result', globalThis.blobResult + '|' + globalThis.readerLog + '|' + globalThis.req.headers.get('content-type') + ':' + globalThis.req.bodyUsed + '|' + text); });\n",
        "blob-filereader");
    if (!ok) return "script failed\n";
    ok = engine.runScript("document.body.setAttribute('data-final', document.body.getAttribute('data-result'));\n", "blob-filereader-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-final") + "\n" : "missing body\n";
}

static std::string RunFormCollectionSubmitSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body>"
        "<form id=\"f\" name=\"search\" action=\"/w/index.php\" method=\"get\">"
        "<label id=\"lbl\" for=\"q\">Query</label>"
        "<input id=\"q\" name=\"title\" value=\"Vertex\">"
        "<input id=\"hidden\" type=\"hidden\" name=\"action\" value=\"view\">"
        "<select id=\"lang\" name=\"lang\"><option value=\"en\">English</option><option value=\"fr\" selected>French</option></select>"
        "<button id=\"go\" name=\"go\" value=\"1\">Go</button>"
        "</form>"
        "</body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "var form = document.forms[0];\n"
        "var input = document.getElementById('q');\n"
        "var label = document.getElementById('lbl');\n"
        "var select = document.getElementById('lang');\n"
        "globalThis.submitLog = '';\n"
        "form.addEventListener('submit', function(e) { globalThis.submitLog += e.type + ':' + e.submitter.id + ':' + e.cancelable + ';'; e.preventDefault(); });\n"
        "var fd = new FormData(form);\n"
        "form.requestSubmit(document.getElementById('go'));\n"
        "document.body.setAttribute('data-result', document.forms.length + ':' + document.forms.search.id + '|' + form.elements.length + ':' + form.elements.title.value + ':' + form.elements.lang.value + '|' + input.form.id + ':' + label.control.id + '|' + select.options.length + ':' + select.selectedIndex + ':' + select.value + '|' + fd.get('title') + ':' + fd.get('action') + ':' + fd.get('lang') + '|' + globalThis.submitLog);\n",
        "form-collections-submit");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunPromiseConstructorCombinatorsSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.promiseOut = 'p:';\n"
        "new Promise(function(resolve) { resolve('made'); })\n"
        "  .then(function(value) { globalThis.promiseOut += value; return value + '-chain'; })\n"
        "  .then(function(value) { globalThis.promiseOut += '|' + value; });\n"
        "Promise.all([Promise.resolve('a'), 'b']).then(function(values) {\n"
        "  globalThis.promiseOut += '|all=' + values[0] + values[1];\n"
        "});\n"
        "Promise.race([Promise.resolve('r'), Promise.resolve('s')]).then(function(value) {\n"
        "  globalThis.promiseOut += '|race=' + value;\n"
        "});\n"
        "Promise.allSettled([Promise.resolve('ok'), Promise.reject('bad')]).then(function(values) {\n"
        "  globalThis.promiseOut += '|settled=' + values[0].status + '/' + values[1].status;\n"
        "});\n",
        "promise-combinators");
    if (!ok) return "script failed\n";
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.promiseOut);\n",
        "promise-combinators-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomEventCancellationSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"outer\"><button id=\"btn\"></button></div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.events = 'e:';\n"
        "var outer = document.getElementById('outer');\n"
        "var btn = document.getElementById('btn');\n"
        "outer.addEventListener('click', function(e) { globalThis.events += '|outer:' + e.defaultPrevented; });\n"
        "btn.addEventListener('click', function(e) { globalThis.events += 'btn:' + e.currentTarget.id + '/' + e.target.id; e.preventDefault(); e.stopPropagation(); });\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-stage', 'listeners');\n",
        "dom-event-listeners");
    if (!ok) {
        Node* body = FindByTag(dom.get(), "body");
        return std::string("failed:") + (body ? body->attr("data-stage") : "no-body") + "\n";
    }
    ok = engine.runScript(
        "var btn = document.getElementById('btn');\n"
        "var ev = new Event('click', { bubbles: true, cancelable: true });\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-stage', 'event');\n"
        "var result = btn.dispatchEvent(ev);\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-stage', 'dispatch');\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.events + '|result=' + result + '|default=' + ev.defaultPrevented);\n",
        "dom-event-cancel");
    if (!ok) {
        Node* body = FindByTag(dom.get(), "body");
        return std::string("failed:") + (body ? body->attr("data-stage") : "no-body") + "\n";
    }
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomElementClickSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body>"
        "<input id=\"check\" type=\"checkbox\">"
        "<button id=\"btn\"></button>"
        "</body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.clickLog = 'start:';\n"
        "var check = document.getElementById('check');\n"
        "var btn = document.getElementById('btn');\n"
        "check.onclick = function(e) { globalThis.clickLog += 'prop:' + check.checked + ':' + e.target.id + ';'; };\n"
        "check.addEventListener('click', function(e) { globalThis.clickLog += 'listener:' + e.currentTarget.id + ':' + e.defaultPrevented + ';'; });\n"
        "check.addEventListener('change', function(e) { globalThis.clickLog += 'change:' + check.checked + ';'; });\n"
        "btn.onclick = function(e) { globalThis.clickLog += 'button:' + e.type + ';'; };\n"
        "check.click();\n"
        "btn.click();\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.clickLog + 'checked=' + check.checked);\n",
        "element-click");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomDatasetReflectionSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"box\" data-old-name=\"kept\"></div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var box = document.getElementById('box');\n"
        "box.dataset.mwState = 'open';\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', box.getAttribute('data-mw-state') + '|' + document.querySelector('[data-mw-state=\"open\"]').id + '|' + box.dataset.oldName);\n",
        "dataset-reflection");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomClassListCompatSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"box\" class=\"a b\"></div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var box = document.getElementById('box');\n"
        "var list = box.classList;\n"
        "var before = list.length + ':' + list.item(1);\n"
        "var replaced = list.replace('b', 'c');\n"
        "list.add('d');\n"
        "list.remove('a');\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', before + '|' + replaced + '|' + box.className + '|' + list.length + '|' + list.toString());\n",
        "class-list-compat");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomFragmentCloneMutationSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body>"
        "<div id=\"root\"></div>"
        "<div id=\"source\"><span class=\"label\">Source</span></div>"
        "</body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var root = document.getElementById('root');\n"
        "var source = document.getElementById('source');\n"
        "var body = document.getElementsByTagName('body')[0];\n"
        "body.setAttribute('data-stage', 'start');\n"
        "var deep = source.cloneNode(true);\n"
        "deep.id = 'deep';\n"
        "body.setAttribute('data-stage', 'deep');\n"
        "var shallow = source.cloneNode(false);\n"
        "shallow.id = 'shallow';\n"
        "body.setAttribute('data-stage', 'shallow');\n"
        "var frag = document.createDocumentFragment();\n"
        "var item = document.createElement('p');\n"
        "item.id = 'frag-child';\n"
        "item.textContent = 'Frag';\n"
        "body.setAttribute('data-stage', 'made');\n"
        "frag.appendChild(item);\n"
        "body.setAttribute('data-stage', 'frag-append');\n"
        "root.appendChild(frag);\n"
        "body.setAttribute('data-stage', 'root-frag');\n"
        "root.appendChild(deep);\n"
        "root.appendChild(shallow);\n"
        "body.setAttribute('data-stage', 'root-clones');\n"
        "body.setAttribute('data-result',\n"
        "  document.querySelector('#deep .label').textContent + '|' +\n"
        "  (document.querySelector('#shallow .label') ? 'bad' : 'empty') + '|' +\n"
        "  root.contains(document.getElementById('frag-child')) + '|' +\n"
        "  body.contains(root));\n",
        "fragment-clone-mutation");
    Node* body = FindByTag(dom.get(), "body");
    if (!ok) return std::string("script failed:") + (body ? body->attr("data-stage") : "no-body") + "\n";
    Node* root = FindById(dom.get(), "root");
    std::string direct;
    if (root) {
        for (const auto& child : root->children) {
            if (!direct.empty()) direct += ",";
            direct += child->tagName + "#" + child->attr("id");
        }
    }
    return body ? body->attr("data-result") + "|direct=" + direct + "\n" : "missing body\n";
}

static std::string RunDomNodeSurfaceSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"root\">old</div><div id=\"after\"></div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var root = document.getElementById('root');\n"
        "var text = root.firstChild;\n"
        "var loose = document.createElement('section');\n"
        "text.data = 'new';\n"
        "root.append('A', 'B');\n"
        "root.normalize();\n"
        "root.role = 'button'; root.tabIndex = 2; root.hidden = true; root.ariaLabel = 'Root';\n"
        "var pos = root.compareDocumentPosition(document.getElementById('after'));\n"
        "var rects = root.getClientRects();\n"
        "root.scrollTo(3, 4); root.scrollBy(2, 1);\n"
        "loose.replaceChildren('fresh', document.createElement('b'));\n"
        "var same = root.isSameNode(document.querySelector('#root'));\n"
        "var roots = root.getRootNode().nodeType + ':' + text.ownerDocument.nodeType + ':' + loose.ownerDocument.nodeType;\n"
        "var connected = root.isConnected + ':' + loose.isConnected;\n"
        "document.body.setAttribute('data-result', same + '|' + roots + '|' + connected + '|' + text.nodeValue + '|' + root.textContent + '|' + root.getAttribute('role') + ':' + root.tabIndex + ':' + root.hidden + ':' + root.getAttribute('aria-label') + '|' + pos + ':' + rects.length + ':' + root.scrollLeft + ',' + root.scrollTop + '|' + loose.childNodes.length + ':' + loose.textContent);\n",
        "dom-node-surface");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomDispatchEventPropertyHandlerSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"outer\"><button id=\"btn\"></button></div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.dispatchLog = 'start:';\n"
        "var outer = document.getElementById('outer');\n"
        "var btn = document.getElementById('btn');\n"
        "btn.onclick = function(e) { globalThis.dispatchLog += 'prop:' + e.currentTarget.id + '/' + e.target.id + ';'; };\n"
        "outer.addEventListener('click', function(e) { globalThis.dispatchLog += 'outer:' + e.defaultPrevented + ';'; });\n"
        "var ev = new Event('click', { bubbles: true, cancelable: true });\n"
        "var result = btn.dispatchEvent(ev);\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.dispatchLog + result);\n",
        "dispatch-event-property-handler");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomFocusBlurSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><input id=\"search\"><button id=\"other\"></button></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.focusLog = 'start:';\n"
        "var search = document.getElementById('search');\n"
        "search.onfocus = function(e) { globalThis.focusLog += 'focus:' + e.target.id + ';'; };\n"
        "search.addEventListener('blur', function(e) { globalThis.focusLog += 'blur:' + e.target.id + ';'; });\n"
        "search.focus();\n"
        "var active = document.activeElement ? document.activeElement.id : 'none';\n"
        "search.blur();\n"
        "var after = document.activeElement ? document.activeElement.tagName : 'none';\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.focusLog + active + '|' + after);\n",
        "focus-blur");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDocumentElementShortcutsSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><head><title>T</title></head><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var body = document.getElementsByTagName('body')[0];\n"
        "document.body.setAttribute('data-result', document.documentElement.tagName + '|' + document.head.tagName + '|' + document.body.tagName);\n",
        "document-element-shortcuts");
    Node* body = FindByTag(dom.get(), "body");
    if (!ok) return std::string("script failed:") + (body ? body->attr("data-stage") : "no-body") + "\n";
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomModernMutationConveniencesSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><ul id=\"list\"><li id=\"a\">A</li><li id=\"b\">B</li></ul><p id=\"tail\">T</p></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var list = document.getElementById('list');\n"
        "var a = document.getElementById('a');\n"
        "var b = document.getElementById('b');\n"
        "var tail = document.getElementById('tail');\n"
        "list.prepend('zero');\n"
        "var c = document.createElement('li'); c.id = 'c'; c.textContent = 'C';\n"
        "list.append(c, 'done');\n"
        "a.before(document.createElement('hr'));\n"
        "b.after('after-b');\n"
        "tail.replaceWith(document.createElement('section'), 'end');\n"
        "list.insertAdjacentHTML('afterbegin', '<li id=\"html-first\">H</li>');\n"
        "list.insertAdjacentText('beforeend', 'text-tail');\n"
        "var moved = document.createElement('li'); moved.id = 'moved'; moved.textContent = 'M';\n"
        "list.insertAdjacentElement('beforeend', moved);\n"
        "var names = [];\n"
        "for (var i = 0; i < list.childNodes.length; i++) names.push(list.childNodes[i].nodeName + ':' + list.childNodes[i].textContent);\n"
        "document.body.setAttribute('data-result', names.join('|') + ';adj=' + document.getElementById('html-first').textContent + document.getElementById('moved').textContent + ':' + list.textContent + ';tail=' + (document.getElementById('tail') ? 'yes' : 'no') + ';section=' + document.getElementsByTagName('section').length);\n",
        "modern-mutation-conveniences");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomAttributeNsSiblingSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"box\" class=\"card\"></div><span id=\"next\"></span></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var box = document.getElementById('box');\n"
        "document.body.setAttribute('data-stage', 'box');\n"
        "box.setAttribute('data-a', '1');\n"
        "document.body.setAttribute('data-stage', 'set');\n"
        "var first = box.toggleAttribute('hidden');\n"
        "document.body.setAttribute('data-stage', 'toggle1');\n"
        "var second = box.toggleAttribute('hidden', false);\n"
        "document.body.setAttribute('data-stage', 'toggle2');\n"
        "var third = box.toggleAttribute('hidden', true);\n"
        "document.body.setAttribute('data-stage', 'toggle3');\n"
        "var names = box.getAttributeNames();\n"
        "document.body.setAttribute('data-stage', 'names');\n"
        "var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');\n"
        "document.body.setAttribute('data-stage', 'create-ns');\n"
        "svg.setAttribute('viewBox', '0 0 1 1');\n"
        "box.after(svg);\n"
        "document.body.setAttribute('data-stage', 'after');\n"
        "document.body.setAttribute('data-stage', 'join');\n"
        "var joined = names.join(',');\n"
        "document.body.setAttribute('data-stage', 'svg-attr');\n"
        "var vb = svg.getAttribute('viewBox');\n"
        "document.body.setAttribute('data-stage', 'siblings');\n"
        "var ns = box.nextSibling.tagName;\n"
        "var ps = document.getElementById('next').previousSibling.tagName;\n"
        "document.body.setAttribute('data-stage', 'result');\n"
        "document.body.setAttribute('data-result', first + '/' + second + '/' + third + '|' + joined + '|' + svg.tagName + ':' + svg.namespaceURI + ':' + vb + '|' + ns + '|' + ps);\n",
        "attribute-ns-sibling");
    Node* body = FindByTag(dom.get(), "body");
    if (!ok) return std::string("script failed:") + (body ? body->attr("data-stage") : "no-body") + "\n";
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunStorageLengthSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var before = sessionStorage.length;\n"
        "sessionStorage.setItem('a', '1');\n"
        "sessionStorage.setItem('b', '2');\n"
        "var afterSet = sessionStorage.length + ':' + sessionStorage.key(1);\n"
        "sessionStorage.removeItem('a');\n"
        "var afterRemove = sessionStorage.length;\n"
        "sessionStorage.clear();\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', before + '|' + afterSet + '|' + afterRemove + '|' + sessionStorage.length);\n",
        "storage-length");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDocumentCookieWriteSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "document.cookie = 'vertex_fill_a=1; Path=/';\n"
        "document.cookie = 'vertex_fill_b=2; Path=/';\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', document.cookie);\n",
        "document-cookie-write");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunMatchMediaListenerSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var mq = matchMedia('(min-width: 1px)');\n"
        "globalThis.mqLog = 'start:';\n"
        "function onChange(e) { globalThis.mqLog += 'on:' + e.matches + ';'; }\n"
        "function legacy(e) { globalThis.mqLog += 'legacy:' + e.matches + ';'; }\n"
        "mq.addEventListener('change', onChange);\n"
        "mq.addListener(legacy);\n"
        "var first = mq.dispatchEvent(new Event('change'));\n"
        "mq.removeEventListener('change', onChange);\n"
        "mq.removeListener(legacy);\n"
        "var second = mq.dispatchEvent(new Event('change'));\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.mqLog + first + ':' + second);\n",
        "match-media-listeners");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunWebPlatformSurfaceSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page?old=1#top");
    bool ok = engine.runScript(
        "localStorage.setItem('mw-test', '42');\n"
        "sessionStorage.temp = 'tab';\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-stage', 'storage');\n"
        "var u = new URL('/w/index.php?title=Vertex&oldid=7#History', location.href);\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-stage', 'url');\n"
        "u.searchParams.set('action', 'view');\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-stage', 'url-set');\n"
        "var params = new URLSearchParams('a=1&b=two');\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-stage', 'params');\n"
        "params.append('a', '3');\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-stage', 'append');\n"
        "var mq = matchMedia('(min-width: 1px) and (prefers-color-scheme: light)');\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-stage', 'media');\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', localStorage.getItem('mw-test') + '|' + sessionStorage.getItem('temp') + '|' + u.pathname + '|' + u.searchParams.get('title') + '|' + u.searchParams.get('action') + '|' + params.getAll('a').join(',') + '|' + mq.matches);\n",
        "web-platform-surface");
    if (!ok) {
        Node* body = FindByTag(dom.get(), "body");
        return std::string("failed:") + (body ? body->attr("data-stage") : "no-body") + "\n";
    }
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunGeneralPlatformStubsSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><button id=\"btn\"></button></body></html>");
    engine.setDocument(dom, []() {}, "https://example.org/wiki/Page");
    bool ok = engine.runScript(
        "var ev = document.createEvent('Event');\n"
        "ev.initEvent('vertex', true, true);\n"
        "var custom = document.createEvent('CustomEvent');\n"
        "custom.initCustomEvent('data', false, false, { value: 7 });\n"
        "var btn = document.getElementById('btn');\n"
        "var body = document.getElementsByTagName('body')[0];\n"
        "body.setAttribute('data-log', 'start:');\n"
        "btn.addEventListener('vertex', function(e) { body.setAttribute('data-log', body.getAttribute('data-log') + e.type + ':' + e.bubbles + ':' + e.cancelable + ';'); });\n"
        "btn.dispatchEvent(ev);\n"
        "performance.mark('start');\n"
        "performance.mark('end');\n"
        "performance.measure('span', 'start', 'end');\n"
        "var marks = performance.getEntriesByType('mark').length;\n"
        "var measures = performance.getEntriesByName('span').length;\n"
        "var arr = [0, 0, 0, 0];\n"
        "crypto.getRandomValues(arr);\n"
        "var randomish = arr.length + ':' + (arr[0] >= 0) + ':' + (crypto.randomUUID().length >= 32);\n"
        "globalThis.idleOut = 'pending';\n"
        "requestIdleCallback(function(deadline) { globalThis.idleOut = 'idle:' + deadline.didTimeout + ':' + (deadline.timeRemaining() >= 0) + ';'; });\n"
        "var parsed = new DOMParser().parseFromString('<main id=\"parsed\">P</main>', 'text/html');\n"
        "var made = document.implementation.createHTMLDocument('Made');\n"
        "var css = CSS.supports('display', 'inline flow-root') + ':' + CSS.supports('(border-collapse: collapse)') + ':' + CSS.escape('a b');\n"
        "var blobUrl = URL.createObjectURL(new Blob(['x']));\n"
        "var canParse = URL.canParse('/wiki/Test', location.href);\n"
        "URL.revokeObjectURL(blobUrl);\n"
        "globalThis.clipboardOut = 'pending';\n"
        "navigator.clipboard.writeText('clip').then(function(){ return navigator.clipboard.readText(); }).then(function(text){ globalThis.clipboardOut = text; });\n"
        "body.setAttribute('data-immediate', document.compatMode + '|' + document.visibilityState + '|' + document.hidden + '|' + document.baseURI + '|' + document.domain + '|' + navigator.languages[0] + ':' + navigator.hardwareConcurrency + ':' + navigator.maxTouchPoints + '|' + (top === window) + ':' + (parent === window) + '|' + navigator.userAgentData.mobile + '|' + marks + ':' + measures + '|' + randomish + '|' + custom.detail.value + '|' + parsed.getElementById('parsed').textContent + '|' + made.getElementsByTagName('title')[0].textContent + '|' + css + '|' + canParse + ':' + (blobUrl.indexOf('blob:') === 0));\n",
        "general-platform-stubs");
    if (!ok) return "script failed\n";
    while (engine.hasPendingMacrotasks()) engine.runMacrotasks();
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.clipboardOut + '|' + globalThis.idleOut);\n",
        "general-platform-stubs-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-immediate") + "|" + body->attr("data-log") + "|" + body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunScriptBudgetProfileSnapshot() {
    JsEngine engine;
    JsScriptBudget budget;
    budget.maxScriptBytes = 32;
    engine.setScriptBudget(budget);
    bool small = engine.runScript("globalThis.__small = 1;", "small.js");
    bool large = engine.runScript(std::string(64, ' ') + "globalThis.__large = 1;", "large.js");
    const auto stats = engine.scriptStats();
    std::string actual;
    actual += std::string("small=") + (small ? "yes" : "no") + "\n";
    actual += std::string("large=") + (large ? "yes" : "no") + "\n";
    actual += "attempted=" + std::to_string(stats.scriptsAttempted) + "\n";
    actual += "executed=" + std::to_string(stats.scriptsExecuted) + "\n";
    actual += "skipped=" + std::to_string(stats.scriptsSkippedByBudget) + "\n";
    actual += std::string("timed=") + (stats.parseMs >= 0.0 && stats.compileRunMs >= 0.0 ? "yes" : "no") + "\n";
    return actual;
}

TestResult RunJsTests() {
    TestResult result;

    ExpectEqual(
        "js/engine/script-comment-survives-html-extraction",
        RunScriptCommentSurvivesHtmlExtractionSnapshot(),
        "text=AFTER-COMMENT\n",
        result);

    ExpectEqual(
        "js/dom/selector-compatibility",
        RunDomSelectorCompatibilitySnapshot(),
        "body|History|2|match|#History|2|History|Refs|only|sister|2|1\n",
        result);

    ExpectEqual(
        "js/dom/history-location-state",
        RunDomHistoryLocationSnapshot(),
        "https://en.wikipedia.org/wiki/Vertex#History|/wiki/Vertex|#History|2\n"
        "https://en.wikipedia.org/wiki/Vertex?search=browser|?search=browser|2\n"
        "https://www.wikipedia.org/|www.wikipedia.org|3\n"
        "https://en.wikipedia.org/wiki/Vertex?search=browser|3\n"
        "https://www.wikipedia.org/|3\n",
        result);

    ExpectEqual(
        "js/dom/scroll-apis-update-window-offsets",
        RunDomScrollApiSnapshot(),
        "120:120|155:155|240:240\n",
        result);

    ExpectEqual(
        "js/window/event-listeners-dispatch-and-remove",
        RunWindowEventListenerSnapshot(),
        "7\n",
        result);

    ExpectEqual(
        "js/window/startup-events-dispatch",
        RunStartupEventListenerSnapshot(),
        "events:DOMContentLoaded|load\n",
        result);

    ExpectEqual(
        "js/async/timers-and-raf-can-cancel",
        RunTimerCancellationSnapshot(),
        "5\n",
        result);

    ExpectEqual(
        "js/async/fetch-uses-real-promise-shape",
        RunFetchPromiseShapeSnapshot(),
        "pending|true:200:data:text/plain,hello:hello\n",
        result);

    ExpectEqual(
        "js/async/fetch-abort-controller",
        RunFetchAbortSnapshot(),
        "AbortError:true\n",
        result);

    ExpectEqual(
        "js/async/fetch-headers-and-window-open-compat",
        RunFetchHeadersWindowOpenSnapshot(),
        "text/plain:a, b:false|false:c|https://example.org/wiki/Popup:https://example.org/wiki/Page:false:true|true:200:OK:basic:application/json:false:true:application/json:{\"ok\":true}\n",
        result);

    ExpectEqual(
        "js/async/request-response-constructors",
        RunRequestResponseConstructorSnapshot(),
        "https://example.org/api/data:POST:1:false|true:201:Created:text/plain:false:true:ok\n",
        result);

    ExpectEqual(
        "js/web-platform/urlsearchparams-formdata",
        RunUrlSearchParamsFormDataSnapshot(),
        "hello world:hello world|second:true:q=hello+world&empty=&plus=a%2Bb&q=second|empty=&plus=a%2Bb&q=hello+world&q=second|plus=a%2Bb&q=hello+world&q=second|Final:Final:true:false\n",
        result);

    ExpectEqual(
        "js/web-platform/fetch-body-init-and-events",
        RunFetchBodyInitAndEventSnapshot(),
        "POST:application/x-www-form-urlencoded;charset=UTF-8:false|keydown:Enter:Enter:true:true|28:text/custom|title=Vertex&space+value=a+b\n",
        result);

    ExpectEqual(
        "js/web-platform/range-selection-surface",
        RunRangeSelectionSnapshot(),
        "Alpha Beta Gamma|Beta|Beta|made:Made|1:Beta:true\n",
        result);

    ExpectEqual(
        "js/web-platform/blob-filereader-body",
        RunBlobFileReaderSnapshot(),
        "12:text/plain:Hello Vertex|load:Hello Vertex:load;end:2:loadend;|text/plain:false|Hello Vertex\n",
        result);

    ExpectEqual(
        "js/dom/form-collections-submit",
        RunFormCollectionSubmitSnapshot(),
        "1:f|4:Vertex:fr|f:q|2:1:fr|Vertex:view:fr|submit:go:true;\n",
        result);

    ExpectEqual(
        "js/async/promise-constructor-and-combinators",
        RunPromiseConstructorCombinatorsSnapshot(),
        "p:made|made-chain|all=ab|race=r|settled=fulfilled/rejected\n",
        result);

    ExpectEqual(
        "js/dom/event-cancellation-bubbling-and-targets",
        RunDomEventCancellationSnapshot(),
        "e:btn:btn/btn|result=false|default=true\n",
        result);

    ExpectEqual(
        "js/dom/element-click-dispatches-and-toggles",
        RunDomElementClickSnapshot(),
        "start:prop:true:check;listener:check:false;change:true;button:click;checked=true\n",
        result);

    ExpectEqual(
        "js/dom/dataset-writes-reflect-to-attributes",
        RunDomDatasetReflectionSnapshot(),
        "open|box|kept\n",
        result);

    ExpectEqual(
        "js/dom/classlist-replace-item-length",
        RunDomClassListCompatSnapshot(),
        "2:b|true|c d|2|c d\n",
        result);

    ExpectEqual(
        "js/dom/fragments-deep-clone-and-contains",
        RunDomFragmentCloneMutationSnapshot(),
        "Source|empty|true|true|direct=p#frag-child,div#deep,div#shallow\n",
        result);

    ExpectEqual(
        "js/dom/node-surface-connectivity-and-identity",
        RunDomNodeSurfaceSnapshot(),
        "true|9:9:9|true:false|newAB|newAB|button:2:true:Root|4:1:5,5|2:fresh\n",
        result);

    ExpectEqual(
        "js/dom/dispatchevent-runs-property-handlers",
        RunDomDispatchEventPropertyHandlerSnapshot(),
        "start:prop:btn/btn;outer:false;true\n",
        result);

    ExpectEqual(
        "js/dom/focus-blur-active-element",
        RunDomFocusBlurSnapshot(),
        "start:focus:search;blur:search;search|body\n",
        result);

    ExpectEqual(
        "js/dom/document-element-shortcuts-are-wrappers",
        RunDocumentElementShortcutsSnapshot(),
        "html|head|body\n",
        result);

    ExpectEqual(
        "js/dom/modern-mutation-conveniences",
        RunDomModernMutationConveniencesSnapshot(),
        "li:H|#text:zero|hr:|li:A|li:B|#text:after-b|li:C|#text:done|#text:text-tail|li:M;adj=HM:HzeroABafter-bCdonetext-tailM;tail=no;section=1\n",
        result);

    ExpectEqual(
        "js/dom/attribute-ns-and-sibling-compat",
        RunDomAttributeNsSiblingSnapshot(),
        "true/false/true|class,data-a,hidden,id|svg:http://www.w3.org/2000/svg:0 0 1 1|svg|svg\n",
        result);

    ExpectEqual(
        "js/web-platform/storage-length-is-live",
        RunStorageLengthSnapshot(),
        "0|2:b|1|0\n",
        result);

    ExpectEqual(
        "js/web-platform/document-cookie-writes-cookie-jar",
        RunDocumentCookieWriteSnapshot(),
        "vertex_fill_a=1; vertex_fill_b=2\n",
        result);

    ExpectEqual(
        "js/web-platform/matchmedia-listeners-dispatch",
        RunMatchMediaListenerSnapshot(),
        "start:on:true;legacy:true;true:true\n",
        result);

    ExpectEqual(
        "js/web-platform/storage-url-and-matchmedia",
        RunWebPlatformSurfaceSnapshot(),
        "42|tab|/w/index.php|Vertex|view|1,3|true\n",
        result);

    ExpectEqual(
        "js/web-platform/general-platform-stubs",
        RunGeneralPlatformStubsSnapshot(),
        "CSS1Compat|visible|false|https://example.org/wiki/Page|example.org|en-US:4:0|true:true|false|2:1|4:true:true|7|P||true:true:a\\ b|true:true|start:vertex:true:true;|clip|idle:false:true;\n",
        result);

    ExpectEqual(
        "js/engine/script-budget-profile-counters",
        RunScriptBudgetProfileSnapshot(),
        "small=yes\nlarge=no\nattempted=2\nexecuted=1\nskipped=1\ntimed=yes\n",
        result);

    ExpectEqual(
        "js/engine/macrotask-pump-respects-frame-budget",
        RunMacrotaskBudgetSnapshot(),
        "first=2:pending final=5:empty\n",
        result);

    ExpectEqual(
        "js/dom/property-writes-reflect-to-node",
        RunDomReflectionSnapshot(),
        "class=b c style=display: none text=Bye\n",
        result);

    ExpectEqual(
        "js/dom/cssom-geometry",
        RunDomCssomGeometrySnapshot(),
        "123x45:12,7:123px:block\n",
        result);

    ExpectEqual(
        "js/dom/observers-fire",
        RunDomObserverSnapshot(),
        "1:0:0->2:1:1\n",
        result);

    ExpectEqual(
        "js/dom/observer-lifecycle-methods",
        RunObserverLifecycleSnapshot(),
        "io:1:box:true:true;1:other:true:true;1:box:true:true;|take=0|ro:1:box:10x20:true;\n",
        result);

    ExpectEqual(
        "js/dom/observer-options-and-size-aliases",
        RunObserverOptionsSnapshot(),
        "0,0.5,1:1px 2px:1:true|1:1:1\n",
        result);

    ExpectEqual(
        "js/window/lifecycle-surface",
        RunWindowLifecycleSurfaceSnapshot(),
        "false:true:docs:https://example.org/wiki/Page|true:true\n",
        result);

    ExpectEqual(
        "js/dom/direct-property-writes-coalesce-repaint",
        RunDomDirtyCoalescingSnapshot(),
        "1\n",
        result);

    ExpectEqual(
        "js/dom/paint-only-style-skips-layout-dirty",
        RunDomPaintOnlyDirtySnapshot(),
        "layout=1 paint=1\n",
        result);

    ExpectJsResult(
        "object/static-property",
        "var product = { title: 'book' };\n"
        "product.price = 12;\n"
        "__result = product.title + ':' + product.price;\n",
        "string: book:12\n",
        result);

    ExpectJsResult(
        "function/direct-call",
        "function add(left, right) { return left + right; }\n"
        "__result = add(2, 3);\n",
        "number: 5\n",
        result);

    ExpectJsResult(
        "array/callback-reduce",
        "var total = 0;\n"
        "[1, 2, 3].forEach(function (value) { total += value; });\n"
        "__result = total;\n",
        "number: 6\n",
        result);

    ExpectJsResult(
        "destructuring/array-rest-preserves-tail",
        "var [first, ...rest] = [1, 2, 3, 4];\n"
        "__result = first + ':' + rest.length + ':' + rest[0] + ':' + rest[2];\n",
        "string: 1:3:2:4\n",
        result);

    ExpectJsResult(
        "destructuring/object-rest-copies-remaining-props",
        "var { a, ...rest } = { a: 1, b: 2, c: 3 };\n"
        "__result = a + ':' + rest.b + ':' + rest.c + ':' + rest.a;\n",
        "string: 1:2:3:undefined\n",
        result);

    ExpectJsResult(
        "weak-collections/object-identity-keys",
        "var a = {}, b = {};\n"
        "var wm = new WeakMap();\n"
        "wm.set(a, 'a');\n"
        "wm.set(b, 'b');\n"
        "var ws = new WeakSet();\n"
        "ws.add(a);\n"
        "__result = wm.get(a) + ':' + wm.get(b) + ':' + wm.has({}) + ':' + ws.has(a) + ':' + ws.has(b);\n",
        "string: a:b:false:true:false\n",
        result);

    ExpectJsResult(
        "map-set/object-identity-and-live-size",
        "var a = {}, b = {};\n"
        "var m = new Map([[a, 'a']]);\n"
        "m.set(b, 'b');\n"
        "m.set(a, 'aa');\n"
        "var s = new Set([a, a]);\n"
        "s.add(b);\n"
        "var seen = 'seen:';\n"
        "m.forEach(function(value, key) { if (key === a || key === b) seen += value; });\n"
        "__result = m.get(a) + ':' + m.get(b) + ':' + m.has({}) + ':' + m.size + ':' + s.has(a) + ':' + s.has({}) + ':' + s.size + ':' + seen;\n",
        "string: aa:b:false:2:true:false:2:seen:aab\n",
        result);

    ExpectJsResult(
        "date/constructor-parse-and-iso",
        "var epoch = new Date(0);\n"
        "var parsed = new Date('1970-01-01T00:00:01.000Z');\n"
        "__result = epoch.toISOString() + '|' + parsed.getTime() + '|' + Date.parse('1970-01-01T00:00:02.000Z') + '|' + (Date.now() > 0);\n",
        "string: 1970-01-01T00:00:00.000Z|1000|2000|true\n",
        result);

    ExpectJsResult(
        "compiler/call-args-stay-in-order-after-a-void-call",
        // Regression test: a call whose result is discarded (obj.noop()),
        // immediately followed by a multi-arg call, used to scramble the
        // second call's arguments. The compiler allocated argument registers
        // via the general free-list (allocReg()), which can return
        // non-contiguous registers once anything has been freed out of
        // stack order — but the VM's OP_CALL/OP_NEW opcodes read arguments
        // as REG(fnReg+1..fnReg+argc), assuming strict contiguity with
        // fnReg. Enough "clutter" from prior statements broke that
        // assumption, silently reading the wrong register for an argument
        // (observed as swapped values for a 2-arg call). Fixed by reserving
        // fnReg + all argument registers as one contiguous run
        // (allocRegRun) instead of allocating them one at a time.
        "var obj = {\n"
        "  noop: function() { return undefined; },\n"
        "  two: function(a, b) { return a + ':' + b; }\n"
        "};\n"
        "var obj = {\n"
        "  style: '',\n"
        "  fourArg: function(a,b,c,d) { return undefined; },\n"
        "  noop: function() { return undefined; },\n"
        "  sixArg: function(a,b,c,d,e,f) { return undefined; },\n"
        "  two: function(a, b) { return a + ':' + b; }\n"
        "};\n"
        "obj.style = 'red';\n"
        "obj.fourArg(10, 10, 50, 50);\n"
        "obj.noop();\n"
        "obj.style = 'blue';\n"
        "obj.sixArg(80, 80, 15, 0, 6, false);\n"
        "obj.noop();\n"
        "obj.noop();\n"
        "__result = obj.two(60, 5);\n",
        "string: 60:5\n",
        result);

    ExpectJsResult(
        "generator/basic-next-shape",
        "function* values() { yield 7; }\n"
        "var iter = values();\n"
        "var first = iter.next();\n"
        "var second = iter.next();\n"
        "__result = first.value + ':' + first.done + ':' + second.value + ':' + second.done;\n",
        "string: 7:false:undefined:true\n",
        result);

    ExpectJsResult(
        "symbol/registry-and-description-compat",
        "var a = Symbol('topic');\n"
        "var b = Symbol('topic');\n"
        "var sharedA = Symbol.for('mediawiki');\n"
        "var sharedB = Symbol.for('mediawiki');\n"
        "var bracket = Symbol['for']('portal');\n"
        "__result = (a === b) + ':' + (sharedA === sharedB) + ':' + typeof sharedA + ':' + Symbol.keyFor(sharedA) + ':' + Symbol.keyFor(bracket) + ':' + Symbol.description(a) + ':' + Symbol.toString(a);\n",
        "string: false:true:string:mediawiki:portal:topic:Symbol(topic)\n",
        result);

    ExpectEqual(
        "js/engine/dom-registration-does-not-recurse",
        RunEngineDomRegistrationSnapshot(),
        "registered\n",
        result);

    ExpectEqual(
        "js/engine/deep-dom-registration-does-not-overflow",
        RunEngineDeepDomRegistrationSnapshot(),
        "registered\n",
        result);

    ExpectEqual(
        "js/dom/canvas-getcontext-2d-surface-and-no-op-without-backend",
        RunCanvasContext2DSnapshot(),
        "true:true:300x150:640x480\n",
        result);

    ExpectEqual(
        "js/dom/canvas-width-height-writes-reflect-to-attributes",
        RunCanvasResizeSnapshot(),
        "500x400\n",
        result);

    ExpectEqual(
        "js/dom/websocket-constructor-shape-and-constants",
        RunWebSocketShapeSnapshot(),
        "true:true:0:ws://127.0.0.1:9/\n",
        result);

    return result;
}
