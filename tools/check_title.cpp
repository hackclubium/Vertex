#include "network/http_client.h"
#include "network/text_decode.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    auto res = FetchHttp(argv[1]);
    std::string html = DecodeTextToUtf8(res.body, res.contentType);
    size_t t1 = html.find("<title");
    size_t t2 = html.find("</title>");
    if (t1 != std::string::npos && t2 != std::string::npos) {
        std::printf("RAW TITLE: %s\n", html.substr(t1 + 6, t2 - t1 - 6).c_str());
    } else {
        std::printf("No title tag found\n");
    }
    return 0;
}
