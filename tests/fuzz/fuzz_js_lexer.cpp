#include "js/lexer.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Lexer lexer(std::string((const char*)data, size));
    (void)lexer.tokenize();
    return 0;
}
