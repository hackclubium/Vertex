#include "test/fixture.h"

#include "html/parser.h"
#include "layout/layout.h"
#include "layout/scroll.h"

TestResult RunLayoutTests() {
    TestResult result;
    auto root = FindRepoRoot();

    auto input = ReadTextFile(root / "tests/fixtures/layout/basic.in.html");
    auto expected = ReadTextFile(root / "tests/fixtures/layout/basic.expected.txt");
    auto dom = ParseHtml(input);
    auto layout = BuildSimpleLayout(dom, 800.0f);
    auto actual = SerializeLayout(layout);
    ExpectEqual("layout/basic", actual, expected, result);

    ExpectEqual("layout/fragment-anchor-remains-scrollable",
        std::to_string(FragmentReachableDocumentHeight(2740.f, 2400.f, 536.f)),
        "2936.000000",
        result);

    return result;
}
