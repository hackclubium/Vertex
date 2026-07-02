#pragma once
#include "css/style.h"
#include "html/dom.h"
#include <vector>
#include <string>
#include <cstddef>
#include <unordered_map>

enum class CssAttrMatch {
    Exists,
    Exact,
    Includes,
    DashPrefix,
    Prefix,
    Suffix,
    Substring
};

struct CssSelectorPart {
    std::string tag;                    // "" = any
    std::vector<std::string> classes;  // all must match
    std::string id;                    // "" = any
    std::string attrName;  // "" = no attribute selector
    std::string attrValue; // only used when attrHasValue = true
    bool attrHasValue = false;
    CssAttrMatch attrMatch = CssAttrMatch::Exists;
    std::vector<std::string> pseudos;
    std::vector<std::string> notSelectors;  // :not() argument text (simple selectors)
    std::vector<std::vector<std::string>> matchSelectorLists; // :is()/:where() selector-list args
    int functionalSpecificity = 0;  // max selector specificity contributed by :is()
    std::string pseudoElement;  // "before" or "after" for generated content
    bool neverMatch = false;
    char combinator = 0;   // 0=first, ' '=descendant, '>'=child, '+'=adjacent, '~'=general sibling
};

// One branch of an @media query list. A rule without media conditions is
// unconditional; otherwise any branch in its list may enable the rule.
struct CssMediaCondition {
    float minWidth = -1.f;
    float maxWidth = -1.f;
    float minHeight = -1.f;
    float maxHeight = -1.f;
    bool supported = true;

    bool matches(float width, float height) const {
        return supported
            && (minWidth < 0.f || width >= minWidth)
            && (maxWidth < 0.f || width <= maxWidth)
            && (minHeight < 0.f || height >= minHeight)
            && (maxHeight < 0.f || height <= maxHeight);
    }
};

struct CssRule {
    std::string tag;       // "" = any
    std::string cls;       // "" = any  (matches class attribute)
    std::string id;        // "" = any
    std::vector<CssSelectorPart> selector;
    ComputedStyle style;
    std::vector<CssMediaCondition> media;

    int specificity() const;

    bool matches(const Node* node) const;
};

struct FontFace {
    std::string family;
    std::string srcUrl;
};

struct Stylesheet {
    std::vector<CssRule> rules;
    std::vector<FontFace> fontFaces;
    std::unordered_map<std::string, std::vector<size_t>> idRuleBuckets;
    std::unordered_map<std::string, std::vector<size_t>> classRuleBuckets;
    std::unordered_map<std::string, std::vector<size_t>> tagRuleBuckets;
    std::vector<size_t> universalRuleBucket;
    float viewportWidth = 800.f;
    float viewportHeight = 600.f;
    float rootRemBase = 16.f;
    bool rootRemBaseSet = false;

    void setViewport(float width, float height) {
        viewportWidth = width;
        viewportHeight = height;
    }

    void rebuildRuleBuckets();

    // Compute style for a node from the sheet + its inline style=""
    ComputedStyle resolve(const Node* node) const;
};

Stylesheet ParseStylesheet(const std::string& css);
ComputedStyle ParseInlineStyle(const std::string& style);
void ResolveStyleVariables(ComputedStyle& style);
void SetCssHoverNode(const Node* node);
void SetCssFocusNode(const Node* node);
void SetCssViewport(float w, float h);
std::string SerializeStylesheet(const Stylesheet& sheet);
std::string SerializeComputedStyle(const ComputedStyle& style);
