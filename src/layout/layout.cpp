#include "layout/layout.h"

#include <algorithm>
#include <sstream>

static bool IsIgnored(const Node* node) {
    if (!node || node->type != NodeType::Element) return false;
    return node->tagName == "head"
        || node->tagName == "script"
        || node->tagName == "style"
        || node->tagName == "meta"
        || node->tagName == "link"
        || node->tagName == "title";
}

static std::string NodeName(const Node* node) {
    if (!node) return "null";
    if (node->type == NodeType::Document) return "#document";
    if (node->type == NodeType::Text) return "#text";
    return node->tagName;
}

static float TextHeight(const std::string& text) {
    return text.empty() ? 0.0f : 20.0f;
}

static LayoutBox BuildNode(const Node* node, float x, float y, float width) {
    LayoutBox box;
    box.name = NodeName(node);
    box.x = x;
    box.y = y;
    box.width = width;

    if (!node) return box;

    if (node->type == NodeType::Text) {
        box.text = node->text;
        box.height = TextHeight(node->text);
        return box;
    }

    float cursorY = y;
    for (const auto& child : node->children) {
        if (IsIgnored(child.get())) continue;
        LayoutBox childBox = BuildNode(child.get(), x + 16.0f, cursorY, std::max(0.0f, width - 32.0f));
        if (childBox.height <= 0.0f && childBox.children.empty()) continue;
        cursorY += childBox.height;
        box.children.push_back(std::move(childBox));
    }

    box.height = std::max(20.0f, cursorY - y);
    return box;
}

LayoutBox BuildSimpleLayout(const std::shared_ptr<Node>& root, float viewportWidth) {
    return BuildNode(root.get(), 0.0f, 0.0f, viewportWidth);
}

static void SerializeBox(const LayoutBox& box, int depth, std::ostringstream& out) {
    std::string indent(depth * 2, ' ');
    out << indent << box.name
        << " x=" << box.x
        << " y=" << box.y
        << " w=" << box.width
        << " h=" << box.height;
    if (!box.text.empty()) out << " text=\"" << box.text << "\"";
    out << "\n";

    for (const auto& child : box.children) {
        SerializeBox(child, depth + 1, out);
    }
}

std::string SerializeLayout(const LayoutBox& box) {
    std::ostringstream out;
    SerializeBox(box, 0, out);
    return out.str();
}
