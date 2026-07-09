#include "fixture.h"

#include <fstream>
#include <iostream>
#include <sstream>

std::filesystem::path FindRepoRoot() {
    std::filesystem::path current = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(current / "CMakeLists.txt")
            && std::filesystem::exists(current / "src")) {
            return current;
        }
        if (!current.has_parent_path()) break;
        current = current.parent_path();
    }
    return std::filesystem::current_path();
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static std::string NormalizeNewlines(std::string text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') continue;
            out += '\n';
        } else {
            out += text[i];
        }
    }
    return out;
}

bool ExpectEqual(const std::string& name, const std::string& actual, const std::string& expected, TestResult& result) {
    std::string a = NormalizeNewlines(actual);
    std::string e = NormalizeNewlines(expected);
    if (a == e) {
        ++result.passed;
        std::cout << "PASS " << name << "\n";
        return true;
    }

    ++result.failed;
    std::cout << "FAIL " << name << "\n";
    std::cout << "--- expected ---\n" << e;
    std::cout << "--- actual ---\n" << a;
    return false;
}
