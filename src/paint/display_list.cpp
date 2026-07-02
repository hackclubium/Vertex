#include "paint/display_list.h"

#include <sstream>

static void CollectDisplayItems(const LayoutBox& box, std::vector<DisplayItem>& items) {
    if (box.name == "#text" && !box.text.empty()) {
        items.push_back({ "text", box.text, box.x, box.y, box.width, box.height });
    } else if (box.name != "#document") {
        items.push_back({ "box", box.name, box.x, box.y, box.width, box.height });
    }

    for (const auto& child : box.children) {
        CollectDisplayItems(child, items);
    }
}

std::vector<DisplayItem> BuildDisplayList(const LayoutBox& root) {
    std::vector<DisplayItem> items;
    CollectDisplayItems(root, items);
    return items;
}

std::string SerializeDisplayList(const std::vector<DisplayItem>& items) {
    std::ostringstream out;
    for (const auto& item : items) {
        out << item.kind
            << " x=" << item.x
            << " y=" << item.y
            << " w=" << item.width
            << " h=" << item.height;
        if (!item.text.empty()) out << " text=\"" << item.text << "\"";
        out << "\n";
    }
    return out.str();
}
