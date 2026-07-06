#include "test/fixture.h"

#include <iostream>
#include <string>

static int Finish(const TestResult& result) {
    std::cout << "\nPassed: " << result.passed << "\n";
    std::cout << "Failed: " << result.failed << "\n";
    return result.failed == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cout << "Usage: vertex-tests <html|css|layout|paint|js|network|codec>\n";
        return 2;
    }

    std::string suite = argv[1];
    if (suite == "html") return Finish(RunHtmlTests());
    if (suite == "css") return Finish(RunCssTests());
    if (suite == "layout") return Finish(RunLayoutTests());
    if (suite == "paint") return Finish(RunPaintTests());
    if (suite == "js") return Finish(RunJsTests());
    if (suite == "network") return Finish(RunNetworkTests());
    if (suite == "codec") return Finish(RunCodecTests());

    std::cout << "Unknown suite: " << suite << "\n";
    return 2;
}
