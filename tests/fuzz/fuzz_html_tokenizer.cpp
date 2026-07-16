#include "html/tokenizer.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    HtmlTokenizer tokenizer;
    tokenizer.tokenize(std::string((const char*)data, size), [](const HtmlToken&) {});
    return 0;
}
