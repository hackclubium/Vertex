#pragma once

#include <filesystem>
#include <string>

struct TestResult {
    int passed = 0;
    int failed = 0;
};

std::filesystem::path FindRepoRoot();
std::string ReadTextFile(const std::filesystem::path& path);
bool ExpectEqual(const std::string& name, const std::string& actual, const std::string& expected, TestResult& result);

TestResult RunHtmlTests();
TestResult RunCssTests();
TestResult RunLayoutTests();
TestResult RunPaintTests();
TestResult RunJsTests();
TestResult RunNetworkTests();
