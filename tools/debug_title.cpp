#include "html/parser.h"
#include <cstdio>
#include <string>

int main() {
    const char* html = "<!DOCTYPE html><html><head><title>Hello World</title></head><body><p>Test</p></body></html>";
    auto dom = ParseHtml(html);

    std::vector<const Node*> stack{dom.get()};
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        if (n->type == NodeType::Element) {
            std::printf("ELEMENT <%s> children=%zu text='%s'\n",
                n->tagName.c_str(), n->children.size(),
                n->type == NodeType::Text ? n->text.c_str() : "");
        } else if (n->type == NodeType::Text) {
            std::printf("TEXT '%s'\n", n->text.c_str());
        }
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return 0;
}
