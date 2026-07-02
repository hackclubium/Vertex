#pragma once
#include "html/dom.h"
#include "layout/box.h"
#include <string>
#include <map>
#include <functional>

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

    // Find the enclosing <form> element and build a GET query string.
    std::string buildFormQuery() const {
        if (!focusedInput) return "";
        Node* form = focusedInput->parent;
        while (form && form->tagName != "form") form = form->parent;
        if (!form) return "";
        std::string action = form->attr("action");
        std::string query;
        // Collect all named inputs within this form.
        std::function<void(Node*)> collect = [&](Node* n) {
            if (!n) return;
            if (n->type == NodeType::Element
                && (n->tagName == "input" || n->tagName == "textarea")
                && !n->attr("name").empty()) {
                std::string name = n->attr("name");
                std::string val = getValue(const_cast<Node*>(n));
                if (!query.empty()) query += '&';
                query += urlEncode(name) + "=" + urlEncode(val);
            }
            for (auto& c : n->children) collect(c.get());
        };
        collect(form);
        if (action.empty()) action = "/";
        return action + (action.find('?') != std::string::npos ? "&" : "?") + query;
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
        std::function<void(const LayoutBox&)> walk = [&](const LayoutBox& box) {
            float bx = box.x, by = box.y;
            float bw = box.borderBoxW(), bh = box.borderBoxH();
            bool inside = x >= bx && x <= bx + bw && docY >= by && docY <= by + bh;
            if (box.node && box.node->type == NodeType::Element) {
                if (inside)
                    found = box.node;  // deeper nodes override shallower ones
            }
            if (inside || &box == &root) {
                for (auto& k : box.kids) walk(*k);
            } else if (!box.style.overflowHidden) {
                for (auto& k : box.kids) {
                    if (k->isOutOfFlow() || k->isFloat() || k->style.positionMode == 1)
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
        std::function<void(const LayoutBox&)> walk = [&](const LayoutBox& box) {
            if (box.kind == BoxKind::Replaced && box.node
                && (box.node->tagName == "input" || box.node->tagName == "textarea")) {
                float bx = box.x, by = box.y;
                float bw = box.borderBoxW(), bh = box.borderBoxH();
                if (x >= bx && x <= bx + bw && docY >= by && docY <= by + bh)
                    found = const_cast<Node*>(box.node);
            }
            for (auto& k : box.kids) walk(*k);
        };
        walk(root);
        return found;
    }
};
