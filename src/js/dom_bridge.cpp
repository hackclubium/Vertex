#include "js/dom_bridge.h"
#include "js/canvas_bridge.h"
#include "css/stylesheet.h"
#include "html/parser.h"
#include "network/resource_cache.h"
#include "network/cookies.h"
#include "network/url.h"
#include "network/websocket.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#define NATIVE(name) [](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue
#define ARG(i) (args.size() > (size_t)(i) ? args[i] : JsValue::undefined())
#define ARG_STR(i) ARG(i).toString()
#define ARG_NUM(i) ARG(i).toNumber()
#define ARG_INT(i) ARG(i).toInt32()

static void addNative(VM& vm, JsObject* obj, const std::string& name, NativeFn fn) {
    auto* fnObj = vm.gc().newNativeFunction(std::move(fn), name);
    obj->setProp(name, JsValue::object(fnObj));
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

static bool eventFlag(JsObject* ev, const std::string& name) {
    if (!ev) return false;
    return ev->getProp(name).toBool();
}

static void installEventMethods(VM& vm, JsObject* ev) {
    if (!ev) return;
    addNative(vm, ev, "preventDefault", [ev](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        if (eventFlag(ev, "cancelable"))
            ev->setProp("defaultPrevented", JsValue::boolean(true));
        return JsValue::undefined();
    });
    addNative(vm, ev, "stopPropagation", [ev](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        ev->setProp("__vertexStopped", JsValue::boolean(true));
        return JsValue::undefined();
    });
}

// ── class-token helpers (for classList) ───────────────────────────────────────
static std::vector<std::string> classTokens(const std::string& cls) {
    std::vector<std::string> out;
    std::istringstream ss(cls);
    std::string t;
    while (ss >> t) out.push_back(t);
    return out;
}
static std::string classJoin(const std::vector<std::string>& toks) {
    std::string s;
    for (size_t i = 0; i < toks.size(); ++i) { if (i) s += ' '; s += toks[i]; }
    return s;
}
static bool classHas(const std::vector<std::string>& toks, const std::string& t) {
    return std::find(toks.begin(), toks.end(), t) != toks.end();
}

// ── Node store ────────────────────────────────────────────────────────────────
// We keep shared_ptr alive by storing them in a global registry keyed by raw ptr.
// This is safe because the VM and document share the same lifetime.

static std::unordered_map<Node*, std::shared_ptr<Node>> g_nodeStore;
static std::unordered_map<Node*, JsObject*> g_wrapperStore;
static std::unordered_set<JsObject*> g_datasetObjects;
static std::unordered_set<JsObject*> g_readyWrappers;
static std::shared_ptr<Node> g_currentDocument;
static const char* kNamespaceAttr = "__vertex_namespaceURI";

// Event listeners: node -> [(eventName, callbackFn), ...]
struct EventListener { std::string event; JsValue fn; };
static std::unordered_map<Node*, std::vector<EventListener>> g_eventListeners;
static std::vector<EventListener> g_windowEventListeners;
// Persistent GC roots for cached DOM wrappers. These live at STABLE heap
// addresses so the GC can mark them safely (rooting a stack local was a
// use-after-return bug). Cleared per document in registerDom().
static std::vector<std::unique_ptr<JsValue>> g_wrapperRoots;
static std::vector<std::unique_ptr<JsValue>> g_observerRoots;
// Keeps each `new WebSocket(...)` object alive independent of script-level
// reachability — a real WebSocket keeps firing events even if the script
// drops its only reference, and OpenWebSocket()'s callback only ever holds a
// raw JsValue (see registerDom()'s "fetch"/"WebSocket" globals), not a GC
// root by itself. Cleared alongside g_observerRoots on navigation; not
// removed per-connection-close (same coarse per-page lifetime the observer
// roots already use, not per-object precision).
static std::vector<std::unique_ptr<JsValue>> g_wsRoots;

struct MutationObserverEntry {
    JsObject* observer = nullptr;
    Node* target = nullptr;
    bool active = false;
    bool subtree = false;
    bool attributes = true;
    bool childList = true;
};
static std::vector<MutationObserverEntry> g_mutationObservers;

static std::shared_ptr<Node> getShared(Node* raw) {
    auto it = g_nodeStore.find(raw);
    if (it != g_nodeStore.end()) return it->second;
    return nullptr;
}

static void registerSubtree(const std::shared_ptr<Node>& node) {
    if (!node) return;
    g_nodeStore[node.get()] = node;
    for (auto& child : node->children)
        registerSubtree(child);
}

static std::shared_ptr<Node> cloneDomNode(Node* source, bool deep) {
    if (!source) return nullptr;
    auto clone = std::make_shared<Node>();
    clone->type = source->type;
    clone->tagName = source->tagName;
    clone->text = source->text;
    clone->attrs = source->attrs;
    if (deep) {
        for (const auto& child : source->children) {
            auto childClone = cloneDomNode(child.get(), true);
            if (childClone) clone->appendChild(childClone);
        }
    }
    registerSubtree(clone);
    return clone;
}

static bool isDocumentFragment(const Node* node) {
    return node && node->tagName == "#document-fragment";
}

static void detachFromParent(Node* child) {
    if (!child || !child->parent) return;
    auto& siblings = child->parent->children;
    siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
        [&](const std::shared_ptr<Node>& node) { return node.get() == child; }),
        siblings.end());
    child->parent = nullptr;
}

static void insertSharedChild(Node* parent, std::shared_ptr<Node> child, Node* before = nullptr) {
    if (!parent || !child) return;
    auto insertOne = [&](std::shared_ptr<Node> node, Node* ref) {
        if (!node) return;
        detachFromParent(node.get());
        node->parent = parent;
        auto& children = parent->children;
        if (!ref) {
            children.push_back(std::move(node));
            return;
        }
        auto it = std::find_if(children.begin(), children.end(),
            [&](const std::shared_ptr<Node>& c) { return c.get() == ref; });
        if (it != children.end()) children.insert(it, std::move(node));
        else children.push_back(std::move(node));
    };

    if (isDocumentFragment(child.get())) {
        std::vector<std::shared_ptr<Node>> fragmentChildren;
        fragmentChildren.swap(child->children);
        for (auto& fragmentChild : fragmentChildren) {
            if (fragmentChild) fragmentChild->parent = nullptr;
            insertOne(std::move(fragmentChild), before);
        }
        return;
    }
    insertOne(std::move(child), before);
}

static bool nodeContains(Node* root, Node* needle) {
    if (!root || !needle) return false;
    for (Node* cur = needle; cur; cur = cur->parent)
        if (cur == root) return true;
    return false;
}

static Node* rootNode(Node* n) {
    while (n && n->parent) n = n->parent;
    return n;
}

static Node* ownerDocumentNode(Node* n) {
    Node* root = rootNode(n);
    if (root && root->type == NodeType::Document) return root;
    return g_currentDocument.get();
}

static bool isConnectedNode(Node* n) {
    Node* root = rootNode(n);
    return root && root->type == NodeType::Document;
}

static Node* siblingNode(Node* node, int direction) {
    if (!node || !node->parent) return nullptr;
    const auto& siblings = node->parent->children;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() != node) continue;
        if (direction < 0) return i > 0 ? siblings[i - 1].get() : nullptr;
        return i + 1 < siblings.size() ? siblings[i + 1].get() : nullptr;
    }
    return nullptr;
}

static Node* findFirstTag(Node* root, const std::string& tag) {
    if (!root) return nullptr;
    std::vector<Node*> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        if (!n) continue;
        if (n->tagName == tag) return n;
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return nullptr;
}

static Node* documentRootFor(Node* node) {
    if (!node) return nullptr;
    Node* root = node;
    while (root->parent) root = root->parent;
    return root;
}

static void setDocumentActiveElement(VM& vm, Node* node) {
    JsValue doc = vm.getGlobal("document");
    if (!doc.isObject()) return;
    auto shared = getShared(node);
    if (!shared && node)
        shared = std::shared_ptr<Node>(node, [](Node*) {});
    doc.asObject()->setProp("activeElement", shared ? wrapNode(vm, shared) : JsValue::null());
}

Node* unwrapNode(JsValue val) {
    if (!val.isObject()) return nullptr;
    return static_cast<Node*>(val.asObject()->domNode);
}

static std::string textContent(Node* n);

static std::string trimCopy(std::string s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool hasAttr(const Node* n, const std::string& name) {
    return n && n->attrs.find(name) != n->attrs.end();
}

static float parseFloatOr(const std::string& s, float fallback) {
    if (s.empty()) return fallback;
    try {
        size_t consumed = 0;
        float v = std::stof(s, &consumed);
        return consumed > 0 ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

static std::map<std::string, std::string> parseStyleMap(const std::string& raw) {
    std::map<std::string, std::string> out;
    std::istringstream ss(raw);
    std::string decl;
    while (std::getline(ss, decl, ';')) {
        size_t pos = decl.find(':');
        if (pos == std::string::npos) continue;
        std::string key = lowerCopy(trimCopy(decl.substr(0, pos)));
        std::string val = trimCopy(decl.substr(pos + 1));
        if (!key.empty()) out[key] = val;
    }
    return out;
}

static bool cssSupportsDeclaration(const std::string& property, const std::string& value) {
    std::string prop = lowerCopy(trimCopy(property));
    std::string val = lowerCopy(trimCopy(value));
    if (prop.empty() || val.empty()) return false;
    if (prop == "display") {
        return val == "none" || val == "block" || val == "inline" || val == "inline-block"
            || val == "flex" || val == "grid" || val == "table" || val == "table-cell"
            || val == "contents" || val == "flow-root" || val == "block flow"
            || val == "inline flow" || val == "inline flow-root";
    }
    if (prop == "position") return val == "static" || val == "relative" || val == "absolute" || val == "fixed" || val == "sticky";
    if (prop == "color" || prop == "background" || prop == "background-color" || prop == "border-color")
        return ParseCssColor(value).valid || val == "transparent" || val == "currentcolor";
    if (prop == "width" || prop == "height" || prop == "min-width" || prop == "max-width"
        || prop == "min-height" || prop == "max-height" || prop == "margin" || prop == "padding"
        || prop == "top" || prop == "right" || prop == "bottom" || prop == "left")
        return val == "auto" || val == "0" || val.find("px") != std::string::npos
            || val.find("%") != std::string::npos || val.find("calc(") != std::string::npos;
    if (prop == "transform") return val == "none" || val.find("translate") != std::string::npos || val.find("scale") != std::string::npos || val.find("rotate") != std::string::npos;
    if (prop == "border-collapse") return val == "collapse" || val == "separate";
    if (prop == "caption-side") return val == "top" || val == "bottom";
    if (prop == "direction") return val == "ltr" || val == "rtl";
    return prop == "opacity" || prop == "visibility" || prop == "box-sizing"
        || prop == "white-space" || prop == "overflow" || prop == "cursor"
        || prop == "user-select" || prop == "pointer-events" || prop == "font-size"
        || prop == "font-family" || prop == "font-weight" || prop == "line-height";
}

static bool cssSupportsCondition(const std::string& condition) {
    std::string c = trimCopy(condition);
    if (c.size() >= 2 && c.front() == '(' && c.back() == ')') c = trimCopy(c.substr(1, c.size() - 2));
    size_t colon = c.find(':');
    if (colon == std::string::npos) return false;
    return cssSupportsDeclaration(c.substr(0, colon), c.substr(colon + 1));
}

static std::string numberCss(float v) {
    if (std::fabs(v - std::round(v)) < 0.01f) return std::to_string((int)std::round(v));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

static std::string pxCss(float v) {
    return numberCss(v) + "px";
}

static std::string colorCss(const CssColor& c) {
    if (!c.valid) return "";
    int r = (int)std::round(std::clamp(c.r, 0.f, 1.f) * 255.f);
    int g = (int)std::round(std::clamp(c.g, 0.f, 1.f) * 255.f);
    int b = (int)std::round(std::clamp(c.b, 0.f, 1.f) * 255.f);
    if (c.a < 0.999f) return "rgba(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) + ", " + numberCss(c.a) + ")";
    return "rgb(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) + ")";
}

static JsValue makeEventObject(VM& vm, const std::string& ctorName, const std::string& type, JsValue options = JsValue::undefined()) {
    auto* ev = vm.gc().newObject(ObjKind::Plain);
    bool bubbles = false;
    bool cancelable = false;
    JsValue detail = JsValue::null();
    if (options.isObject()) {
        auto* opts = options.asObject();
        bubbles = opts->getProp("bubbles").toBool();
        cancelable = opts->getProp("cancelable").toBool();
        JsValue d = opts->getProp("detail");
        if (!d.isUndefined()) detail = d;
    }
    ev->setProp("type", type.empty() ? JsValue::undefined() : vm.str(type));
    ev->setProp("bubbles", JsValue::boolean(bubbles));
    ev->setProp("cancelable", JsValue::boolean(cancelable));
    ev->setProp("target", JsValue::null());
    ev->setProp("currentTarget", JsValue::null());
    ev->setProp("defaultPrevented", JsValue::boolean(false));
    ev->setProp("timeStamp", JsValue::number((double)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0));
    if (ctorName == "CustomEvent") ev->setProp("detail", detail);
    if (options.isObject()) {
        auto* opts = options.asObject();
        static const char* passthrough[] = {
            "key", "code", "altKey", "ctrlKey", "shiftKey", "metaKey",
            "clientX", "clientY", "screenX", "screenY", "button", "buttons",
            "deltaX", "deltaY", "deltaZ", "inputType", "data"
        };
        for (const char* prop : passthrough) {
            JsValue value = opts->getProp(prop);
            if (!value.isUndefined()) ev->setProp(prop, value);
        }
    }
    installEventMethods(vm, ev);
    addNative(vm, ev, "initEvent", [ev](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        ev->setProp("type", args.empty() ? JsValue::undefined() : args[0]);
        ev->setProp("bubbles", JsValue::boolean(args.size() > 1 ? args[1].toBool() : false));
        ev->setProp("cancelable", JsValue::boolean(args.size() > 2 ? args[2].toBool() : false));
        ev->setProp("defaultPrevented", JsValue::boolean(false));
        return JsValue::undefined();
    });
    addNative(vm, ev, "initCustomEvent", [ev](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        ev->setProp("type", args.empty() ? JsValue::undefined() : args[0]);
        ev->setProp("bubbles", JsValue::boolean(args.size() > 1 ? args[1].toBool() : false));
        ev->setProp("cancelable", JsValue::boolean(args.size() > 2 ? args[2].toBool() : false));
        ev->setProp("detail", args.size() > 3 ? args[3] : JsValue::null());
        ev->setProp("defaultPrevented", JsValue::boolean(false));
        return JsValue::undefined();
    });
    return JsValue::object(ev);
}

static std::string defaultDisplayForTag(const Node* n) {
    if (!n || n->type != NodeType::Element) return "";
    const std::string& t = n->tagName;
    if (t == "span" || t == "a" || t == "b" || t == "strong" || t == "i"
        || t == "em" || t == "small" || t == "label") return "inline";
    if (t == "script" || t == "style" || t == "template" || (t == "dialog" && !hasAttr(n, "open"))) return "none";
    return "block";
}

static std::string displayName(const ComputedStyle& s, const Node* n) {
    switch (s.display) {
        case 1: return "block";
        case 2: return "inline";
        case 3: return "none";
        case 4: return "flex";
        case 5: return "table";
        case 6: return "table-cell";
        case 7: return "inline-block";
        case 8: return "list-item";
        case 9: return "table-row";
        case 10: return "table-row-group";
        case 11: return "grid";
        case 12: return "flow-root";
        case 13: return "contents";
        default: return defaultDisplayForTag(n);
    }
}

static std::string positionName(const ComputedStyle& s) {
    switch (s.positionMode) {
        case 1: return "relative";
        case 2: return "absolute";
        case 3: return "fixed";
        case 4: return "sticky";
        default: return "static";
    }
}

static std::string overflowName(const ComputedStyle& s) {
    switch (s.overflowMode) {
        case 1: return "hidden";
        case 2: return "auto";
        case 3: return "scroll";
        default: return "visible";
    }
}

struct DomMetrics {
    float left = 0;
    float top = 0;
    float width = 0;
    float height = 0;
};

static DomMetrics computeDomMetrics(Node* n) {
    DomMetrics m;
    if (!n || n->type != NodeType::Element) return m;
    ComputedStyle s = ParseInlineStyle(n->attr("style"));
    ResolveStyleVariables(s);
    if (s.display == 3 || (n->tagName == "dialog" && !hasAttr(n, "open"))) return m;

    m.width = s.width >= 0 ? s.width : parseFloatOr(n->attr("width"), -1.f);
    m.height = s.height >= 0 ? s.height : parseFloatOr(n->attr("height"), -1.f);

    auto directTextLen = [](Node* node) {
        size_t len = 0;
        for (const auto& child : node->children) {
            if (child && child->type == NodeType::Text) {
                len += child->text.size();
                if (len > 4096) return len;
            }
        }
        return len;
    };

    if (m.width < 0) {
        size_t textLen = directTextLen(n);
        m.width = textLen == 0 ? 0.f : std::max(16.f, (float)textLen * 8.f);
        if (textLen > 0) m.width += 8.f;
    }
    if (m.height < 0) {
        m.height = directTextLen(n) > 0 ? std::max(16.f, s.lineHeight > 0 ? s.lineHeight : 16.f) : 0.f;
    }

    float padX = std::max(0.f, s.paddingLeft) + std::max(0.f, s.paddingRight);
    float padY = std::max(0.f, s.paddingTop) + std::max(0.f, s.paddingBottom);
    float borderX = std::max(0.f, s.borderLeftWidth >= 0 ? s.borderLeftWidth : s.borderWidth)
                  + std::max(0.f, s.borderRightWidth >= 0 ? s.borderRightWidth : s.borderWidth);
    float borderY = std::max(0.f, s.borderTopWidth >= 0 ? s.borderTopWidth : s.borderWidth)
                  + std::max(0.f, s.borderBottomWidth >= 0 ? s.borderBottomWidth : s.borderWidth);
    if (s.boxSizing != 1) {
        m.width += padX + borderX;
        m.height += padY + borderY;
    }
    if (s.leftSet && !s.leftPercent) m.left = s.left;
    if (s.topSet && !s.topPercent) m.top = s.top;
    if (s.transformSet) {
        if (!s.transformTxPercent) m.left += s.transformTx;
        if (!s.transformTyPercent) m.top += s.transformTy;
        if (s.transformScale > 0) {
            m.width *= s.transformScale;
            m.height *= s.transformScale;
        }
    }
    return m;
}

static void markDomDirty(VM& vm, Node* target, const std::string& type);

// ── Build the text content of a subtree ──────────────────────────────────────

static std::string textContent(Node* n) {
    if (!n) return "";
    std::string s;
    std::vector<Node*> stack;
    stack.push_back(n);
    while (!stack.empty()) {
        Node* cur = stack.back();
        stack.pop_back();
        if (!cur) continue;
        if (cur->type == NodeType::Text) {
            s += cur->text;
            continue;
        }
        for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return s;
}

static std::string innerHTML(VM& vm, Node* n);
static JsValue wrapNodeInternal(VM& vm, std::shared_ptr<Node> node, bool materializeRelations);

static bool isFormControl(const Node* n) {
    if (!n || n->type != NodeType::Element) return false;
    return n->tagName == "input" || n->tagName == "select"
        || n->tagName == "textarea" || n->tagName == "button";
}

static void collectDescendants(Node* root, std::vector<Node*>& out,
                               const std::function<bool(Node*)>& accept) {
    if (!root) return;
    std::vector<Node*> stack;
    for (auto it = root->children.rbegin(); it != root->children.rend(); ++it)
        stack.push_back(it->get());
    while (!stack.empty()) {
        Node* cur = stack.back();
        stack.pop_back();
        if (!cur) continue;
        if (accept(cur)) out.push_back(cur);
        for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it)
            stack.push_back(it->get());
    }
}

static Node* findElementById(Node* root, const std::string& id) {
    if (!root || id.empty()) return nullptr;
    std::vector<Node*> stack{ root };
    while (!stack.empty()) {
        Node* cur = stack.back();
        stack.pop_back();
        if (!cur) continue;
        if (cur->type == NodeType::Element && cur->attr("id") == id) return cur;
        for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return nullptr;
}

static Node* formOwner(Node* control) {
    for (Node* cur = control; cur; cur = cur->parent)
        if (cur->tagName == "form") return cur;
    return nullptr;
}

static std::vector<Node*> formControls(Node* form) {
    std::vector<Node*> controls;
    collectDescendants(form, controls, [](Node* n) { return isFormControl(n); });
    return controls;
}

static Node* selectedOption(Node* select) {
    if (!select || select->tagName != "select") return nullptr;
    Node* first = nullptr;
    std::vector<Node*> options;
    collectDescendants(select, options, [](Node* n) { return n && n->tagName == "option"; });
    for (Node* option : options) {
        if (!first) first = option;
        if (hasAttr(option, "selected")) return option;
    }
    return first;
}

static int selectedIndex(Node* select) {
    if (!select || select->tagName != "select") return -1;
    std::vector<Node*> options;
    collectDescendants(select, options, [](Node* n) { return n && n->tagName == "option"; });
    for (size_t i = 0; i < options.size(); ++i)
        if (hasAttr(options[i], "selected")) return (int)i;
    return options.empty() ? -1 : 0;
}

static std::string elementValue(Node* n) {
    if (!n) return "";
    if (n->tagName == "select") {
        Node* option = selectedOption(n);
        if (!option) return "";
        std::string value = option->attr("value");
        return value.empty() ? textContent(option) : value;
    }
    if (n->tagName == "option") {
        std::string value = n->attr("value");
        return value.empty() ? textContent(n) : value;
    }
    if (n->tagName == "textarea") return n->attr("value").empty() ? textContent(n) : n->attr("value");
    return n->attr("value");
}

static void setElementValue(Node* n, const std::string& value) {
    if (!n) return;
    if (n->tagName == "select") {
        std::vector<Node*> options;
        collectDescendants(n, options, [](Node* node) { return node && node->tagName == "option"; });
        bool matched = false;
        for (Node* option : options) {
            bool isMatch = elementValue(option) == value;
            if (isMatch && !matched) {
                option->attrs["selected"] = "selected";
                matched = true;
            } else {
                option->attrs.erase("selected");
            }
        }
        return;
    }
    n->attrs["value"] = value;
}

static JsObject* makeNodeCollection(VM& vm, const std::vector<Node*>& nodes) {
    auto* arr = newArrayWithPrototype(vm);
    for (Node* node : nodes) {
        auto shared = getShared(node);
        if (!shared) {
            shared = std::shared_ptr<Node>(node, [](Node*) {});
            g_nodeStore[node] = shared;
        }
        JsValue wrapped = wrapNodeInternal(vm, shared, false);
        arr->arrayPush(wrapped);
        std::string id = node->attr("id");
        std::string name = node->attr("name");
        if (!id.empty()) arr->setProp(id, wrapped);
        if (!name.empty()) arr->setProp(name, wrapped);
    }
    arr->setProp("length", JsValue::integer((int32_t)nodes.size()));
    return arr;
}

struct RangeState {
    Node* node = nullptr;
    bool contentsOnly = false;
};

static std::shared_ptr<Node> makeDocumentFragmentFromHtml(const std::string& html) {
    auto frag = Node::makeElement("#document-fragment");
    frag->type = NodeType::Document;
    frag->tagName = "#document-fragment";
    auto parsed = ParseHtml(html);
    Node* body = findFirstTag(parsed.get(), "body");
    Node* source = body ? body : parsed.get();
    if (source) {
        for (const auto& child : source->children) {
            auto clone = cloneDomNode(child.get(), true);
            if (clone) frag->appendChild(clone);
        }
    }
    registerSubtree(frag);
    return frag;
}

static std::shared_ptr<Node> cloneRangeContents(const RangeState& state) {
    auto frag = Node::makeElement("#document-fragment");
    frag->type = NodeType::Document;
    frag->tagName = "#document-fragment";
    if (state.node) {
        if (state.contentsOnly) {
            for (const auto& child : state.node->children) {
                auto clone = cloneDomNode(child.get(), true);
                if (clone) frag->appendChild(clone);
            }
        } else {
            auto clone = cloneDomNode(state.node, true);
            if (clone) frag->appendChild(clone);
        }
    }
    registerSubtree(frag);
    return frag;
}

static JsValue makeRangeObject(VM& vm) {
    auto state = std::make_shared<RangeState>();
    auto* range = vm.gc().newObject(ObjKind::Plain);
    auto updateCollapsed = [range, state]() {
        range->setProp("collapsed", JsValue::boolean(!state->node));
    };
    updateCollapsed();
    addNative(vm, range, "selectNode", [state, updateCollapsed](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        state->node = args.empty() ? nullptr : unwrapNode(args[0]);
        state->contentsOnly = false;
        updateCollapsed();
        return JsValue::undefined();
    });
    addNative(vm, range, "selectNodeContents", [state, updateCollapsed](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        state->node = args.empty() ? nullptr : unwrapNode(args[0]);
        state->contentsOnly = true;
        updateCollapsed();
        return JsValue::undefined();
    });
    addNative(vm, range, "setStart", [state, updateCollapsed](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        state->node = args.empty() ? nullptr : unwrapNode(args[0]);
        state->contentsOnly = true;
        updateCollapsed();
        return JsValue::undefined();
    });
    addNative(vm, range, "setEnd", [](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        return JsValue::undefined();
    });
    addNative(vm, range, "collapse", [state, updateCollapsed](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        state->node = nullptr;
        state->contentsOnly = false;
        updateCollapsed();
        return JsValue::undefined();
    });
    addNative(vm, range, "toString", [state](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        return vm.str(textContent(state->node));
    });
    addNative(vm, range, "cloneContents", [state](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        return wrapNode(vm, cloneRangeContents(*state));
    });
    addNative(vm, range, "createContextualFragment", [](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        return wrapNode(vm, makeDocumentFragmentFromHtml(args.empty() ? "" : args[0].toString()));
    });
    return JsValue::object(range);
}

static bool observerCoversNode(const MutationObserverEntry& entry, Node* target) {
    if (!entry.active || !entry.target || !target) return false;
    if (entry.target == target) return true;
    if (!entry.subtree) return false;
    for (Node* cur = target->parent; cur; cur = cur->parent)
        if (cur == entry.target) return true;
    return false;
}

static void notifyMutationObservers(VM& vm, Node* target, const std::string& type) {
    for (auto& entry : g_mutationObservers) {
        if (!observerCoversNode(entry, target)) continue;
        if (type == "attributes" && !entry.attributes) continue;
        if (type == "childList" && !entry.childList) continue;
        JsValue callback = entry.observer ? entry.observer->getProp("_callback") : JsValue::undefined();
        if (!callback.isCallable()) continue;

        auto* record = vm.gc().newObject(ObjKind::Plain);
        record->setProp("type", vm.str(type));
        auto shared = getShared(target);
        record->setProp("target", shared ? wrapNode(vm, shared) : JsValue::null());
        auto* records = vm.gc().newArray();
        records->arrayPush(JsValue::object(record));
        try {
            vm.call(callback, JsValue::undefined(), { JsValue::object(records), JsValue::object(entry.observer) });
        } catch (...) {
        }
    }
}

static bool g_domDirtyCoalesced = false;
static bool g_domPaintDirtyCoalesced = false;
static DomBridgeCallbacks g_domCallbacks;
static float g_windowScrollX = 0.f;
static float g_windowScrollY = 0.f;

static void syncWindowScrollGlobals(VM& vm) {
    vm.setGlobal("pageXOffset", JsValue::integer((int32_t)std::round(g_windowScrollX)));
    vm.setGlobal("pageYOffset", JsValue::integer((int32_t)std::round(g_windowScrollY)));
    vm.setGlobal("scrollX",     JsValue::integer((int32_t)std::round(g_windowScrollX)));
    vm.setGlobal("scrollY",     JsValue::integer((int32_t)std::round(g_windowScrollY)));
}

static void setWindowScrollTo(VM& vm, float x, float y) {
    g_windowScrollX = std::max(0.f, x);
    g_windowScrollY = std::max(0.f, y);
    syncWindowScrollGlobals(vm);
}

static bool IsPaintOnlyDirtyType(const std::string& type) {
    if (type == "canvas:draw") return true;
    if (type.rfind("style:", 0) != 0) return false;
    std::string prop = type.substr(6);
    for (char& c : prop) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return prop == "color"
        || prop == "background"
        || prop == "background-color"
        || prop == "border-color"
        || prop == "outline-color"
        || prop == "text-decoration"
        || prop == "text-decoration-color"
        || prop == "opacity"
        || prop == "visibility";
}

void notifyDomDirtyCoalesced(VM& vm, bool affectsLayout) {
    vm.domDirty = true;
    if (!affectsLayout && vm.onDomPaintDirty) {
        if (!g_domPaintDirtyCoalesced) {
            g_domPaintDirtyCoalesced = true;
            vm.onDomPaintDirty();
        }
        return;
    }
    if (!g_domDirtyCoalesced && vm.onDomDirty) {
        g_domDirtyCoalesced = true;
        vm.onDomDirty();
    }
}

static void markDomDirty(VM& vm, Node* target, const std::string& type) {
    notifyMutationObservers(vm, target, type);
    notifyDomDirtyCoalesced(vm, !IsPaintOnlyDirtyType(type));
}

ICanvasSurface* GetCanvasSurfaceForNode(Node* n) {
    return g_domCallbacks.getCanvasSurface ? g_domCallbacks.getCanvasSurface(n) : nullptr;
}

void MarkCanvasDirty(VM& vm, Node* n) {
    markDomDirty(vm, n, "canvas:draw");
}

// Call this from the platform timer to reset coalescing for the next batch.
void resetDomDirtyCoalesce() {
    g_domDirtyCoalesced = false;
    g_domPaintDirtyCoalesced = false;
}

static bool CanEagerSerialize(Node* n) {
    if (!n) return false;
    struct Entry { Node* node; int depth; };
    std::vector<Entry> stack;
    stack.push_back({ n, 0 });
    size_t count = 0;
    while (!stack.empty()) {
        Entry cur = stack.back();
        stack.pop_back();
        if (!cur.node) continue;
        if (++count > 512 || cur.depth > 64) return false;
        for (auto& child : cur.node->children)
            stack.push_back({ child.get(), cur.depth + 1 });
    }
    return true;
}

static std::string outerHTML(VM& vm, Node* n) {
    if (!n) return "";
    if (n->type == NodeType::Text) {
        std::string s = n->text;
        std::string r;
        for (char c : s) {
            if      (c == '<') r += "&lt;";
            else if (c == '>') r += "&gt;";
            else if (c == '&') r += "&amp;";
            else r += c;
        }
        return r;
    }
    std::string html = "<" + n->tagName;
    for (auto& [k, v] : n->attrs) {
        if (k == kNamespaceAttr) continue;
        html += " " + k + "=\"" + v + "\"";
    }
    html += ">" + innerHTML(vm, n) + "</" + n->tagName + ">";
    return html;
}

static std::string innerHTML(VM& vm, Node* n) {
    std::string s;
    for (auto& c : n->children) s += outerHTML(vm, c.get());
    return s;
}

static std::shared_ptr<Node> nodeFromDomArg(JsValue value) {
    if (Node* existing = unwrapNode(value)) {
        auto shared = getShared(existing);
        if (shared) return shared;
    }
    auto text = Node::makeText(value.toString());
    registerSubtree(text);
    return text;
}

static std::vector<std::shared_ptr<Node>> nodesFromDomArgs(const std::vector<JsValue>& args) {
    std::vector<std::shared_ptr<Node>> out;
    out.reserve(args.size());
    for (const auto& arg : args) {
        auto node = nodeFromDomArg(arg);
        if (node) out.push_back(node);
    }
    return out;
}

static void insertDomArgs(Node* parent, Node* before, const std::vector<JsValue>& args) {
    if (!parent) return;
    for (auto& node : nodesFromDomArgs(args))
        insertSharedChild(parent, node, before);
}

static void insertHtmlFragment(Node* parent, Node* before, const std::string& html) {
    if (!parent) return;
    auto frag = makeDocumentFragmentFromHtml(html);
    std::vector<std::shared_ptr<Node>> nodes = frag ? frag->children : std::vector<std::shared_ptr<Node>>{};
    for (auto& node : nodes)
        insertSharedChild(parent, node, before);
}

static bool adjacentInsertionPoint(Node* target, const std::string& rawPosition,
                                   Node*& parent, Node*& before) {
    parent = nullptr;
    before = nullptr;
    if (!target) return false;
    std::string position = lowerCopy(rawPosition);
    if (position == "beforebegin") {
        if (!target->parent) return false;
        parent = target->parent;
        before = target;
        return true;
    }
    if (position == "afterbegin") {
        parent = target;
        before = target->children.empty() ? nullptr : target->children.front().get();
        return true;
    }
    if (position == "beforeend") {
        parent = target;
        return true;
    }
    if (position == "afterend") {
        if (!target->parent) return false;
        parent = target->parent;
        before = siblingNode(target, 1);
        return true;
    }
    return false;
}

static void replaceNodeWithArgs(Node* node, const std::vector<JsValue>& args) {
    if (!node || !node->parent) return;
    Node* parent = node->parent;
    Node* before = siblingNode(node, 1);
    detachFromParent(node);
    insertDomArgs(parent, before, args);
}

static void normalizeTextChildren(Node* node) {
    if (!node) return;
    std::vector<std::shared_ptr<Node>> normalized;
    for (auto& child : node->children) {
        if (!child) continue;
        if (child->type == NodeType::Text) {
            if (child->text.empty()) continue;
            if (!normalized.empty() && normalized.back()->type == NodeType::Text) {
                normalized.back()->text += child->text;
                continue;
            }
        } else {
            normalizeTextChildren(child.get());
        }
        child->parent = node;
        normalized.push_back(child);
    }
    node->children = std::move(normalized);
}

static bool isBeforeInDocumentOrder(Node* a, Node* b) {
    if (!a || !b) return false;
    Node* root = rootNode(a);
    std::vector<Node*> stack{ root };
    while (!stack.empty()) {
        Node* cur = stack.back();
        stack.pop_back();
        if (cur == a) return true;
        if (cur == b) return false;
        for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return false;
}

static std::string ariaAttrForProperty(const std::string& key) {
    if (key.rfind("aria", 0) != 0 || key.size() <= 4) return "";
    std::string out = "aria-";
    for (size_t i = 4; i < key.size(); ++i) {
        char c = key[i];
        if (c >= 'A' && c <= 'Z') {
            if (i > 4) out += '-';
            out += (char)(c - 'A' + 'a');
        } else {
            out += c;
        }
    }
    return out;
}

static bool isIdentChar(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_' || c == '-' || c == '\\';
}

static std::string stripQuotes(std::string s) {
    s = trimCopy(std::move(s));
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
        || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

static std::string datasetAttrName(const std::string& key) {
    std::string out = "data-";
    for (char c : key) {
        if (c >= 'A' && c <= 'Z') {
            out += '-';
            out += (char)(c - 'A' + 'a');
        } else {
            out += c;
        }
    }
    return out;
}

static std::vector<std::string> splitSelectorTopLevel(const std::string& input, char delimiter) {
    std::vector<std::string> out;
    std::string cur;
    int bracket = 0, paren = 0;
    char quote = 0;
    for (char c : input) {
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '[') bracket++;
        else if (c == ']') bracket = std::max(0, bracket - 1);
        else if (c == '(') paren++;
        else if (c == ')') paren = std::max(0, paren - 1);
        if (c == delimiter && bracket == 0 && paren == 0) {
            std::string part = trimCopy(cur);
            if (!part.empty()) out.push_back(part);
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string part = trimCopy(cur);
    if (!part.empty()) out.push_back(part);
    return out;
}

struct SelectorPart {
    std::string simple;
    char combinatorBefore = 0;
};

struct ParsedSelectorGroup {
    std::vector<SelectorPart> parts;
};

static std::vector<SelectorPart> parseSelectorParts(const std::string& selector) {
    std::vector<SelectorPart> parts;
    std::string cur;
    int bracket = 0, paren = 0;
    char quote = 0;
    char pendingCombinator = 0;

    auto flush = [&]() {
        std::string simple = trimCopy(cur);
        if (!simple.empty()) {
            parts.push_back({ simple, parts.empty() ? char(0) : (pendingCombinator ? pendingCombinator : ' ') });
            cur.clear();
            pendingCombinator = ' ';
        }
    };

    for (size_t i = 0; i < selector.size(); ++i) {
        char c = selector[i];
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '[') { bracket++; cur += c; continue; }
        if (c == ']') { bracket = std::max(0, bracket - 1); cur += c; continue; }
        if (c == '(') { paren++; cur += c; continue; }
        if (c == ')') { paren = std::max(0, paren - 1); cur += c; continue; }

        if (bracket == 0 && paren == 0 && (c == '>' || c == '+' || c == '~')) {
            flush();
            pendingCombinator = c;
            while (i + 1 < selector.size()
                && std::isspace(static_cast<unsigned char>(selector[i + 1]))) ++i;
            continue;
        }
        if (bracket == 0 && paren == 0 && std::isspace(static_cast<unsigned char>(c))) {
            flush();
            if (pendingCombinator == 0) pendingCombinator = ' ';
            continue;
        }
        cur += c;
    }
    flush();
    return parts;
}

static std::vector<ParsedSelectorGroup> parseSelectorGroups(const std::string& selector) {
    std::vector<ParsedSelectorGroup> groups;
    for (const auto& group : splitSelectorTopLevel(selector, ',')) {
        auto parts = parseSelectorParts(group);
        if (!parts.empty()) groups.push_back({ std::move(parts) });
    }
    return groups;
}

static const std::vector<ParsedSelectorGroup>& CachedSelectorGroups(const std::string& selector) {
    static std::unordered_map<std::string, std::vector<ParsedSelectorGroup>> cache;
    constexpr size_t kMaxSelectorCacheEntries = 512;
    auto it = cache.find(selector);
    if (it != cache.end()) return it->second;
    if (cache.size() >= kMaxSelectorCacheEntries) cache.clear();
    auto inserted = cache.emplace(selector, parseSelectorGroups(selector));
    return inserted.first->second;
}

static const std::vector<ParsedSelectorGroup>& cachedSelectorGroups(const std::string& selector) {
    return CachedSelectorGroups(selector);
}

static bool hasClassToken(const Node* n, const std::string& cls) {
    if (!n || cls.empty()) return false;
    return classHas(classTokens(n->attr("class")), cls);
}

static std::vector<Node*> elementSiblings(const Node* n) {
    std::vector<Node*> out;
    if (!n || !n->parent) return out;
    for (const auto& child : n->parent->children)
        if (child && child->type == NodeType::Element)
            out.push_back(child.get());
    return out;
}

static Node* previousElementSibling(const Node* n) {
    Node* prev = nullptr;
    for (Node* sibling : elementSiblings(n)) {
        if (sibling == n) return prev;
        prev = sibling;
    }
    return nullptr;
}

static Node* nextElementSibling(const Node* n) {
    bool found = false;
    for (Node* sibling : elementSiblings(n)) {
        if (found) return sibling;
        if (sibling == n) found = true;
    }
    return nullptr;
}

static std::vector<Node*> typeSiblings(const Node* n) {
    std::vector<Node*> out;
    if (!n || !n->parent) return out;
    std::string tag = lowerCopy(n->tagName);
    for (const auto& child : n->parent->children) {
        if (child && child->type == NodeType::Element && lowerCopy(child->tagName) == tag)
            out.push_back(child.get());
    }
    return out;
}

static int indexInList(Node* n, const std::vector<Node*>& nodes, bool fromEnd) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] != n) continue;
        return fromEnd ? (int)(nodes.size() - i) : (int)i + 1;
    }
    return 0;
}

static bool parseNthExpression(std::string expr, int& a, int& b) {
    expr = lowerCopy(trimCopy(expr));
    expr.erase(std::remove_if(expr.begin(), expr.end(),
        [](unsigned char c) { return std::isspace(c); }), expr.end());
    if (expr.empty()) return false;
    if (expr == "odd") { a = 2; b = 1; return true; }
    if (expr == "even") { a = 2; b = 0; return true; }

    size_t npos = expr.find('n');
    try {
        if (npos == std::string::npos) {
            a = 0;
            b = std::stoi(expr);
            return true;
        }
        std::string coeff = expr.substr(0, npos);
        if (coeff.empty() || coeff == "+") a = 1;
        else if (coeff == "-") a = -1;
        else a = std::stoi(coeff);

        std::string offset = expr.substr(npos + 1);
        b = offset.empty() ? 0 : std::stoi(offset);
        return true;
    } catch (...) {
        return false;
    }
}

static bool nthMatches(int index, const std::string& expr) {
    int a = 0, b = 0;
    if (index <= 0 || !parseNthExpression(expr, a, b)) return false;
    if (a == 0) return index == b;
    int diff = index - b;
    if (diff % a != 0) return false;
    int n = diff / a;
    return n >= 0;
}

static bool attrMatches(const Node* n, const std::string& raw) {
    std::string expr = trimCopy(raw);
    if (expr.empty()) return false;
    std::istringstream ss(expr);
    std::string first;
    ss >> first;
    expr = first.empty() ? expr : first;

    const std::vector<std::string> ops = { "~=", "|=", "^=", "$=", "*=", "=" };
    std::string name, op, value;
    size_t pos = std::string::npos;
    for (const auto& candidate : ops) {
        pos = expr.find(candidate);
        if (pos != std::string::npos) {
            op = candidate;
            name = trimCopy(expr.substr(0, pos));
            value = stripQuotes(expr.substr(pos + candidate.size()));
            break;
        }
    }
    if (op.empty()) name = trimCopy(expr);
    if (name.empty() || !hasAttr(n, name)) return false;
    if (op.empty()) return true;

    std::string actual = n->attr(name);
    if (op == "=") return actual == value;
    if (op == "^=") return actual.rfind(value, 0) == 0;
    if (op == "$=") return actual.size() >= value.size()
        && actual.compare(actual.size() - value.size(), value.size(), value) == 0;
    if (op == "*=") return actual.find(value) != std::string::npos;
    if (op == "~=") return classHas(classTokens(actual), value);
    if (op == "|=") return actual == value || actual.rfind(value + "-", 0) == 0;
    return false;
}

static bool matchesSelector(Node* n, const std::string& selector, Node* scope);
static bool matchesSelectorParts(Node* n, const std::vector<SelectorPart>& parts, int index, Node* scope);

static bool matchesSelectorCached(Node* n, const std::string& selector, Node* scope) {
    const auto& groups = CachedSelectorGroups(selector);
    for (const auto& group : groups) {
        if (!group.parts.empty() && matchesSelectorParts(n, group.parts, (int)group.parts.size() - 1, scope))
            return true;
    }
    return false;
}

static size_t findTopLevelOfKeyword(const std::string& text) {
    int paren = 0;
    int bracket = 0;
    char quote = 0;
    for (size_t i = 0; i + 3 < text.size(); ++i) {
        char c = text[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') { ++paren; continue; }
        if (c == ')' && paren > 0) { --paren; continue; }
        if (c == '[') { ++bracket; continue; }
        if (c == ']' && bracket > 0) { --bracket; continue; }
        if (paren == 0 && bracket == 0
            && std::isspace(static_cast<unsigned char>(c))
            && i + 3 < text.size()
            && lowerCopy(text.substr(i + 1, 2)) == "of"
            && std::isspace(static_cast<unsigned char>(text[i + 3]))) {
            return i;
        }
    }
    return std::string::npos;
}

static bool descendantMatchesSelector(Node* root, const std::string& selector, Node* scope) {
    if (!root) return false;
    for (const auto& child : root->children) {
        if (!child || child->type != NodeType::Element) continue;
        Node* candidate = child.get();
        if (matchesSelectorCached(candidate, selector, scope)) return true;
        if (descendantMatchesSelector(candidate, selector, scope)) return true;
    }
    return false;
}

static bool hasRelativeSelector(Node* n, const std::string& rawSelector, Node* scope) {
    std::string selector = trimCopy(rawSelector);
    if (!n || selector.empty()) return false;
    if (selector[0] == '>') {
        std::string childSelector = trimCopy(selector.substr(1));
        for (const auto& child : n->children) {
            if (child && child->type == NodeType::Element
                && matchesSelectorCached(child.get(), childSelector, scope))
                return true;
        }
        return false;
    }
    if (selector[0] == '+') {
        Node* next = nextElementSibling(n);
        return next && matchesSelectorCached(next, trimCopy(selector.substr(1)), scope);
    }
    if (selector[0] == '~') {
        std::string siblingSelector = trimCopy(selector.substr(1));
        for (Node* next = nextElementSibling(n); next; next = nextElementSibling(next)) {
            if (matchesSelectorCached(next, siblingSelector, scope)) return true;
        }
        return false;
    }
    return descendantMatchesSelector(n, selector, scope);
}

static bool matchesPseudo(Node* n, const std::string& name, const std::string& arg, Node* scope) {
    if (!n) return false;
    if (name == "not") return !matchesSelectorCached(n, arg, scope);
    if (name == "is" || name == "where") return matchesSelectorCached(n, arg, scope);
    if (name == "has") {
        for (const auto& selector : splitSelectorTopLevel(arg, ',')) {
            if (hasRelativeSelector(n, selector, scope)) return true;
        }
        return false;
    }
    if (name == "checked") return hasAttr(n, "checked") || hasAttr(n, "selected");
    if (name == "disabled") return hasAttr(n, "disabled");
    if (name == "enabled") return !hasAttr(n, "disabled");
    if (name == "empty") {
        for (const auto& child : n->children) {
            if (!child) continue;
            if (child->type == NodeType::Element) return false;
            if (child->type == NodeType::Text && !trimCopy(child->text).empty()) return false;
        }
        return true;
    }
    if (name == "first-child" || name == "last-child" || name == "only-child") {
        auto siblings = elementSiblings(n);
        if (siblings.empty()) return false;
        if (name == "first-child") return siblings.front() == n;
        if (name == "last-child") return siblings.back() == n;
        return siblings.size() == 1 && siblings.front() == n;
    }
    if (name == "first-of-type" || name == "last-of-type" || name == "only-of-type") {
        auto siblings = typeSiblings(n);
        if (siblings.empty()) return false;
        if (name == "first-of-type") return siblings.front() == n;
        if (name == "last-of-type") return siblings.back() == n;
        return siblings.size() == 1 && siblings.front() == n;
    }
    if (name == "nth-child" || name == "nth-last-child"
        || name == "nth-of-type" || name == "nth-last-of-type") {
        bool ofType = name.find("of-type") != std::string::npos;
        bool fromEnd = name.find("nth-last") == 0;
        auto siblings = ofType ? typeSiblings(n) : elementSiblings(n);
        std::string nthArg = arg;
        if (!ofType) {
            size_t ofPos = findTopLevelOfKeyword(arg);
            if (ofPos != std::string::npos) {
                nthArg = trimCopy(arg.substr(0, ofPos));
                std::string filter = trimCopy(arg.substr(ofPos + 4));
                siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
                    [&](Node* sibling) { return !matchesSelectorCached(sibling, filter, scope); }),
                    siblings.end());
            }
        }
        return nthMatches(indexInList(n, siblings, fromEnd), nthArg);
    }
    if (name == "root") return n->parent && n->parent->type == NodeType::Document;
    if (name == "scope") return n == scope;
    return false;
}

static bool matchesSimpleSelector(Node* n, const std::string& simple, Node* scope) {
    if (!n || n->type != NodeType::Element) return false;
    size_t i = 0;
    bool sawAnySelector = false;
    while (i < simple.size()) {
        char c = simple[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '*') { sawAnySelector = true; ++i; continue; }
        if (c == '#') {
            ++i;
            size_t start = i;
            while (i < simple.size() && isIdentChar(simple[i])) ++i;
            if (n->attr("id") != simple.substr(start, i - start)) return false;
            sawAnySelector = true;
            continue;
        }
        if (c == '.') {
            ++i;
            size_t start = i;
            while (i < simple.size() && isIdentChar(simple[i])) ++i;
            if (!hasClassToken(n, simple.substr(start, i - start))) return false;
            sawAnySelector = true;
            continue;
        }
        if (c == '[') {
            int depth = 1;
            size_t start = ++i;
            char quote = 0;
            while (i < simple.size() && depth > 0) {
                char ac = simple[i];
                if (quote) {
                    if (ac == quote) quote = 0;
                } else if (ac == '"' || ac == '\'') quote = ac;
                else if (ac == '[') depth++;
                else if (ac == ']') {
                    if (--depth == 0) break;
                }
                ++i;
            }
            if (i >= simple.size() || !attrMatches(n, simple.substr(start, i - start))) return false;
            ++i;
            sawAnySelector = true;
            continue;
        }
        if (c == ':') {
            ++i;
            size_t start = i;
            while (i < simple.size() && isIdentChar(simple[i])) ++i;
            std::string name = lowerCopy(simple.substr(start, i - start));
            std::string arg;
            if (i < simple.size() && simple[i] == '(') {
                int depth = 1;
                size_t argStart = ++i;
                char quote = 0;
                while (i < simple.size() && depth > 0) {
                    char pc = simple[i];
                    if (quote) {
                        if (pc == quote) quote = 0;
                    } else if (pc == '"' || pc == '\'') quote = pc;
                    else if (pc == '(') depth++;
                    else if (pc == ')') {
                        if (--depth == 0) break;
                    }
                    ++i;
                }
                if (i >= simple.size()) return false;
                arg = simple.substr(argStart, i - argStart);
                ++i;
            }
            if (!matchesPseudo(n, name, arg, scope)) return false;
            sawAnySelector = true;
            continue;
        }

        size_t start = i;
        while (i < simple.size() && isIdentChar(simple[i])) ++i;
        if (start == i) return false;
        std::string tag = lowerCopy(simple.substr(start, i - start));
        if (tag != lowerCopy(n->tagName)) return false;
        sawAnySelector = true;
    }
    return sawAnySelector;
}

static bool matchesSelectorParts(Node* n, const std::vector<SelectorPart>& parts, int index, Node* scope) {
    if (index < 0) return true;
    if (!matchesSimpleSelector(n, parts[(size_t)index].simple, scope)) return false;
    if (index == 0) return true;

    char combinator = parts[(size_t)index].combinatorBefore;
    if (combinator == '>') {
        return n->parent && matchesSelectorParts(n->parent, parts, index - 1, scope);
    }
    if (combinator == '+') {
        Node* prev = previousElementSibling(n);
        return prev && matchesSelectorParts(prev, parts, index - 1, scope);
    }
    if (combinator == '~') {
        for (Node* prev = previousElementSibling(n); prev; prev = previousElementSibling(prev))
            if (matchesSelectorParts(prev, parts, index - 1, scope)) return true;
        return false;
    }

    for (Node* cur = n->parent; cur; cur = cur->parent)
        if (matchesSelectorParts(cur, parts, index - 1, scope)) return true;
    return false;
}

static bool matchesSelector(Node* n, const std::string& selector, Node* scope) {
    const auto& groups = cachedSelectorGroups(selector);
    for (const auto& group : groups) {
        if (!group.parts.empty() && matchesSelectorParts(n, group.parts, (int)group.parts.size() - 1, scope))
            return true;
    }
    return false;
}

static bool matchesParsedSelector(Node* n, const std::vector<ParsedSelectorGroup>& groups, Node* scope) {
    for (const auto& group : groups) {
        if (!group.parts.empty() && matchesSelectorParts(n, group.parts, (int)group.parts.size() - 1, scope))
            return true;
    }
    return false;
}

static std::vector<std::shared_ptr<Node>> domQueryAll(Node* root, const std::vector<ParsedSelectorGroup>& groups) {
    std::vector<std::shared_ptr<Node>> result;
    size_t visited = 0;
    int depth = 0;
    std::function<void(Node*)> walk = [&](Node* n) {
        if (++depth > 1000) { --depth; return; }   // guard against deep/cyclic trees
        for (auto& c : n->children) {
            if (++visited > 200000) break;          // guard against runaway traversal
            if (matchesParsedSelector(c.get(), groups, root)) result.push_back(c);
            walk(c.get());
        }
        --depth;
    };
    walk(root);
    return result;
}

static std::vector<std::shared_ptr<Node>> domQueryAll(Node* root, const std::string& sel) {
    return domQueryAll(root, cachedSelectorGroups(sel));
}

static std::shared_ptr<Node> domQueryFirst(Node* root, const std::string& sel) {
    const auto& groups = cachedSelectorGroups(sel);
    size_t visited = 0;
    int depth = 0;
    std::shared_ptr<Node> result;
    std::function<void(Node*)> walk = [&](Node* n) {
        if (result || ++depth > 1000) { if (depth > 0) --depth; return; }
        for (auto& c : n->children) {
            if (result || ++visited > 200000) break;
            if (matchesParsedSelector(c.get(), groups, root)) { result = c; break; }
            walk(c.get());
        }
        --depth;
    };
    walk(root);
    return result;
}

// ── wrapNode ─────────────────────────────────────────────────────────────────

static JsValue wrapNodeInternal(VM& vm, std::shared_ptr<Node> node, bool materializeRelations) {
    if (!node) return JsValue::null();

    // Register in store
    Node* raw = node.get();
    g_nodeStore[raw] = node;

    auto cached = g_wrapperStore.find(raw);
    if (cached != g_wrapperStore.end()) {
        if (!materializeRelations || g_readyWrappers.find(cached->second) != g_readyWrappers.end())
            return JsValue::object(cached->second);
        g_wrapperStore.erase(cached);
    }

    auto* obj = vm.gc().newObject(ObjKind::DomWrapper);
    obj->domNode = raw;
    g_wrapperStore[raw] = obj;

    // Keep the wrapper alive while it stays cached in g_wrapperStore by rooting
    // it at a stable heap address. Previously this rooted the address of a local
    // (&objValue); after the function returned, the GC dereferenced that
    // dangling stack pointer during marking — a non-deterministic crash/hang
    // that worsened as more nodes were wrapped (large DOMs).
    g_wrapperRoots.push_back(std::make_unique<JsValue>(JsValue::object(obj)));
    vm.gc().addRoot(g_wrapperRoots.back().get());
    JsValue objValue = *g_wrapperRoots.back();

    // ── Core properties ──

    if (node->type == NodeType::Text) {
        obj->setProp("nodeType", JsValue::integer(3));
        obj->setProp("nodeName", vm.str("#text"));
        obj->setProp("data", vm.str(node->text));
        obj->setProp("nodeValue", vm.str(node->text));
    } else if (node->type == NodeType::Document) {
        obj->setProp("nodeType", JsValue::integer(9));
        obj->setProp("nodeName", vm.str("#document"));
        obj->setProp("nodeValue", JsValue::null());
    } else {
        obj->setProp("nodeType", JsValue::integer(1));
        obj->setProp("nodeName", vm.str(node->tagName));
        obj->setProp("tagName",  vm.str(node->tagName));
        obj->setProp("localName",vm.str(node->tagName));
        obj->setProp("namespaceURI", node->attrs.count(kNamespaceAttr)
            ? vm.str(node->attr(kNamespaceAttr))
            : JsValue::null());
        obj->setProp("nodeValue", JsValue::null());
    }

    // ── id/className ──
    auto addPropAccessor = [&](const std::string& attr) {
        std::string val = node->attr(attr);
        obj->setProp(attr, vm.str(val));
    };
    addPropAccessor("id");
    addPropAccessor("class");
    obj->setProp("className", vm.str(node->attr("class")));

    // ── textContent / innerHTML / outerHTML ──
    // Only serialize small subtrees eagerly. For large nodes (document, html,
    // body…) this is skipped: serializing every wrapped node's whole subtree is
    // O(n^2) over a page and makes DOM-heavy scripts crawl to a halt.
    if (CanEagerSerialize(raw)) {
        obj->setProp("textContent", vm.str(textContent(raw)));
        obj->setProp("innerHTML",   vm.str(innerHTML(vm, raw)));
        obj->setProp("outerHTML",   vm.str(outerHTML(vm, raw)));
    } else {
        obj->setProp("textContent", vm.str(""));
        obj->setProp("innerHTML",   vm.str(""));
        obj->setProp("outerHTML",   vm.str(""));
    }

    // ── attributes map ──
    {
        auto* attrsObj = vm.gc().newObject(ObjKind::Plain);
        for (auto& [k, v] : node->attrs) attrsObj->setProp(k, vm.str(v));
        obj->setProp("attributes", JsValue::object(attrsObj));
    }

    // ── style object ──
    {
        auto* style = vm.gc().newObject(ObjKind::Plain);
        style->domNode = raw;  // Plain+domNode marks a style obj for the set hook
        // Parse inline style
        const std::string& styleStr = node->attr("style");
        std::istringstream ss(styleStr);
        std::string decl;
        while (std::getline(ss, decl, ';')) {
            auto pos = decl.find(':');
            if (pos != std::string::npos) {
                std::string k = decl.substr(0, pos), v = decl.substr(pos+1);
                auto trim = [](std::string s) { size_t b=s.find_first_not_of(" \t"); size_t e=s.find_last_not_of(" \t"); return b==std::string::npos?"":s.substr(b,e-b+1); };
                k = trim(k); v = trim(v);
                if (!k.empty()) style->setProp(k, vm.str(v));
            }
        }
        // setProperty / removeProperty helpers
        addNative(vm, style, "setProperty", [](VM& vm2, JsValue t, std::vector<JsValue> a) -> JsValue {
            if (!t.isObject()) return JsValue::undefined();
            std::string prop = a.size()>0?a[0].toString():"";
            std::string val  = a.size()>1?a[1].toString():"";
            if (!prop.empty()) {
                t.asObject()->setProp(prop, vm2.str(val));
                if (t.asObject()->domNode && vm2.onDomPropSet)
                    vm2.onDomPropSet(t.asObject(), prop, vm2.str(val));
            }
            return JsValue::undefined();
        });
        addNative(vm, style, "removeProperty", [](VM& vm2, JsValue t, std::vector<JsValue> a) -> JsValue {
            if (!t.isObject() || a.empty()) return JsValue::undefined();
            std::string prop = a[0].toString();
            JsValue old = t.asObject()->getProp(prop);
            t.asObject()->props.erase(prop);
            if (t.asObject()->domNode && vm2.onDomPropSet)
                vm2.onDomPropSet(t.asObject(), prop, vm2.str(""));
            return old;
        });
        obj->setProp("style", JsValue::object(style));
    }

    // ── DOM traversal ──
    auto addNativeM = [&](const std::string& name, NativeFn fn) {
        obj->setProp(name, JsValue::object(vm.gc().newNativeFunction(fn, name)));
    };

    addNativeM("getAttribute", NATIVE("getAttribute") {
        Node* n = unwrapNode(thisVal);
        if (!n) return JsValue::null();
        std::string k = args.empty() ? "" : args[0].toString();
        if (k == kNamespaceAttr) return JsValue::null();
        auto it = n->attrs.find(k);
        return it != n->attrs.end() ? vm.str(it->second) : JsValue::null();
    });
    addNativeM("setAttribute", NATIVE("setAttribute") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.size() < 2) return JsValue::undefined();
        n->attrs[args[0].toString()] = args[1].toString();
        markDomDirty(vm, n, "attributes");
        return JsValue::undefined();
    });
    addNativeM("removeAttribute", NATIVE("removeAttribute") {
        Node* n = unwrapNode(thisVal);
        if (n && !args.empty()) n->attrs.erase(args[0].toString());
        markDomDirty(vm, n, "attributes");
        return JsValue::undefined();
    });
    addNativeM("hasAttribute", NATIVE("hasAttribute") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::boolean(false);
        return JsValue::boolean(n->attrs.count(args[0].toString()) > 0);
    });
    addNativeM("toggleAttribute", NATIVE("toggleAttribute") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::boolean(false);
        std::string name = ARG_STR(0);
        bool has = n->attrs.count(name) > 0;
        bool want = args.size() > 1 ? ARG(1).toBool() : !has;
        if (want) n->attrs[name] = "";
        else n->attrs.erase(name);
        markDomDirty(vm, n, "attributes");
        return JsValue::boolean(want);
    });
    addNativeM("getAttributeNames", NATIVE("getAttributeNames") {
        Node* n = unwrapNode(thisVal);
        auto* names = newArrayWithPrototype(vm);
        if (!n) return JsValue::object(names);
        for (const auto& [k, _] : n->attrs) {
            if (k == kNamespaceAttr) continue;
            names->arrayPush(vm.str(k));
        }
        return JsValue::object(names);
    });

    // classList — back-pointer to the owner node lets the methods mutate the
    // real class attribute and trigger relayout (used by show/hide toggles).
    {
        auto* cl = vm.gc().newObject(ObjKind::Plain);
        cl->domNode = raw;  // owner element, so unwrapNode(thisVal) resolves it
        auto syncClassListLength = [](JsObject* list, Node* n) {
            if (list && n)
                list->setProp("length", JsValue::integer((int32_t)classTokens(n->attr("class")).size()));
        };
        syncClassListLength(cl, raw);
        addNative(vm, cl, "add", NATIVE("classList_add") {
            Node* n = unwrapNode(thisVal);
            if (!n) return JsValue::undefined();
            auto toks = classTokens(n->attr("class"));
            for (auto& a : args) { std::string t = a.toString();
                if (!t.empty() && !classHas(toks, t)) toks.push_back(t); }
            n->attrs["class"] = classJoin(toks);
            if (thisVal.isObject())
                thisVal.asObject()->setProp("length", JsValue::integer((int32_t)toks.size()));
            markDomDirty(vm, n, "attributes");
            return JsValue::undefined();
        });
        addNative(vm, cl, "remove", NATIVE("classList_remove") {
            Node* n = unwrapNode(thisVal);
            if (!n) return JsValue::undefined();
            auto toks = classTokens(n->attr("class"));
            for (auto& a : args) { std::string t = a.toString();
                toks.erase(std::remove(toks.begin(), toks.end(), t), toks.end()); }
            n->attrs["class"] = classJoin(toks);
            if (thisVal.isObject())
                thisVal.asObject()->setProp("length", JsValue::integer((int32_t)toks.size()));
            markDomDirty(vm, n, "attributes");
            return JsValue::undefined();
        });
        addNative(vm, cl, "toggle", NATIVE("classList_toggle") {
            Node* n = unwrapNode(thisVal);
            if (!n || args.empty()) return JsValue::boolean(false);
            std::string t = args[0].toString();
            auto toks = classTokens(n->attr("class"));
            bool has = classHas(toks, t);
            bool want = args.size() > 1 ? args[1].toBool() : !has;
            if (want && !has) toks.push_back(t);
            else if (!want && has) toks.erase(std::remove(toks.begin(), toks.end(), t), toks.end());
            n->attrs["class"] = classJoin(toks);
            if (thisVal.isObject())
                thisVal.asObject()->setProp("length", JsValue::integer((int32_t)toks.size()));
            markDomDirty(vm, n, "attributes");
            return JsValue::boolean(want);
        });
        addNative(vm, cl, "replace", NATIVE("classList_replace") {
            Node* n = unwrapNode(thisVal);
            if (!n || args.size() < 2) return JsValue::boolean(false);
            std::string oldToken = args[0].toString();
            std::string newToken = args[1].toString();
            auto toks = classTokens(n->attr("class"));
            auto it = std::find(toks.begin(), toks.end(), oldToken);
            if (it == toks.end() || newToken.empty()) return JsValue::boolean(false);
            if (!classHas(toks, newToken)) *it = newToken;
            else toks.erase(it);
            n->attrs["class"] = classJoin(toks);
            if (thisVal.isObject())
                thisVal.asObject()->setProp("length", JsValue::integer((int32_t)toks.size()));
            markDomDirty(vm, n, "attributes");
            return JsValue::boolean(true);
        });
        addNative(vm, cl, "item", NATIVE("classList_item") {
            Node* n = unwrapNode(thisVal);
            if (!n || args.empty()) return JsValue::null();
            auto toks = classTokens(n->attr("class"));
            int index = ARG_INT(0);
            if (index < 0 || index >= static_cast<int>(toks.size())) return JsValue::null();
            return vm.str(toks[(size_t)index]);
        });
        addNative(vm, cl, "contains", NATIVE("classList_contains") {
            Node* n = unwrapNode(thisVal);
            if (!n || args.empty()) return JsValue::boolean(false);
            return JsValue::boolean(classHas(classTokens(n->attr("class")), args[0].toString()));
        });
        addNative(vm, cl, "toString", NATIVE("classList_toString") {
            Node* n = unwrapNode(thisVal);
            return vm.str(n ? n->attr("class") : "");
        });
        obj->setProp("classList", JsValue::object(cl));
    }

    // children / childNodes
    if (materializeRelations) {
        auto* children  = newArrayWithPrototype(vm);
        auto* childNodes = newArrayWithPrototype(vm);
        for (auto& c : node->children) {
            JsValue wrapped = wrapNodeInternal(vm, c, false);
            childNodes->arrayPush(wrapped);
            if (c->type == NodeType::Element) children->arrayPush(wrapped);
        }
        obj->setProp("children",   JsValue::object(children));
        obj->setProp("childNodes", JsValue::object(childNodes));
        obj->setProp("childElementCount", JsValue::integer((int32_t)children->arrayLength()));
    } else {
        obj->setProp("children", JsValue::object(newArrayWithPrototype(vm)));
        obj->setProp("childNodes", JsValue::object(newArrayWithPrototype(vm)));
        obj->setProp("childElementCount", JsValue::integer(0));
    }

    // firstChild / lastChild / firstElementChild / lastElementChild
    if (materializeRelations && !node->children.empty()) {
        obj->setProp("firstChild", wrapNodeInternal(vm, node->children.front(), false));
        obj->setProp("lastChild",  wrapNodeInternal(vm, node->children.back(), false));
        for (auto& c : node->children) {
            if (c->type == NodeType::Element) { obj->setProp("firstElementChild", wrapNodeInternal(vm, c, false)); break; }
        }
        for (int i = (int)node->children.size()-1; i >= 0; i--) {
            if (node->children[i]->type == NodeType::Element) { obj->setProp("lastElementChild", wrapNodeInternal(vm, node->children[i], false)); break; }
        }
    } else {
        obj->setProp("firstChild",        JsValue::null());
        obj->setProp("lastChild",         JsValue::null());
        obj->setProp("firstElementChild", JsValue::null());
        obj->setProp("lastElementChild",  JsValue::null());
    }

    // parentNode / parentElement
    if (materializeRelations && node->parent) {
        auto parentShared = getShared(node->parent);
        if (parentShared) {
            JsValue parentWrapped = wrapNodeInternal(vm, parentShared, false);
            obj->setProp("parentNode",    parentWrapped);
            obj->setProp("parentElement", parentWrapped);
        }
    } else {
        obj->setProp("parentNode",    JsValue::null());
        obj->setProp("parentElement", JsValue::null());
    }

    // Sibling traversal
    if (raw && raw->parent) {
        const auto& siblings = raw->parent->children;
        Node* prevNode = siblingNode(raw, -1);
        Node* nextNode = siblingNode(raw, 1);
        auto prevShared = getShared(prevNode);
        auto nextShared = getShared(nextNode);
        obj->setProp("previousSibling", prevShared ? wrapNodeInternal(vm, prevShared, false) : JsValue::null());
        obj->setProp("nextSibling",     nextShared ? wrapNodeInternal(vm, nextShared, false) : JsValue::null());
        // nextElementSibling / previousElementSibling
        bool foundSelf = false;
        std::shared_ptr<Node> prevElem, nextElem;
        for (size_t si = 0; si < siblings.size(); ++si) {
            if (siblings[si].get() == raw) {
                foundSelf = true;
                continue;
            }
            if (siblings[si]->type == NodeType::Element) {
                if (!foundSelf) prevElem = siblings[si];
                else if (!nextElem) nextElem = siblings[si];
            }
        }
        obj->setProp("nextElementSibling",     nextElem ? wrapNodeInternal(vm, nextElem, false) : JsValue::null());
        obj->setProp("previousElementSibling", prevElem ? wrapNodeInternal(vm, prevElem, false) : JsValue::null());
    } else {
        obj->setProp("nextSibling",          JsValue::null());
        obj->setProp("previousSibling",      JsValue::null());
        obj->setProp("nextElementSibling",     JsValue::null());
        obj->setProp("previousElementSibling", JsValue::null());
    }

    // dataset — proxy for data-* attributes.
    {
        auto* ds = vm.gc().newObject(ObjKind::Plain);
        ds->domNode = raw;
        g_datasetObjects.insert(ds);
        if (raw) {
            for (auto& [k, v] : raw->attrs) {
                if (k.rfind("data-", 0) == 0) {
                    // Convert "data-my-attr" to "myAttr" (camelCase).
                    std::string camel;
                    bool upper = false;
                    for (size_t ci = 5; ci < k.size(); ++ci) {
                        if (k[ci] == '-') { upper = true; continue; }
                        camel += upper ? (char)std::toupper((unsigned char)k[ci]) : k[ci];
                        upper = false;
                    }
                    ds->setProp(camel, vm.str(v));
                }
            }
        }
        obj->setProp("dataset", JsValue::object(ds));
    }

    // Layout dimensions. The JS bridge does not own the renderer's box tree,
    // but inline CSS/HTML dimensions are enough for many visibility checks.
    DomMetrics metrics = computeDomMetrics(raw);
    obj->setProp("offsetWidth",  JsValue::integer((int32_t)std::round(metrics.width)));
    obj->setProp("offsetHeight", JsValue::integer((int32_t)std::round(metrics.height)));
    obj->setProp("offsetTop",    JsValue::integer((int32_t)std::round(metrics.top)));
    obj->setProp("offsetLeft",   JsValue::integer((int32_t)std::round(metrics.left)));
    obj->setProp("offsetParent", JsValue::null());
    obj->setProp("clientWidth",  JsValue::integer((int32_t)std::round(metrics.width)));
    obj->setProp("clientHeight", JsValue::integer((int32_t)std::round(metrics.height)));
    obj->setProp("scrollWidth",  JsValue::integer((int32_t)std::round(metrics.width)));
    obj->setProp("scrollHeight", JsValue::integer((int32_t)std::round(metrics.height)));
    obj->setProp("scrollTop",    JsValue::integer(0));
    obj->setProp("scrollLeft",   JsValue::integer(0));

    // DOM mutations
    addNativeM("appendChild", NATIVE("appendChild") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return ARG(0);
        Node* child = unwrapNode(ARG(0));
        if (!child) return ARG(0);
        auto childShared = getShared(child);
        if (childShared) {
            insertSharedChild(n, childShared);
            markDomDirty(vm, n, "childList");
        }
        return ARG(0);
    });
    addNativeM("removeChild", NATIVE("removeChild") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return ARG(0);
        Node* child = unwrapNode(ARG(0));
        if (!child) return ARG(0);
        auto& ch = n->children;
        child->parent = nullptr;
        ch.erase(std::remove_if(ch.begin(), ch.end(), [&](auto& c){ return c.get()==child; }), ch.end());
        markDomDirty(vm, n, "childList");
        return ARG(0);
    });
    addNativeM("insertBefore", NATIVE("insertBefore") {
        Node* n = unwrapNode(thisVal);
        if (!n) return ARG(0);
        Node* child = unwrapNode(ARG(0)), *ref = unwrapNode(ARG(1));
        if (!child) return ARG(0);
        auto childShared = getShared(child);
        if (!childShared) return ARG(0);
        insertSharedChild(n, childShared, ref);
        markDomDirty(vm, n, "childList");
        return ARG(0);
    });
    addNativeM("replaceChild", NATIVE("replaceChild") {
        Node* n = unwrapNode(thisVal);
        if (!n) return ARG(0);
        Node* newNode = unwrapNode(ARG(0)), *oldNode = unwrapNode(ARG(1));
        if (!newNode || !oldNode) return ARG(0);
        auto newShared = getShared(newNode);
        if (!newShared) return ARG(0);
        auto it = std::find_if(n->children.begin(), n->children.end(),
            [&](const std::shared_ptr<Node>& c) { return c.get() == oldNode; });
        if (it != n->children.end()) {
            Node* before = (std::next(it) != n->children.end()) ? std::next(it)->get() : nullptr;
            oldNode->parent = nullptr;
            n->children.erase(it);
            insertSharedChild(n, newShared, before);
        }
        markDomDirty(vm, n, "childList");
        return ARG(1);
    });
    addNativeM("cloneNode", NATIVE("cloneNode") {
        Node* n = unwrapNode(thisVal);
        if (!n) return JsValue::null();
        auto clone = cloneDomNode(n, !args.empty() && ARG(0).toBool());
        return wrapNode(vm, clone);
    });
    addNativeM("append", NATIVE("append") {
        Node* n = unwrapNode(thisVal);
        insertDomArgs(n, nullptr, args);
        markDomDirty(vm, n, "childList");
        return JsValue::undefined();
    });
    addNativeM("prepend", NATIVE("prepend") {
        Node* n = unwrapNode(thisVal);
        Node* before = (n && !n->children.empty()) ? n->children.front().get() : nullptr;
        insertDomArgs(n, before, args);
        markDomDirty(vm, n, "childList");
        return JsValue::undefined();
    });
    addNativeM("before", NATIVE("before") {
        Node* n = unwrapNode(thisVal);
        if (n && n->parent) {
            Node* parent = n->parent;
            insertDomArgs(parent, n, args);
            markDomDirty(vm, parent, "childList");
        }
        return JsValue::undefined();
    });
    addNativeM("after", NATIVE("after") {
        Node* n = unwrapNode(thisVal);
        if (n && n->parent) {
            Node* parent = n->parent;
            insertDomArgs(parent, siblingNode(n, 1), args);
            markDomDirty(vm, parent, "childList");
        }
        return JsValue::undefined();
    });
    addNativeM("replaceWith", NATIVE("replaceWith") {
        Node* n = unwrapNode(thisVal);
        Node* parent = n ? n->parent : nullptr;
        replaceNodeWithArgs(n, args);
        markDomDirty(vm, parent, "childList");
        return JsValue::undefined();
    });
    addNativeM("replaceChildren", NATIVE("replaceChildren") {
        Node* n = unwrapNode(thisVal);
        if (!n) return JsValue::undefined();
        n->children.clear();
        insertDomArgs(n, nullptr, args);
        markDomDirty(vm, n, "childList");
        return JsValue::undefined();
    });
    addNativeM("insertAdjacentHTML", NATIVE("insertAdjacentHTML") {
        Node* n = unwrapNode(thisVal);
        Node* parent = nullptr;
        Node* before = nullptr;
        if (args.size() >= 2 && adjacentInsertionPoint(n, ARG_STR(0), parent, before)) {
            insertHtmlFragment(parent, before, ARG_STR(1));
            markDomDirty(vm, parent, "childList");
        }
        return JsValue::undefined();
    });
    addNativeM("insertAdjacentText", NATIVE("insertAdjacentText") {
        Node* n = unwrapNode(thisVal);
        Node* parent = nullptr;
        Node* before = nullptr;
        if (args.size() >= 2 && adjacentInsertionPoint(n, ARG_STR(0), parent, before)) {
            auto text = Node::makeText(ARG_STR(1));
            registerSubtree(text);
            insertSharedChild(parent, text, before);
            markDomDirty(vm, parent, "childList");
        }
        return JsValue::undefined();
    });
    addNativeM("insertAdjacentElement", NATIVE("insertAdjacentElement") {
        Node* n = unwrapNode(thisVal);
        Node* element = unwrapNode(ARG(1));
        Node* parent = nullptr;
        Node* before = nullptr;
        if (element && args.size() >= 2 && adjacentInsertionPoint(n, ARG_STR(0), parent, before)) {
            auto shared = getShared(element);
            if (shared) {
                insertSharedChild(parent, shared, before);
                markDomDirty(vm, parent, "childList");
                return ARG(1);
            }
        }
        return JsValue::null();
    });

    // querySelector / querySelectorAll share the DOM selector matcher.
    addNativeM("querySelector", NATIVE("querySelector") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::null();
        auto found = domQueryFirst(n, args[0].toString());
        return found ? wrapNode(vm, found) : JsValue::null();
    });
    addNativeM("querySelectorAll", NATIVE("querySelectorAll") {
        Node* n = unwrapNode(thisVal);
        auto* arr = newArrayWithPrototype(vm);
        if (!n || args.empty()) return JsValue::object(arr);
        for (auto& found : domQueryAll(n, args[0].toString())) arr->arrayPush(wrapNode(vm, found));
        return JsValue::object(arr);
    });
    addNativeM("getElementById", NATIVE("getElementById") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::null();
        auto results = domQueryAll(n, "#" + args[0].toString());
        return results.empty() ? JsValue::null() : wrapNode(vm, results[0]);
    });
    addNativeM("getElementsByClassName", NATIVE("getElementsByClassName") {
        Node* n = unwrapNode(thisVal);
        auto* arr = newArrayWithPrototype(vm);
        if (!n || args.empty()) return JsValue::object(arr);
        for (auto& found : domQueryAll(n, "." + args[0].toString())) arr->arrayPush(wrapNode(vm, found));
        return JsValue::object(arr);
    });
    addNativeM("getElementsByTagName", NATIVE("getElementsByTagName") {
        Node* n = unwrapNode(thisVal);
        auto* arr = newArrayWithPrototype(vm);
        if (!n || args.empty()) return JsValue::object(arr);
        for (auto& found : domQueryAll(n, args[0].toString())) arr->arrayPush(wrapNode(vm, found));
        return JsValue::object(arr);
    });
    addNativeM("createElement", NATIVE("createElement") {
        auto newNode = Node::makeElement(ARG_STR(0));
        g_nodeStore[newNode.get()] = newNode;
        return wrapNode(vm, newNode);
    });
    addNativeM("createElementNS", NATIVE("createElementNS") {
        auto newNode = Node::makeElement(ARG_STR(1));
        newNode->attrs[kNamespaceAttr] = ARG_STR(0);
        g_nodeStore[newNode.get()] = newNode;
        return wrapNode(vm, newNode);
    });
    addNativeM("createTextNode", NATIVE("createTextNode") {
        auto newNode = Node::makeText(ARG_STR(0));
        g_nodeStore[newNode.get()] = newNode;
        return wrapNode(vm, newNode);
    });
    addNativeM("createComment", NATIVE("createComment") {
        auto newNode = Node::makeElement("!--");
        newNode->text = ARG_STR(0);
        g_nodeStore[newNode.get()] = newNode;
        return wrapNode(vm, newNode);
    });
    addNativeM("createDocumentFragment", NATIVE("createDocumentFragment") {
        auto frag = Node::makeElement("#document-fragment");
        frag->type = NodeType::Document;
        g_nodeStore[frag.get()] = frag;
        return wrapNode(vm, frag);
    });
    addNativeM("createRange", NATIVE("createRange") {
        return makeRangeObject(vm);
    });
    addNativeM("createEvent", NATIVE("createEvent") {
        std::string kind = args.empty() ? "Event" : ARG_STR(0);
        std::string low = lowerCopy(kind);
        std::string ctor = (low.find("custom") != std::string::npos) ? "CustomEvent" : "Event";
        return makeEventObject(vm, ctor, "");
    });

    addNativeM("addEventListener", NATIVE("addEventListener") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.size() < 2 || !ARG(1).isCallable()) return JsValue::undefined();
        g_eventListeners[n].push_back({ ARG_STR(0), ARG(1) });
        return JsValue::undefined();
    });
    addNativeM("removeEventListener", NATIVE("removeEventListener") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.size() < 2) return JsValue::undefined();
        std::string ev = ARG_STR(0);
        auto it = g_eventListeners.find(n);
        if (it != g_eventListeners.end()) {
            auto& list = it->second;
            for (auto li = list.begin(); li != list.end(); ++li)
                if (li->event == ev && li->fn.strictEq(ARG(1))) { list.erase(li); break; }
        }
        return JsValue::undefined();
    });
    addNativeM("dispatchEvent", NATIVE("dispatchEvent") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty() || !ARG(0).isObject()) return JsValue::boolean(true);
        JsObject* ev = ARG(0).asObject();
        std::string type = ev->getProp("type").toString();
        if (type.empty()) return JsValue::boolean(true);
        auto shared = getShared(n);
        ev->setProp("target", shared ? wrapNode(vm, shared) : thisVal);
        installEventMethods(vm, ev);
        Node* cur = n;
        while (cur) {
            auto curShared = getShared(cur);
            JsValue thisObj = curShared ? wrapNode(vm, curShared) : JsValue::undefined();
            ev->setProp("currentTarget", thisObj);
            JsValue propertyHandler = thisObj.isObject()
                ? thisObj.asObject()->getProp("on" + type)
                : JsValue::undefined();
            if (propertyHandler.isCallable()) {
                try { vm.call(propertyHandler, thisObj, { ARG(0) }); }
                catch (...) {}
                if (eventFlag(ev, "__vertexStopped")) break;
            }
            auto it = g_eventListeners.find(cur);
            if (it != g_eventListeners.end()) {
                auto listeners = it->second;
                for (const auto& listener : listeners) {
                    if (listener.event != type || !listener.fn.isCallable()) continue;
                    try { vm.call(listener.fn, thisObj, { ARG(0) }); }
                    catch (...) {}
                    if (eventFlag(ev, "__vertexStopped")) break;
                }
            }
            if (eventFlag(ev, "__vertexStopped") || !eventFlag(ev, "bubbles")) break;
            cur = cur->parent;
        }
        return JsValue::boolean(!eventFlag(ev, "defaultPrevented"));
    });
    addNativeM("submit", NATIVE("submit") {
        Node* n = unwrapNode(thisVal);
        if (!n || n->tagName != "form") return JsValue::undefined();
        auto* ev = vm.gc().newObject(ObjKind::Plain);
        ev->setProp("type", vm.str("submit"));
        ev->setProp("bubbles", JsValue::boolean(true));
        ev->setProp("cancelable", JsValue::boolean(true));
        ev->setProp("defaultPrevented", JsValue::boolean(false));
        ev->setProp("target", JsValue::null());
        ev->setProp("currentTarget", JsValue::null());
        ev->setProp("submitter", JsValue::null());
        installEventMethods(vm, ev);
        JsValue dispatch = thisVal.isObject() ? thisVal.asObject()->getProp("dispatchEvent") : JsValue::undefined();
        if (dispatch.isCallable()) vm.call(dispatch, thisVal, { JsValue::object(ev) });
        return JsValue::undefined();
    });
    addNativeM("requestSubmit", NATIVE("requestSubmit") {
        Node* n = unwrapNode(thisVal);
        if (!n || n->tagName != "form") return JsValue::undefined();
        Node* submitterNode = args.empty() ? nullptr : unwrapNode(ARG(0));
        auto* ev = vm.gc().newObject(ObjKind::Plain);
        ev->setProp("type", vm.str("submit"));
        ev->setProp("bubbles", JsValue::boolean(true));
        ev->setProp("cancelable", JsValue::boolean(true));
        ev->setProp("defaultPrevented", JsValue::boolean(false));
        ev->setProp("target", JsValue::null());
        ev->setProp("currentTarget", JsValue::null());
        auto submitterShared = getShared(submitterNode);
        ev->setProp("submitter", submitterShared ? wrapNodeInternal(vm, submitterShared, false) : JsValue::null());
        installEventMethods(vm, ev);
        JsValue dispatch = thisVal.isObject() ? thisVal.asObject()->getProp("dispatchEvent") : JsValue::undefined();
        if (dispatch.isCallable()) vm.call(dispatch, thisVal, { JsValue::object(ev) });
        return JsValue::undefined();
    });
    addNativeM("matches", NATIVE("matches") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::boolean(false);
        return JsValue::boolean(matchesSelector(n, ARG_STR(0), n));
    });
    addNativeM("closest", NATIVE("closest") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::null();
        std::string sel = ARG_STR(0);
        Node* cur = n;
        while (cur) {
            if (matchesSelector(cur, sel, n)) { auto s = getShared(cur); if (s) return wrapNode(vm, s); }
            cur = cur->parent;
        }
        return JsValue::null();
    });
    addNativeM("contains", NATIVE("contains") {
        Node* n = unwrapNode(thisVal);
        Node* other = unwrapNode(ARG(0));
        return JsValue::boolean(nodeContains(n, other));
    });
    addNativeM("isSameNode", NATIVE("isSameNode") {
        return JsValue::boolean(unwrapNode(thisVal) == unwrapNode(ARG(0)));
    });
    addNativeM("compareDocumentPosition", NATIVE("compareDocumentPosition") {
        Node* a = unwrapNode(thisVal);
        Node* b = unwrapNode(ARG(0));
        if (!a || !b) return JsValue::integer(1); // DISCONNECTED
        if (a == b) return JsValue::integer(0);
        if (nodeContains(a, b)) return JsValue::integer(20); // FOLLOWING | CONTAINED_BY
        if (nodeContains(b, a)) return JsValue::integer(10); // PRECEDING | CONTAINS
        return JsValue::integer(isBeforeInDocumentOrder(a, b) ? 4 : 2);
    });
    addNativeM("getRootNode", NATIVE("getRootNode") {
        Node* root = rootNode(unwrapNode(thisVal));
        auto shared = getShared(root);
        return shared ? wrapNode(vm, shared) : JsValue::null();
    });
    addNativeM("normalize", NATIVE("normalize") {
        Node* n = unwrapNode(thisVal);
        normalizeTextChildren(n);
        markDomDirty(vm, n, "childList");
        return JsValue::undefined();
    });

    addNativeM("getBoundingClientRect", NATIVE("getBoundingClientRect") {
        Node* n = unwrapNode(thisVal);
        DomMetrics rect = computeDomMetrics(n);
        auto* r = vm.gc().newObject(ObjKind::Plain);
        r->setProp("top",    JsValue::number(rect.top));
        r->setProp("left",   JsValue::number(rect.left));
        r->setProp("bottom", JsValue::number(rect.top + rect.height));
        r->setProp("right",  JsValue::number(rect.left + rect.width));
        r->setProp("width",  JsValue::number(rect.width));
        r->setProp("height", JsValue::number(rect.height));
        r->setProp("x",      JsValue::number(rect.left));
        r->setProp("y",      JsValue::number(rect.top));
        return JsValue::object(r);
    });
    addNativeM("getClientRects", NATIVE("getClientRects") {
        Node* n = unwrapNode(thisVal);
        auto* arr = newArrayWithPrototype(vm);
        if (!n) return JsValue::object(arr);
        JsValue rectFn = thisVal.isObject() ? thisVal.asObject()->getProp("getBoundingClientRect") : JsValue::undefined();
        arr->arrayPush(rectFn.isCallable() ? vm.call(rectFn, thisVal, {}) : JsValue::object(vm.gc().newObject(ObjKind::Plain)));
        return JsValue::object(arr);
    });
    addNativeM("scrollTo", NATIVE("element_scrollTo") {
        Node* n = unwrapNode(thisVal);
        if (!n || !thisVal.isObject()) return JsValue::undefined();
        int left = args.size() > 0 ? ARG(0).toInt32() : 0;
        int top = args.size() > 1 ? ARG(1).toInt32() : 0;
        if (!args.empty() && ARG(0).isObject()) {
            JsObject* opts = ARG(0).asObject();
            left = opts->getProp("left").isUndefined() ? thisVal.asObject()->getProp("scrollLeft").toInt32() : opts->getProp("left").toInt32();
            top = opts->getProp("top").isUndefined() ? thisVal.asObject()->getProp("scrollTop").toInt32() : opts->getProp("top").toInt32();
        }
        thisVal.asObject()->setProp("scrollLeft", JsValue::integer(std::max(0, left)));
        thisVal.asObject()->setProp("scrollTop", JsValue::integer(std::max(0, top)));
        return JsValue::undefined();
    });
    addNativeM("scroll", NATIVE("element_scroll") {
        JsValue fn = thisVal.isObject() ? thisVal.asObject()->getProp("scrollTo") : JsValue::undefined();
        return fn.isCallable() ? vm.call(fn, thisVal, args) : JsValue::undefined();
    });
    addNativeM("scrollBy", NATIVE("element_scrollBy") {
        if (!thisVal.isObject()) return JsValue::undefined();
        int left = thisVal.asObject()->getProp("scrollLeft").toInt32() + (args.size() > 0 ? ARG(0).toInt32() : 0);
        int top = thisVal.asObject()->getProp("scrollTop").toInt32() + (args.size() > 1 ? ARG(1).toInt32() : 0);
        JsValue fn = thisVal.asObject()->getProp("scrollTo");
        return fn.isCallable() ? vm.call(fn, thisVal, { JsValue::integer(left), JsValue::integer(top) }) : JsValue::undefined();
    });
    addNativeM("scrollIntoView", NATIVE("scrollIntoView") {
        Node* n = unwrapNode(thisVal);
        if (!n) return JsValue::undefined();
        DomMetrics rect = computeDomMetrics(n);
        setWindowScrollTo(vm, g_windowScrollX, rect.top);
        if (g_domCallbacks.scrollIntoView) g_domCallbacks.scrollIntoView(n);
        return JsValue::undefined();
    });
    addNativeM("focus",  NATIVE("focus")  {
        if (Node* n = unwrapNode(thisVal)) {
            SetCssFocusNode(n);
            setDocumentActiveElement(vm, n);
            dispatchDomEvent(vm, n, "focus");
            markDomDirty(vm, n, "attributes");
        }
        return JsValue::undefined();
    });
    addNativeM("blur",   NATIVE("blur")   {
        if (Node* n = unwrapNode(thisVal)) {
            SetCssFocusNode(nullptr);
            dispatchDomEvent(vm, n, "blur");
            Node* body = findFirstTag(documentRootFor(n), "body");
            setDocumentActiveElement(vm, body);
            markDomDirty(vm, n, "attributes");
        }
        return JsValue::undefined();
    });
    addNativeM("click",  NATIVE("click")  {
        Node* n = unwrapNode(thisVal);
        if (n) activateDomElement(vm, n);
        return JsValue::undefined();
    });
    addNativeM("remove", NATIVE("remove") {
        Node* n = unwrapNode(thisVal);
        if (!n || !n->parent) return JsValue::undefined();
        auto& ch = n->parent->children;
        ch.erase(std::remove_if(ch.begin(), ch.end(), [&](auto& c){ return c.get()==n; }), ch.end());
        markDomDirty(vm, n->parent, "childList");
        return JsValue::undefined();
    });

    // value / checked for form elements
    obj->setProp("value",   vm.str(node->attr("value")));
    obj->setProp("checked", JsValue::boolean(hasAttr(raw, "checked")));
    obj->setProp("disabled",JsValue::boolean(hasAttr(raw, "disabled")));
    obj->setProp("href",    vm.str(node->attr("href")));
    obj->setProp("src",     vm.str(node->attr("src")));
    obj->setProp("alt",     vm.str(node->attr("alt")));
    obj->setProp("type",    vm.str(node->attr("type")));
    obj->setProp("name",    vm.str(node->attr("name")));

    if (raw->tagName == "canvas") {
        obj->setProp("width",  JsValue::integer((int)parseFloatOr(raw->attr("width"), 300.f)));
        obj->setProp("height", JsValue::integer((int)parseFloatOr(raw->attr("height"), 150.f)));
        addNativeM("getContext", NATIVE("getContext") {
            Node* n = unwrapNode(thisVal);
            if (!n) return JsValue::null();
            std::string kind = ARG_STR(0);
            if (kind != "2d") return JsValue::null();  // webgl/bitmaprenderer: not supported
            return MakeCanvasRenderingContext2D(vm, n);
        });
    }

    g_readyWrappers.insert(obj);
    vm.gc().removeRoot(&objValue);
    return objValue;
}

JsValue wrapNode(VM& vm, std::shared_ptr<Node> node) {
    return wrapNodeInternal(vm, std::move(node), true);
}

// ── registerDom ───────────────────────────────────────────────────────────────

// camelCase CSS property → kebab-case (backgroundColor → background-color).
static std::string cssKebab(const std::string& k) {
    std::string out;
    for (char c : k) {
        if (c >= 'A' && c <= 'Z') { out += '-'; out += (char)(c - 'A' + 'a'); }
        else out += c;
    }
    return out;
}

// Re-serialize a style object's own CSS properties into the node's style attr.
static void rebuildStyleAttr(Node* n, JsObject* styleObj) {
    std::string s;
    for (const auto& key : styleObj->ownEnumKeys()) {
        JsValue v = styleObj->getProp(key);
        if (v.isObject()) continue;             // skip methods like setProperty
        std::string val = v.toString();
        if (val.empty()) continue;
        if (!s.empty()) s += "; ";
        s += cssKebab(key) + ": " + val;
    }
    n->attrs["style"] = s;
}

static std::string cssCamel(const std::string& key) {
    std::string out;
    bool upper = false;
    for (char c : key) {
        if (c == '-') { upper = true; continue; }
        out += upper ? (char)std::toupper((unsigned char)c) : c;
        upper = false;
    }
    return out;
}

static std::map<std::string, std::string> computedStyleMap(Node* n) {
    std::map<std::string, std::string> props;
    if (!n) return props;
    auto raw = parseStyleMap(n->attr("style"));
    ComputedStyle s = ParseInlineStyle(n->attr("style"));
    ResolveStyleVariables(s);

    auto set = [&](const std::string& kebab, const std::string& val) {
        props[kebab] = val;
        props[cssCamel(kebab)] = val;
    };
    auto rawOr = [&](const std::string& key, const std::string& fallback) {
        auto it = raw.find(key);
        return it != raw.end() ? it->second : fallback;
    };

    set("display", rawOr("display", displayName(s, n)));
    set("position", rawOr("position", positionName(s)));
    set("visibility", rawOr("visibility", s.visibilityHidden ? "hidden" : "visible"));
    set("overflow", rawOr("overflow", overflowName(s)));
    set("width", rawOr("width", s.width >= 0 ? pxCss(s.width) : ""));
    set("height", rawOr("height", s.height >= 0 ? pxCss(s.height) : ""));
    set("margin-top", rawOr("margin-top", s.marginTopSet() ? pxCss(s.marginTop) : "0px"));
    set("margin-right", rawOr("margin-right", s.marginRightSet() ? pxCss(s.marginRight) : "0px"));
    set("margin-bottom", rawOr("margin-bottom", s.marginBottomSet() ? pxCss(s.marginBottom) : "0px"));
    set("margin-left", rawOr("margin-left", s.marginLeftSet() ? pxCss(s.marginLeft) : "0px"));
    set("padding-top", rawOr("padding-top", s.paddingTop >= 0 ? pxCss(s.paddingTop) : "0px"));
    set("padding-right", rawOr("padding-right", s.paddingRight >= 0 ? pxCss(s.paddingRight) : "0px"));
    set("padding-bottom", rawOr("padding-bottom", s.paddingBottom >= 0 ? pxCss(s.paddingBottom) : "0px"));
    set("padding-left", rawOr("padding-left", s.paddingLeft >= 0 ? pxCss(s.paddingLeft) : "0px"));
    set("color", rawOr("color", colorCss(s.color)));
    set("background-color", rawOr("background-color", s.bgColorSet ? colorCss(s.bgColor) : "rgba(0, 0, 0, 0)"));
    set("background-image", rawOr("background-image", s.backgroundImageSet ? "url(" + s.backgroundImage + ")" : "none"));
    set("font-size", rawOr("font-size", s.fontSize > 0 ? pxCss(s.fontSize) : "16px"));
    set("font-family", rawOr("font-family", s.fontFamily));
    set("font-weight", rawOr("font-weight", s.bold ? "700" : "400"));
    set("opacity", rawOr("opacity", numberCss(s.opacity)));
    set("z-index", rawOr("z-index", s.zIndexSet ? std::to_string(s.zIndex) : "auto"));
    set("top", rawOr("top", s.topSet && !s.topPercent ? pxCss(s.top) : "auto"));
    set("left", rawOr("left", s.leftSet && !s.leftPercent ? pxCss(s.left) : "auto"));
    set("right", rawOr("right", s.rightSet && !s.rightPercent ? pxCss(s.right) : "auto"));
    set("bottom", rawOr("bottom", s.bottomSet && !s.bottomPercent ? pxCss(s.bottom) : "auto"));
    set("transform", rawOr("transform", s.transformSet ? "matrix(1, 0, 0, 1, " + numberCss(s.transformTx) + ", " + numberCss(s.transformTy) + ")" : "none"));
    set("transition", rawOr("transition", s.transitionSet ? s.transitionProperty + " " + numberCss(s.transitionDuration) + "s" : ""));

    for (const auto& [key, val] : raw) set(key, val);
    return props;
}

// Replace a node's children with the parsed contents of an HTML fragment.
static void setInnerHtml(Node* parent, const std::string& html) {
    if (!parent) return;
    auto doc = ParseHtml(html);
    std::function<Node*(Node*, const std::string&)> findTag =
        [&](Node* node, const std::string& tag) -> Node* {
            for (auto& c : node->children) {
                if (c->tagName == tag) return c.get();
                if (Node* r = findTag(c.get(), tag)) return r;
            }
            return nullptr;
        };
    Node* content = doc.get();
    if (Node* body = findTag(doc.get(), "body")) content = body;
    parent->children.clear();
    for (auto& c : content->children) {
        c->parent = parent;
        parent->children.push_back(c);
    }
}

struct ParsedDomUrl {
    std::string href;
    std::string protocol;
    std::string host;
    std::string hostname;
    std::string port;
    std::string pathname = "/";
    std::string search;
    std::string hash;
    std::string origin;
};

static std::string removeHash(const std::string& href) {
    size_t hash = href.find('#');
    return hash == std::string::npos ? href : href.substr(0, hash);
}

static std::string removeQueryAndHash(const std::string& href) {
    size_t stop = href.find_first_of("?#");
    return stop == std::string::npos ? href : href.substr(0, stop);
}

static std::string resolveDomUrl(const std::string& href, const std::string& base) {
    if (href.empty()) return base;
    if (href[0] == '#') return removeHash(base) + href;
    if (href[0] == '?') return removeQueryAndHash(base) + href;
    return ResolveUrlAgainstBase(href, base);
}

static ParsedDomUrl parseDomUrl(const std::string& href) {
    ParsedDomUrl out;
    out.href = href;
    size_t scheme = href.find("://");
    if (scheme == std::string::npos) return out;

    out.protocol = href.substr(0, scheme + 1);
    size_t hostStart = scheme + 3;
    size_t hostEnd = href.find_first_of("/?#", hostStart);
    out.host = href.substr(hostStart,
        hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
    out.hostname = out.host;
    size_t colon = out.host.rfind(':');
    if (colon != std::string::npos && out.host.find(']') == std::string::npos) {
        out.hostname = out.host.substr(0, colon);
        out.port = out.host.substr(colon + 1);
    }
    out.origin = href.substr(0, scheme + 3) + out.host;

    if (hostEnd != std::string::npos && href[hostEnd] == '/') {
        size_t qmark = href.find('?', hostEnd);
        size_t hashMark = href.find('#', hostEnd);
        size_t pathEnd = std::min(qmark, hashMark);
        out.pathname = href.substr(hostEnd,
            pathEnd != std::string::npos ? pathEnd - hostEnd : std::string::npos);
    }
    size_t qmark = href.find('?', hostEnd == std::string::npos ? hostStart : hostEnd);
    size_t hashMark = href.find('#', hostEnd == std::string::npos ? hostStart : hostEnd);
    if (qmark != std::string::npos) {
        size_t searchEnd = (hashMark != std::string::npos && hashMark > qmark)
            ? hashMark : std::string::npos;
        out.search = href.substr(qmark,
            searchEnd != std::string::npos ? searchEnd - qmark : std::string::npos);
    }
    if (hashMark != std::string::npos) out.hash = href.substr(hashMark);
    return out;
}

static void applyLocationProps(VM& vm, JsObject* loc, const std::string& href) {
    ParsedDomUrl parsed = parseDomUrl(href);
    loc->setProp("href",     vm.str(parsed.href));
    loc->setProp("protocol", vm.str(parsed.protocol));
    loc->setProp("host",     vm.str(parsed.host));
    loc->setProp("hostname", vm.str(parsed.hostname));
    loc->setProp("pathname", vm.str(parsed.pathname));
    loc->setProp("search",   vm.str(parsed.search));
    loc->setProp("hash",     vm.str(parsed.hash));
    loc->setProp("origin",   vm.str(parsed.origin));
    loc->setProp("port",     vm.str(parsed.port));
}

static std::string urlDecodeComponent(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            int hi = hex(input[i + 1]), lo = hex(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += input[i] == '+' ? ' ' : input[i];
    }
    return out;
}

static std::string urlEncodeComponent(const std::string& input) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else if (c == ' ') out += '+';
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

using QueryPairs = std::vector<std::pair<std::string, std::string>>;

static QueryPairs parseQueryPairs(std::string query) {
    if (!query.empty() && query.front() == '?') query.erase(query.begin());
    QueryPairs pairs;
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!part.empty()) {
            size_t eq = part.find('=');
            std::string key = eq == std::string::npos ? part : part.substr(0, eq);
            std::string value = eq == std::string::npos ? "" : part.substr(eq + 1);
            pairs.push_back({ urlDecodeComponent(key), urlDecodeComponent(value) });
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return pairs;
}

static std::string serializeQueryPairs(const QueryPairs& pairs) {
    std::string out;
    for (const auto& [key, value] : pairs) {
        if (!out.empty()) out += '&';
        out += urlEncodeComponent(key);
        out += '=';
        out += urlEncodeComponent(value);
    }
    return out;
}

static void syncFormEncodedBody(VM& vm, JsObject* obj, const QueryPairs& pairs, const std::string& type) {
    if (!obj) return;
    obj->setProp("__vertexBodyText", vm.str(serializeQueryPairs(pairs)));
    obj->setProp("__vertexBodyType", vm.str(type));
}

static QueryPairs queryPairsFromInit(VM& vm, JsValue init) {
    if (init.isNullOrUndefined()) return {};
    if (!init.isObject()) return parseQueryPairs(init.toString());
    JsObject* obj = init.asObject();
    JsValue bodyText = obj->getProp("__vertexBodyText");
    if (!bodyText.isUndefined()) return parseQueryPairs(bodyText.toString());
    if (obj->kind == ObjKind::Array) {
        QueryPairs pairs;
        for (uint32_t i = 0; i < obj->arrayLength(); ++i) {
            JsValue pairVal = obj->arrayGet(i);
            if (!pairVal.isObject()) continue;
            JsObject* pairObj = pairVal.asObject();
            pairs.push_back({ pairObj->arrayGet(0).toString(), pairObj->arrayGet(1).toString() });
        }
        return pairs;
    }
    JsValue toStringFn = obj->getProp("toString");
    if (toStringFn.isCallable() && obj->hasOwn("__vertexUrlSearchParams"))
        return parseQueryPairs(vm.call(toStringFn, init, {}).toString());
    QueryPairs pairs;
    for (const auto& key : obj->ownEnumKeys()) {
        if (key.rfind("__vertex", 0) == 0) continue;
        JsValue value = obj->getProp(key);
        if (!value.isCallable())
            pairs.push_back({ key, value.isNullOrUndefined() ? "" : value.toString() });
    }
    return pairs;
}

static QueryPairs collectFormDataPairs(Node* form) {
    QueryPairs pairs;
    if (!form || form->tagName != "form") return pairs;
    for (Node* control : formControls(form)) {
        if (!control || hasAttr(control, "disabled")) continue;
        std::string name = control->attr("name");
        if (name.empty()) continue;
        std::string tag = control->tagName;
        std::string type = lowerCopy(control->attr("type"));
        if (tag == "button" || type == "button" || type == "submit" || type == "reset" || type == "file")
            continue;
        if ((type == "checkbox" || type == "radio") && !hasAttr(control, "checked"))
            continue;
        std::string value = elementValue(control);
        if ((type == "checkbox" || type == "radio") && value.empty()) value = "on";
        pairs.push_back({ name, value });
    }
    return pairs;
}

static QueryPairs formDataPairsFromInit(VM& vm, JsValue init) {
    if (Node* form = unwrapNode(init)) {
        if (form->tagName == "form") return collectFormDataPairs(form);
    }
    return queryPairsFromInit(vm, init);
}

static JsValue makeURLSearchParams(VM& vm, QueryPairs initial, std::function<void(const std::string&)> onChange = {}) {
    auto pairs = std::make_shared<QueryPairs>(std::move(initial));
    auto notify = [pairs, onChange]() {
        if (onChange) onChange(serializeQueryPairs(*pairs));
    };
    auto* params = vm.gc().newObject(ObjKind::Plain);
    params->setProp("__vertexUrlSearchParams", JsValue::boolean(true));
    syncFormEncodedBody(vm, params, *pairs, "application/x-www-form-urlencoded;charset=UTF-8");
    VM* vmPtr = &vm;
    auto syncAndNotify = [params, pairs, notify, vmPtr]() {
        syncFormEncodedBody(*vmPtr, params, *pairs, "application/x-www-form-urlencoded;charset=UTF-8");
        notify();
    };
    addNative(vm, params, "get", [pairs](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        for (const auto& pair : *pairs)
            if (pair.first == key) return vm.str(pair.second);
        return JsValue::null();
    });
    addNative(vm, params, "getAll", [pairs](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        auto* arr = vm.gc().newArray();
        JsValue arrayCtor = vm.getGlobal("Array");
        if (arrayCtor.isObject()) {
            JsValue proto = arrayCtor.asObject()->getProp("prototype");
            if (proto.isObject()) arr->proto = proto.asObject();
        }
        for (const auto& pair : *pairs)
            if (pair.first == key) arr->arrayPush(vm.str(pair.second));
        return JsValue::object(arr);
    });
    addNative(vm, params, "has", [pairs](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        for (const auto& pair : *pairs)
            if (pair.first == key) return JsValue::boolean(true);
        return JsValue::boolean(false);
    });
    addNative(vm, params, "set", [pairs, syncAndNotify](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        std::string value = args.size() > 1 ? args[1].toString() : "";
        bool replaced = false;
        for (auto it = pairs->begin(); it != pairs->end();) {
            if (it->first == key) {
                if (!replaced) { it->second = value; replaced = true; ++it; }
                else it = pairs->erase(it);
            } else ++it;
        }
        if (!replaced) pairs->push_back({ key, value });
        syncAndNotify();
        return JsValue::undefined();
    });
    addNative(vm, params, "append", [pairs, syncAndNotify](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        pairs->push_back({ args.empty() ? "" : args[0].toString(), args.size() > 1 ? args[1].toString() : "" });
        syncAndNotify();
        return JsValue::undefined();
    });
    addNative(vm, params, "delete", [pairs, syncAndNotify](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        pairs->erase(std::remove_if(pairs->begin(), pairs->end(),
            [&](const auto& pair) { return pair.first == key; }), pairs->end());
        syncAndNotify();
        return JsValue::undefined();
    });
    addNative(vm, params, "sort", [pairs, syncAndNotify](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        std::stable_sort(pairs->begin(), pairs->end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        syncAndNotify();
        return JsValue::undefined();
    });
    addNative(vm, params, "toString", [pairs](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        return vm.str(serializeQueryPairs(*pairs));
    });
    addNative(vm, params, "forEach", [pairs](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
        if (args.empty() || !args[0].isCallable()) return JsValue::undefined();
        JsValue thisArg = args.size() > 1 ? args[1] : JsValue::undefined();
        auto snapshot = *pairs;
        for (const auto& pair : snapshot)
            vm.call(args[0], thisArg, { vm.str(pair.second), vm.str(pair.first), thisVal });
        return JsValue::undefined();
    });
    addNative(vm, params, "keys", [pairs](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        auto* arr = newArrayWithPrototype(vm);
        for (const auto& pair : *pairs) arr->arrayPush(vm.str(pair.first));
        return JsValue::object(arr);
    });
    addNative(vm, params, "values", [pairs](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        auto* arr = newArrayWithPrototype(vm);
        for (const auto& pair : *pairs) arr->arrayPush(vm.str(pair.second));
        return JsValue::object(arr);
    });
    addNative(vm, params, "entries", [pairs](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        auto* arr = newArrayWithPrototype(vm);
        for (const auto& pair : *pairs) {
            auto* entry = newArrayWithPrototype(vm);
            entry->arrayPush(vm.str(pair.first));
            entry->arrayPush(vm.str(pair.second));
            arr->arrayPush(JsValue::object(entry));
        }
        return JsValue::object(arr);
    });
    return JsValue::object(params);
}

static JsValue makeURLSearchParams(VM& vm, const std::string& query, std::function<void(const std::string&)> onChange = {}) {
    return makeURLSearchParams(vm, parseQueryPairs(query), std::move(onChange));
}

static JsValue makeFormData(VM& vm, QueryPairs initial = {}) {
    auto pairs = std::make_shared<QueryPairs>(std::move(initial));
    auto* form = vm.gc().newObject(ObjKind::Plain);
    form->setProp("__vertexFormData", JsValue::boolean(true));
    syncFormEncodedBody(vm, form, *pairs, "application/x-www-form-urlencoded;charset=UTF-8");
    VM* vmPtr = &vm;
    auto sync = [form, pairs, vmPtr]() {
        syncFormEncodedBody(*vmPtr, form, *pairs, "application/x-www-form-urlencoded;charset=UTF-8");
    };
    addNative(vm, form, "append", [pairs, sync](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        pairs->push_back({ args.empty() ? "" : args[0].toString(), args.size() > 1 ? args[1].toString() : "" });
        sync();
        return JsValue::undefined();
    });
    addNative(vm, form, "set", [pairs, sync](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        std::string value = args.size() > 1 ? args[1].toString() : "";
        pairs->erase(std::remove_if(pairs->begin(), pairs->end(),
            [&](const auto& pair) { return pair.first == key; }), pairs->end());
        pairs->push_back({ key, value });
        sync();
        return JsValue::undefined();
    });
    addNative(vm, form, "get", [pairs](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        for (const auto& pair : *pairs)
            if (pair.first == key) return vm.str(pair.second);
        return JsValue::null();
    });
    addNative(vm, form, "getAll", [pairs](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        auto* arr = newArrayWithPrototype(vm);
        for (const auto& pair : *pairs)
            if (pair.first == key) arr->arrayPush(vm.str(pair.second));
        return JsValue::object(arr);
    });
    addNative(vm, form, "has", [pairs](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        for (const auto& pair : *pairs)
            if (pair.first == key) return JsValue::boolean(true);
        return JsValue::boolean(false);
    });
    addNative(vm, form, "delete", [pairs, sync](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        pairs->erase(std::remove_if(pairs->begin(), pairs->end(),
            [&](const auto& pair) { return pair.first == key; }), pairs->end());
        sync();
        return JsValue::undefined();
    });
    addNative(vm, form, "forEach", [pairs](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
        if (args.empty() || !args[0].isCallable()) return JsValue::undefined();
        JsValue thisArg = args.size() > 1 ? args[1] : JsValue::undefined();
        auto snapshot = *pairs;
        for (const auto& pair : snapshot)
            vm.call(args[0], thisArg, { vm.str(pair.second), vm.str(pair.first), thisVal });
        return JsValue::undefined();
    });
    return JsValue::object(form);
}

static JsValue makeStorageObject(VM& vm, std::shared_ptr<std::map<std::string, std::string>> storage) {
    auto* obj = vm.gc().newObject(ObjKind::Plain);
    auto syncLength = [storage](JsObject* target) {
        if (target)
            target->setProp("length", JsValue::integer(static_cast<int32_t>(storage->size())));
    };
    addNative(vm, obj, "getItem", [storage](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        auto it = storage->find(key);
        if (it != storage->end()) return vm.str(it->second);
        if (thisVal.isObject()) {
            JsValue prop = thisVal.asObject()->getProp(key);
            if (!prop.isUndefined()) return vm.str(prop.toString());
        }
        return JsValue::null();
    });
    addNative(vm, obj, "setItem", [storage, syncLength](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        std::string value = args.size() > 1 ? args[1].toString() : "";
        (*storage)[key] = value;
        if (thisVal.isObject()) {
            thisVal.asObject()->setProp(key, vm.str(value));
            syncLength(thisVal.asObject());
        }
        return JsValue::undefined();
    });
    addNative(vm, obj, "removeItem", [storage, syncLength](VM&, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
        std::string key = args.empty() ? "" : args[0].toString();
        storage->erase(key);
        if (thisVal.isObject()) {
            thisVal.asObject()->deleteProp(key);
            syncLength(thisVal.asObject());
        }
        return JsValue::undefined();
    });
    addNative(vm, obj, "clear", [storage, syncLength](VM&, JsValue thisVal, std::vector<JsValue>) -> JsValue {
        storage->clear();
        if (thisVal.isObject()) syncLength(thisVal.asObject());
        return JsValue::undefined();
    });
    addNative(vm, obj, "key", [storage](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        int index = args.empty() ? 0 : args[0].toInt32();
        if (index < 0 || index >= static_cast<int>(storage->size())) return JsValue::null();
        auto it = storage->begin();
        std::advance(it, index);
        return vm.str(it->first);
    });
    obj->setProp("length", JsValue::integer(static_cast<int32_t>(storage->size())));
    return JsValue::object(obj);
}

using HeaderStore = std::map<std::string, std::pair<std::string, std::string>>;

static std::string normalizeHeaderName(std::string name) {
    name = trimCopy(name);
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return name;
}

static void headersSet(HeaderStore& headers, const std::string& name, const std::string& value) {
    std::string normalized = normalizeHeaderName(name);
    if (normalized.empty()) return;
    headers[normalized] = { trimCopy(name), trimCopy(value) };
}

static void headersAppend(HeaderStore& headers, const std::string& name, const std::string& value) {
    std::string normalized = normalizeHeaderName(name);
    if (normalized.empty()) return;
    auto it = headers.find(normalized);
    if (it == headers.end()) headers[normalized] = { trimCopy(name), trimCopy(value) };
    else it->second.second += ", " + trimCopy(value);
}

static JsValue makeHeadersObject(VM& vm, HeaderStore initial = {}) {
    auto state = std::make_shared<HeaderStore>(std::move(initial));
    auto* headers = vm.gc().newObject(ObjKind::Plain);
    auto makeArray = [state](VM& vm, const std::string& mode) {
        auto* arr = newArrayWithPrototype(vm);
        for (const auto& [name, entry] : *state) {
            if (mode == "keys") arr->arrayPush(vm.str(name));
            else if (mode == "values") arr->arrayPush(vm.str(entry.second));
            else {
                auto* pair = newArrayWithPrototype(vm);
                pair->arrayPush(vm.str(name));
                pair->arrayPush(vm.str(entry.second));
                arr->arrayPush(JsValue::object(pair));
            }
        }
        return JsValue::object(arr);
    };
    addNative(vm, headers, "append", [state](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (args.size() >= 2) headersAppend(*state, args[0].toString(), args[1].toString());
        return JsValue::undefined();
    });
    addNative(vm, headers, "set", [state](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (args.size() >= 2) headersSet(*state, args[0].toString(), args[1].toString());
        return JsValue::undefined();
    });
    addNative(vm, headers, "get", [state](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        if (args.empty()) return JsValue::null();
        auto it = state->find(normalizeHeaderName(args[0].toString()));
        return it == state->end() ? JsValue::null() : vm.str(it->second.second);
    });
    addNative(vm, headers, "has", [state](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        return JsValue::boolean(!args.empty()
            && state->find(normalizeHeaderName(args[0].toString())) != state->end());
    });
    addNative(vm, headers, "delete", [state](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (!args.empty()) state->erase(normalizeHeaderName(args[0].toString()));
        return JsValue::undefined();
    });
    addNative(vm, headers, "forEach", [state](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
        if (args.empty() || !args[0].isCallable()) return JsValue::undefined();
        auto snapshot = *state;
        JsValue thisArg = args.size() > 1 ? args[1] : JsValue::undefined();
        for (const auto& [name, entry] : snapshot)
            vm.call(args[0], thisArg, { vm.str(entry.second), vm.str(name), thisVal });
        return JsValue::undefined();
    });
    addNative(vm, headers, "keys", [makeArray](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        return makeArray(vm, "keys");
    });
    addNative(vm, headers, "values", [makeArray](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        return makeArray(vm, "values");
    });
    addNative(vm, headers, "entries", [makeArray](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        return makeArray(vm, "entries");
    });
    return JsValue::object(headers);
}

static HeaderStore headersFromInit(JsValue init) {
    HeaderStore headers;
    if (!init.isObject()) return headers;
    auto* obj = init.asObject();
    if (obj->kind == ObjKind::Array) {
        for (uint32_t i = 0; i < obj->arrayLength(); ++i) {
            JsValue pairVal = obj->arrayGet(i);
            if (!pairVal.isObject()) continue;
            auto* pair = pairVal.asObject();
            headersAppend(headers, pair->arrayGet(0).toString(), pair->arrayGet(1).toString());
        }
        return headers;
    }
    for (const auto& key : obj->ownEnumKeys()) {
        JsValue value = obj->getProp(key);
        if (!value.isCallable()) headersSet(headers, key, value.toString());
    }
    return headers;
}

static JsValue makeBlobObject(VM& vm, const std::string& body, const std::string& type) {
    auto* blob = vm.gc().newObject(ObjKind::Plain);
    blob->setProp("__vertexBodyText", vm.str(body));
    blob->setProp("__vertexBodyType", vm.str(type));
    blob->setProp("size", JsValue::integer((int32_t)body.size()));
    blob->setProp("type", vm.str(type));
    addNative(vm, blob, "text", [body](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
        return v.promiseResolve(v.str(body));
    });
    addNative(vm, blob, "arrayBuffer", [body](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
        auto* buffer = v.gc().newObject(ObjKind::Plain);
        buffer->setProp("byteLength", JsValue::integer((int32_t)body.size()));
        buffer->setProp("__vertexBytes", v.str(body));
        return v.promiseResolve(JsValue::object(buffer));
    });
    addNative(vm, blob, "slice", [body, type](VM& v, JsValue, std::vector<JsValue> args) -> JsValue {
        int32_t start = args.empty() ? 0 : args[0].toInt32();
        int32_t end = args.size() > 1 && !args[1].isNullOrUndefined() ? args[1].toInt32() : (int32_t)body.size();
        if (start < 0) start = std::max<int32_t>(0, (int32_t)body.size() + start);
        if (end < 0) end = std::max<int32_t>(0, (int32_t)body.size() + end);
        start = std::clamp<int32_t>(start, 0, (int32_t)body.size());
        end = std::clamp<int32_t>(end, start, (int32_t)body.size());
        std::string sliceType = args.size() > 2 ? args[2].toString() : type;
        return makeBlobObject(v, body.substr((size_t)start, (size_t)(end - start)), sliceType);
    });
    return JsValue::object(blob);
}

static std::string bodyFromInit(JsValue init, std::string* typeOut = nullptr) {
    if (typeOut) typeOut->clear();
    if (init.isNullOrUndefined()) return "";
    if (init.isObject()) {
        JsObject* obj = init.asObject();
        JsValue bodyText = obj->getProp("__vertexBodyText");
        if (!bodyText.isUndefined()) {
            JsValue bodyType = obj->getProp("__vertexBodyType");
            if (typeOut && !bodyType.isUndefined()) *typeOut = bodyType.toString();
            return bodyText.toString();
        }
    }
    return init.toString();
}

static std::string headerValue(const HeaderStore& headers, const std::string& name) {
    auto it = headers.find(normalizeHeaderName(name));
    return it == headers.end() ? "" : it->second.second;
}

static void installBodyMethods(VM& vm, JsObject* target, const std::string& body, const std::string& type) {
    target->setProp("bodyUsed", JsValue::boolean(false));
    addNative(vm, target, "text", [target, body](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
        target->setProp("bodyUsed", JsValue::boolean(true));
        return v.promiseResolve(v.str(body));
    });
    addNative(vm, target, "json", [target, body](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
        target->setProp("bodyUsed", JsValue::boolean(true));
        JsValue jsonVal = v.getGlobal("JSON");
        JsValue parsed = JsValue::undefined();
        if (jsonVal.isObject()) {
            JsValue parseFn = jsonVal.asObject()->getProp("parse");
            if (parseFn.isCallable())
                parsed = v.call(parseFn, jsonVal, { v.str(body) });
        }
        return v.promiseResolve(parsed);
    });
    addNative(vm, target, "arrayBuffer", [target, body](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
        target->setProp("bodyUsed", JsValue::boolean(true));
        auto* buffer = v.gc().newObject(ObjKind::Plain);
        buffer->setProp("byteLength", JsValue::integer((int32_t)body.size()));
        buffer->setProp("__vertexBytes", v.str(body));
        return v.promiseResolve(JsValue::object(buffer));
    });
    addNative(vm, target, "blob", [target, body, type](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
        target->setProp("bodyUsed", JsValue::boolean(true));
        return v.promiseResolve(makeBlobObject(v, body, type));
    });
}

static JsValue makeFetchResponse(VM& vm, const std::string& url, const FetchResult& res) {
    auto* response = vm.gc().newObject(ObjKind::Plain);
    int status = res.status > 0 ? res.status : (res.success ? 200 : 0);
    std::string finalUrl = !res.finalUrl.empty() ? res.finalUrl : url;
    response->setProp("ok", JsValue::boolean(res.success));
    response->setProp("status", JsValue::integer(status));
    response->setProp("statusText", vm.str(status >= 200 && status < 300 ? "OK" : ""));
    response->setProp("url", vm.str(finalUrl));
    response->setProp("type", vm.str("basic"));
    response->setProp("redirected", JsValue::boolean(finalUrl != url));
    response->setProp("bodyUsed", JsValue::boolean(false));
    HeaderStore headers;
    if (!res.contentType.empty()) headersSet(headers, "content-type", res.contentType);
    response->setProp("headers", makeHeadersObject(vm, std::move(headers)));
    std::string body = res.success ? res.body : "";
    installBodyMethods(vm, response, body, res.contentType);
    addNative(vm, response, "clone", [url, res](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
        return makeFetchResponse(v, url, res);
    });
    return JsValue::object(response);
}

void registerDom(VM& vm, std::shared_ptr<Node> docNode,
                 std::function<void()> onRepaint,
                 const std::string& pageUrl,
                 DomBridgeCallbacks callbacks) {
    vm.onDomDirty = onRepaint;
    vm.onDomPaintDirty = callbacks.repaintOnly ? callbacks.repaintOnly : onRepaint;
    g_domCallbacks = std::move(callbacks);
    g_windowScrollX = 0.f;
    g_windowScrollY = 0.f;
    g_currentDocument = docNode;

    // Reflect live JS property writes onto the backing DOM node.
    vm.onDomPropSet = [vmPtr = &vm, pageUrl](JsObject* wrapper, const std::string& key, JsValue val) {
        Node* n = static_cast<Node*>(wrapper->domNode);
        if (!n) return;
        // A Plain object carrying a domNode is the style object (reflect CSS) or
        // classList (mutated via its own methods, so ignore stray writes here).
        if (wrapper->kind != ObjKind::DomWrapper) {
            if (g_datasetObjects.find(wrapper) != g_datasetObjects.end()) {
                n->attrs[datasetAttrName(key)] = val.toString();
                markDomDirty(*vmPtr, n, "attributes");
                return;
            }
            if (wrapper->hasOwn("toggle")) return;   // classList, not a style obj
            rebuildStyleAttr(n, wrapper);
            markDomDirty(*vmPtr, n, "style:" + key);
            return;
        }
        if (key == "cookie" && n->type == NodeType::Document) {
            CookieJar::instance().setFromJS(val.toString(), pageUrl);
            wrapper->setProp("cookie", vmPtr->str(CookieJar::instance().documentCookies(pageUrl)));
            return;
        }
        if (key == "className" || key == "class") n->attrs["class"] = val.toString();
        else if (key == "id")    n->attrs["id"] = val.toString();
        else if (key == "role")  n->attrs["role"] = val.toString();
        else if (key == "tabIndex") n->attrs["tabindex"] = val.toString();
        else if (key == "hidden") {
            if (val.toBool()) n->attrs["hidden"] = "";
            else n->attrs.erase("hidden");
        }
        else if (!ariaAttrForProperty(key).empty()) n->attrs[ariaAttrForProperty(key)] = val.toString();
        else if (key == "value") setElementValue(n, val.toString());
        else if (n->type == NodeType::Text
            && (key == "data" || key == "nodeValue" || key == "textContent")) {
            n->text = val.toString();
            wrapper->setProp("data", vmPtr->str(n->text));
            wrapper->setProp("nodeValue", vmPtr->str(n->text));
            wrapper->setProp("textContent", vmPtr->str(n->text));
            markDomDirty(*vmPtr, n->parent ? n->parent : n, "characterData");
            return;
        }
        else if (key == "checked") {
            if (val.toBool()) n->attrs["checked"] = "checked";
            else n->attrs.erase("checked");
        } else if (key == "selected") {
            if (val.toBool()) n->attrs["selected"] = "selected";
            else n->attrs.erase("selected");
        } else if (key == "selectedIndex" && n->tagName == "select") {
            int wanted = val.toInt32();
            std::vector<Node*> options;
            collectDescendants(n, options, [](Node* node) { return node && node->tagName == "option"; });
            for (size_t i = 0; i < options.size(); ++i) {
                if ((int)i == wanted) options[i]->attrs["selected"] = "selected";
                else options[i]->attrs.erase("selected");
            }
        } else if (key == "disabled") {
            if (val.toBool()) n->attrs["disabled"] = "disabled";
            else n->attrs.erase("disabled");
        } else if ((key == "width" || key == "height") && n->tagName == "canvas") {
            // Per spec, setting a canvas's width/height resets its bitmap
            // (clears all drawing state), in addition to changing layout size.
            n->attrs[key] = std::to_string(std::max(0, val.toInt32()));
            if (ICanvasSurface* surf = GetCanvasSurfaceForNode(n)) {
                int w = (int)parseFloatOr(n->attr("width"), 300.f);
                int h = (int)parseFloatOr(n->attr("height"), 150.f);
                surf->Resize(w, h);
            }
        }
        else if (key == "textContent" || key == "innerText") {
            n->children.clear();
            std::string t = val.toString();
            if (!t.empty()) n->appendChild(Node::makeText(t));
        } else if (key == "innerHTML") {
            setInnerHtml(n, val.toString());
        }
        markDomDirty(*vmPtr, n, (key == "textContent" || key == "innerText" || key == "innerHTML") ? "childList" : "attributes");
    };

    // Symmetric live getter: keep className/id/value reads in sync with the node
    // after classList/setAttribute mutations (a stale snapshot otherwise clobbers
    // on read-modify-write, e.g. el.className += ' x').
    vm.onDomPropGet = [vmPtr = &vm, pageUrl](JsObject* wrapper, const std::string& key, JsValue& out) -> bool {
        if (wrapper->kind != ObjKind::DomWrapper) return false;
        Node* n = static_cast<Node*>(wrapper->domNode);
        if (!n) return false;
        if (n->type == NodeType::Document
            && (key == "body" || key == "head" || key == "documentElement")) {
            const char* tag = key == "documentElement" ? "html" : key.c_str();
            Node* found = findFirstTag(n, tag);
            if (!found) { out = JsValue::null(); return true; }
            g_wrapperStore.erase(found);
            auto shared = getShared(found);
            if (!shared) shared = std::shared_ptr<Node>(found, [](Node*) {});
            out = wrapNode(*vmPtr, shared);
            return true;
        }
        if (key == "cookie" && n->type == NodeType::Document) {
            out = vmPtr->str(CookieJar::instance().documentCookies(pageUrl));
            return true;
        }
        if (key == "className") { out = vmPtr->str(n->attr("class")); return true; }
        if (key == "id")        { out = vmPtr->str(n->attr("id"));    return true; }
        if (key == "role")      { out = vmPtr->str(n->attr("role"));  return true; }
        if (key == "tabIndex") {
            std::string value = n->attr("tabindex");
            out = JsValue::integer(value.empty() ? -1 : std::atoi(value.c_str()));
            return true;
        }
        if (key == "hidden") { out = JsValue::boolean(hasAttr(n, "hidden")); return true; }
        std::string ariaAttr = ariaAttrForProperty(key);
        if (!ariaAttr.empty()) { out = vmPtr->str(n->attr(ariaAttr)); return true; }
        if (key == "value")     { out = vmPtr->str(elementValue(n));  return true; }
        if (n->type == NodeType::Text && (key == "data" || key == "nodeValue" || key == "textContent")) {
            out = vmPtr->str(n->text);
            return true;
        }
        if (key == "textContent") {
            out = vmPtr->str(textContent(n));
            return true;
        }
        if (key == "nodeValue") {
            out = JsValue::null();
            return true;
        }
        if (key == "isConnected") {
            out = JsValue::boolean(isConnectedNode(n));
            return true;
        }
        if (key == "ownerDocument") {
            Node* doc = ownerDocumentNode(n);
            auto shared = getShared(doc);
            out = shared ? wrapNodeInternal(*vmPtr, shared, false) : JsValue::null();
            return true;
        }
        if (key == "checked")   { out = JsValue::boolean(hasAttr(n, "checked")); return true; }
        if (key == "selected")  { out = JsValue::boolean(hasAttr(n, "selected")); return true; }
        if (key == "selectedIndex" && n->tagName == "select") {
            out = JsValue::integer(selectedIndex(n));
            return true;
        }
        if (key == "options" && n->tagName == "select") {
            std::vector<Node*> options;
            collectDescendants(n, options, [](Node* node) { return node && node->tagName == "option"; });
            out = JsValue::object(makeNodeCollection(*vmPtr, options));
            return true;
        }
        if (key == "form" && isFormControl(n)) {
            auto shared = getShared(formOwner(n));
            out = shared ? wrapNodeInternal(*vmPtr, shared, false) : JsValue::null();
            return true;
        }
        if (key == "control" && n->tagName == "label") {
            Node* control = findElementById(ownerDocumentNode(n), n->attr("for"));
            auto shared = getShared(control);
            out = shared ? wrapNodeInternal(*vmPtr, shared, false) : JsValue::null();
            return true;
        }
        if (key == "elements" && n->tagName == "form") {
            out = JsValue::object(makeNodeCollection(*vmPtr, formControls(n)));
            return true;
        }
        if (key == "disabled")  { out = JsValue::boolean(hasAttr(n, "disabled")); return true; }
        if (key == "forms" && n->type == NodeType::Document) {
            std::vector<Node*> forms;
            collectDescendants(n, forms, [](Node* node) { return node && node->tagName == "form"; });
            out = JsValue::object(makeNodeCollection(*vmPtr, forms));
            return true;
        }
        if (key == "namespaceURI") {
            auto it = n->attrs.find(kNamespaceAttr);
            out = it == n->attrs.end() ? JsValue::null() : vmPtr->str(it->second);
            return true;
        }
        if (key == "children" || key == "childNodes") {
        auto* arr = newArrayWithPrototype(*vmPtr);
            for (auto& c : n->children) {
                if (key == "children" && c->type != NodeType::Element) continue;
                arr->arrayPush(wrapNodeInternal(*vmPtr, c, false));
            }
            out = JsValue::object(arr);
            return true;
        }
        if (key == "childElementCount") {
            int32_t count = 0;
            for (const auto& c : n->children)
                if (c && c->type == NodeType::Element) ++count;
            out = JsValue::integer(count);
            return true;
        }
        if (key == "firstChild" || key == "lastChild") {
            if (n->children.empty()) out = JsValue::null();
            else out = wrapNodeInternal(*vmPtr, key == "firstChild" ? n->children.front() : n->children.back(), false);
            return true;
        }
        if (key == "firstElementChild" || key == "lastElementChild") {
            std::shared_ptr<Node> found;
            if (key == "firstElementChild") {
                for (auto& c : n->children) {
                    if (c && c->type == NodeType::Element) { found = c; break; }
                }
            } else {
                for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) {
                    if (*it && (*it)->type == NodeType::Element) { found = *it; break; }
                }
            }
            out = found ? wrapNodeInternal(*vmPtr, found, false) : JsValue::null();
            return true;
        }
        if (key == "nextSibling" || key == "previousSibling"
            || key == "nextElementSibling" || key == "previousElementSibling") {
            bool wantNext = key == "nextSibling" || key == "nextElementSibling";
            bool wantElement = key == "nextElementSibling" || key == "previousElementSibling";
            Node* found = nullptr;
            for (Node* cur = siblingNode(n, wantNext ? 1 : -1); cur; cur = siblingNode(cur, wantNext ? 1 : -1)) {
                if (!wantElement || cur->type == NodeType::Element) { found = cur; break; }
            }
            auto shared = getShared(found);
            out = shared ? wrapNodeInternal(*vmPtr, shared, false) : JsValue::null();
            return true;
        }
        if (key == "parentNode" || key == "parentElement") {
            Node* parent = n->parent;
            if (key == "parentElement" && parent && parent->type != NodeType::Element) parent = nullptr;
            auto shared = getShared(parent);
            out = shared ? wrapNodeInternal(*vmPtr, shared, false) : JsValue::null();
            return true;
        }
        return false;
    };

    // Release the previous document's wrapper roots before rewrapping.
    for (auto& r : g_wrapperRoots) vm.gc().removeRoot(r.get());
    g_wrapperRoots.clear();
    g_wrapperStore.clear();
    g_readyWrappers.clear();
    g_datasetObjects.clear();
    g_eventListeners.clear();
    g_windowEventListeners.clear();
    for (auto& r : g_observerRoots) vm.gc().removeRoot(r.get());
    g_observerRoots.clear();
    g_mutationObservers.clear();
    for (auto& r : g_wsRoots) vm.gc().removeRoot(r.get());
    g_wsRoots.clear();

    JsValue docVal = wrapNode(vm, docNode);
    vm.setGlobal("document", docVal);

    auto selectionRanges = std::make_shared<std::vector<JsValue>>();
    auto* selection = vm.gc().newObject(ObjKind::Plain);
    auto syncSelection = [selection, selectionRanges]() {
        selection->setProp("rangeCount", JsValue::integer((int32_t)selectionRanges->size()));
        selection->setProp("__vertexRange0", selectionRanges->empty() ? JsValue::undefined() : selectionRanges->front());
    };
    syncSelection();
    addNative(vm, selection, "removeAllRanges", [selectionRanges, syncSelection](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        selectionRanges->clear();
        syncSelection();
        return JsValue::undefined();
    });
    addNative(vm, selection, "addRange", [selectionRanges, syncSelection](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (!args.empty()) selectionRanges->push_back(args[0]);
        syncSelection();
        return JsValue::undefined();
    });
    addNative(vm, selection, "getRangeAt", [selectionRanges](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        int32_t index = args.empty() ? 0 : args[0].toInt32();
        if (index < 0 || index >= (int32_t)selectionRanges->size()) return JsValue::undefined();
        return (*selectionRanges)[(size_t)index];
    });
    addNative(vm, selection, "toString", [selectionRanges](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
        if (selectionRanges->empty() || !selectionRanges->front().isObject()) return v.str("");
        JsValue toStringFn = selectionRanges->front().asObject()->getProp("toString");
        return toStringFn.isCallable() ? v.call(toStringFn, selectionRanges->front(), {}) : v.str("");
    });
    vm.setGlobal("__vertexSelection", JsValue::object(selection));
    vm.setGlobal("getSelection", JsValue::object(vm.gc().newNativeFunction(
        [selection](VM&, JsValue, std::vector<JsValue>) -> JsValue {
            return JsValue::object(selection);
        }, "getSelection")));

    // document.body / document.head / document.documentElement
    auto findFirst = [&](const std::string& tag) -> JsValue {
        std::vector<Node*> stack;
        stack.push_back(docNode.get());
        while (!stack.empty()) {
            Node* n = stack.back();
            stack.pop_back();
            if (!n) continue;
            if (n->tagName == tag) {
                auto shared = getShared(n);
                return shared ? wrapNode(vm, shared) : JsValue::null();
            }
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                stack.push_back(it->get());
        }
        return JsValue::null();
    };

    if (docVal.isObject()) {
        docVal.asObject()->setProp("body",            findFirst("body"));
        docVal.asObject()->setProp("head",            findFirst("head"));
        docVal.asObject()->setProp("documentElement", findFirst("html"));
        docVal.asObject()->setProp("title",           vm.str(""));
        docVal.asObject()->setProp("URL",             vm.str(pageUrl));
        docVal.asObject()->setProp("baseURI",         vm.str(pageUrl));
        docVal.asObject()->setProp("readyState",      vm.str("complete"));
        docVal.asObject()->setProp("domain",          vm.str(parseDomUrl(pageUrl).hostname));
        docVal.asObject()->setProp("cookie",          vm.str(CookieJar::instance().documentCookies(pageUrl)));
        docVal.asObject()->setProp("referrer",        vm.str(""));
        docVal.asObject()->setProp("lastModified",    vm.str(""));
        docVal.asObject()->setProp("characterSet",    vm.str("UTF-8"));
        docVal.asObject()->setProp("activeElement",   findFirst("body"));
        docVal.asObject()->setProp("compatMode",      vm.str("CSS1Compat"));
        docVal.asObject()->setProp("visibilityState", vm.str("visible"));
        docVal.asObject()->setProp("hidden",          JsValue::boolean(false));
        addNative(vm, docVal.asObject(), "getSelection", [selection](VM&, JsValue, std::vector<JsValue>) -> JsValue {
            return JsValue::object(selection);
        });
        auto* impl = vm.gc().newObject(ObjKind::Plain);
        addNative(vm, impl, "createHTMLDocument", [](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string title = args.empty() ? "" : args[0].toString();
            auto doc = ParseHtml("<html><head><title>" + title + "</title></head><body></body></html>");
            registerSubtree(doc);
            return wrapNode(vm, doc);
        });
        docVal.asObject()->setProp("implementation", JsValue::object(impl));
    }

    // window globals — populate location from the page URL.
    auto* winLocation = vm.gc().newObject(ObjKind::Plain);
    auto* winHistory = vm.gc().newObject(ObjKind::Plain);
    struct HistoryStack {
        std::vector<std::string> entries;
        size_t index = 0;
    };
    auto historyStack = std::make_shared<HistoryStack>();
    historyStack->entries.push_back(pageUrl);

    auto syncLocation = [winLocation, &vm](const std::string& href) {
        applyLocationProps(vm, winLocation, href);
    };
    auto syncHistoryLength = [winHistory, historyStack]() {
        winHistory->setProp("length", JsValue::integer((int32_t)historyStack->entries.size()));
    };
    syncLocation(pageUrl);
    syncHistoryLength();
    winHistory->setProp("state", JsValue::null());

    addNative(vm, winLocation, "assign",
        [historyStack, syncLocation, syncHistoryLength, baseUrl = pageUrl]
        (VM&, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string current = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            std::string href = resolveDomUrl(args.empty() ? "" : args[0].toString(), current);
            if (historyStack->index + 1 < historyStack->entries.size())
                historyStack->entries.erase(historyStack->entries.begin() + historyStack->index + 1, historyStack->entries.end());
            historyStack->entries.push_back(href);
            historyStack->index = historyStack->entries.size() - 1;
            syncLocation(href);
            syncHistoryLength();
            if (g_domCallbacks.navigate) g_domCallbacks.navigate(href, false);
            return JsValue::undefined();
        });
    addNative(vm, winLocation, "replace",
        [historyStack, syncLocation, syncHistoryLength, baseUrl = pageUrl]
        (VM&, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string current = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            std::string href = resolveDomUrl(args.empty() ? "" : args[0].toString(), current);
            if (historyStack->entries.empty()) historyStack->entries.push_back(href);
            else historyStack->entries[historyStack->index] = href;
            syncLocation(href);
            syncHistoryLength();
            if (g_domCallbacks.navigate) g_domCallbacks.navigate(href, true);
            return JsValue::undefined();
        });
    addNative(vm, winLocation, "reload",
        [historyStack, baseUrl = pageUrl](VM&, JsValue, std::vector<JsValue>) -> JsValue {
            std::string href = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            if (g_domCallbacks.navigate) g_domCallbacks.navigate(href, true);
            return JsValue::undefined();
        });
    vm.setGlobal("location", JsValue::object(winLocation));

    addNative(vm, winHistory, "pushState",
        [historyStack, syncLocation, syncHistoryLength, winHistory, baseUrl = pageUrl]
        (VM&, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string current = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            std::string href = args.size() >= 3 && !args[2].isNullOrUndefined()
                ? resolveDomUrl(args[2].toString(), current)
                : current;
            if (historyStack->index + 1 < historyStack->entries.size())
                historyStack->entries.erase(historyStack->entries.begin() + historyStack->index + 1, historyStack->entries.end());
            historyStack->entries.push_back(href);
            historyStack->index = historyStack->entries.size() - 1;
            winHistory->setProp("state", args.empty() ? JsValue::null() : args[0]);
            syncLocation(href);
            syncHistoryLength();
            return JsValue::undefined();
        });
    addNative(vm, winHistory, "replaceState",
        [historyStack, syncLocation, syncHistoryLength, winHistory, baseUrl = pageUrl]
        (VM&, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string current = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            std::string href = args.size() >= 3 && !args[2].isNullOrUndefined()
                ? resolveDomUrl(args[2].toString(), current)
                : current;
            if (historyStack->entries.empty()) historyStack->entries.push_back(href);
            else historyStack->entries[historyStack->index] = href;
            winHistory->setProp("state", args.empty() ? JsValue::null() : args[0]);
            syncLocation(href);
            syncHistoryLength();
            return JsValue::undefined();
        });
    addNative(vm, winHistory, "back",
        [historyStack, syncLocation](VM&, JsValue, std::vector<JsValue>) -> JsValue {
            if (historyStack->index > 0) {
                historyStack->index--;
                syncLocation(historyStack->entries[historyStack->index]);
            }
            return JsValue::undefined();
        });
    addNative(vm, winHistory, "forward",
        [historyStack, syncLocation](VM&, JsValue, std::vector<JsValue>) -> JsValue {
            if (historyStack->index + 1 < historyStack->entries.size()) {
                historyStack->index++;
                syncLocation(historyStack->entries[historyStack->index]);
            }
            return JsValue::undefined();
        });
    vm.setGlobal("history", JsValue::object(winHistory));

    auto* winNavigator = vm.gc().newObject(ObjKind::Plain);
    winNavigator->setProp("userAgent",   vm.str("Vertex/1.0"));
    winNavigator->setProp("language",    vm.str("en-US"));
    auto* languages = newArrayWithPrototype(vm);
    languages->arrayPush(vm.str("en-US"));
    languages->arrayPush(vm.str("en"));
    winNavigator->setProp("languages", JsValue::object(languages));
    winNavigator->setProp("onLine",      JsValue::boolean(true));
    winNavigator->setProp("platform",    vm.str("Win32"));
    winNavigator->setProp("cookieEnabled",JsValue::boolean(true));
    winNavigator->setProp("hardwareConcurrency", JsValue::integer(4));
    winNavigator->setProp("maxTouchPoints", JsValue::integer(0));
    {
        auto* uaData = vm.gc().newObject(ObjKind::Plain);
        auto* brands = newArrayWithPrototype(vm);
        auto* vertexBrand = vm.gc().newObject(ObjKind::Plain);
        vertexBrand->setProp("brand", vm.str("Vertex"));
        vertexBrand->setProp("version", vm.str("1"));
        brands->arrayPush(JsValue::object(vertexBrand));
        uaData->setProp("brands", JsValue::object(brands));
        uaData->setProp("mobile", JsValue::boolean(false));
        uaData->setProp("platform", vm.str("Windows"));
        addNative(vm, uaData, "getHighEntropyValues", [uaData](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
            auto* out = vm.gc().newObject(ObjKind::Plain);
            out->setProp("brands", uaData->getProp("brands"));
            out->setProp("mobile", uaData->getProp("mobile"));
            out->setProp("platform", uaData->getProp("platform"));
            out->setProp("architecture", vm.str("x86"));
            out->setProp("model", vm.str(""));
            out->setProp("uaFullVersion", vm.str("1.0"));
            return vm.promiseResolve(JsValue::object(out));
        });
        winNavigator->setProp("userAgentData", JsValue::object(uaData));
    }
    {
        auto* clipboard = vm.gc().newObject(ObjKind::Plain);
        auto text = std::make_shared<std::string>();
        addNative(vm, clipboard, "writeText", [text](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            *text = args.empty() ? "" : args[0].toString();
            return vm.promiseResolve(JsValue::undefined());
        });
        addNative(vm, clipboard, "readText", [text](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
            return vm.promiseResolve(vm.str(*text));
        });
        winNavigator->setProp("clipboard", JsValue::object(clipboard));
    }
    vm.setGlobal("navigator", JsValue::object(winNavigator));

    vm.setGlobal("top", JsValue::object(vm.globals()));
    vm.setGlobal("parent", JsValue::object(vm.globals()));

    auto* screen = vm.gc().newObject(ObjKind::Plain);
    screen->setProp("width",  JsValue::integer(1280));
    screen->setProp("height", JsValue::integer(800));
    screen->setProp("availWidth",  JsValue::integer(1280));
    screen->setProp("availHeight", JsValue::integer(800));
    screen->setProp("colorDepth",  JsValue::integer(24));
    vm.setGlobal("screen", JsValue::object(screen));

    vm.setGlobal("innerWidth",  JsValue::integer(1280));
    vm.setGlobal("innerHeight", JsValue::integer(800));
    syncWindowScrollGlobals(vm);
    vm.setGlobal("devicePixelRatio", JsValue::number(1.0));

    static auto localStorageData = std::make_shared<std::map<std::string, std::string>>();
    auto sessionStorageData = std::make_shared<std::map<std::string, std::string>>();
    vm.setGlobal("localStorage", makeStorageObject(vm, localStorageData));
    vm.setGlobal("sessionStorage", makeStorageObject(vm, sessionStorageData));

    vm.setGlobal("URLSearchParams", JsValue::object(vm.gc().newNativeFunction(
        [](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            return makeURLSearchParams(vm, queryPairsFromInit(vm, args.empty() ? JsValue::undefined() : args[0]));
        }, "URLSearchParams")));

    auto blobUrls = std::make_shared<std::map<std::string, JsValue>>();

    vm.setGlobal("FormData", JsValue::object(vm.gc().newNativeFunction(
        [](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            return makeFormData(vm, formDataPairsFromInit(vm, args.empty() ? JsValue::undefined() : args[0]));
        }, "FormData")));

    auto* urlCtor = vm.gc().newNativeFunction(
        [pageUrl](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string input = args.empty() ? "" : args[0].toString();
            std::string base = args.size() > 1 ? args[1].toString() : pageUrl;
            std::string href = ResolveUrlAgainstBase(input, base);
            auto* url = vm.gc().newObject(ObjKind::Plain);
            VM* vmPtr = &vm;
            auto apply = [url, vmPtr](const std::string& nextHref) {
                ParsedDomUrl parsed = parseDomUrl(nextHref);
                url->setProp("href", vmPtr->str(parsed.href));
                url->setProp("origin", vmPtr->str(parsed.origin));
                url->setProp("protocol", vmPtr->str(parsed.protocol));
                url->setProp("host", vmPtr->str(parsed.host));
                url->setProp("hostname", vmPtr->str(parsed.hostname));
                url->setProp("pathname", vmPtr->str(parsed.pathname));
                url->setProp("search", vmPtr->str(parsed.search));
                url->setProp("hash", vmPtr->str(parsed.hash));
            };
            apply(href);
            auto updateSearch = [url, apply](const std::string& query) mutable {
                std::string origin = url->getProp("origin").toString();
                std::string path = url->getProp("pathname").toString();
                std::string hash = url->getProp("hash").toString();
                std::string next = origin + path + (query.empty() ? "" : "?" + query) + hash;
                apply(next);
            };
            url->setProp("searchParams", makeURLSearchParams(vm, parseDomUrl(href).search, updateSearch));
            addNative(vm, url, "toString", [url](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
                return vm.str(url->getProp("href").toString());
            });
            return JsValue::object(url);
        }, "URL");
    addNative(vm, urlCtor, "canParse", [pageUrl](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (args.empty()) return JsValue::boolean(false);
        std::string base = args.size() > 1 ? args[1].toString() : pageUrl;
        std::string href = ResolveUrlAgainstBase(args[0].toString(), base);
        return JsValue::boolean(!href.empty());
    });
    addNative(vm, urlCtor, "createObjectURL", [blobUrls](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        static int nextBlobUrl = 1;
        std::string url = "blob:vertex-" + std::to_string(nextBlobUrl++);
        (*blobUrls)[url] = args.empty() ? JsValue::undefined() : args[0];
        return vm.str(url);
    });
    addNative(vm, urlCtor, "revokeObjectURL", [blobUrls](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (!args.empty()) blobUrls->erase(args[0].toString());
        return JsValue::undefined();
    });
    vm.setGlobal("URL", JsValue::object(urlCtor));

    auto* cssObj = vm.gc().newObject(ObjKind::Plain);
    addNative(vm, cssObj, "supports", [](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (args.size() >= 2) return JsValue::boolean(cssSupportsDeclaration(args[0].toString(), args[1].toString()));
        return JsValue::boolean(!args.empty() && cssSupportsCondition(args[0].toString()));
    });
    addNative(vm, cssObj, "escape", [](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string in = args.empty() ? "" : args[0].toString();
        std::string out;
        for (char c : in) {
            if (std::isalnum((unsigned char)c) || c == '_' || c == '-') out += c;
            else { out += '\\'; out += c; }
        }
        return vm.str(out);
    });
    vm.setGlobal("CSS", JsValue::object(cssObj));

    vm.setGlobal("DOMParser", JsValue::object(vm.gc().newNativeFunction(
        [](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            auto* parser = vm.gc().newObject(ObjKind::Plain);
            addNative(vm, parser, "parseFromString", [](VM& vm, JsValue, std::vector<JsValue> parseArgs) -> JsValue {
                auto doc = ParseHtml(parseArgs.empty() ? "" : parseArgs[0].toString());
                registerSubtree(doc);
                return wrapNode(vm, doc);
            });
            return JsValue::object(parser);
        }, "DOMParser")));

    // getComputedStyle(element): lightweight CSSOM from inline style + defaults.
    vm.setGlobal("getComputedStyle", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("getComputedStyle") {
            Node* n = unwrapNode(ARG(0));
            auto* style = vm.gc().newObject(ObjKind::Plain);
            auto props = computedStyleMap(n);
            for (const auto& [key, val] : props) style->setProp(key, vm.str(val));
            addNative(vm, style, "getPropertyValue", NATIVE("getPropertyValue") {
                if (!thisVal.isObject() || args.empty()) return vm.str("");
                std::string key = lowerCopy(trimCopy(ARG_STR(0)));
                JsValue direct = thisVal.asObject()->getProp(key);
                if (!direct.isUndefined()) return direct;
                JsValue camel = thisVal.asObject()->getProp(cssCamel(key));
                return camel.isUndefined() ? vm.str("") : camel;
            });
            return JsValue::object(style);
        }, "getComputedStyle")));

    // matchMedia(query) — returns { matches: false, media: query }.
    vm.setGlobal("matchMedia", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("matchMedia") {
            std::string query = lowerCopy(ARG_STR(0));
            auto* mql = vm.gc().newObject(ObjKind::Plain);
            bool matches = true;
            if (query.find("not ") != std::string::npos) matches = false;
            if (query.find("screen") != std::string::npos || query.find("all") != std::string::npos) matches = matches && true;
            if (query.find("prefers-color-scheme: dark") != std::string::npos) matches = false;
            if (query.find("prefers-color-scheme: light") != std::string::npos) matches = matches && true;
            auto parsePxAfter = [&](const std::string& needle, float fallback) {
                size_t pos = query.find(needle);
                if (pos == std::string::npos) return fallback;
                pos += needle.size();
                while (pos < query.size() && (query[pos] == ' ' || query[pos] == ':')) ++pos;
                try { return std::stof(query.substr(pos)); } catch (...) { return fallback; }
            };
            float minW = parsePxAfter("min-width", -1.f);
            float maxW = parsePxAfter("max-width", -1.f);
            float minH = parsePxAfter("min-height", -1.f);
            float maxH = parsePxAfter("max-height", -1.f);
            if (minW >= 0) matches = matches && 1280.f >= minW;
            if (maxW >= 0) matches = matches && 1280.f <= maxW;
            if (minH >= 0) matches = matches && 800.f >= minH;
            if (maxH >= 0) matches = matches && 800.f <= maxH;
            mql->setProp("matches", JsValue::boolean(matches));
            mql->setProp("media", vm.str(ARG_STR(0)));
            auto listeners = std::make_shared<std::vector<EventListener>>();
            addNative(vm, mql, "addEventListener", [listeners](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
                if (args.size() >= 2 && args[1].isCallable())
                    listeners->push_back({ args[0].toString(), args[1] });
                return JsValue::undefined();
            });
            addNative(vm, mql, "removeEventListener", [listeners](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
                if (args.size() < 2) return JsValue::undefined();
                std::string eventName = args[0].toString();
                JsValue fn = args[1];
                auto it = std::find_if(listeners->begin(), listeners->end(),
                    [&](const EventListener& listener) {
                        return listener.event == eventName && listener.fn.strictEq(fn);
                    });
                if (it != listeners->end()) listeners->erase(it);
                return JsValue::undefined();
            });
            addNative(vm, mql, "addListener", [listeners](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
                if (!args.empty() && args[0].isCallable())
                    listeners->push_back({ "change", args[0] });
                return JsValue::undefined();
            });
            addNative(vm, mql, "removeListener", [listeners](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
                if (args.empty()) return JsValue::undefined();
                JsValue fn = args[0];
                auto it = std::find_if(listeners->begin(), listeners->end(),
                    [&](const EventListener& listener) {
                        return listener.event == "change" && listener.fn.strictEq(fn);
                    });
                if (it != listeners->end()) listeners->erase(it);
                return JsValue::undefined();
            });
            addNative(vm, mql, "dispatchEvent", [listeners, mql](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
                if (args.empty() || !args[0].isObject()) return JsValue::boolean(true);
                JsObject* ev = args[0].asObject();
                std::string type = ev->getProp("type").toString();
                if (type.empty()) type = "change";
                ev->setProp("target", JsValue::object(mql));
                ev->setProp("currentTarget", JsValue::object(mql));
                ev->setProp("matches", mql->getProp("matches"));
                ev->setProp("media", mql->getProp("media"));
                installEventMethods(vm, ev);
                JsValue propertyHandler = mql->getProp("on" + type);
                if (propertyHandler.isCallable()) {
                    try { vm.call(propertyHandler, thisVal, { args[0] }); } catch (...) {}
                }
                auto snapshot = *listeners;
                for (const auto& listener : snapshot) {
                    if (listener.event != type || !listener.fn.isCallable()) continue;
                    try { vm.call(listener.fn, thisVal, { args[0] }); } catch (...) {}
                    if (eventFlag(ev, "__vertexStopped")) break;
                }
                vm.drainMicrotasks();
                return JsValue::boolean(!eventFlag(ev, "defaultPrevented"));
            });
            return JsValue::object(mql);
        }, "matchMedia")));

    // fetch/body primitives.
    vm.setGlobal("Blob", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("Blob") {
            std::string body;
            if (!args.empty() && ARG(0).isObject()) {
                JsObject* parts = ARG(0).asObject();
                if (parts->kind == ObjKind::Array) {
                    for (uint32_t i = 0; i < parts->arrayLength(); ++i)
                        body += bodyFromInit(parts->arrayGet(i));
                } else {
                    body = bodyFromInit(ARG(0));
                }
            }
            std::string type;
            if (args.size() > 1 && ARG(1).isObject()) {
                JsValue optType = ARG(1).asObject()->getProp("type");
                if (!optType.isUndefined()) type = optType.toString();
            }
            return makeBlobObject(vm, body, type);
        }, "Blob")));

    vm.setGlobal("FileReader", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("FileReader") {
            auto* reader = vm.gc().newObject(ObjKind::Plain);
            reader->setProp("EMPTY", JsValue::integer(0));
            reader->setProp("LOADING", JsValue::integer(1));
            reader->setProp("DONE", JsValue::integer(2));
            reader->setProp("readyState", JsValue::integer(0));
            reader->setProp("result", JsValue::null());
            reader->setProp("error", JsValue::null());
            auto dispatchReaderEvent = [reader](VM& v, const std::string& type) {
                JsValue ev = makeEventObject(v, "Event", type);
                if (ev.isObject()) {
                    ev.asObject()->setProp("target", JsValue::object(reader));
                    ev.asObject()->setProp("currentTarget", JsValue::object(reader));
                }
                JsValue handler = reader->getProp("on" + type);
                if (handler.isCallable()) {
                    try { v.call(handler, JsValue::object(reader), { ev }); } catch (...) {}
                }
            };
            addNative(vm, reader, "readAsText", [reader, dispatchReaderEvent](VM& v, JsValue, std::vector<JsValue> args) -> JsValue {
                reader->setProp("readyState", JsValue::integer(1));
                std::string text = args.empty() ? "" : bodyFromInit(args[0]);
                reader->setProp("result", v.str(text));
                reader->setProp("readyState", JsValue::integer(2));
                dispatchReaderEvent(v, "load");
                dispatchReaderEvent(v, "loadend");
                return JsValue::undefined();
            });
            addNative(vm, reader, "readAsArrayBuffer", [reader, dispatchReaderEvent](VM& v, JsValue, std::vector<JsValue> args) -> JsValue {
                reader->setProp("readyState", JsValue::integer(1));
                std::string text = args.empty() ? "" : bodyFromInit(args[0]);
                auto* buffer = v.gc().newObject(ObjKind::Plain);
                buffer->setProp("byteLength", JsValue::integer((int32_t)text.size()));
                buffer->setProp("__vertexBytes", v.str(text));
                reader->setProp("result", JsValue::object(buffer));
                reader->setProp("readyState", JsValue::integer(2));
                dispatchReaderEvent(v, "load");
                dispatchReaderEvent(v, "loadend");
                return JsValue::undefined();
            });
            addNative(vm, reader, "abort", [reader, dispatchReaderEvent](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
                reader->setProp("readyState", JsValue::integer(2));
                dispatchReaderEvent(v, "abort");
                dispatchReaderEvent(v, "loadend");
                return JsValue::undefined();
            });
            return JsValue::object(reader);
        }, "FileReader")));

    vm.setGlobal("Headers", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("Headers") {
            HeaderStore initial = args.empty() ? HeaderStore{} : headersFromInit(ARG(0));
            return makeHeadersObject(vm, std::move(initial));
        }, "Headers")));

    vm.setGlobal("Request", JsValue::object(vm.gc().newNativeFunction(
        [baseUrl = pageUrl](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            JsValue input = args.empty() ? JsValue::undefined() : args[0];
            JsObject* options = args.size() > 1 && args[1].isObject() ? args[1].asObject() : nullptr;
            std::string url;
            std::string method = "GET";
            HeaderStore headers;
            std::string body;
            std::string bodyType;
            if (input.isObject()) {
                JsObject* inputObj = input.asObject();
                JsValue inputUrl = inputObj->getProp("url");
                url = inputUrl.isUndefined() ? input.toString() : inputUrl.toString();
                JsValue inputMethod = inputObj->getProp("method");
                if (!inputMethod.isUndefined()) method = inputMethod.toString();
                JsValue inputHeaders = inputObj->getProp("headers");
                if (inputHeaders.isObject()) headers = headersFromInit(inputHeaders);
            } else {
                url = input.toString();
            }
            if (options) {
                JsValue optMethod = options->getProp("method");
                if (!optMethod.isUndefined()) method = optMethod.toString();
                JsValue optHeaders = options->getProp("headers");
                if (!optHeaders.isUndefined()) headers = headersFromInit(optHeaders);
                JsValue optBody = options->getProp("body");
                if (!optBody.isUndefined() && !optBody.isNull())
                    body = bodyFromInit(optBody, &bodyType);
            }
            std::transform(method.begin(), method.end(), method.begin(),
                [](unsigned char c) { return (char)std::toupper(c); });
            if (!body.empty() && !bodyType.empty() && headerValue(headers, "content-type").empty())
                headersSet(headers, "content-type", bodyType);
            auto* request = vm.gc().newObject(ObjKind::Plain);
            request->setProp("url", vm.str(resolveDomUrl(url, baseUrl)));
            request->setProp("method", vm.str(method.empty() ? "GET" : method));
            request->setProp("headers", makeHeadersObject(vm, std::move(headers)));
            request->setProp("mode", vm.str("cors"));
            request->setProp("credentials", vm.str("same-origin"));
            request->setProp("cache", vm.str("default"));
            request->setProp("redirect", vm.str("follow"));
            request->setProp("referrer", vm.str("about:client"));
            installBodyMethods(vm, request, body, "");
            addNative(vm, request, "clone", [baseUrl](VM& v, JsValue thisVal, std::vector<JsValue>) -> JsValue {
                auto* cloned = v.gc().newObject(ObjKind::Plain);
                if (thisVal.isObject()) {
                    auto* src = thisVal.asObject();
                    cloned->setProp("url", src->getProp("url"));
                    cloned->setProp("method", src->getProp("method"));
                    cloned->setProp("headers", src->getProp("headers"));
                    cloned->setProp("mode", src->getProp("mode"));
                    cloned->setProp("credentials", src->getProp("credentials"));
                    cloned->setProp("cache", src->getProp("cache"));
                    cloned->setProp("redirect", src->getProp("redirect"));
                    cloned->setProp("referrer", src->getProp("referrer"));
                } else {
                    cloned->setProp("url", v.str(baseUrl));
                    cloned->setProp("method", v.str("GET"));
                    cloned->setProp("headers", makeHeadersObject(v));
                }
                installBodyMethods(v, cloned, "", "");
                return JsValue::object(cloned);
            });
            return JsValue::object(request);
        }, "Request")));

    vm.setGlobal("Response", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("Response") {
            std::string body = args.empty() || ARG(0).isNullOrUndefined() ? "" : ARG(0).toString();
            std::string bodyType;
            if (!args.empty()) body = bodyFromInit(ARG(0), &bodyType);
            JsObject* options = args.size() > 1 && ARG(1).isObject() ? ARG(1).asObject() : nullptr;
            int status = 200;
            std::string statusText = "OK";
            HeaderStore headers;
            if (options) {
                JsValue optStatus = options->getProp("status");
                if (!optStatus.isUndefined()) status = optStatus.toInt32();
                JsValue optStatusText = options->getProp("statusText");
                if (!optStatusText.isUndefined()) statusText = optStatusText.toString();
                JsValue optHeaders = options->getProp("headers");
                if (!optHeaders.isUndefined()) headers = headersFromInit(optHeaders);
            }
            std::string responseType = headerValue(headers, "content-type");
            if (responseType.empty()) responseType = bodyType;
            if (!body.empty() && !bodyType.empty() && headerValue(headers, "content-type").empty())
                headersSet(headers, "content-type", bodyType);
            auto* response = vm.gc().newObject(ObjKind::Plain);
            response->setProp("ok", JsValue::boolean(status >= 200 && status < 300));
            response->setProp("status", JsValue::integer(status));
            response->setProp("statusText", vm.str(statusText));
            response->setProp("url", vm.str(""));
            response->setProp("type", vm.str("default"));
            response->setProp("redirected", JsValue::boolean(false));
            response->setProp("headers", makeHeadersObject(vm, std::move(headers)));
            installBodyMethods(vm, response, body, responseType);
            addNative(vm, response, "clone", [body, status, statusText, responseType](VM& v, JsValue thisVal, std::vector<JsValue>) -> JsValue {
                auto* cloned = v.gc().newObject(ObjKind::Plain);
                cloned->setProp("ok", JsValue::boolean(status >= 200 && status < 300));
                cloned->setProp("status", JsValue::integer(status));
                cloned->setProp("statusText", v.str(statusText));
                cloned->setProp("url", v.str(""));
                cloned->setProp("type", v.str("default"));
                cloned->setProp("redirected", JsValue::boolean(false));
                cloned->setProp("headers", thisVal.isObject() ? thisVal.asObject()->getProp("headers") : makeHeadersObject(v));
                installBodyMethods(v, cloned, body, responseType);
                return JsValue::object(cloned);
            });
            return JsValue::object(response);
        }, "Response")));

    vm.setGlobal("AbortController", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("AbortController") {
            auto* signal = vm.gc().newObject(ObjKind::Plain);
            signal->setProp("aborted", JsValue::boolean(false));
            signal->setProp("reason", JsValue::undefined());
            auto* controller = vm.gc().newObject(ObjKind::Plain);
            controller->setProp("signal", JsValue::object(signal));
            addNative(vm, controller, "abort", [signal](VM& v, JsValue, std::vector<JsValue> args) -> JsValue {
                signal->setProp("aborted", JsValue::boolean(true));
                signal->setProp("reason", args.empty() ? v.makeError("AbortError", "aborted") : args[0]);
                return JsValue::undefined();
            });
            return JsValue::object(controller);
        }, "AbortController")));

    vm.setGlobal("fetch", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("fetch") {
            std::string url;
            if (ARG(0).isObject()) {
                JsValue requestUrl = ARG(0).asObject()->getProp("url");
                url = requestUrl.isUndefined() ? ARG_STR(0) : requestUrl.toString();
            } else {
                url = ARG_STR(0);
            }
            auto* promise = vm.gc().newPromise();
            vm.initPromiseObject(promise);
            JsValue signal = JsValue::undefined();
            if (args.size() > 1 && ARG(1).isObject())
                signal = ARG(1).asObject()->getProp("signal");
            if (signal.isObject() && signal.asObject()->getProp("aborted").toBool()) {
                JsValue reason = signal.asObject()->getProp("reason");
                vm.settlePromiseObject(promise,
                    reason.isUndefined() ? vm.makeError("AbortError", "aborted") : reason,
                    true);
                return JsValue::object(promise);
            }
            VM* vmPtr = &vm;
            auto alive = vm.lifetimeToken();
            FetchResourceAsync(url, 12 * 1024 * 1024, ResourceKind::Other,
                [vmPtr, alive, promise, url](FetchResult res) {
                    if (alive.expired()) return;
                    if (res.success) {
                        vmPtr->settlePromiseObject(promise, makeFetchResponse(*vmPtr, url, res), false);
                    } else {
                        vmPtr->settlePromiseObject(promise, vmPtr->makeError("TypeError", "fetch failed"), true);
                    }
                    vmPtr->drainMicrotasks();
                });
            return JsValue::object(promise);
        }, "fetch")));

    vm.setGlobal("WebSocket", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("WebSocket") {
            std::string url = ARG_STR(0);
            auto* ws = vm.gc().newObject(ObjKind::Plain);
            ws->setProp("url", vm.str(url));
            ws->setProp("readyState", JsValue::number(0)); // CONNECTING
            ws->setProp("onopen", JsValue::undefined());
            ws->setProp("onmessage", JsValue::undefined());
            ws->setProp("onclose", JsValue::undefined());
            ws->setProp("onerror", JsValue::undefined());
            ws->setProp("CONNECTING", JsValue::number(0));
            ws->setProp("OPEN", JsValue::number(1));
            ws->setProp("CLOSING", JsValue::number(2));
            ws->setProp("CLOSED", JsValue::number(3));
            installEventMethods(vm, ws);

            g_wsRoots.push_back(std::make_unique<JsValue>(JsValue::object(ws)));
            vm.gc().addRoot(g_wsRoots.back().get());
            JsValue wsVal = *g_wsRoots.back();

            VM* vmPtr = &vm;
            auto alive = vm.lifetimeToken();
            int handle = OpenWebSocket(url, [vmPtr, alive, wsVal](WsEvent wsEv) {
                if (alive.expired()) return;
                VM& vm2 = *vmPtr;
                JsObject* wsObj = wsVal.asObject();

                auto* ev = vm2.gc().newObject(ObjKind::Plain);
                ev->setProp("target", wsVal);
                ev->setProp("currentTarget", wsVal);
                ev->setProp("bubbles", JsValue::boolean(false));
                ev->setProp("cancelable", JsValue::boolean(false));
                ev->setProp("defaultPrevented", JsValue::boolean(false));
                installEventMethods(vm2, ev);
                std::string eventName;

                switch (wsEv.kind) {
                case WsEventKind::Open:
                    wsObj->setProp("readyState", JsValue::number(1));
                    eventName = "open";
                    break;
                case WsEventKind::Message:
                    eventName = "message";
                    // Binary frames are handed over as their raw bytes in a
                    // string for now (no ArrayBuffer/Blob for incoming
                    // binary yet) — same "start simple, defer extras"
                    // scoping already used for Canvas 2D's deferred features.
                    ev->setProp("data", vm2.str(wsEv.data));
                    break;
                case WsEventKind::Close:
                    wsObj->setProp("readyState", JsValue::number(3));
                    eventName = "close";
                    ev->setProp("code", JsValue::number(wsEv.code));
                    ev->setProp("reason", vm2.str(wsEv.reason));
                    ev->setProp("wasClean", JsValue::boolean(wsEv.wasClean));
                    break;
                case WsEventKind::Error:
                    eventName = "error";
                    ev->setProp("message", vm2.str(wsEv.data));
                    break;
                }
                ev->setProp("type", vm2.str(eventName));

                JsValue handler = wsObj->getProp("on" + eventName);
                if (handler.isCallable()) {
                    try { vm2.call(handler, wsVal, { JsValue::object(ev) }); } catch (...) {}
                }
                vm2.drainMicrotasks();
            });

            addNative(vm, ws, "send", [handle](VM&, JsValue, std::vector<JsValue> sendArgs) -> JsValue {
                if (!sendArgs.empty()) SendWebSocketText(handle, sendArgs[0].toString());
                return JsValue::undefined();
            });
            addNative(vm, ws, "close", [handle](VM&, JsValue thisVal, std::vector<JsValue> closeArgs) -> JsValue {
                int code = closeArgs.size() > 0 ? (int)closeArgs[0].toNumber() : 1000;
                std::string reason = closeArgs.size() > 1 ? closeArgs[1].toString() : "";
                if (thisVal.isObject()) thisVal.asObject()->setProp("readyState", JsValue::number(2)); // CLOSING
                CloseWebSocket(handle, code, reason);
                return JsValue::undefined();
            });

            return wsVal;
        }, "WebSocket")));

    // window.addEventListener / removeEventListener
    vm.setGlobal("addEventListener", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("win_addEventListener") {
            if (args.size() >= 2 && ARG(1).isCallable())
                g_windowEventListeners.push_back({ ARG_STR(0), ARG(1) });
            return JsValue::undefined();
        }, "addEventListener")));
    vm.setGlobal("removeEventListener", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("win_removeEventListener") {
            if (args.size() < 2) return JsValue::undefined();
            std::string eventName = ARG_STR(0);
            JsValue fn = ARG(1);
            auto it = std::find_if(g_windowEventListeners.begin(), g_windowEventListeners.end(),
                [&](const EventListener& listener) {
                    return listener.event == eventName && listener.fn.strictEq(fn);
                });
            if (it != g_windowEventListeners.end()) g_windowEventListeners.erase(it);
            return JsValue::undefined();
        }, "removeEventListener")));

    // window.scrollTo / scroll / open
    vm.setGlobal("scrollTo", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("scrollTo") {
            float x = 0.f, y = 0.f;
            if (!args.empty() && args[0].isObject()) {
                auto* options = args[0].asObject();
                JsValue left = options->getProp("left");
                JsValue top = options->getProp("top");
                x = left.isUndefined() ? g_windowScrollX : (float)left.toNumber();
                y = top.isUndefined() ? g_windowScrollY : (float)top.toNumber();
            } else {
                x = args.size() > 0 ? (float)ARG_NUM(0) : g_windowScrollX;
                y = args.size() > 1 ? (float)ARG_NUM(1) : g_windowScrollY;
            }
            setWindowScrollTo(vm, x, y);
            if (g_domCallbacks.scrollTo) g_domCallbacks.scrollTo(g_windowScrollX, g_windowScrollY);
            return JsValue::undefined();
        }, "scrollTo")));
    vm.setGlobal("scroll", vm.getGlobal("scrollTo"));
    vm.setGlobal("scrollBy", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("scrollBy") {
            float dx = 0.f, dy = 0.f;
            if (!args.empty() && args[0].isObject()) {
                auto* options = args[0].asObject();
                JsValue left = options->getProp("left");
                JsValue top = options->getProp("top");
                dx = left.isUndefined() ? 0.f : (float)left.toNumber();
                dy = top.isUndefined() ? 0.f : (float)top.toNumber();
            } else {
                dx = args.size() > 0 ? (float)ARG_NUM(0) : 0.f;
                dy = args.size() > 1 ? (float)ARG_NUM(1) : 0.f;
            }
            setWindowScrollTo(vm, g_windowScrollX + dx, g_windowScrollY + dy);
            if (g_domCallbacks.scrollBy) g_domCallbacks.scrollBy(dx, dy);
            else if (g_domCallbacks.scrollTo) g_domCallbacks.scrollTo(g_windowScrollX, g_windowScrollY);
            return JsValue::undefined();
        }, "scrollBy")));
    vm.setGlobal("open", JsValue::object(vm.gc().newNativeFunction(
        [historyStack, baseUrl = pageUrl](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string current = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            std::string href = resolveDomUrl(args.empty() ? "" : args[0].toString(), current);
            auto* popup = vm.gc().newObject(ObjKind::Plain);
            popup->setProp("closed", JsValue::boolean(false));
            popup->setProp("opener", JsValue::object(vm.globals()));
            popup->setProp("name", vm.str(args.size() > 1 ? args[1].toString() : ""));
            auto* popupLocation = vm.gc().newObject(ObjKind::Plain);
            applyLocationProps(vm, popupLocation, href);
            popup->setProp("location", JsValue::object(popupLocation));
            addNative(vm, popup, "close", [popup](VM&, JsValue, std::vector<JsValue>) -> JsValue {
                popup->setProp("closed", JsValue::boolean(true));
                return JsValue::undefined();
            });
            addNative(vm, popup, "focus", [](VM&, JsValue, std::vector<JsValue>) -> JsValue {
                return JsValue::undefined();
            });
            addNative(vm, popup, "blur", [](VM&, JsValue, std::vector<JsValue>) -> JsValue {
                return JsValue::undefined();
            });
            return JsValue::object(popup);
        }, "open")));
    vm.setGlobal("closed", JsValue::boolean(false));
    vm.setGlobal("opener", JsValue::null());
    vm.setGlobal("close", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("window_close") {
            vm.setGlobal("closed", JsValue::boolean(true));
            return JsValue::undefined();
        }, "close")));
    vm.setGlobal("focus", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("window_focus") {
            vm.setGlobal("documentHasFocus", JsValue::boolean(true));
            return JsValue::undefined();
        }, "focus")));
    vm.setGlobal("blur", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("window_blur") {
            vm.setGlobal("documentHasFocus", JsValue::boolean(false));
            return JsValue::undefined();
        }, "blur")));
    vm.setGlobal("print", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("window_print") {
            vm.setGlobal("printRequested", JsValue::boolean(true));
            return JsValue::undefined();
        }, "print")));
    vm.setGlobal("atob", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("atob") {
            // Minimal base64 decode.
            std::string input = ARG_STR(0), out;
            static const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            int val = 0, bits = -8;
            for (char c : input) {
                size_t p = b64.find(c);
                if (p == std::string::npos) continue;
                val = (val << 6) | (int)p; bits += 6;
                if (bits >= 0) { out += (char)((val >> bits) & 0xFF); bits -= 8; }
            }
            return vm.str(out);
        }, "atob")));
    vm.setGlobal("btoa", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("btoa") {
            static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string input = ARG_STR(0), out;
            int val = 0, bits = 0;
            for (unsigned char c : input) {
                val = (val << 8) | c; bits += 8;
                while (bits >= 6) { bits -= 6; out += b64[(val >> bits) & 0x3F]; }
            }
            if (bits > 0) out += b64[(val << (6 - bits)) & 0x3F];
            while (out.size() % 4) out += '=';
            return vm.str(out);
        }, "btoa")));

    auto* crypto = vm.gc().newObject(ObjKind::Plain);
    addNative(vm, crypto, "getRandomValues", NATIVE("crypto.getRandomValues") {
        if (args.empty() || !ARG(0).isObject()) return ARG(0);
        auto* arr = ARG(0).asObject();
        uint32_t len = arr->arrayLength();
        uint64_t seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        for (uint32_t i = 0; i < len; ++i) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            arr->arraySet(i, JsValue::integer((int32_t)((seed >> 32) & 0xFF)));
        }
        return ARG(0);
    });
    addNative(vm, crypto, "randomUUID", NATIVE("crypto.randomUUID") {
        uint64_t seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        auto nextHex = [&]() {
            seed = seed * 2862933555777941757ULL + 3037000493ULL;
            const char* hex = "0123456789abcdef";
            std::string out;
            for (int i = 0; i < 4; ++i) out += hex[(seed >> (i * 4)) & 0xF];
            return out;
        };
        std::string uuid = nextHex() + nextHex() + "-" + nextHex() + "-" + nextHex() + "-" + nextHex() + "-" + nextHex() + nextHex() + nextHex();
        return vm.str(uuid);
    });
    vm.setGlobal("crypto", JsValue::object(crypto));

    // window.alert / confirm / prompt
    vm.setGlobal("alert", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("alert") { fprintf(stderr, "[ALERT] %s\n", ARG_STR(0).c_str()); return JsValue::undefined(); }, "alert")));
    vm.setGlobal("confirm", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("confirm") { return JsValue::boolean(false); }, "confirm")));
    vm.setGlobal("prompt", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("prompt") { return JsValue::null(); }, "prompt")));

    // performance.now() + a small Performance Timeline surface.
    auto* perf = vm.gc().newObject(ObjKind::Plain);
    struct PerfEntry {
        std::string name;
        std::string type;
        double startTime = 0;
        double duration = 0;
    };
    auto perfEntries = std::make_shared<std::vector<PerfEntry>>();
    auto nowMs = []() {
        auto t = std::chrono::steady_clock::now().time_since_epoch();
        return (double)std::chrono::duration_cast<std::chrono::microseconds>(t).count() / 1000.0;
    };
    auto makePerfEntryArray = [perfEntries](VM& vm, const std::string& filter, bool byType) {
        auto* arr = newArrayWithPrototype(vm);
        for (const auto& entry : *perfEntries) {
            if (!filter.empty()) {
                if (byType && entry.type != filter) continue;
                if (!byType && entry.name != filter) continue;
            }
            auto* obj = vm.gc().newObject(ObjKind::Plain);
            obj->setProp("name", vm.str(entry.name));
            obj->setProp("entryType", vm.str(entry.type));
            obj->setProp("startTime", JsValue::number(entry.startTime));
            obj->setProp("duration", JsValue::number(entry.duration));
            arr->arrayPush(JsValue::object(obj));
        }
        return JsValue::object(arr);
    };
    addNative(vm, perf, "now", NATIVE("performance.now") {
        auto t = std::chrono::steady_clock::now().time_since_epoch();
        return JsValue::number((double)std::chrono::duration_cast<std::chrono::microseconds>(t).count() / 1000.0);
    });
    addNative(vm, perf, "mark", [perfEntries, nowMs](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        perfEntries->push_back({ args.empty() ? "" : args[0].toString(), "mark", nowMs(), 0 });
        return JsValue::undefined();
    });
    addNative(vm, perf, "measure", [perfEntries, nowMs](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string name = args.empty() ? "" : args[0].toString();
        double start = nowMs();
        double end = start;
        if (args.size() > 1) {
            std::string startName = args[1].toString();
            for (const auto& entry : *perfEntries)
                if (entry.type == "mark" && entry.name == startName) { start = entry.startTime; break; }
        }
        if (args.size() > 2) {
            std::string endName = args[2].toString();
            for (const auto& entry : *perfEntries)
                if (entry.type == "mark" && entry.name == endName) { end = entry.startTime; break; }
        }
        perfEntries->push_back({ name, "measure", start, std::max(0.0, end - start) });
        return JsValue::undefined();
    });
    addNative(vm, perf, "getEntries", [makePerfEntryArray](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
        return makePerfEntryArray(vm, "", true);
    });
    addNative(vm, perf, "getEntriesByType", [makePerfEntryArray](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        return makePerfEntryArray(vm, args.empty() ? "" : args[0].toString(), true);
    });
    addNative(vm, perf, "getEntriesByName", [makePerfEntryArray](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        return makePerfEntryArray(vm, args.empty() ? "" : args[0].toString(), false);
    });
    addNative(vm, perf, "clearMarks", [perfEntries](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string name = args.empty() ? "" : args[0].toString();
        perfEntries->erase(std::remove_if(perfEntries->begin(), perfEntries->end(),
            [&](const PerfEntry& entry) { return entry.type == "mark" && (name.empty() || entry.name == name); }),
            perfEntries->end());
        return JsValue::undefined();
    });
    addNative(vm, perf, "clearMeasures", [perfEntries](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        std::string name = args.empty() ? "" : args[0].toString();
        perfEntries->erase(std::remove_if(perfEntries->begin(), perfEntries->end(),
            [&](const PerfEntry& entry) { return entry.type == "measure" && (name.empty() || entry.name == name); }),
            perfEntries->end());
        return JsValue::undefined();
    });
    vm.setGlobal("performance", JsValue::object(perf));

    // requestAnimationFrame (queued as macrotask with 0 delay)
    vm.setGlobal("requestAnimationFrame", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("requestAnimationFrame") {
            if (!ARG(0).isCallable()) return JsValue::integer(0);
            auto t = std::chrono::steady_clock::now().time_since_epoch();
            double timestamp = (double)std::chrono::duration_cast<std::chrono::microseconds>(t).count() / 1000.0;
            return JsValue::integer(vm.scheduleMacrotask(ARG(0), { JsValue::number(timestamp) }, 0, false));
        }, "requestAnimationFrame")));
    vm.setGlobal("cancelAnimationFrame", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("cancelAnimationFrame") {
            vm.cancelMacrotask(ARG_INT(0));
            return JsValue::undefined();
        }, "cancelAnimationFrame")));
    vm.setGlobal("requestIdleCallback", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("requestIdleCallback") {
            if (!ARG(0).isCallable()) return JsValue::integer(0);
            auto* deadline = vm.gc().newObject(ObjKind::Plain);
            deadline->setProp("didTimeout", JsValue::boolean(false));
            addNative(vm, deadline, "timeRemaining", NATIVE("idle_timeRemaining") {
                return JsValue::number(50.0);
            });
            int delay = 1;
            if (args.size() > 1 && ARG(1).isObject()) {
                JsValue timeout = ARG(1).asObject()->getProp("timeout");
                if (!timeout.isUndefined()) delay = std::max(0, timeout.toInt32());
            }
            return JsValue::integer(vm.scheduleMacrotask(ARG(0), { JsValue::object(deadline) }, delay, false));
        }, "requestIdleCallback")));
    vm.setGlobal("cancelIdleCallback", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("cancelIdleCallback") {
            vm.cancelMacrotask(ARG_INT(0));
            return JsValue::undefined();
        }, "cancelIdleCallback")));

    vm.setGlobal("MutationObserver", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("MutationObserver") {
            auto* obs = vm.gc().newObject(ObjKind::Plain);
            obs->setProp("_callback", ARG(0));
            g_mutationObservers.push_back({ obs, nullptr, false, false, true, true });
            JsValue obsVal = JsValue::object(obs);
            g_observerRoots.push_back(std::make_unique<JsValue>(obsVal));
            vm.gc().addRoot(g_observerRoots.back().get());
            addNative(vm, obs, "observe", NATIVE("obs_observe") {
                if (!thisVal.isObject()) return JsValue::undefined();
                Node* target = unwrapNode(ARG(0));
                for (auto& entry : g_mutationObservers) {
                    if (entry.observer != thisVal.asObject()) continue;
                    entry.target = target;
                    entry.active = target != nullptr;
                    if (ARG(1).isObject()) {
                        auto* options = ARG(1).asObject();
                        JsValue subtree = options->getProp("subtree");
                        JsValue attributes = options->getProp("attributes");
                        JsValue childList = options->getProp("childList");
                        if (!subtree.isUndefined()) entry.subtree = subtree.toBool();
                        if (!attributes.isUndefined()) entry.attributes = attributes.toBool();
                        if (!childList.isUndefined()) entry.childList = childList.toBool();
                    }
                    break;
                }
                return JsValue::undefined();
            });
            addNative(vm, obs, "disconnect", NATIVE("obs_disconnect") {
                if (!thisVal.isObject()) return JsValue::undefined();
                for (auto& entry : g_mutationObservers)
                    if (entry.observer == thisVal.asObject()) entry.active = false;
                return JsValue::undefined();
            });
            return obsVal;
        }, "MutationObserver")));

    vm.setGlobal("IntersectionObserver", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("IntersectionObserver") {
            auto* obs = vm.gc().newObject(ObjKind::Plain);
            obs->setProp("_callback", ARG(0));
            obs->setProp("root", JsValue::null());
            obs->setProp("rootMargin", vm.str("0px"));
            auto* thresholds = newArrayWithPrototype(vm);
            thresholds->arrayPush(JsValue::integer(0));
            obs->setProp("thresholds", JsValue::object(thresholds));
            if (ARG(1).isObject()) {
                auto* options = ARG(1).asObject();
                JsValue root = options->getProp("root");
                if (!root.isUndefined()) obs->setProp("root", root);
                JsValue rootMargin = options->getProp("rootMargin");
                if (!rootMargin.isUndefined()) obs->setProp("rootMargin", vm.str(rootMargin.toString()));
                JsValue threshold = options->getProp("threshold");
                if (!threshold.isUndefined()) {
                    auto* thresholdArray = newArrayWithPrototype(vm);
                    if (threshold.isObject() && threshold.asObject()->kind == ObjKind::Array) {
                        for (uint32_t i = 0; i < threshold.asObject()->arrayLength(); ++i)
                            thresholdArray->arrayPush(JsValue::number(threshold.asObject()->arrayGet(i).toNumber()));
                    } else {
                        thresholdArray->arrayPush(JsValue::number(threshold.toNumber()));
                    }
                    obs->setProp("thresholds", JsValue::object(thresholdArray));
                }
            }
            auto observed = std::make_shared<std::vector<JsValue>>();
            auto makeRecord = [](VM& vm, JsValue targetVal) -> JsValue {
                Node* target = unwrapNode(targetVal);
                DomMetrics rect = computeDomMetrics(target);
                auto* bounds = vm.gc().newObject(ObjKind::Plain);
                bounds->setProp("top", JsValue::number(rect.top));
                bounds->setProp("left", JsValue::number(rect.left));
                bounds->setProp("bottom", JsValue::number(rect.top + rect.height));
                bounds->setProp("right", JsValue::number(rect.left + rect.width));
                bounds->setProp("width", JsValue::number(rect.width));
                bounds->setProp("height", JsValue::number(rect.height));
                auto* record = vm.gc().newObject(ObjKind::Plain);
                record->setProp("target", targetVal);
                record->setProp("time", JsValue::number(0));
                record->setProp("rootBounds", JsValue::null());
                record->setProp("isIntersecting", JsValue::boolean(target != nullptr));
                record->setProp("intersectionRatio", JsValue::number(target ? 1.0 : 0.0));
                record->setProp("boundingClientRect", JsValue::object(bounds));
                record->setProp("intersectionRect", JsValue::object(bounds));
                return JsValue::object(record);
            };
            addNative(vm, obs, "observe", [observed, makeRecord](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
                if (!thisVal.isObject()) return JsValue::undefined();
                JsValue callback = thisVal.asObject()->getProp("_callback");
                if (!callback.isCallable()) return JsValue::undefined();
                JsValue targetVal = args.empty() ? JsValue::undefined() : args[0];
                if (std::any_of(observed->begin(), observed->end(),
                    [&](const JsValue& value) { return value.strictEq(targetVal); }))
                    return JsValue::undefined();
                observed->push_back(targetVal);
                auto* records = newArrayWithPrototype(vm);
                records->arrayPush(makeRecord(vm, targetVal));
                JsValue recordsVal = JsValue::object(records);
                JsValue observerVal = thisVal;
                auto* deliver = vm.gc().newNativeFunction(
                    [callback, observerVal, recordsVal](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
                        try { vm.call(callback, observerVal, { recordsVal, observerVal }); } catch (...) {}
                        return JsValue::undefined();
                    },
                    "IntersectionObserverDelivery");
                vm.scheduleMacrotask(JsValue::object(deliver), {}, 0, false);
                return JsValue::undefined();
            });
            addNative(vm, obs, "unobserve", [observed](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
                if (args.empty()) return JsValue::undefined();
                JsValue targetVal = args[0];
                observed->erase(std::remove_if(observed->begin(), observed->end(),
                    [&](const JsValue& value) { return value.strictEq(targetVal); }), observed->end());
                return JsValue::undefined();
            });
            addNative(vm, obs, "disconnect", [observed](VM&, JsValue, std::vector<JsValue>) -> JsValue {
                observed->clear();
                return JsValue::undefined();
            });
            addNative(vm, obs, "takeRecords", [](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
                return JsValue::object(newArrayWithPrototype(vm));
            });
            return JsValue::object(obs);
        }, "IntersectionObserver")));

    vm.setGlobal("ResizeObserver", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("ResizeObserver") {
            auto* obs = vm.gc().newObject(ObjKind::Plain);
            obs->setProp("_callback", ARG(0));
            auto observed = std::make_shared<std::vector<JsValue>>();
            auto makeRecord = [](VM& vm, JsValue targetVal) -> JsValue {
                Node* target = unwrapNode(targetVal);
                DomMetrics rect = computeDomMetrics(target);
                auto* contentRect = vm.gc().newObject(ObjKind::Plain);
                contentRect->setProp("x", JsValue::number(rect.left));
                contentRect->setProp("y", JsValue::number(rect.top));
                contentRect->setProp("top", JsValue::number(rect.top));
                contentRect->setProp("left", JsValue::number(rect.left));
                contentRect->setProp("right", JsValue::number(rect.left + rect.width));
                contentRect->setProp("bottom", JsValue::number(rect.top + rect.height));
                contentRect->setProp("width", JsValue::number(rect.width));
                contentRect->setProp("height", JsValue::number(rect.height));
                auto* size = vm.gc().newObject(ObjKind::Plain);
                size->setProp("inlineSize", JsValue::number(rect.width));
                size->setProp("blockSize", JsValue::number(rect.height));
                auto* sizes = newArrayWithPrototype(vm);
                sizes->arrayPush(JsValue::object(size));
                auto* borderSizes = newArrayWithPrototype(vm);
                borderSizes->arrayPush(JsValue::object(size));
                auto* devicePixelSizes = newArrayWithPrototype(vm);
                devicePixelSizes->arrayPush(JsValue::object(size));
                auto* record = vm.gc().newObject(ObjKind::Plain);
                record->setProp("target", targetVal);
                record->setProp("contentRect", JsValue::object(contentRect));
                record->setProp("contentBoxSize", JsValue::object(sizes));
                record->setProp("borderBoxSize", JsValue::object(borderSizes));
                record->setProp("devicePixelContentBoxSize", JsValue::object(devicePixelSizes));
                return JsValue::object(record);
            };
            addNative(vm, obs, "observe", [observed, makeRecord](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
                if (!thisVal.isObject()) return JsValue::undefined();
                JsValue callback = thisVal.asObject()->getProp("_callback");
                if (!callback.isCallable()) return JsValue::undefined();
                JsValue targetVal = args.empty() ? JsValue::undefined() : args[0];
                if (std::any_of(observed->begin(), observed->end(),
                    [&](const JsValue& value) { return value.strictEq(targetVal); }))
                    return JsValue::undefined();
                observed->push_back(targetVal);
                auto* records = newArrayWithPrototype(vm);
                records->arrayPush(makeRecord(vm, targetVal));
                JsValue recordsVal = JsValue::object(records);
                JsValue observerVal = thisVal;
                auto* deliver = vm.gc().newNativeFunction(
                    [callback, observerVal, recordsVal](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
                        try { vm.call(callback, observerVal, { recordsVal, observerVal }); } catch (...) {}
                        return JsValue::undefined();
                    },
                    "ResizeObserverDelivery");
                vm.scheduleMacrotask(JsValue::object(deliver), {}, 0, false);
                return JsValue::undefined();
            });
            addNative(vm, obs, "unobserve", [observed](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
                if (args.empty()) return JsValue::undefined();
                JsValue targetVal = args[0];
                observed->erase(std::remove_if(observed->begin(), observed->end(),
                    [&](const JsValue& value) { return value.strictEq(targetVal); }), observed->end());
                return JsValue::undefined();
            });
            addNative(vm, obs, "disconnect", [observed](VM&, JsValue, std::vector<JsValue>) -> JsValue {
                observed->clear();
                return JsValue::undefined();
            });
            addNative(vm, obs, "takeRecords", [](VM& vm, JsValue, std::vector<JsValue>) -> JsValue {
                return JsValue::object(newArrayWithPrototype(vm));
            });
            return JsValue::object(obs);
        }, "ResizeObserver")));

    // CustomEvent / Event
    auto makeEventCtor = [&](const char* name) {
        std::string ctorName = name;
        vm.setGlobal(name, JsValue::object(vm.gc().newNativeFunction(
            [ctorName](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
            return makeEventObject(vm, ctorName, args.empty() ? "" : args[0].toString(),
                args.size() > 1 ? args[1] : JsValue::undefined());
        }, name)));
    };
    makeEventCtor("Event");
    makeEventCtor("CustomEvent");
    makeEventCtor("MouseEvent");
    makeEventCtor("KeyboardEvent");
    makeEventCtor("InputEvent");
    makeEventCtor("FocusEvent");
    makeEventCtor("PointerEvent");
    makeEventCtor("WheelEvent");
    makeEventCtor("TouchEvent");
    makeEventCtor("DragEvent");
    makeEventCtor("SubmitEvent");
}

bool dispatchDomEvent(VM& vm, Node* target, const std::string& eventName) {
    if (!target) return true;
    // Build a minimal event object.
    auto* ev = vm.gc().newObject(ObjKind::Plain);
    ev->setProp("type", vm.str(eventName));
    ev->setProp("target", wrapNode(vm, getShared(target) ? getShared(target)
                                       : std::shared_ptr<Node>(target, [](Node*){})));
    ev->setProp("currentTarget", JsValue::null());
    ev->setProp("bubbles", JsValue::boolean(true));
    ev->setProp("cancelable", JsValue::boolean(true));
    ev->setProp("defaultPrevented", JsValue::boolean(false));
    installEventMethods(vm, ev);
    JsValue evVal = JsValue::object(ev);

    // Walk from target up through ancestors (bubble phase).
    Node* cur = target;
    while (cur) {
        auto curShared = getShared(cur);
        JsValue thisObj = curShared ? wrapNode(vm, curShared) : JsValue::undefined();
        JsValue propertyHandler = thisObj.isObject()
            ? thisObj.asObject()->getProp("on" + eventName)
            : JsValue::undefined();
        if (propertyHandler.isCallable()) {
            ev->setProp("currentTarget", thisObj);
            try { vm.call(propertyHandler, thisObj, { evVal }); }
            catch (...) {}
            if (eventFlag(ev, "__vertexStopped")) break;
        }
        auto it = g_eventListeners.find(cur);
        if (it != g_eventListeners.end()) {
            for (auto& listener : it->second) {
                if (listener.event == eventName) {
                    ev->setProp("currentTarget", thisObj);
                    try { vm.call(listener.fn, thisObj, { evVal }); }
                    catch (...) {}
                    if (eventFlag(ev, "__vertexStopped")) break;
                }
            }
        }
        if (eventFlag(ev, "__vertexStopped") || !eventFlag(ev, "bubbles")) break;
        cur = cur->parent;
    }
    vm.drainMicrotasks();
    return !eventFlag(ev, "defaultPrevented");
}

static bool isCheckableInput(const Node* n) {
    if (!n || n->tagName != "input") return false;
    std::string type = lowerCopy(n->attr("type"));
    return type == "checkbox" || type == "radio";
}

static void setCheckedAttr(Node* n, bool checked) {
    if (!n) return;
    if (checked) n->attrs["checked"] = "checked";
    else n->attrs.erase("checked");
}

bool activateDomElement(VM& vm, Node* target) {
    if (!target || hasAttr(target, "disabled")) return false;
    const bool checkable = isCheckableInput(target);
    const bool oldChecked = hasAttr(target, "checked");
    if (checkable) {
        setCheckedAttr(target, !oldChecked);
        markDomDirty(vm, target, "attributes");
    }

    bool allowed = dispatchDomEvent(vm, target, "click");
    if (checkable && !allowed) {
        setCheckedAttr(target, oldChecked);
        markDomDirty(vm, target, "attributes");
    } else if (checkable) {
        dispatchDomEvent(vm, target, "input");
        dispatchDomEvent(vm, target, "change");
    }
    return allowed;
}

void dispatchWindowEvent(VM& vm, const std::string& eventName, JsValue eventValue) {
    JsValue evVal = eventValue;
    if (!evVal.isObject()) {
        auto* ev = vm.gc().newObject(ObjKind::Plain);
        ev->setProp("type", vm.str(eventName));
        ev->setProp("target", JsValue::object(vm.globals()));
        ev->setProp("currentTarget", JsValue::object(vm.globals()));
        ev->setProp("bubbles", JsValue::boolean(false));
        ev->setProp("cancelable", JsValue::boolean(true));
        ev->setProp("defaultPrevented", JsValue::boolean(false));
        installEventMethods(vm, ev);
        evVal = JsValue::object(ev);
    } else {
        installEventMethods(vm, evVal.asObject());
    }

    JsValue propertyHandler = vm.getGlobal("on" + eventName);
    if (propertyHandler.isCallable()) {
        try { vm.call(propertyHandler, JsValue::object(vm.globals()), { evVal }); } catch (...) {}
    }
    auto listeners = g_windowEventListeners;
    for (const auto& listener : listeners) {
        if (listener.event != eventName || !listener.fn.isCallable()) continue;
        try { vm.call(listener.fn, JsValue::object(vm.globals()), { evVal }); } catch (...) {}
        if (evVal.isObject() && eventFlag(evVal.asObject(), "__vertexStopped")) break;
    }
    vm.drainMicrotasks();
}
