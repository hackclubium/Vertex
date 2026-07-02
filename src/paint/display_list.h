#pragma once

#include "layout/layout.h"

#include <string>
#include <vector>

struct DisplayItem {
    std::string kind;
    std::string text;
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

std::vector<DisplayItem> BuildDisplayList(const LayoutBox& root);
std::string SerializeDisplayList(const std::vector<DisplayItem>& items);
