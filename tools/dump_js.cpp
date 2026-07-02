// dump_js.cpp — offline JS repro. Loads an HTML file, attaches it to the JS
// engine, and runs every inline <script>, printing progress. Reproduces JS
// crashes (e.g. stack overflow) without the GUI so they can be diagnosed.
#include "html/parser.h"
#include "js/engine.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: dump_js file.html\n"); return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    std::string html = ss.str();

    printf("parsing (%zu bytes)...\n", html.size()); fflush(stdout);
    auto dom = ParseHtml(html);
    printf("parsed. attaching to JS engine...\n"); fflush(stdout);

    JsEngine js;
    js.setDocument(dom, []() {});
    printf("setDocument OK. running inline scripts...\n"); fflush(stdout);

    std::vector<const Node*> stack{ dom.get() };
    int n = 0;
    while (!stack.empty()) {
        const Node* node = stack.back(); stack.pop_back();
        if (!node) continue;
        if (node->type == NodeType::Element && node->tagName == "script") {
            std::string src;
            for (auto& c : node->children) if (c->type == NodeType::Text) src += c->text;
            if (!src.empty()) {
                printf("  script #%d (%zu bytes)...\n", n, src.size()); fflush(stdout);
                js.runScript(src, "inline");
                printf("  script #%d done\n", n); fflush(stdout);
            }
            ++n;
        }
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
            stack.push_back(it->get());
    }
    printf("all scripts done. running macrotasks...\n"); fflush(stdout);
    js.runMacrotasks();
    printf("DONE — no crash\n");
    return 0;
}
