#pragma once

#include <string>

// Convert a fetched text resource to UTF-8. HTTP charset wins over a document
// declaration; HTML may otherwise provide a <meta charset=...> fallback.
std::string DecodeTextToUtf8(const std::string& bytes,
                             const std::string& contentType,
                             bool sniffHtmlCharset = false);
