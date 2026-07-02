#pragma once
#include "html/dom.h"
#include <memory>
#include <string>

// Builds a DOM tree from raw HTML.
std::shared_ptr<Node> ParseHtml(const std::string& html);
