#pragma once

#include "html/dom.h"

#include <memory>
#include <string>
#include <vector>

struct LayoutBox {
    std::string name;
    std::string text;
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    std::vector<LayoutBox> children;
};

LayoutBox BuildSimpleLayout(const std::shared_ptr<Node>& root, float viewportWidth);
std::string SerializeLayout(const LayoutBox& box);
