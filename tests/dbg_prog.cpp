#include "fixture.h"
#include <iostream>

int main() {
    TestResult r = RunCodecTests();
    std::cout << "Passed: " << r.passed << "\nFailed: " << r.failed << "\n";
    return r.failed == 0 ? 0 : 1;
}
