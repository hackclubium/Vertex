#include "html/serializer.h"

#include <algorithm>
#include <sstream>

static std::string EscapeSnapshotText(const std::string& input) {
    std::string out;
    for (char c : input) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

static std::string TokenTypeName(TokenType type) {
    switch (type) {
    case TokenType::StartTag: return "StartTag";
    case TokenType::EndTag: return "EndTag";
    case TokenType::Text: return "Text";
    case TokenType::Comment: return "Comment";
    case TokenType::Doctype: return "Doctype";
    case TokenType::EOF_: return "EOF";
    }
    return "Unknown";
}

std::string SerializeToken(const HtmlToken& token) {
    std::ostringstream out;
    out << TokenTypeName(token.type);
    if (!token.name.empty()) out << " name=" << token.name;
    if (!token.data.empty()) out << " data=\"" << EscapeSnapshotText(token.data) << "\"";
    if (token.selfClosing) out << " selfClosing=true";

    std::vector<std::pair<std::string, std::string>> attrs(token.attrs.begin(), token.attrs.end());
    std::sort(attrs.begin(), attrs.end());
    for (const auto& [key, value] : attrs) {
        out << " attr." << key << "=\"" << EscapeSnapshotText(value) << "\"";
    }
    return out.str();
}

std::string SerializeTokens(const std::vector<HtmlToken>& tokens) {
    std::ostringstream out;
    for (const auto& token : tokens) {
        out << SerializeToken(token) << "\n";
    }
    return out.str();
}

static void SerializeNode(const Node* node, int depth, std::ostringstream& out) {
    if (!node) return;
    std::string indent(depth * 2, ' ');

    if (node->type == NodeType::Document) {
        out << indent << "#document\n";
    } else if (node->type == NodeType::Text) {
        out << indent << "#text \"" << EscapeSnapshotText(node->text) << "\"\n";
        return;
    } else {
        out << indent << "<" << node->tagName;
        std::vector<std::pair<std::string, std::string>> attrs(node->attrs.begin(), node->attrs.end());
        std::sort(attrs.begin(), attrs.end());
        for (const auto& [key, value] : attrs) {
            out << " " << key << "=\"" << EscapeSnapshotText(value) << "\"";
        }
        out << ">\n";
    }

    for (const auto& child : node->children) {
        SerializeNode(child.get(), depth + 1, out);
    }
}

std::string SerializeDom(const std::shared_ptr<Node>& root) {
    std::ostringstream out;
    SerializeNode(root.get(), 0, out);
    return out.str();
}
