#pragma once

#include "html/dom.h"

#include <memory>
#include <string>

// Fetch classic <script src> resources into their DOM nodes. The JS engine
// executes these nodes in document order alongside inline scripts.
void LoadExternalScriptSources(const std::shared_ptr<Node>& document,
                               const std::string& pageUrl);
