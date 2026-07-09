#include "fixture.h"

#include "html/parser.h"
#include "html/serializer.h"
#include "html/tokenizer.h"

#include <vector>

static std::vector<HtmlToken> TokenizeForTest(const std::string& html) {
    std::vector<HtmlToken> tokens;
    HtmlTokenizer tokenizer;
    tokenizer.tokenize(html, [&](const HtmlToken& token) {
        tokens.push_back(token);
    });
    return tokens;
}

static void RunTokenizerFixture(const std::string& name, TestResult& result) {
    auto root = FindRepoRoot();
    auto input = ReadTextFile(root / ("tests/fixtures/html/tokenizer/" + name + ".in.html"));
    auto expected = ReadTextFile(root / ("tests/fixtures/html/tokenizer/" + name + ".expected.txt"));
    auto actual = SerializeTokens(TokenizeForTest(input));
    ExpectEqual("html/tokenizer/" + name, actual, expected, result);
}

static void RunDomFixture(const std::string& name, TestResult& result) {
    auto root = FindRepoRoot();
    auto input = ReadTextFile(root / ("tests/fixtures/html/dom/" + name + ".in.html"));
    auto expected = ReadTextFile(root / ("tests/fixtures/html/dom/" + name + ".expected.txt"));
    auto actual = SerializeDom(ParseHtml(input));
    ExpectEqual("html/dom/" + name, actual, expected, result);
}

TestResult RunHtmlTests() {
    TestResult result;

    RunTokenizerFixture("basic", result);
    RunTokenizerFixture("doctype-comment-entity", result);
    RunTokenizerFixture("rawtext", result);
    RunTokenizerFixture("invalid-numeric-entity", result);

    RunDomFixture("basic", result);
    RunDomFixture("rawtext-style", result);
    RunDomFixture("rawtext-script-multiline", result);
    RunDomFixture("autoclose-paragraphs", result);

    {
        std::string actual = SerializeDom(ParseHtml("<p>A&nbsp;B</p>"));
        std::string expected =
            "#document\n"
            "  <html>\n"
            "    <body>\n"
            "      <p>\n"
            "        #text \"A" + std::string("\xC2\xA0") + "B\"\n";
        ExpectEqual("html/dom/non-breaking-space", actual, expected, result);
    }

    {
        std::string actual = SerializeDom(ParseHtml("<p>A&nbsp B&amp C</p>"));
        std::string expected =
            "#document\n"
            "  <html>\n"
            "    <body>\n"
            "      <p>\n"
            "        #text \"A" + std::string("\xC2\xA0") + " B& C\"\n";
        ExpectEqual("html/dom/named-entities-without-semicolon", actual, expected, result);
    }

    {
        std::string actual = SerializeDom(ParseHtml(
            "<html><body><div class=\"picture\"><p><table><tr><td></table>"
            "<p class=\"bad\"><blockquote class=\"first one\"></blockquote>"
            "<div class=\"forehead\"></div></div></body></html>"));
        std::string expected =
            "#document\n"
            "  <html>\n"
            "    <body>\n"
            "      <div class=\"picture\">\n"
            "        <p>\n"
            "        <table>\n"
            "          <tbody>\n"
            "            <tr>\n"
            "              <td>\n"
            "        <p class=\"bad\">\n"
            "        <blockquote class=\"first one\">\n"
            "        <div class=\"forehead\">\n";
        ExpectEqual("html/dom/p-closed-by-block-starts", actual, expected, result);
    }

    return result;
}
