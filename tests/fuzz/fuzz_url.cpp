#include "network/url.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string value((const char*)data, size);
    (void)HasUrlScheme(value);
    (void)ResolveUrlAgainstBase(value, "https://example.test/a/b/index.html?x=1#frag");
    (void)UnwrapBingRedirect(value);
    (void)UnwrapDuckDuckGoRedirect(value);
    return 0;
}
