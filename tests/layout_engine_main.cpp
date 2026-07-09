#include "fixture.h"

#include <iostream>

TestResult RunLayoutEngineTests();

int main() {
    const TestResult result = RunLayoutEngineTests();
    std::cout << "\nPassed: " << result.passed << "\n";
    std::cout << "Failed: " << result.failed << "\n";
    return result.failed == 0 ? 0 : 1;
}
