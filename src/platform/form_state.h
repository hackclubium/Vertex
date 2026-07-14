#pragma once
#include "html/dom.h"
#include "layout/box.h"
#include "network/fetcher.h"
#include "network/url.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <map>
#include <functional>
#include <vector>

// Tracks form control state (text values, focused input) across the browser.
// Shared between the platform shell (keyboard input) and the box painter
// (rendering input text + cursor).

struct FormState {
    // The currently focused input element (nullptr = no input focused).
    Node* focusedInput = nullptr;

    // Text values for form controls, keyed by Node*.
    std::map<Node*, std::string> values;

    // Cursor position within the focused input's value.
    size_t cursorPos = 0;

    // Whether the cursor blink is currently visible (toggled by timer).
    bool cursorVisible = true;

    std::string getValue(Node* n) const {
        auto it = values.find(n);
        if (it != values.end()) return it->second;
        if (n) {
            std::string v = n->attr("value");
            if (!v.empty()) return v;
            if (n->tagName == "textarea") {
                for (auto& c : n->children) if (c->type == NodeType::Text) v += c->text;
                return v;
            }
        }
        return "";
    }

    void setValue(Node* n, const std::string& v) {
        values[n] = v;
    }

    void focus(Node* n) {
        focusedInput = n;
        cursorPos = getValue(n).size();
        cursorVisible = true;
    }

    void blur() {
        focusedInput = nullptr;
    }

    void insertChar(char c) {
        if (!focusedInput) return;
        auto& v = values[focusedInput];
        if (v.empty()) v = getValue(focusedInput);
        if (cursorPos > v.size()) cursorPos = v.size();
        v.insert(v.begin() + cursorPos, c);
        cursorPos++;
    }

    void backspace() {
        if (!focusedInput) return;
        auto& v = values[focusedInput];
        if (v.empty()) v = getValue(focusedInput);
        if (cursorPos > 0 && cursorPos <= v.size()) {
            v.erase(v.begin() + cursorPos - 1);
            cursorPos--;
        }
    }

    void deleteChar() {
        if (!focusedInput) return;
        auto& v = values[focusedInput];
        if (v.empty()) v = getValue(focusedInput);
        if (cursorPos < v.size()) {
            v.erase(v.begin() + cursorPos);
        }
    }

    struct Submission {
        std::string url;
        FetchRequest request;
    };

    static Node* enclosingForm(Node* node) {
        Node* form = node;
        while (form && form->tagName != "form") form = form->parent;
        return form;
    }

    static bool isSubmitControl(const Node* n) {
        if (!n || n->type != NodeType::Element) return false;
        std::string type = n->attr("type");
        for (char& c : type) c = (char)std::tolower((unsigned char)c);
        return n->tagName == "button" || (n->tagName == "input" && (type == "submit" || type == "image"));
    }

    // Find the enclosing <form> element and build a GET query string.
    std::string buildFormQuery() const {
        Submission sub;
        return buildSubmission(focusedInput, nullptr, "", sub) ? sub.url : "";
    }

    bool buildSubmission(Node* control, Node* submitter, const std::string& baseUrl, Submission& out) const {
        if (!control && !submitter) return false;
        Node* form = enclosingForm(submitter ? submitter : control);
        if (!form) return false;
        std::string action = form->attr("action");
        if (action.empty()) action = baseUrl.empty() ? "/" : baseUrl;
        else if (!baseUrl.empty()) action = ResolveUrlAgainstBase(action, baseUrl);
        std::string method = form->attr("method");
        for (char& c : method) c = (char)std::tolower((unsigned char)c);
        if (method != "post") method = "get";
        std::string query;
        // Collect all named inputs within this form.
        std::function<void(Node*)> collect = [&](Node* n) {
            if (!n) return;
            if (n->type == NodeType::Element && n->attrs.find("disabled") != n->attrs.end()) return;
            if (n->type == NodeType::Element
                && (n->tagName == "input" || n->tagName == "textarea" || n->tagName == "button" || n->tagName == "select")
                && !n->attr("name").empty()) {
                std::string type = n->attr("type");
                for (char& c : type) c = (char)std::tolower((unsigned char)c);
                if (n->tagName == "input" && (type == "button" || type == "reset" || type == "file")) {
                    for (auto& c : n->children) collect(c.get());
                    return;
                }
                if (isSubmitControl(n) && n != submitter) {
                    for (auto& c : n->children) collect(c.get());
                    return;
                }
                std::string name = n->attr("name");
                std::string val = getValue(const_cast<Node*>(n));
                if (n->tagName == "select") {
                    bool found = false;
                    for (auto& c : n->children) {
                        if (c->type == NodeType::Element && c->tagName == "option" && c->attrs.find("selected") != c->attrs.end()) {
                            val = c->attr("value");
                            if (val.empty()) for (auto& t : c->children) if (t->type == NodeType::Text) val += t->text;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        for (auto& c : n->children) {
                            if (c->type == NodeType::Element && c->tagName == "option") {
                                val = c->attr("value");
                                if (val.empty()) for (auto& t : c->children) if (t->type == NodeType::Text) val += t->text;
                                break;
                            }
                        }
                    }
                }
                if (n->tagName == "input" && (type == "checkbox" || type == "radio") && n->attrs.find("checked") == n->attrs.end()) {
                    for (auto& c : n->children) collect(c.get());
                    return;
                }
                if (n->tagName == "input" && (type == "checkbox" || type == "radio") && val.empty()) val = "on";
                if (n == submitter && val.empty() && n->tagName == "button") {
                    for (auto& c : n->children) if (c->type == NodeType::Text) val += c->text;
                }
                if (!query.empty()) query += '&';
                query += urlEncode(name) + "=" + urlEncode(val);
            }
            for (auto& c : n->children) collect(c.get());
        };
        collect(form);
        out.url = method == "get" ? action + (action.find('?') != std::string::npos ? "&" : "?") + query : action;
        out.request.url = out.url;
        out.request.method = method == "post" ? "POST" : "GET";
        if (method == "post") {
            out.request.body = query;
            out.request.contentType = "application/x-www-form-urlencoded";
        }
        return true;
    }

    static std::string urlEncode(const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
                out += (char)c;
            else if (c == ' ') out += '+';
            else {
                const char hex[] = "0123456789ABCDEF";
                out += '%'; out += hex[c >> 4]; out += hex[c & 0xF];
            }
        }
        return out;
    }

    // Walk a layout tree and find the input/textarea Node at (x,y) document coords.
    // Walk a layout tree and find the deepest Node at (x,y) document coords.
    static const Node* hitTestNode(const LayoutBox& root, float x, float y, float scrollY, float topInset) {
        float docY = y + scrollY - topInset;
        const Node* found = nullptr;
        auto paintOrderedChildren = [](const LayoutBox& box) {
            std::vector<const LayoutBox*> ordered;
            std::vector<const LayoutBox*> negZ, inflow, floats, posZ;
            for (const auto& kptr : box.kids) {
                const LayoutBox* k = kptr.get();
                if (k->isOutOfFlow()) {
                    if (k->style.zIndexSet && k->style.zIndex < 0) negZ.push_back(k);
                    else posZ.push_back(k);
                } else if (k->isFloat()) {
                    floats.push_back(k);
                } else if ((k->style.positionMode == 1 || k->style.positionMode == 4)) {
                    posZ.push_back(k);
                } else {
                    inflow.push_back(k);
                }
            }
            auto byZ = [](const LayoutBox* a, const LayoutBox* b) {
                int za = a->style.zIndexSet ? a->style.zIndex : 0;
                int zb = b->style.zIndexSet ? b->style.zIndex : 0;
                return za < zb;
            };
            std::stable_sort(negZ.begin(), negZ.end(), byZ);
            std::stable_sort(posZ.begin(), posZ.end(), byZ);
            ordered.insert(ordered.end(), negZ.begin(), negZ.end());
            ordered.insert(ordered.end(), inflow.begin(), inflow.end());
            ordered.insert(ordered.end(), floats.begin(), floats.end());
            ordered.insert(ordered.end(), posZ.begin(), posZ.end());
            return ordered;
        };
        std::function<void(const LayoutBox&)> walk = [&](const LayoutBox& box) {
            float bx = box.x, by = box.y;
            float bw = box.borderBoxW(), bh = box.borderBoxH();
            bool inside = x >= bx && x <= bx + bw && docY >= by && docY <= by + bh;
            if (&box != &root && box.style.overflowHidden && !inside)
                return;
            if (box.node && box.node->type == NodeType::Element) {
                if (inside)
                    found = box.node;  // deeper nodes override shallower ones
            }
            auto ordered = paintOrderedChildren(box);
            if (inside || &box == &root) {
                for (const LayoutBox* k : ordered) walk(*k);
            } else if (!box.style.overflowHidden) {
                for (const LayoutBox* k : ordered) {
                    if (k->isOutOfFlow() || k->isFloat() || (k->style.positionMode == 1 || k->style.positionMode == 4))
                        walk(*k);
                }
            }
        };
        walk(root);
        return found;
    }

    static Node* hitTestInput(const LayoutBox& root, float x, float y, float scrollY, float topInset) {
        // Adjust y from screen to document coords.
        float docY = y + scrollY - topInset;
        Node* found = nullptr;
        auto paintOrderedChildren = [](const LayoutBox& box) {
            std::vector<const LayoutBox*> ordered;
            std::vector<const LayoutBox*> negZ, inflow, floats, posZ;
            for (const auto& kptr : box.kids) {
                const LayoutBox* k = kptr.get();
                if (k->isOutOfFlow()) {
                    if (k->style.zIndexSet && k->style.zIndex < 0) negZ.push_back(k);
                    else posZ.push_back(k);
                } else if (k->isFloat()) {
                    floats.push_back(k);
                } else if ((k->style.positionMode == 1 || k->style.positionMode == 4)) {
                    posZ.push_back(k);
                } else {
                    inflow.push_back(k);
                }
            }
            auto byZ = [](const LayoutBox* a, const LayoutBox* b) {
                int za = a->style.zIndexSet ? a->style.zIndex : 0;
                int zb = b->style.zIndexSet ? b->style.zIndex : 0;
                return za < zb;
            };
            std::stable_sort(negZ.begin(), negZ.end(), byZ);
            std::stable_sort(posZ.begin(), posZ.end(), byZ);
            ordered.insert(ordered.end(), negZ.begin(), negZ.end());
            ordered.insert(ordered.end(), inflow.begin(), inflow.end());
            ordered.insert(ordered.end(), floats.begin(), floats.end());
            ordered.insert(ordered.end(), posZ.begin(), posZ.end());
            return ordered;
        };
        std::function<void(const LayoutBox&)> walk = [&](const LayoutBox& box) {
            float bx = box.x, by = box.y;
            float bw = box.borderBoxW(), bh = box.borderBoxH();
            bool inside = x >= bx && x <= bx + bw && docY >= by && docY <= by + bh;
            if (&box != &root && box.style.overflowHidden && !inside)
                return;
            if (box.kind == BoxKind::Replaced && box.node
                && (box.node->tagName == "input" || box.node->tagName == "textarea" || box.node->tagName == "button")) {
                if (inside)
                    found = const_cast<Node*>(box.node);
            }
            auto ordered = paintOrderedChildren(box);
            if (inside || &box == &root) {
                for (const LayoutBox* k : ordered) walk(*k);
            } else if (!box.style.overflowHidden) {
                for (const LayoutBox* k : ordered) {
                    if (k->isOutOfFlow() || k->isFloat() || (k->style.positionMode == 1 || k->style.positionMode == 4))
                        walk(*k);
                }
            }
        };
        walk(root);
        return found;
    }
};
