#include "css/stylesheet.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    (void)ParseStylesheet(std::string((const char*)data, size));
    return 0;
}
