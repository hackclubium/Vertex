#include "html/resources.h"

#include "network/resource_cache.h"
#include "network/text_decode.h"
#include "network/url.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace {
constexpr size_t kMaxScripts = 64;
constexpr size_t kMaxTotalBytes = 2 * 1024 * 1024;
constexpr size_t kMaxScriptBytes = 1024 * 1024;

std::string LowerTrim(std::string value) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool IsClassicJavaScript(const Node& script) {
    const std::string type = LowerTrim(script.attr("type"));
    return type.empty() || type == "text/javascript" || type == "application/javascript"
        || type == "application/ecmascript" || type == "text/ecmascript";
}
} // namespace

void LoadExternalScriptSources(const std::shared_ptr<Node>& document,
                               const std::string& pageUrl) {
    if (!document) return;

    std::vector<Node*> stack{ document.get() };
    size_t loaded = 0;
    size_t loadedBytes = 0;
    while (!stack.empty() && loaded < kMaxScripts && loadedBytes < kMaxTotalBytes) {
        Node* node = stack.back();
        stack.pop_back();

        if (node->type == NodeType::Element && node->tagName == "script"
            && IsClassicJavaScript(*node) && !node->attr("src").empty()) {
            const std::string url = ResolveUrlAgainstBase(node->attr("src"), pageUrl);
            const FetchResult response = FetchResourceCached(url, kMaxScriptBytes, ResourceKind::Script);
            if (response.success && !response.body.empty()
                && response.body.size() <= kMaxScriptBytes
                && loadedBytes + response.body.size() <= kMaxTotalBytes) {
                node->children.clear();
                node->appendChild(Node::makeText(
                    DecodeTextToUtf8(response.body, response.contentType)));
                node->attrs["__vertex_script_filename"] = url;
                loadedBytes += response.body.size();
                ++loaded;
            }
        }

        for (auto& child : node->children)
            stack.push_back(child.get());
    }
}
