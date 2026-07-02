#pragma once

#include "html/dom.h"
#include "html/tokenizer.h"

#include <memory>
#include <string>
#include <vector>

std::string SerializeToken(const HtmlToken& token);
std::string SerializeTokens(const std::vector<HtmlToken>& tokens);
std::string SerializeDom(const std::shared_ptr<Node>& root);
