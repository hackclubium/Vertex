#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>

enum class NodeType { Document, Element, Text };

struct Node {
    NodeType    type    = NodeType::Element;
    std::string tagName;                              // lowercase; "#document" for root
    std::string text;                                 // only for Text nodes
    std::map<std::string, std::string> attrs;
    std::vector<std::shared_ptr<Node>> children;
    Node*       parent  = nullptr;

    ~Node() {
        auto moveUniqueChildren = [](Node* node, std::vector<std::shared_ptr<Node>>& stack) {
            std::vector<std::shared_ptr<Node>> retained;
            retained.reserve(node->children.size());
            for (auto& child : node->children) {
                if (child && child.use_count() == 1)
                    stack.push_back(std::move(child));
                else
                    retained.push_back(std::move(child));
            }
            node->children = std::move(retained);
        };

        std::vector<std::shared_ptr<Node>> stack;
        moveUniqueChildren(this, stack);
        while (!stack.empty()) {
            auto current = std::move(stack.back());
            stack.pop_back();
            if (!current) continue;
            moveUniqueChildren(current.get(), stack);
        }
    }

    std::string attr(const std::string& key) const {
        auto it = attrs.find(key);
        return it != attrs.end() ? it->second : "";
    }

    void appendChild(std::shared_ptr<Node> child) {
        child->parent = this;
        children.push_back(std::move(child));
    }

    static std::shared_ptr<Node> makeDocument() {
        auto n = std::make_shared<Node>();
        n->type    = NodeType::Document;
        n->tagName = "#document";
        return n;
    }
    static std::shared_ptr<Node> makeElement(const std::string& tag) {
        auto n = std::make_shared<Node>();
        n->type    = NodeType::Element;
        n->tagName = tag;
        return n;
    }
    static std::shared_ptr<Node> makeText(const std::string& t) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::Text;
        n->text = t;
        return n;
    }
};
