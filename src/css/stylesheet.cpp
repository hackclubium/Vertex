#include "css/stylesheet.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <unordered_map>

// ─── length parsing ──────────────────────────────────────────────────────────

// Temporary parsing base for relative font values.  Rule parsing must restore
// this after each selector: a declaration on one element never changes the
// inherited font size of unrelated selectors.
static float g_emBase = 16.f;
static float g_remBase = 16.f;
static int g_parseDepth = 0;
static const Node* g_hoverNodeCss = nullptr;
static const Node* g_focusNodeCss = nullptr;
void SetCssHoverNode(const Node* node) { g_hoverNodeCss = node; }
void SetCssFocusNode(const Node* node) { g_focusNodeCss = node; }

// Viewport dimensions for vw/vh/vmin/vmax units. Set by the layout engine
// before style resolution so CSS lengths resolve against the real window.
static float g_viewportW = 800.f;
static float g_viewportH = 600.f;

void SetCssViewport(float w, float h) { g_viewportW = w; g_viewportH = h; }

// Parse a CSS length into pixels.  Returns -1 for inherit/auto/none/unknown.
static float ParseLength(const std::string& raw, float emBase = -1.f) {
    if (emBase < 0.f) emBase = g_emBase;
    std::string s = raw;
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    if (s.empty()) return -1;
    std::string low;
    for (char c : s) low += (char)std::tolower((unsigned char)c);
    if (low == "inherit" || low == "initial" || low == "unset" || low == "normal") return -1;
    if (low == "none" || low == "auto") return -1;
    if (low == "0") return 0;
    // clamp(min, val, max) / min(...) / max(...): split top-level comma args.
    {
        const char* fn = nullptr;
        if (low.rfind("clamp(", 0) == 0) fn = "clamp";
        else if (low.rfind("min(", 0) == 0) fn = "min";
        else if (low.rfind("max(", 0) == 0) fn = "max";
        if (fn && low.back() == ')') {
            size_t open = low.find('(');
            std::string inner = s.substr(open + 1, s.size() - open - 2);
            std::vector<float> args;
            int depth = 0; size_t start = 0; bool ok = true;
            for (size_t j = 0; j <= inner.size(); ++j) {
                if (j == inner.size() || (inner[j] == ',' && depth == 0)) {
                    float v = ParseLength(inner.substr(start, j - start), emBase);
                    if (v <= -1e5f) { ok = false; break; }
                    args.push_back(v); start = j + 1;
                } else if (inner[j] == '(') depth++;
                else if (inner[j] == ')' && depth > 0) depth--;
            }
            if (ok && !args.empty()) {
                if (std::string(fn) == "clamp" && args.size() == 3)
                    return std::max(args[0], std::min(args[1], args[2]));
                if (std::string(fn) == "min")
                    return *std::min_element(args.begin(), args.end());
                if (std::string(fn) == "max")
                    return *std::max_element(args.begin(), args.end());
            }
            return -1;
        }
    }
    // calc(): evaluate simple binary expressions like calc(100vw - 40px).
    if (low.rfind("calc(", 0) == 0 && low.back() == ')') {
        std::string inner = s.substr(5, s.size() - 6);
        // Find the operator (+/-) that isn't inside parens and isn't a sign.
        size_t opPos = std::string::npos;
        char op = 0;
        int depth = 0;
        for (size_t j = 1; j < inner.size(); ++j) {
            if (inner[j] == '(') depth++;
            else if (inner[j] == ')') { if (depth > 0) depth--; }
            else if (depth == 0 && (inner[j] == '+' || inner[j] == '-')
                     && j > 0 && inner[j-1] == ' ') {
                opPos = j; op = inner[j]; break;
            } else if (depth == 0 && inner[j] == '*') { opPos = j; op = '*'; break; }
            else if (depth == 0 && inner[j] == '/') { opPos = j; op = '/'; break; }
        }
        if (opPos != std::string::npos && op) {
            float a = ParseLength(inner.substr(0, opPos), emBase);
            float b = ParseLength(inner.substr(opPos + 1), emBase);
            if (a > -1e5f && b > -1e5f) {
                if (op == '+') return a + b;
                if (op == '-') return a - b;
                if (op == '*') return a * b;
                if (op == '/' && b != 0) return a / b;
            }
        }
        // Single-term calc(100vw) etc.
        float single = ParseLength(inner, emBase);
        if (single > -1e5f) return single;
        return -1;
    }
    // CSS border-width keywords
    if (low == "thin")   return 1.f;
    if (low == "medium") return 3.f;
    if (low == "thick")  return 5.f;
    // parse numeric part
    size_t i = 0;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) i++;
    size_t numStart = i;
    bool dot = false;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || (s[i] == '.' && !dot))) {
        if (s[i] == '.') dot = true;
        i++;
    }
    if (i == numStart) return -1;
    float num = 0;
    try { num = std::stof(s.substr(0, i)); } catch (...) { return 0; }
    std::string unit = low.substr(i);
    while (!unit.empty() && unit[0] == ' ') unit.erase(unit.begin());
    if (unit.empty() || unit == "px") return num;
    if (unit == "em") return num * emBase;
    if (unit == "rem") return num * g_remBase;
    if (unit == "pt")  return num * 1.333f;
    if (unit == "pc")  return num * emBase;
    if (unit == "in")  return num * 96.f;
    if (unit == "cm")  return num * (96.f / 2.54f);
    if (unit == "mm")  return num * (96.f / 25.4f);
    if (unit == "ex" || unit == "ch") return num * 8.f;
    if (unit == "%")   return num * 0.16f;  // rough: 100% = 16px base
    if (unit == "vw")   return num * (g_viewportW / 100.f);
    if (unit == "vh")   return num * (g_viewportH / 100.f);
    if (unit == "vmin") return num * (std::min(g_viewportW, g_viewportH) / 100.f);
    if (unit == "vmax") return num * (std::max(g_viewportW, g_viewportH) / 100.f);
    return num;  // unrecognized unit, treat as px
}

static float ParsePercentage(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    if (s.empty() || s.back() != '%') return -1;
    try {
        return std::stof(s.substr(0, s.size() - 1));
    } catch (...) {
        return -1;
    }
}

// ─── string helpers ──────────────────────────────────────────────────────────

static std::string sLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string sTrim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    return s;
}
static std::string stripQuotes(std::string s) {
    s = sTrim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
                       || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

static bool ParseCalcPercentOffset(const std::string& raw, float& percent, float& offset) {
    std::string s = sTrim(raw);
    std::string low = sLower(s);
    if (low.rfind("calc(", 0) != 0 || low.back() != ')') return false;
    std::string inner = s.substr(5, s.size() - 6);
    size_t opPos = std::string::npos;
    char op = 0;
    int depth = 0;
    for (size_t i = 1; i < inner.size(); ++i) {
        char c = inner[i];
        if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        else if (depth == 0 && (c == '+' || c == '-')) {
            opPos = i;
            op = c;
            break;
        }
    }
    if (opPos == std::string::npos) return false;

    std::string left = sTrim(inner.substr(0, opPos));
    std::string right = sTrim(inner.substr(opPos + 1));
    float lp = ParsePercentage(left);
    float rp = ParsePercentage(right);
    auto parseLengthTerm = [](const std::string& v, float& out) {
        if (ParsePercentage(v) >= 0) return false;
        float f = ParseLength(v);
        if (f <= -1e5f) return false;
        out = f;
        return true;
    };

    float ll = 0, rl = 0;
    if (lp >= 0 && parseLengthTerm(right, rl)) {
        percent = lp;
        offset = (op == '-') ? -rl : rl;
        return true;
    }
    if (rp >= 0 && parseLengthTerm(left, ll)) {
        percent = (op == '-') ? -rp : rp;
        offset = ll;
        return true;
    }
    return false;
}

static bool IsUnitlessNumber(const std::string& raw) {
    std::string s = sTrim(raw);
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') ++i;
    bool digit = false;
    bool dot = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (std::isdigit((unsigned char)c)) {
            digit = true;
            continue;
        }
        if (c == '.' && !dot) {
            dot = true;
            continue;
        }
        return false;
    }
    return digit;
}

static float ParseLineHeightValue(const std::string& raw, float baseFontSize) {
    std::string v = sLower(sTrim(raw));
    if (v.empty() || v == "normal" || v == "inherit" || v == "initial" || v == "unset")
        return 0;
    float pct = ParsePercentage(v);
    if (pct >= 0) return baseFontSize * (pct / 100.f);
    if (IsUnitlessNumber(v)) {
        try { return std::stof(v) * baseFontSize; } catch (...) { return 0; }
    }
    float length = ParseLength(v);
    return length > 0 ? length : 0;
}

static bool IsIgnoredCssKeyword(const std::string& raw) {
    std::string v = sLower(sTrim(raw));
    return v.empty() || v == "inherit" || v == "initial" || v == "unset";
}

static std::vector<std::string> SplitCssWhitespace(const std::string& raw) {
    std::vector<std::string> out;
    std::string tok;
    int parenDepth = 0;
    int bracketDepth = 0;
    char quote = 0;
    for (char c : raw) {
        if (quote) {
            tok += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            tok += c;
            continue;
        }
        if (c == '(') ++parenDepth;
        else if (c == ')' && parenDepth > 0) --parenDepth;
        else if (c == '[') ++bracketDepth;
        else if (c == ']' && bracketDepth > 0) --bracketDepth;
        if (std::isspace((unsigned char)c) && parenDepth == 0 && bracketDepth == 0) {
            if (!tok.empty()) {
                out.push_back(tok);
                tok.clear();
            }
        } else {
            tok += c;
        }
    }
    if (!tok.empty()) out.push_back(tok);
    return out;
}

static void ApplyMarginToken(float& side, const std::string& token) {
    std::string v = sLower(sTrim(token));
    if (v == "auto") {
        side = kCssAuto;
    } else if (!IsIgnoredCssKeyword(v)) {
        float f = ParseLength(v);
        if (f > -1e5f) side = f;
    }
}

static void ApplyPaddingToken(float& side, const std::string& token) {
    if (IsIgnoredCssKeyword(token)) return;
    float f = ParseLength(token);
    if (f >= 0) side = f;
}

static void ApplyInsetToken(float& side, bool& sideSet, bool& sidePercent, const std::string& token) {
    std::string v = sTrim(token);
    std::string low = sLower(v);
    if (low == "auto" || IsIgnoredCssKeyword(low)) return;
    sideSet = true;
    float pct = ParsePercentage(v);
    if (pct >= 0) {
        side = pct;
        sidePercent = true;
    } else {
        side = ParseLength(v);
        sidePercent = false;
    }
}

static bool ParseTransformLengthToken(const std::string& raw, float& value, bool& isPercent) {
    std::string t = sTrim(raw);
    if (t.empty()) return false;
    isPercent = !t.empty() && t.back() == '%';
    if (isPercent) {
        try { value = std::stof(t.substr(0, t.size() - 1)); return true; } catch (...) { value = 0; return false; }
    }
    float f = ParseLength(t);
    if (f <= -1e5f) {
        try { f = std::stof(t); } catch (...) { return false; }
    }
    value = f;
    return true;
}

static bool ParseAngleToken(const std::string& raw, float& degrees) {
    std::string t = sLower(sTrim(raw));
    if (t.empty()) return false;
    try {
        if (t.size() > 3 && t.substr(t.size() - 3) == "deg") {
            degrees = std::stof(t.substr(0, t.size() - 3));
            return true;
        }
        if (t.size() > 4 && t.substr(t.size() - 4) == "turn") {
            degrees = std::stof(t.substr(0, t.size() - 4)) * 360.f;
            return true;
        }
        if (t.size() > 3 && t.substr(t.size() - 3) == "rad") {
            degrees = std::stof(t.substr(0, t.size() - 3)) * 57.2957795f;
            return true;
        }
        degrees = std::stof(t);
        return true;
    } catch (...) {
        return false;
    }
}

static void ApplyDeclaration(const std::string& prop,
                             const std::string& val,
                             ComputedStyle& out);

static bool ApplyLogicalDeclaration(const std::string& prop,
                                    const std::string& val,
                                    ComputedStyle& out) {
    if (prop == "inline-size") { ApplyDeclaration("width", val, out); return true; }
    if (prop == "block-size") { ApplyDeclaration("height", val, out); return true; }
    if (prop == "min-inline-size") { ApplyDeclaration("min-width", val, out); return true; }
    if (prop == "max-inline-size") { ApplyDeclaration("max-width", val, out); return true; }
    if (prop == "min-block-size") { ApplyDeclaration("min-height", val, out); return true; }
    if (prop == "max-block-size") { ApplyDeclaration("max-height", val, out); return true; }

    if (prop == "margin-inline") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyMarginToken(out.marginLeft, vals[0]);
            ApplyMarginToken(out.marginRight, vals.size() > 1 ? vals[1] : vals[0]);
        }
        return true;
    }
    if (prop == "margin-block") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyMarginToken(out.marginTop, vals[0]);
            ApplyMarginToken(out.marginBottom, vals.size() > 1 ? vals[1] : vals[0]);
        }
        return true;
    }
    if (prop == "margin-inline-start") { ApplyMarginToken(out.marginLeft, val); return true; }
    if (prop == "margin-inline-end") { ApplyMarginToken(out.marginRight, val); return true; }
    if (prop == "margin-block-start") { ApplyMarginToken(out.marginTop, val); return true; }
    if (prop == "margin-block-end") { ApplyMarginToken(out.marginBottom, val); return true; }

    if (prop == "padding-inline") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyPaddingToken(out.paddingLeft, vals[0]);
            ApplyPaddingToken(out.paddingRight, vals.size() > 1 ? vals[1] : vals[0]);
        }
        return true;
    }
    if (prop == "padding-block") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyPaddingToken(out.paddingTop, vals[0]);
            ApplyPaddingToken(out.paddingBottom, vals.size() > 1 ? vals[1] : vals[0]);
        }
        return true;
    }
    if (prop == "padding-inline-start") { ApplyPaddingToken(out.paddingLeft, val); return true; }
    if (prop == "padding-inline-end") { ApplyPaddingToken(out.paddingRight, val); return true; }
    if (prop == "padding-block-start") { ApplyPaddingToken(out.paddingTop, val); return true; }
    if (prop == "padding-block-end") { ApplyPaddingToken(out.paddingBottom, val); return true; }

    if (prop == "border-inline-width") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyDeclaration("border-left-width", vals[0], out);
            ApplyDeclaration("border-right-width", vals.size() > 1 ? vals[1] : vals[0], out);
        }
        return true;
    }
    if (prop == "border-block-width") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyDeclaration("border-top-width", vals[0], out);
            ApplyDeclaration("border-bottom-width", vals.size() > 1 ? vals[1] : vals[0], out);
        }
        return true;
    }
    if (prop == "border-inline-start-width") { ApplyDeclaration("border-left-width", val, out); return true; }
    if (prop == "border-inline-end-width") { ApplyDeclaration("border-right-width", val, out); return true; }
    if (prop == "border-block-start-width") { ApplyDeclaration("border-top-width", val, out); return true; }
    if (prop == "border-block-end-width") { ApplyDeclaration("border-bottom-width", val, out); return true; }

    if (prop == "border-inline-color") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyDeclaration("border-left-color", vals[0], out);
            ApplyDeclaration("border-right-color", vals.size() > 1 ? vals[1] : vals[0], out);
        }
        return true;
    }
    if (prop == "border-block-color") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyDeclaration("border-top-color", vals[0], out);
            ApplyDeclaration("border-bottom-color", vals.size() > 1 ? vals[1] : vals[0], out);
        }
        return true;
    }
    if (prop == "border-inline-start-color") { ApplyDeclaration("border-left-color", val, out); return true; }
    if (prop == "border-inline-end-color") { ApplyDeclaration("border-right-color", val, out); return true; }
    if (prop == "border-block-start-color") { ApplyDeclaration("border-top-color", val, out); return true; }
    if (prop == "border-block-end-color") { ApplyDeclaration("border-bottom-color", val, out); return true; }

    if (prop == "inset") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            const std::string& top = vals[0];
            const std::string& right = vals.size() > 1 ? vals[1] : vals[0];
            const std::string& bottom = vals.size() > 2 ? vals[2] : vals[0];
            const std::string& left = vals.size() > 3 ? vals[3] : right;
            ApplyInsetToken(out.top, out.topSet, out.topPercent, top);
            ApplyInsetToken(out.right, out.rightSet, out.rightPercent, right);
            ApplyInsetToken(out.bottom, out.bottomSet, out.bottomPercent, bottom);
            ApplyInsetToken(out.left, out.leftSet, out.leftPercent, left);
        }
        return true;
    }
    if (prop == "inset-inline") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyInsetToken(out.left, out.leftSet, out.leftPercent, vals[0]);
            ApplyInsetToken(out.right, out.rightSet, out.rightPercent, vals.size() > 1 ? vals[1] : vals[0]);
        }
        return true;
    }
    if (prop == "inset-block") {
        auto vals = SplitCssWhitespace(val);
        if (!vals.empty()) {
            ApplyInsetToken(out.top, out.topSet, out.topPercent, vals[0]);
            ApplyInsetToken(out.bottom, out.bottomSet, out.bottomPercent, vals.size() > 1 ? vals[1] : vals[0]);
        }
        return true;
    }
    if (prop == "inset-inline-start") { ApplyInsetToken(out.left, out.leftSet, out.leftPercent, val); return true; }
    if (prop == "inset-inline-end") { ApplyInsetToken(out.right, out.rightSet, out.rightPercent, val); return true; }
    if (prop == "inset-block-start") { ApplyInsetToken(out.top, out.topSet, out.topPercent, val); return true; }
    if (prop == "inset-block-end") { ApplyInsetToken(out.bottom, out.bottomSet, out.bottomPercent, val); return true; }

    return false;
}

static bool IsHexDigit(char c) {
    return std::isxdigit((unsigned char)c) != 0;
}

static int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return 0;
}

static std::string CssUnescape(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c != '\\' || i + 1 >= input.size()) {
            out += c;
            continue;
        }

        size_t j = i + 1;
        if (IsHexDigit(input[j])) {
            int value = 0;
            int count = 0;
            while (j < input.size() && count < 6 && IsHexDigit(input[j])) {
                value = value * 16 + HexValue(input[j]);
                ++j;
                ++count;
            }
            if (j < input.size() && std::isspace((unsigned char)input[j])) ++j;
            if (value > 0 && value <= 0x7f) out += (char)value;
            i = j - 1;
        } else {
            out += input[j];
            i = j;
        }
    }
    return out;
}

static std::vector<std::string> SplitDeclarations(const std::string& block) {
    std::vector<std::string> out;
    std::string cur;
    int parenDepth = 0;
    char quote = 0;
    bool escaped = false;

    for (char c : block) {
        if (escaped) {
            cur += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            cur += c;
            escaped = true;
            continue;
        }
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            cur += c;
            quote = c;
            continue;
        }
        if (c == '(') {
            ++parenDepth;
            cur += c;
            continue;
        }
        if (c == ')') {
            if (parenDepth > 0) --parenDepth;
            cur += c;
            continue;
        }
        if (c == ';' && parenDepth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ─── color parsing ───────────────────────────────────────────────────────────

static const std::map<std::string,CssColor>& namedColors() {
    static std::map<std::string,CssColor> m = {
        #define C(n,r,g,b) {n,{true,r,g,b,1}}
        C("black",0,0,0),         C("white",1,1,1),
        C("red",1,0,0),           C("green",0,.502f,0),
        C("blue",0,0,1),          C("yellow",1,1,0),
        C("orange",1,.647f,0),    C("purple",.502f,0,.502f),
        C("pink",1,.753f,.796f),  C("cyan",0,1,1),
        C("magenta",1,0,1),       C("gray",.502f,.502f,.502f),
        C("grey",.502f,.502f,.502f), C("silver",.753f,.753f,.753f),
        C("teal",0,.502f,.502f),  C("navy",0,0,.502f),
        C("maroon",.502f,0,0),    C("olive",.502f,.502f,0),
        C("lime",0,1,0),          C("aqua",0,1,1),
        C("fuchsia",1,0,1),       C("brown",.647f,.165f,.165f),
        C("coral",1,.498f,.314f), C("crimson",.863f,.078f,.235f),
        C("darkblue",0,0,.545f),  C("darkgreen",0,.392f,0),
        C("darkred",.545f,0,0),   C("gold",1,.843f,0),
        C("hotpink",1,.412f,.706f),C("indigo",.294f,0,.510f),
        C("lavender",.902f,.902f,.980f),
        C("lightblue",.678f,.847f,.902f),
        C("lightgray",.827f,.827f,.827f),
        C("lightgrey",.827f,.827f,.827f),
        C("lightgreen",.565f,.933f,.565f),
        C("salmon",.980f,.502f,.447f),
        C("skyblue",.529f,.808f,.922f),
        C("tomato",1,.388f,.278f),
        // transparent handled before map lookup in ParseCssColor
        C("violet",.933f,.510f,.933f),
        C("deepskyblue",0,.749f,1),
        C("dimgray",.412f,.412f,.412f),
        C("dimgrey",.412f,.412f,.412f),
        C("wheat",.961f,.871f,.702f),
        C("tan",.824f,.706f,.549f),
        C("khaki",.941f,.902f,.549f),
        #undef C
    };
    return m;
}

CssColor ParseCssColor(const std::string& raw) {
    std::string s = sLower(sTrim(raw));
    CssColor out;
    if (s.empty() || s == "inherit" || s == "initial") return out;
    if (s == "transparent") return {true, 0, 0, 0, 0};

    auto& nm = namedColors();
    auto it = nm.find(s);
    if (it != nm.end()) return it->second;

    if (!s.empty() && s[0] == '#') {
        std::string h = s.substr(1);
        if (h.size() == 3 || h.size() == 4) {
            std::string e; for (char c : h) e += {c,c}; h = e;
        }
        if (h.size() >= 6) {
            try {
                out.r = std::stoi(h.substr(0,2),nullptr,16)/255.f;
                out.g = std::stoi(h.substr(2,2),nullptr,16)/255.f;
                out.b = std::stoi(h.substr(4,2),nullptr,16)/255.f;
                out.a = h.size()>=8 ? std::stoi(h.substr(6,2),nullptr,16)/255.f : 1.f;
                out.valid = true;
            } catch (...) {}
        }
        return out;
    }

    if (s.rfind("rgb(", 0) == 0 || s.rfind("rgba(", 0) == 0) {
        size_t open = s.find('('), end = s.find(')', open + 1);
        if (end != std::string::npos) {
            std::string inner = s.substr(open + 1, end - open - 1);
            std::replace(inner.begin(), inner.end(), '/', ' ');
            std::replace(inner.begin(), inner.end(), ',', ' ');
            std::istringstream ss(inner);
            std::string tok;
            std::vector<std::string> toks;
            while (ss >> tok) toks.push_back(tok);
            auto rgbValue = [](const std::string& tok, float& out) {
                try {
                    if (!tok.empty() && tok.back() == '%') out = std::stof(tok.substr(0, tok.size() - 1)) / 100.f;
                    else out = std::stof(tok) / 255.f;
                    out = std::max(0.f, std::min(1.f, out));
                    return true;
                } catch (...) { return false; }
            };
            auto alphaValue = [](const std::string& tok, float& out) {
                try {
                    if (!tok.empty() && tok.back() == '%') out = std::stof(tok.substr(0, tok.size() - 1)) / 100.f;
                    else out = std::stof(tok);
                    out = std::max(0.f, std::min(1.f, out));
                    return true;
                } catch (...) { return false; }
            };
            if (toks.size() >= 3
                && rgbValue(toks[0], out.r)
                && rgbValue(toks[1], out.g)
                && rgbValue(toks[2], out.b)) {
                out.a = 1.f;
                if (toks.size() >= 4 && !alphaValue(toks[3], out.a)) return out;
                out.valid = true;
            }
        }
    } else if (s.rfind("hsl(", 0) == 0 || s.rfind("hsla(", 0) == 0) {
        size_t open = s.find('('), end = s.find(')', open + 1);
        if (end != std::string::npos) {
            std::string inner = s.substr(open + 1, end - open - 1);
            std::replace(inner.begin(), inner.end(), '/', ' ');
            std::replace(inner.begin(), inner.end(), ',', ' ');
            std::istringstream ss(inner);
            std::string tok;
            std::vector<std::string> toks;
            while (ss >> tok) toks.push_back(tok);
            auto pctValue = [](const std::string& tok, float& out) {
                if (tok.empty() || tok.back() != '%') return false;
                try { out = std::max(0.f, std::min(1.f, std::stof(tok.substr(0, tok.size() - 1)) / 100.f)); return true; }
                catch (...) { return false; }
            };
            auto alphaValue = [](const std::string& tok, float& out) {
                try {
                    if (!tok.empty() && tok.back() == '%') out = std::stof(tok.substr(0, tok.size() - 1)) / 100.f;
                    else out = std::stof(tok);
                    out = std::max(0.f, std::min(1.f, out));
                    return true;
                } catch (...) { return false; }
            };
            if (toks.size() >= 3) {
                float h = 0, sat = 0, light = 0;
                try { h = std::stof(toks[0]); } catch (...) { return out; }
                if (!pctValue(toks[1], sat) || !pctValue(toks[2], light)) return out;
                h = std::fmod(h, 360.f);
                if (h < 0) h += 360.f;
                float c = (1.f - std::fabs(2.f * light - 1.f)) * sat;
                float x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f));
                float m = light - c / 2.f;
                float r = 0, g = 0, b = 0;
                if (h < 60.f) { r = c; g = x; }
                else if (h < 120.f) { r = x; g = c; }
                else if (h < 180.f) { g = c; b = x; }
                else if (h < 240.f) { g = x; b = c; }
                else if (h < 300.f) { r = x; b = c; }
                else { r = c; b = x; }
                out = {true, r + m, g + m, b + m, 1.f};
                if (toks.size() >= 4 && !alphaValue(toks[3], out.a)) return CssColor{};
            }
        }
    }
    return out;
}

// ─── declaration parsing (shared between inline and stylesheet) ───────────────

// Parse one background-position component into (value, isPercent). Keywords map
// to percentages (left/top=0%, center=50%, right/bottom=100%).
static bool ParseBgPosComponent(const std::string& tok, float& outVal, bool& outPct) {
    if (tok == "left" || tok == "top")    { outVal = 0;   outPct = true; return true; }
    if (tok == "center")                  { outVal = 50;  outPct = true; return true; }
    if (tok == "right" || tok == "bottom"){ outVal = 100; outPct = true; return true; }
    if (!tok.empty() && tok.back() == '%') {
        try { outVal = std::stof(tok.substr(0, tok.size() - 1)); outPct = true; return true; }
        catch (...) { return false; }
    }
    float px = ParseLength(tok);
    if (px > -1e5f) { outVal = px; outPct = false; return true; }
    return false;
}

static void ParseBgPosition(const std::string& v, ComputedStyle& out) {
    std::istringstream ss(v);
    std::vector<std::string> toks; std::string t;
    while (ss >> t) toks.push_back(t);
    if (toks.empty()) return;
    float x = 0, y = 0; bool xp = true, yp = true;
    bool okX = false, okY = false;
    if (toks.size() == 1) {
        if (toks[0] == "top" || toks[0] == "bottom") {
            okY = ParseBgPosComponent(toks[0], y, yp); x = 50; xp = true; okX = true;
        } else {
            okX = ParseBgPosComponent(toks[0], x, xp); y = 50; yp = true; okY = true;
        }
    } else {
        std::string a = toks[0], b = toks[1];
        if (a == "top" || a == "bottom" || b == "left" || b == "right") std::swap(a, b);
        okX = ParseBgPosComponent(a, x, xp);
        okY = ParseBgPosComponent(b, y, yp);
    }
    if (okX && okY) {
        out.bgPosX = x; out.bgPosY = y; out.bgPosXPct = xp; out.bgPosYPct = yp;
        out.bgPosSet = true;
    }
}

static void ParseBgSize(const std::string& v, ComputedStyle& out) {
    if (v == "cover")   { out.bgSizeMode = 1; out.bgSizeSet = true; return; }
    if (v == "contain") { out.bgSizeMode = 2; out.bgSizeSet = true; return; }
    std::istringstream ss(v);
    std::vector<std::string> toks; std::string t;
    while (ss >> t) toks.push_back(t);
    if (toks.empty()) return;
    auto parseDim = [](const std::string& tok, float& outVal, bool& outPct) -> bool {
        if (tok == "auto") { outVal = -1; outPct = false; return true; }
        if (!tok.empty() && tok.back() == '%') {
            try { outVal = std::stof(tok.substr(0, tok.size() - 1)); outPct = true; return true; }
            catch (...) { return false; }
        }
        float px = ParseLength(tok);
        if (px > -1e5f) { outVal = px; outPct = false; return true; }
        return false;
    };
    float w = -1, h = -1; bool wp = false, hp = false;
    if (!parseDim(toks[0], w, wp)) return;
    if (toks.size() > 1) { if (!parseDim(toks[1], h, hp)) return; }
    out.bgSizeMode = 3; out.bgSizeW = w; out.bgSizeH = h;
    out.bgSizeWPct = wp; out.bgSizeHPct = hp; out.bgSizeSet = true;
}

static std::vector<std::string> SplitGridTracks(const std::string& value) {
    std::vector<std::string> tracks;
    std::string token;
    int parens = 0;
    for (char c : value) {
        if (c == '(') ++parens;
        else if (c == ')' && parens > 0) --parens;
        if (std::isspace((unsigned char)c) && parens == 0) {
            if (!token.empty()) { tracks.push_back(token); token.clear(); }
        } else {
            token += c;
        }
    }
    if (!token.empty()) tracks.push_back(token);
    return tracks;
}

// Parse "linear-gradient(...)" into angle + color stops. Returns true if valid.
static bool ParseLinearGradient(const std::string& raw, ComputedStyle& out) {
    std::string low = sLower(raw);
    size_t start = low.find("linear-gradient(");
    if (start == std::string::npos) return false;
    size_t inner = start + 16;
    // Find matching ')'.
    int depth = 1; size_t end = inner;
    for (; end < raw.size() && depth > 0; ++end)
        if (raw[end] == '(') ++depth; else if (raw[end] == ')') --depth;
    if (depth != 0) return false;
    std::string body = sTrim(raw.substr(inner, end - inner - 1));
    if (body.empty()) return false;

    // Split on commas, respecting parens (for rgb()/hsl()).
    std::vector<std::string> parts;
    { std::string cur; int d = 0;
      for (char c : body) {
          if (c == '(') ++d; else if (c == ')' && d > 0) --d;
          if (c == ',' && d == 0) { parts.push_back(sTrim(cur)); cur.clear(); }
          else cur += c;
      }
      if (!cur.empty()) parts.push_back(sTrim(cur));
    }
    if (parts.size() < 2) return false;

    float angle = 180;  // default: to bottom
    size_t colorStart = 0;
    {
        std::string first = sLower(parts[0]);
        bool isDir = false;
        if (first.find("to ") == 0) {
            isDir = true;
            if (first.find("top") != std::string::npos && first.find("right") != std::string::npos) angle = 45;
            else if (first.find("right") != std::string::npos && first.find("bottom") != std::string::npos) angle = 135;
            else if (first.find("bottom") != std::string::npos && first.find("left") != std::string::npos) angle = 225;
            else if (first.find("top") != std::string::npos && first.find("left") != std::string::npos) angle = 315;
            else if (first.find("top") != std::string::npos) angle = 0;
            else if (first.find("right") != std::string::npos) angle = 90;
            else if (first.find("bottom") != std::string::npos) angle = 180;
            else if (first.find("left") != std::string::npos) angle = 270;
        }
        if (!isDir) {
            // Try numeric angle (e.g. "45deg")
            size_t deg = first.find("deg");
            if (deg != std::string::npos) {
                try { angle = std::stof(first.substr(0, deg)); isDir = true; } catch (...) {}
            } else if (!first.empty() && (first[0] == '-' || first[0] == '+' || std::isdigit((unsigned char)first[0]))) {
                try { angle = std::stof(first); isDir = true; } catch (...) {}
            }
        }
        colorStart = isDir ? 1 : 0;
    }

    std::vector<ComputedStyle::GradientStop> stops;
    for (size_t i = colorStart; i < parts.size(); ++i) {
        std::string p = sTrim(parts[i]);
        // Try to split "color position" — the position is the last token if it ends with % or is a length.
        float pos = -1;
        size_t lastSpace = p.rfind(' ');
        std::string colorStr = p;
        if (lastSpace != std::string::npos) {
            std::string posTok = sLower(sTrim(p.substr(lastSpace + 1)));
            bool hasPct = (!posTok.empty() && posTok.back() == '%');
            float pv = -1;
            if (hasPct) { try { pv = std::stof(posTok.substr(0, posTok.size() - 1)) / 100.f; } catch (...) {} }
            else { float l = ParseLength(posTok); if (l > -1e5f) pv = l; }  // px position (approx as fraction later)
            if (pv >= 0) { pos = pv; colorStr = sTrim(p.substr(0, lastSpace)); }
        }
        CssColor c = ParseCssColor(colorStr);
        if (!c.valid) continue;
        stops.push_back({ c, pos });
    }
    if (stops.size() < 2) return false;

    // Auto-distribute missing stop positions.
    if (stops.front().pos < 0) stops.front().pos = 0;
    if (stops.back().pos < 0) stops.back().pos = 1;
    for (size_t i = 1; i + 1 < stops.size(); ++i) {
        if (stops[i].pos < 0) {
            size_t j = i + 1;
            while (j < stops.size() && stops[j].pos < 0) ++j;
            float from = stops[i - 1].pos, to = stops[j].pos;
            float step = (to - from) / (float)(j - i + 1);
            for (size_t k = i; k < j; ++k)
                stops[k].pos = from + step * (float)(k - i + 1);
        }
    }

    out.gradientAngle = angle;
    out.gradientStops = std::move(stops);
    out.gradientSet = true;
    return true;
}

static void ApplyDeclaration(const std::string& prop,
                             const std::string& val,
                             ComputedStyle& out) {
    if (ApplyLogicalDeclaration(prop, val, out)) return;

    if (prop == "color") {
        out.color = ParseCssColor(val);
    } else if (prop == "background-color") {
        out.bgColor = ParseCssColor(val);
        out.bgColorSet = true;
    } else if (prop == "background") {
        // background shorthand: extract url() and color from compound values.
        // Parse into locals first; if the value is invalid (e.g. two colors
        // like "red pink") drop the whole declaration per CSS error handling.
        std::string low = sLower(val);
        if (low.find("linear-gradient(") != std::string::npos) {
            ParseLinearGradient(val, out);
            return;
        }
        std::string bgImg;
        bool noRepeat = (low.find("no-repeat") != std::string::npos);
        bool fixed = (low.find("fixed") != std::string::npos);
        if (low.find("url(") != std::string::npos) {
            size_t us = low.find("url("), ue = val.find(')', us + 4);
            if (ue != std::string::npos)
                bgImg = stripQuotes(sTrim(val.substr(us + 4, ue - us - 4)));
        }
        CssColor bg; int colorCount = 0; bool invalid = false;
        int repeat = -1;                       // -1 = unspecified
        std::vector<std::string> posToks;      // background-position candidates
        std::string sizeStr;                   // text after '/' (background-size)
        bool afterSlash = false;
        {
            size_t i = 0;
            while (i < val.size()) {
                while (i < val.size() && std::isspace((unsigned char)val[i])) i++;
                if (i >= val.size()) break;
                if (val[i] == '/') { afterSlash = true; i++; continue; }
                // Paren-aware token: don't split inside url()/rgb()/hsl().
                size_t j = i; int depth = 0;
                while (j < val.size() && (depth > 0 || (!std::isspace((unsigned char)val[j]) && val[j] != '/'))) {
                    if (val[j] == '(') depth++;
                    else if (val[j] == ')' && depth > 0) depth--;
                    j++;
                }
                std::string tok = val.substr(i, j - i);
                std::string tl = sLower(tok);
                i = j;
                if (tl.rfind("url(", 0) == 0) continue;   // image handled above
                if (tl == "no-repeat") { repeat = 3; continue; }
                if (tl == "repeat")    { repeat = 0; continue; }
                if (tl == "repeat-x")  { repeat = 1; continue; }
                if (tl == "repeat-y")  { repeat = 2; continue; }
                CssColor c = ParseCssColor(tok);
                if (c.valid) { bg = c; colorCount++; continue; }
                if (afterSlash || tl == "cover" || tl == "contain") {
                    sizeStr += (sizeStr.empty() ? "" : " ") + tl; continue;
                }
                bool posKeyword = (tl == "center" || tl == "top" || tl == "bottom"
                    || tl == "left" || tl == "right");
                bool lengthish = !tl.empty() && (std::isdigit((unsigned char)tl[0])
                    || tl[0] == '-' || tl[0] == '+' || tl.find('%') != std::string::npos);
                if (posKeyword || lengthish) { posToks.push_back(tl); continue; }
                bool keyword = (tl == "fixed" || tl == "scroll" || tl == "local" || tl == "none"
                    || tl == "inherit" || tl == "border-box" || tl == "padding-box"
                    || tl == "content-box");
                if (tl == "transparent") { bg = {true,0,0,0,0}; colorCount++; }
                else if (!keyword) {
                    invalid = true;   // unknown token (e.g. a second colour name)
                }
            }
        }
        if (colorCount > 1 || invalid) {
            // invalid background value → ignore declaration entirely
        } else {
            out.backgroundImage = bgImg;
            out.backgroundImageSet = true;
            out.bgNoRepeat = noRepeat;
            out.bgFixed = fixed;
            out.bgColor = bg;
            out.bgColorSet = true;
            if (repeat >= 0) { out.bgRepeat = repeat; out.bgNoRepeat = (repeat == 3); out.bgRepeatSet = true; }
            if (!posToks.empty()) {
                std::string p; for (auto& t : posToks) p += (p.empty() ? "" : " ") + t;
                ParseBgPosition(p, out);
            }
            if (!sizeStr.empty()) ParseBgSize(sizeStr, out);
        }
    } else if (prop == "background-image") {
        std::string low = sLower(val);
        out.backgroundImage.clear();
        out.backgroundImageSet = true;
        if (low.find("linear-gradient(") != std::string::npos) {
            ParseLinearGradient(val, out);
        } else if (low.find("url(") != std::string::npos) {
            size_t us = low.find("url("), ue = val.find(')', us + 4);
            if (ue != std::string::npos) {
                std::string url = sTrim(val.substr(us + 4, ue - us - 4));
                out.backgroundImage = stripQuotes(url);
            }
        }
    } else if (prop == "background-repeat") {
        std::string v = sLower(sTrim(val));
        out.bgRepeat = (v == "no-repeat") ? 3 : (v == "repeat-x") ? 1
                     : (v == "repeat-y") ? 2 : 0;
        out.bgNoRepeat = (out.bgRepeat == 3);
        out.bgRepeatSet = true;
    } else if (prop == "background-position") {
        ParseBgPosition(sLower(sTrim(val)), out);
    } else if (prop == "background-size") {
        ParseBgSize(sLower(sTrim(val)), out);
    } else if (prop == "font-size") {
        std::string v = sLower(sTrim(val));
        if      (v == "small")    out.fontSize = 12;
        else if (v == "medium")   out.fontSize = 15;
        else if (v == "large")    out.fontSize = 18;
        else if (v == "x-large")  out.fontSize = 22;
        else if (v == "xx-large") out.fontSize = 28;
        else if (v == "smaller")  out.fontSize = 12;
        else if (v == "larger")   out.fontSize = 18;
        else {
            float f = ParseLength(val);
            if (f > 0) {
                out.fontSize = f;
                // Only update g_emBase for absolute font sizes (px/pt), not em-relative
                std::string rawLow = sLower(sTrim(val));
                if (rawLow.find("em") == std::string::npos) g_emBase = f;
            }
        }
    } else if (prop == "font-weight") {
        std::string v = sLower(val);
        out.boldSet = true;
        int w = 0; try { w = std::stoi(v); } catch (...) {}
        out.bold = (v == "bold" || v == "bolder" || w >= 600);
    } else if (prop == "font-style") {
        out.italicSet = true;
        out.italic = (sLower(val) == "italic" || sLower(val) == "oblique");
    } else if (prop == "font-family") {
        std::string v = val;
        size_t comma = v.find(',');
        if (comma != std::string::npos) v = v.substr(0, comma);
        v = stripQuotes(sTrim(v));
        std::string low = sLower(v);
        if      (low == "sans-serif")                           v = "Segoe UI";
        else if (low == "serif")                                v = "Georgia";
        else if (low == "monospace" || low == "courier"
              || low == "courier new")                          v = "Consolas";
        else if (low == "helvetica" || low == "arial")          v = "Arial";
        else if (low == "times" || low == "times new roman")    v = "Times New Roman";
        else if (low == "system-ui" || low == "ui-sans-serif")  v = "Segoe UI";
        out.fontFamily = v;
    } else if (prop == "font") {
        std::istringstream vs(val); std::string tok;
        size_t consumed = 0;
        while (vs >> tok) {
            size_t at = val.find(tok, consumed);
            if (at != std::string::npos) consumed = at + tok.size();
            std::string low = sLower(tok);
            if (low == "bold" || low == "bolder") { out.bold = true; out.boldSet = true; }
            else if (low.size() == 3 && std::all_of(low.begin(), low.end(), ::isdigit)) {
                int w = 0; try { w = std::stoi(low); } catch (...) {}
                if (w >= 100 && w <= 900) { out.bold = w >= 600; out.boldSet = true; }
            }
            else if (low == "italic" || low == "oblique") { out.italic = true; out.italicSet = true; }
            else if (low == "normal") { /* no-op */ }
            else {
                // Check for size (maybe size/lineheight)
                size_t slash = low.find('/');
                std::string sizePart = (slash != std::string::npos) ? low.substr(0, slash) : low;
                float sz = ParseLength(sizePart);
                if (sz > 0) {
                    out.fontSize = sz;
                    // Only update g_emBase for absolute font sizes, not em-relative
                    if (sizePart.find("em") == std::string::npos) g_emBase = sz;
                    if (slash != std::string::npos) {
                        float lh = ParseLineHeightValue(low.substr(slash + 1), sz);
                        if (lh > 0) out.lineHeight = lh;
                    }
                    // rest of string is font family
                    std::string rest = sTrim(val.substr(consumed));
                    if (!rest.empty()) {
                        ComputedStyle tmp; ApplyDeclaration("font-family", rest, tmp);
                        out.fontFamily = tmp.fontFamily;
                    }
                    break;
                }
            }
        }
    } else if (prop == "text-decoration" || prop == "text-decoration-line") {
        std::string v = sLower(val);
        out.underline = (v.find("underline") != std::string::npos);
        out.lineThrough = (v.find("line-through") != std::string::npos);
        // "none" explicitly removes decoration, including the default link underline.
        out.noUnderline = (v.find("none") != std::string::npos);
    } else if (prop == "text-transform") {
        std::string v = sLower(val);
        out.textTransformSet = true;
        if      (v == "uppercase")  out.textTransform = 1;
        else if (v == "lowercase")  out.textTransform = 2;
        else if (v == "capitalize") out.textTransform = 3;
        else                        out.textTransform = 0;
    } else if (prop == "white-space") {
        std::string v = sLower(sTrim(val));
        out.whiteSpaceSet = true;
        out.whiteSpaceNowrap = (v == "nowrap" || v == "pre");
        out.whiteSpacePre = (v == "pre" || v == "pre-wrap" || v == "pre-line");
    } else if (prop == "letter-spacing") {
        std::string v = sLower(sTrim(val));
        if (v != "normal") { float f = ParseLength(val); if (f > -1e5f) { out.letterSpacing = f; out.letterSpacingSet = true; } }
        else { out.letterSpacing = 0; out.letterSpacingSet = true; }
    } else if (prop == "word-break" || prop == "overflow-wrap" || prop == "word-wrap") {
        std::string v = sLower(sTrim(val));
        out.wordBreakSet = true;
        if (v == "break-all") out.wordBreak = 1;
        else if (v == "break-word") out.wordBreak = 2;
        else if (v == "anywhere") out.wordBreak = 2;
        else out.wordBreak = 0;
    } else if (prop == "text-overflow") {
        std::string v = sLower(sTrim(val));
        out.textOverflowSet = true;
        out.textOverflow = (v == "ellipsis") ? 1 : 0;
    } else if (prop == "column-count") {
        std::string v = sLower(sTrim(val));
        if (v != "auto") { try { out.columnCount = std::stoi(v); out.columnCountSet = true; } catch (...) {} }
    } else if (prop == "aspect-ratio") {
        std::string v = sTrim(val);
        if (v != "auto") {
            size_t slash = v.find('/');
            if (slash != std::string::npos) {
                try { float w = std::stof(v.substr(0, slash)), h = std::stof(v.substr(slash + 1));
                    if (h > 0) { out.aspectRatio = w / h; out.aspectRatioSet = true; } } catch (...) {}
            } else {
                try { out.aspectRatio = std::stof(v); out.aspectRatioSet = true; } catch (...) {}
            }
        }
    } else if (prop == "text-shadow") {
        std::string v = sTrim(val);
        if (sLower(v) != "none") {
            std::vector<std::string> toks; { std::string cur; int depth=0;
                for (char c : v) { if (c=='(')++depth; else if(c==')')--depth;
                    if (std::isspace((unsigned char)c)&&depth==0) { if (!cur.empty()){toks.push_back(cur);cur.clear();} }
                    else cur+=c; } if (!cur.empty()) toks.push_back(cur); }
            std::vector<float> nums; CssColor color={true,0,0,0,128};
            for (auto& tok : toks) { float f=ParseLength(tok); if(f>-1e5f){nums.push_back(f);continue;} CssColor c=ParseCssColor(tok); if(c.valid)color=c; }
            if (nums.size()>=2) { out.textShadowX=nums[0]; out.textShadowY=nums[1];
                if(nums.size()>=3) out.textShadowBlur=nums[2]; out.textShadowColor=color; out.textShadowSet=true; }
        }
    } else if (prop == "column-gap") {
        float f = ParseLength(sTrim(val));
        if (f >= 0) out.columnGap = f;
    } else if (prop == "display") {
        std::string v = sLower(sTrim(val));
        if      (v == "none")                                          out.display = 3;
        else if (v == "block")                                         out.display = 1;
        else if (v == "inline")                                        out.display = 2;
        else if (v == "flex" || v == "inline-flex")                     out.display = 4;
        else if (v == "grid" || v == "inline-grid")                     out.display = 11;
        else if (v == "table" || v == "inline-table")                  out.display = 5;
        else if (v == "table-cell")                                    out.display = 6;
        else if (v == "inline-block")                                  out.display = 7;
        else if (v == "list-item")                                     out.display = 8;
        else if (v == "table-row")                                     out.display = 9;
        else if (v == "table-row-group" || v == "table-header-group"
              || v == "table-footer-group")                            out.display = 10;
        else if (v == "table-caption")                                  out.display = 14;
        else if (v == "flow-root")                                     out.display = 12;
        else if (v == "contents")                                      out.display = 13;
    } else if (prop == "flex-direction") {
        out.flexDirectionSet = true;
        out.flexDirection = sLower(sTrim(val)) == "column" ? 1 : 0;
    } else if (prop == "align-items") {
        std::string v = sLower(sTrim(val));
        out.alignItemsSet = true;
        out.alignItems = (v == "flex-start" || v == "start") ? 1
                       : (v == "center") ? 2
                       : (v == "flex-end" || v == "end") ? 3 : 0;  // 0 = stretch
    } else if (prop == "justify-content") {
        std::string v = sLower(sTrim(val));
        out.justifyContentSet = true;
        out.justifyContent = (v == "center") ? 1
                           : (v == "flex-end" || v == "end") ? 2
                           : (v == "space-between") ? 3 : 0;
    } else if (prop == "align-content") {
        std::string v = sLower(sTrim(val));
        out.alignContentSet = true;
        out.alignContent = (v == "flex-start" || v == "start") ? 1
                         : (v == "center") ? 2
                         : (v == "flex-end" || v == "end") ? 3
                         : (v == "space-between") ? 4
                         : (v == "space-around") ? 5 : 0;  // 0 = stretch
    } else if (prop == "flex-grow") {
        try { out.flexGrow = std::max(0.f, std::stof(sTrim(val))); out.flexGrowSet = true; } catch (...) {}
    } else if (prop == "flex-shrink") {
        try { out.flexShrink = std::max(0.f, std::stof(sTrim(val))); out.flexShrinkSet = true; } catch (...) {}
    } else if (prop == "flex-basis") {
        std::string v = sLower(sTrim(val));
        if (v == "auto") { out.flexBasis = -1; out.flexBasisSet = true; }
        else { float f = ParseLength(val); if (f > -1e5f) { out.flexBasis = f; out.flexBasisSet = true; } }
    } else if (prop == "flex-wrap") {
        std::string v = sLower(sTrim(val));
        out.flexWrapSet = true;
        if (v == "wrap") out.flexWrap = 1;
        else if (v == "wrap-reverse") out.flexWrap = 2;
        else out.flexWrap = 0;
    } else if (prop == "align-self") {
        std::string v = sLower(sTrim(val));
        out.alignSelfSet = true;
        if (v == "auto") out.alignSelf = -1;
        else if (v == "flex-start" || v == "start") out.alignSelf = 1;
        else if (v == "center") out.alignSelf = 2;
        else if (v == "flex-end" || v == "end") out.alignSelf = 3;
        else out.alignSelf = 0;  // stretch
    } else if (prop == "flex") {
        // flex shorthand: <grow> [<shrink> [<basis>]]
        std::istringstream values(val);
        std::string g, s, b;
        values >> g;
        std::string gl = sLower(g);
        if (gl == "none") { out.flexGrow = 0; out.flexShrink = 0; out.flexGrowSet = out.flexShrinkSet = true; }
        else if (gl == "auto") { out.flexGrow = 1; out.flexShrink = 1; out.flexBasis = -1; out.flexGrowSet = out.flexShrinkSet = out.flexBasisSet = true; }
        else {
            try { out.flexGrow = std::max(0.f, std::stof(g)); out.flexGrowSet = true; } catch (...) {}
            if (values >> s) {
                try { out.flexShrink = std::max(0.f, std::stof(s)); out.flexShrinkSet = true; } catch (...) {}
            }
            if (values >> b) {
                std::string bl = sLower(b);
                if (bl == "auto") { out.flexBasis = -1; out.flexBasisSet = true; }
                else { float f = ParseLength(b); if (f > -1e5f) { out.flexBasis = f; out.flexBasisSet = true; } }
            }
            // flex: <n> with no basis implies basis=0 per spec
            if (out.flexGrowSet && !out.flexBasisSet) { out.flexBasis = 0; out.flexBasisSet = true; }
        }
    } else if (prop == "flex-flow") {
        // flex-flow: <direction> || <wrap>
        std::istringstream values(val);
        std::string tok;
        while (values >> tok) {
            std::string tl = sLower(tok);
            if (tl == "row" || tl == "column") { out.flexDirection = (tl == "column") ? 1 : 0; out.flexDirectionSet = true; }
            else if (tl == "wrap") { out.flexWrap = 1; out.flexWrapSet = true; }
            else if (tl == "wrap-reverse") { out.flexWrap = 2; out.flexWrapSet = true; }
            else if (tl == "nowrap") { out.flexWrap = 0; out.flexWrapSet = true; }
        }
    } else if (prop == "gap") {
        // Universal gap property - applies to both flex and grid
        std::istringstream values(val);
        std::string gap;
        values >> gap;
        float parsed = ParseLength(gap);
        if (parsed >= 0) {
            out.flexGap = parsed;
            out.gridGap = parsed;
        }
    } else if (prop == "row-gap") {
        float parsed = ParseLength(sTrim(val));
        if (parsed >= 0) {
            out.flexRowGap = parsed;
            out.gridRowGap = parsed;
        }
    } else if (prop == "column-gap") {
        float parsed = ParseLength(sTrim(val));
        if (parsed >= 0) {
            out.flexColumnGap = parsed;
            out.gridColumnGap = parsed;
        }
    } else if (prop == "grid-gap") {
        // Legacy grid-gap property
        float parsed = ParseLength(sTrim(val));
        if (parsed >= 0) out.gridGap = parsed;
    } else if (prop == "grid-row-gap") {
        float parsed = ParseLength(sTrim(val));
        if (parsed >= 0) out.gridRowGap = parsed;
    } else if (prop == "grid-column-gap") {
        float parsed = ParseLength(sTrim(val));
        if (parsed >= 0) out.gridColumnGap = parsed;
    } else if (prop == "grid-template-columns") {
        auto tracks = SplitGridTracks(sTrim(val));
        if (!tracks.empty()) { out.gridTemplateColumns = std::move(tracks); out.gridTemplateColumnsSet = true; }
    } else if (prop == "grid-template-rows") {
        auto tracks = SplitGridTracks(sTrim(val));
        if (!tracks.empty()) { out.gridTemplateRows = std::move(tracks); out.gridTemplateRowsSet = true; }
    } else if (prop == "grid-column") {
        // grid-column: start / end or just start
        std::string v = sTrim(val); size_t slash = v.find('/');
        if (slash != std::string::npos) {
            try { out.gridColumnStart = std::stoi(sTrim(v.substr(0, slash))); } catch (...) {}
            try { out.gridColumnEnd = std::stoi(sTrim(v.substr(slash + 1))); } catch (...) {}
        } else { try { out.gridColumnStart = std::stoi(v); } catch (...) {} }
    } else if (prop == "grid-row") {
        std::string v = sTrim(val); size_t slash = v.find('/');
        if (slash != std::string::npos) {
            try { out.gridRowStart = std::stoi(sTrim(v.substr(0, slash))); } catch (...) {}
            try { out.gridRowEnd = std::stoi(sTrim(v.substr(slash + 1))); } catch (...) {}
        } else { try { out.gridRowStart = std::stoi(v); } catch (...) {} }
    } else if (prop == "grid-column-start") {
        try { out.gridColumnStart = std::stoi(sTrim(val)); } catch (...) {}
    } else if (prop == "grid-column-end") {
        try { out.gridColumnEnd = std::stoi(sTrim(val)); } catch (...) {}
    } else if (prop == "grid-row-start") {
        try { out.gridRowStart = std::stoi(sTrim(val)); } catch (...) {}
    } else if (prop == "grid-row-end") {
        try { out.gridRowEnd = std::stoi(sTrim(val)); } catch (...) {}
    } else if (prop == "grid-auto-flow") {
        std::string v = sLower(sTrim(val));
        out.gridAutoFlowSet = true;
        if (v == "column") out.gridAutoFlow = 1;
        else if (v == "row dense" || v == "dense") out.gridAutoFlow = 2;
        else if (v == "column dense") out.gridAutoFlow = 3;
        else out.gridAutoFlow = 0;  // row (default)
    } else if (prop == "margin") {
        std::istringstream vs(val); std::vector<float> v;
        std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "auto") v.push_back(kCssAuto);
            else if (tl == "inherit" || tl == "initial" || tl == "unset") continue;
            else { float f = ParseLength(tok); if (f > -1e5f) v.push_back(f); }
        }
        if (v.size() == 1) {
            out.marginTop = out.marginRight = out.marginBottom = out.marginLeft = v[0];
        } else if (v.size() == 2) {
            out.marginTop = out.marginBottom = v[0];
            out.marginRight = out.marginLeft = v[1];
        } else if (v.size() == 3) {
            out.marginTop = v[0]; out.marginRight = out.marginLeft = v[1]; out.marginBottom = v[2];
        } else if (v.size() >= 4) {
            out.marginTop = v[0]; out.marginRight = v[1]; out.marginBottom = v[2]; out.marginLeft = v[3];
        }
    } else if (prop == "margin-top") {
        std::string v = sLower(sTrim(val));
        if (v == "auto") out.marginTop = kCssAuto;
        else if (v != "inherit" && v != "initial" && v != "unset") { float f = ParseLength(val); if (f > -1e5f) out.marginTop = f; }
    } else if (prop == "margin-bottom") {
        std::string v = sLower(sTrim(val));
        if (v == "auto") out.marginBottom = kCssAuto;
        else if (v != "inherit" && v != "initial" && v != "unset") { float f = ParseLength(val); if (f > -1e5f) out.marginBottom = f; }
    } else if (prop == "margin-right") {
        std::string v = sLower(sTrim(val));
        if (v == "auto") out.marginRight = kCssAuto;
        else if (v != "inherit" && v != "initial" && v != "unset") { float f = ParseLength(val); if (f > -1e5f) out.marginRight = f; }
    } else if (prop == "margin-left") {
        std::string v = sLower(sTrim(val));
        if (v == "auto") out.marginLeft = kCssAuto;
        else if (v != "inherit" && v != "initial" && v != "unset") { float f = ParseLength(val); if (f > -1e5f) out.marginLeft = f; }

    } else if (prop == "padding") {
        std::istringstream vs(val); std::vector<float> v;
        std::string tok;
        while (vs >> tok) { float f = ParseLength(tok); v.push_back(f < 0 ? 0 : f); }
        if (v.size() == 1) {
            out.paddingTop = out.paddingRight = out.paddingBottom = out.paddingLeft = v[0];
        } else if (v.size() == 2) {
            out.paddingTop = out.paddingBottom = v[0];
            out.paddingRight = out.paddingLeft = v[1];
        } else if (v.size() == 3) {
            out.paddingTop = v[0]; out.paddingRight = out.paddingLeft = v[1]; out.paddingBottom = v[2];
        } else if (v.size() >= 4) {
            out.paddingTop = v[0]; out.paddingRight = v[1]; out.paddingBottom = v[2]; out.paddingLeft = v[3];
        }
    } else if (prop == "padding-top") {
        float f = ParseLength(val); if (f >= 0) out.paddingTop    = f;
    } else if (prop == "padding-right") {
        float f = ParseLength(val); if (f >= 0) out.paddingRight  = f;
    } else if (prop == "padding-bottom") {
        float f = ParseLength(val); if (f >= 0) out.paddingBottom = f;
    } else if (prop == "padding-left") {
        float f = ParseLength(val); if (f >= 0) out.paddingLeft   = f;

    } else if (prop == "border-width") {
        std::istringstream vs(val); std::vector<float> widths; std::string tok;
        while (vs >> tok) { float f = ParseLength(tok); if (f >= 0) widths.push_back(f); }
        if (widths.size() == 1) out.borderWidth = widths[0];
        else if (widths.size() == 2) {
            out.borderTopWidth = out.borderBottomWidth = widths[0];
            out.borderRightWidth = out.borderLeftWidth = widths[1];
        } else if (widths.size() == 3) {
            out.borderTopWidth = widths[0]; out.borderRightWidth = out.borderLeftWidth = widths[1]; out.borderBottomWidth = widths[2];
        } else if (widths.size() >= 4) {
            out.borderTopWidth = widths[0]; out.borderRightWidth = widths[1]; out.borderBottomWidth = widths[2]; out.borderLeftWidth = widths[3];
        }
    } else if (prop == "border-color") {
        std::vector<CssColor> colors;
        for (auto& tok : SplitCssWhitespace(val)) { CssColor c = ParseCssColor(tok); if (c.valid) colors.push_back(c); }
        if (colors.size() == 1) out.borderColor = colors[0];
        else if (colors.size() == 2) {
            out.borderTopColor = out.borderBottomColor = colors[0];
            out.borderRightColor = out.borderLeftColor = colors[1];
        } else if (colors.size() == 3) {
            out.borderTopColor = colors[0]; out.borderRightColor = out.borderLeftColor = colors[1]; out.borderBottomColor = colors[2];
        } else if (colors.size() >= 4) {
            out.borderTopColor = colors[0]; out.borderRightColor = colors[1]; out.borderBottomColor = colors[2]; out.borderLeftColor = colors[3];
        }
    } else if (prop == "border-top-color") {
        CssColor c = ParseCssColor(val); if (c.valid) out.borderTopColor = c;
    } else if (prop == "border-right-color") {
        CssColor c = ParseCssColor(val); if (c.valid) out.borderRightColor = c;
    } else if (prop == "border-bottom-color") {
        CssColor c = ParseCssColor(val); if (c.valid) out.borderBottomColor = c;
    } else if (prop == "border-left-color") {
        CssColor c = ParseCssColor(val); if (c.valid) out.borderLeftColor = c;
    } else if (prop == "border-radius") {
        float f = ParseLength(val); if (f >= 0) out.borderRadius = f;
    } else if (prop == "border") {
        std::string low = sLower(sTrim(val));
        if (low == "none" || low == "0") {
            out.borderWidth = 0;
            out.borderTopWidth = out.borderRightWidth = out.borderBottomWidth = out.borderLeftWidth = 0;
        } else {
            std::istringstream vs(val); std::string tok;
            bool hasWidth = false;
            while (vs >> tok) {
                std::string tl = sLower(tok);
                if (tl == "none") { out.borderWidth = 0; out.borderTopWidth = out.borderRightWidth = out.borderBottomWidth = out.borderLeftWidth = 0; break; }
                if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") {
                    if (!hasWidth) { out.borderWidth = 3; hasWidth = true; }
                    continue;
                }
                float f = ParseLength(tok);
                if (f >= 0) { out.borderWidth = f; hasWidth = true; continue; }
                CssColor c = ParseCssColor(tok);
                if (c.valid) out.borderColor = c;
            }
        }
    } else if (prop == "border-style") {
        // Parse 1-4 values and set sides to 0 if "none"/"hidden", leave them if "solid" etc.
        std::vector<std::string> vals;
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) vals.push_back(sLower(sTrim(tok)));
        while (vals.size() < 4) {
            if (vals.size() == 1)      vals.push_back(vals[0]);
            else if (vals.size() == 2) vals.push_back(vals[0]);
            else                       vals.push_back(vals[1]);
        }
        // 0=top,1=right,2=bottom,3=left
        auto applyStyle = [&](const std::string& s, float& side) {
            if (s == "none" || s == "hidden") side = 0;
            // "solid","dashed" etc — border remains at its current width (from 'border:' shorthand)
        };
        applyStyle(vals[0], out.borderTopWidth);
        applyStyle(vals[1], out.borderRightWidth);
        applyStyle(vals[2], out.borderBottomWidth);
        applyStyle(vals[3], out.borderLeftWidth);
    } else if (prop == "border-top" || prop == "border-top-width") {
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "none") { out.borderTopWidth = 0; break; }
            if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") continue;
            float f = ParseLength(tok); if (f >= 0) { out.borderTopWidth = f; continue; }
            CssColor c = ParseCssColor(tok); if (c.valid) out.borderTopColor = c;
        }
    } else if (prop == "border-right" || prop == "border-right-width") {
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "none") { out.borderRightWidth = 0; break; }
            if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") continue;
            float f = ParseLength(tok); if (f >= 0) { out.borderRightWidth = f; continue; }
            CssColor c = ParseCssColor(tok); if (c.valid) out.borderRightColor = c;
        }
    } else if (prop == "border-bottom" || prop == "border-bottom-width") {
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "none") { out.borderBottomWidth = 0; break; }
            if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") continue;
            float f = ParseLength(tok); if (f >= 0) { out.borderBottomWidth = f; continue; }
            CssColor c = ParseCssColor(tok); if (c.valid) out.borderBottomColor = c;
        }
    } else if (prop == "border-left" || prop == "border-left-width") {
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "none") { out.borderLeftWidth = 0; break; }
            if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") continue;
            float f = ParseLength(tok); if (f >= 0) { out.borderLeftWidth = f; continue; }
            CssColor c = ParseCssColor(tok); if (c.valid) out.borderLeftColor = c;
        }

    } else if (prop == "line-height") {
        float lh = ParseLineHeightValue(val, out.fontSize > 0 ? out.fontSize : 16.f);
        if (lh > 0) out.lineHeight = lh;
    } else if (prop == "box-shadow") {
        std::string v = sTrim(val);
        if (sLower(v) == "none") { out.shadowSet = true; }
        // Parse: [inset] offsetX offsetY [blur [spread]] color
        // Tokenize respecting parens for rgb()/hsl().
        std::vector<std::string> toks;
        { std::string cur; int depth = 0;
          for (char c : v) {
              if (c == '(') ++depth; else if (c == ')' && depth > 0) --depth;
              if (std::isspace((unsigned char)c) && depth == 0) {
                  if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
              } else cur += c;
          }
          if (!cur.empty()) toks.push_back(cur);
        }
        bool inset = false;
        std::vector<float> nums;
        CssColor color = {true, 0, 0, 0, 0.3f};  // default: semi-transparent black
        for (auto& tok : toks) {
            if (sLower(tok) == "inset") { inset = true; continue; }
            float f = ParseLength(tok);
            if (f > -1e5f) { nums.push_back(f); continue; }
            CssColor c = ParseCssColor(tok);
            if (c.valid) color = c;
        }
        if (nums.size() >= 2) {
            out.shadowX = nums[0]; out.shadowY = nums[1];
            if (nums.size() >= 3) out.shadowBlur = nums[2];
            if (nums.size() >= 4) out.shadowSpread = nums[3];
            out.shadowColor = color; out.shadowInset = inset; out.shadowSet = true;
        }
    } else if (prop == "box-sizing") {
        std::string v = sLower(sTrim(val));
        out.boxSizing = (v == "border-box") ? 1 : 0;
        out.boxSizingSet = true;
    } else if (prop == "transform") {
        std::string v = sLower(val);
        if (v.find("none") != std::string::npos) {
            out.transformSet = true;
        } else {
            out.transformSet = true;
            // Parse transform functions: translate(x,y), translateX(x), translateY(y),
            // scale(s), rotate(deg)
            size_t pos = 0;
            while (pos < v.size()) {
                size_t paren = v.find('(', pos);
                if (paren == std::string::npos) break;
                std::string fn = sTrim(v.substr(pos, paren - pos));
                size_t end = v.find(')', paren);
                if (end == std::string::npos) break;
                std::string argStr = v.substr(paren + 1, end - paren - 1);
                // Split transform args by comma or CSS whitespace.
                std::vector<float> fargs;
                std::vector<bool> pctArgs;
                std::replace(argStr.begin(), argStr.end(), ',', ' ');
                std::istringstream as(argStr);
                std::string tok;
                while (as >> tok) {
                    std::string t = sTrim(tok);
                    bool isPct = !t.empty() && t.back() == '%';
                    float f = isPct ? -100000.f : ParseLength(t);
                    if (f <= -1e5f) {
                        // Try deg
                        size_t deg = t.find("deg");
                        if (isPct) {
                            try { f = std::stof(t.substr(0, t.size() - 1)); } catch (...) { f = 0; }
                        } else if (deg != std::string::npos) {
                            try { f = std::stof(t.substr(0, deg)); } catch (...) { f = 0; }
                        } else {
                            try { f = std::stof(t); } catch (...) { f = 0; }
                        }
                    }
                    fargs.push_back(f);
                    pctArgs.push_back(isPct);
                }
                if (fn == "translate" && fargs.size() >= 1) {
                    out.transformTx = fargs[0];
                    out.transformTxPercent = pctArgs.size() >= 1 && pctArgs[0];
                    if (fargs.size() >= 2) {
                        out.transformTy = fargs[1];
                        out.transformTyPercent = pctArgs.size() >= 2 && pctArgs[1];
                    }
                } else if (fn == "translatex") {
                    if (!fargs.empty()) {
                        out.transformTx = fargs[0];
                        out.transformTxPercent = !pctArgs.empty() && pctArgs[0];
                    }
                } else if (fn == "translatey") {
                    if (!fargs.empty()) {
                        out.transformTy = fargs[0];
                        out.transformTyPercent = !pctArgs.empty() && pctArgs[0];
                    }
                } else if (fn == "scale" && !fargs.empty()) {
                    out.transformScale = fargs[0];
                } else if (fn == "rotate" && !fargs.empty()) {
                    out.transformRotate = fargs[0];
                }
                pos = end + 1;
            }
        }
    } else if (prop == "translate") {
        std::string low = sLower(sTrim(val));
        if (low != "none" && !IsIgnoredCssKeyword(low)) {
            auto vals = SplitCssWhitespace(val);
            float tx = 0, ty = 0;
            bool txPct = false, tyPct = false;
            if (!vals.empty() && ParseTransformLengthToken(vals[0], tx, txPct)) {
                if (vals.size() > 1) ParseTransformLengthToken(vals[1], ty, tyPct);
                out.transformSet = true;
                out.transformTx = tx;
                out.transformTy = ty;
                out.transformTxPercent = txPct;
                out.transformTyPercent = tyPct;
            }
        }
    } else if (prop == "scale") {
        std::string low = sLower(sTrim(val));
        if (low != "none" && !IsIgnoredCssKeyword(low)) {
            auto vals = SplitCssWhitespace(val);
            if (!vals.empty()) {
                try {
                    out.transformSet = true;
                    out.transformScale = std::stof(vals[0]);
                } catch (...) {}
            }
        }
    } else if (prop == "rotate") {
        std::string low = sLower(sTrim(val));
        if (low != "none" && !IsIgnoredCssKeyword(low)) {
            float degrees = 0;
            if (ParseAngleToken(val, degrees)) {
                out.transformSet = true;
                out.transformRotate = degrees;
            }
        }
    } else if (prop == "object-fit") {
        std::string v = sLower(sTrim(val));
        out.objectFit = (v == "contain") ? 1 : (v == "cover") ? 2
                      : (v == "none") ? 3 : (v == "scale-down") ? 4 : 0;
    } else if (prop == "width") {
        std::string v = sLower(sTrim(val));
        if (v == "min-content") out.widthKeyword = 1;
        else if (v == "max-content") out.widthKeyword = 2;
        else if (v == "fit-content") out.widthKeyword = 3;
        else {
            float calcPct = -1.f, calcOffset = 0.f;
            float pct = ParsePercentage(val);
            if (ParseCalcPercentOffset(val, calcPct, calcOffset)) {
                out.widthCalcPercent = calcPct;
                out.widthCalcOffset = calcOffset;
                out.width = -1;
                out.widthPercent = -1;
            } else if (pct >= 0) {
                out.widthPercent = pct;
                out.width = -1;
                out.widthCalcPercent = -1;
                out.widthCalcOffset = 0;
            } else {
                float f = ParseLength(val);
                if (f >= 0) {
                    out.width = f;
                    out.widthPercent = -1;
                    out.widthCalcPercent = -1;
                    out.widthCalcOffset = 0;
                }
            }
        }
    } else if (prop == "height") {
        std::string v = sLower(sTrim(val));
        if (v == "min-content") out.heightKeyword = 1;
        else if (v == "max-content") out.heightKeyword = 2;
        else if (v == "fit-content") out.heightKeyword = 3;
        else {
            float calcPct = -1.f, calcOffset = 0.f;
            float pct = ParsePercentage(val);
            if (ParseCalcPercentOffset(val, calcPct, calcOffset)) {
                out.heightCalcPercent = calcPct;
                out.heightCalcOffset = calcOffset;
                out.height = -1;
                out.heightPercent = -1;
            } else if (pct >= 0) {
                out.heightPercent = pct;
                out.height = -1;
                out.heightCalcPercent = -1;
                out.heightCalcOffset = 0;
            } else {
                float f = ParseLength(val);
                if (f >= 0) {
                    out.height = f;
                    out.heightPercent = -1;
                    out.heightCalcPercent = -1;
                    out.heightCalcOffset = 0;
                }
            }
        }
    } else if (prop == "max-width") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.maxWidthPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.maxWidth = f; }
    } else if (prop == "min-width") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.minWidthPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.minWidth = f; }
    } else if (prop == "min-height") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.minHeightPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.minHeight = f; }
    } else if (prop == "max-height") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.maxHeightPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.maxHeight = f; }
    } else if (prop == "content") {
        std::string v = sLower(sTrim(val));
        out.contentSet = v != "none" && v != "normal";
        out.content = out.contentSet ? CssUnescape(stripQuotes(sTrim(val))) : "";
    } else if (prop == "text-align") {
        std::string v = sLower(sTrim(val));
        out.textAlignSet = true;
        if      (v == "center")  out.textAlign = 1;
        else if (v == "right")   out.textAlign = 2;
        else if (v == "justify") out.textAlign = 3;
        else                     out.textAlign = 0;
    } else if (prop == "text-indent") {
        float f = ParseLength(sTrim(val));
        if (f > -1e5f) { out.textIndent = f; out.textIndentSet = true; }
    } else if (prop == "vertical-align") {
        std::string v = sLower(sTrim(val));
        out.verticalAlignSet = true;
        if      (v == "sub")        out.verticalAlign = 1;
        else if (v == "super")      out.verticalAlign = 2;
        else if (v == "middle")     out.verticalAlign = 3;
        else if (v == "top" || v == "text-top")       out.verticalAlign = 4;
        else if (v == "bottom" || v == "text-bottom") out.verticalAlign = 5;
        else                        out.verticalAlign = 0;  // baseline
    } else if (prop == "float") {
        std::string v = sLower(sTrim(val));
        if      (v == "left")    out.floatMode = 1;
        else if (v == "right")   out.floatMode = 2;
        else if (v == "inherit") out.floatInherit = true;
        else                     out.floatMode = 0;
    } else if (prop == "clear") {
        std::string v = sLower(sTrim(val));
        if      (v == "left")  out.clearMode = 1;
        else if (v == "right") out.clearMode = 2;
        else if (v == "both")  out.clearMode = 3;
        else                   out.clearMode = 0;
    } else if (prop == "position") {
        std::string v = sLower(sTrim(val));
        if      (v == "relative") out.positionMode = 1;
        else if (v == "absolute") out.positionMode = 2;
        else if (v == "fixed")    out.positionMode = 3;
        else if (v == "sticky")   out.positionMode = 4;
        else                      out.positionMode = 0;
    } else if (prop == "z-index") {
        std::string v = sLower(sTrim(val));
        if (v != "auto") {
            try {
                out.zIndex = std::stoi(v);
                out.zIndexSet = true;
            } catch (...) {}
        }
    } else if (prop == "overflow" || prop == "overflow-x" || prop == "overflow-y") {
        std::string v = sLower(sTrim(val));
        out.overflowSet = true;
        if (v == "hidden")      { out.overflowHidden = true;  out.overflowMode = 1; }
        else if (v == "auto")   { out.overflowHidden = true;  out.overflowMode = 2; }
        else if (v == "scroll") { out.overflowHidden = true;  out.overflowMode = 3; }
        else                    { out.overflowHidden = false; out.overflowMode = 0; }
    } else if (prop == "top") {
        std::string v = sTrim(val);
        if (sLower(v) != "auto" && sLower(v) != "inherit" && sLower(v) != "initial" && sLower(v) != "unset") {
            out.topSet = true;
            if (!v.empty() && v.back() == '%') {
                try { out.top = std::stof(v.substr(0, v.size()-1)); } catch(...) { out.top = 0; }
                out.topPercent = true;
            } else { out.top = ParseLength(v); out.topPercent = false; }
        }
    } else if (prop == "right") {
        std::string v = sTrim(val);
        if (sLower(v) != "auto" && sLower(v) != "inherit" && sLower(v) != "initial" && sLower(v) != "unset") {
            out.rightSet = true;
            if (!v.empty() && v.back() == '%') {
                try { out.right = std::stof(v.substr(0, v.size()-1)); } catch(...) { out.right = 0; }
                out.rightPercent = true;
            } else { out.right = ParseLength(v); out.rightPercent = false; }
        }
    } else if (prop == "bottom") {
        std::string v = sTrim(val);
        if (sLower(v) != "auto" && sLower(v) != "inherit" && sLower(v) != "initial" && sLower(v) != "unset") {
            out.bottomSet = true;
            if (!v.empty() && v.back() == '%') {
                try { out.bottom = std::stof(v.substr(0, v.size()-1)); } catch(...) { out.bottom = 0; }
                out.bottomPercent = true;
            } else { out.bottom = ParseLength(v); out.bottomPercent = false; }
        }
    } else if (prop == "left") {
        std::string v = sTrim(val);
        if (sLower(v) != "auto" && sLower(v) != "inherit" && sLower(v) != "initial" && sLower(v) != "unset") {
            out.leftSet = true;
            if (!v.empty() && v.back() == '%') {
                try { out.left = std::stof(v.substr(0, v.size()-1)); } catch(...) { out.left = 0; }
                out.leftPercent = true;
            } else { out.left = ParseLength(v); out.leftPercent = false; }
        }
    } else if (prop == "opacity") {
        try { out.opacity = std::stof(sTrim(val)); out.opacitySet = true; } catch (...) {}
    // Properties parsed but not yet rendered — prevents rule dropping.
    } else if (prop == "outline") {
        // Shorthand: [width] [style] [color]
        std::vector<std::string> toks; std::istringstream oss(val); std::string otk;
        while (oss >> otk) toks.push_back(otk);
        out.outlineSet = true; out.outlineWidth = 2;
        for (auto& t : toks) {
            std::string tl = sLower(t);
            if (tl == "none") { out.outlineWidth = 0; }
            else if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset") { /* style */ }
            else { float f = ParseLength(t); if (f >= 0) out.outlineWidth = f; else { CssColor c = ParseCssColor(t); if (c.valid) out.outlineColor = c; } }
        }
    } else if (prop == "outline-width") {
        float f = ParseLength(sTrim(val)); if (f >= 0) { out.outlineWidth = f; out.outlineSet = true; }
    } else if (prop == "outline-color") {
        out.outlineColor = ParseCssColor(sTrim(val)); out.outlineSet = true;
    } else if (prop == "outline-style" || prop == "outline-offset") {
    } else if (prop == "cursor") {
        std::string v = sLower(sTrim(val));
        out.cursorSet = true;
        out.cursor = (v == "pointer") ? 1
                   : (v == "text") ? 2
                   : (v == "move") ? 3
                   : (v == "not-allowed") ? 4
                   : (v == "grab") ? 5
                   : (v == "grabbing") ? 6
                   : (v == "crosshair") ? 7
                   : (v == "help") ? 8 : 0;  // 0 = auto
    } else if (prop == "pointer-events") {
        std::string v = sLower(sTrim(val));
        out.pointerEventsSet = true;
        out.pointerEvents = (v == "none") ? 1
                          : (v == "all") ? 2 : 0;  // 0 = auto
    } else if (prop == "user-select" || prop == "-webkit-user-select" || prop == "-moz-user-select") {
        std::string v = sLower(sTrim(val));
        out.userSelectSet = true;
        out.userSelect = (v == "none") ? 1
                       : (v == "text") ? 2
                       : (v == "all") ? 3 : 0;  // 0 = auto
    } else if (prop == "transition") {
        // Shorthand: property duration timing-function delay
        // Simplified: extract duration (first value with s/ms) and property name.
        std::istringstream tss(val); std::string tok;
        out.transitionProperty = "all";
        while (tss >> tok) {
            std::string tl = sLower(tok);
            if (tl.find('s') != std::string::npos && (std::isdigit((unsigned char)tl[0]) || tl[0] == '.')) {
                float dur = 0;
                try { dur = std::stof(tl); } catch (...) {}
                if (tl.find("ms") != std::string::npos) dur /= 1000.f;
                out.transitionDuration = dur;
            } else if (tl != "ease" && tl != "linear" && tl != "ease-in" && tl != "ease-out" && tl != "ease-in-out" && tl.find("cubic") == std::string::npos) {
                out.transitionProperty = tl;
            }
        }
        out.transitionSet = true;
    } else if (prop == "transition-duration") {
        std::string v = sLower(sTrim(val));
        float dur = 0; try { dur = std::stof(v); } catch (...) {}
        if (v.find("ms") != std::string::npos) dur /= 1000.f;
        out.transitionDuration = dur; out.transitionSet = true;
    } else if (prop == "transition-property") {
        out.transitionProperty = sLower(sTrim(val)); out.transitionSet = true;
    } else if (prop == "transition-timing-function" || prop == "transition-delay") {
    } else if (prop == "animation" || prop == "animation-name" || prop == "animation-duration"
            || prop == "animation-timing-function" || prop == "animation-delay"
            || prop == "animation-iteration-count" || prop == "animation-direction"
            || prop == "animation-fill-mode" || prop == "animation-play-state") {
        // Animation properties — parsed but no visual effect yet.
    } else if (prop == "filter") {
        // Parse filter functions: blur(), brightness(), contrast(), grayscale(), opacity(), saturate()
        std::string v = sLower(val);
        out.filterSet = true;
        // Extract blur
        size_t blurPos = v.find("blur(");
        if (blurPos != std::string::npos) {
            size_t endPos = v.find(")", blurPos);
            if (endPos != std::string::npos) {
                std::string blurVal = v.substr(blurPos + 5, endPos - blurPos - 5);
                out.filterBlur = ParseLength(blurVal);
            }
        }
        // Extract brightness
        size_t brightPos = v.find("brightness(");
        if (brightPos != std::string::npos) {
            size_t endPos = v.find(")", brightPos);
            if (endPos != std::string::npos) {
                try { out.filterBrightness = std::stof(v.substr(brightPos + 11, endPos - brightPos - 11)); } catch (...) {}
            }
        }
        // Extract contrast
        size_t contrastPos = v.find("contrast(");
        if (contrastPos != std::string::npos) {
            size_t endPos = v.find(")", contrastPos);
            if (endPos != std::string::npos) {
                try { out.filterContrast = std::stof(v.substr(contrastPos + 9, endPos - contrastPos - 9)); } catch (...) {}
            }
        }
        // Extract grayscale
        size_t grayPos = v.find("grayscale(");
        if (grayPos != std::string::npos) {
            size_t endPos = v.find(")", grayPos);
            if (endPos != std::string::npos) {
                try { out.filterGrayscale = std::stof(v.substr(grayPos + 10, endPos - grayPos - 10)); } catch (...) {}
            }
        }
        // Extract saturate
        size_t satPos = v.find("saturate(");
        if (satPos != std::string::npos) {
            size_t endPos = v.find(")", satPos);
            if (endPos != std::string::npos) {
                try { out.filterSaturate = std::stof(v.substr(satPos + 9, endPos - satPos - 9)); } catch (...) {}
            }
        }
    } else if (prop == "backdrop-filter") {
        // Simplified: only parse blur for now
        std::string v = sLower(val);
        size_t blurPos = v.find("blur(");
        if (blurPos != std::string::npos) {
            size_t endPos = v.find(")", blurPos);
            if (endPos != std::string::npos) {
                std::string blurVal = v.substr(blurPos + 5, endPos - blurPos - 5);
                out.backdropBlur = ParseLength(blurVal);
                out.backdropFilterSet = true;
            }
        }
    } else if (prop == "line-clamp" || prop == "-webkit-line-clamp") {
        try {
            int lines = std::stoi(sTrim(val));
            if (lines > 0) {
                out.lineClamp = lines;
                out.lineClampSet = true;
            }
        } catch (...) {}
    } else if (prop == "scroll-behavior") {
        std::string v = sLower(sTrim(val));
        out.scrollBehaviorSet = true;
        out.scrollBehavior = (v == "smooth") ? 1 : 0;  // 0 = auto
    } else if (prop == "white-space") {
        std::string v = sLower(sTrim(val));
        out.whiteSpaceModeSet = true;
        if (v == "nowrap") out.whiteSpaceMode = 1;
        else if (v == "pre") out.whiteSpaceMode = 2;
        else if (v == "pre-wrap") out.whiteSpaceMode = 3;
        else if (v == "pre-line") out.whiteSpaceMode = 4;
        else if (v == "break-spaces") out.whiteSpaceMode = 5;
        else out.whiteSpaceMode = 0;  // normal
        // Also set legacy flags for backward compatibility
        out.whiteSpaceSet = true;
        out.whiteSpaceNowrap = (out.whiteSpaceMode == 1);
        out.whiteSpacePre = (out.whiteSpaceMode == 2);
    } else if (prop == "will-change" || prop == "contain" || prop == "isolation"
            || prop == "mix-blend-mode"
            || prop == "clip-path" || prop == "mask" || prop == "mask-image"
            || prop == "appearance" || prop == "-webkit-appearance" || prop == "-moz-appearance"
            || prop == "resize" || prop == "direction" || prop == "unicode-bidi"
            || prop == "writing-mode" || prop == "accent-color" || prop == "caret-color"
            || prop == "overscroll-behavior"
            || prop == "touch-action" || prop == "backface-visibility"
            || prop == "-webkit-font-smoothing" || prop == "-moz-osx-font-smoothing"
            || prop == "-webkit-tap-highlight-color" || prop == "-webkit-text-size-adjust"
            || prop == "text-rendering" || prop == "font-feature-settings"
            || prop == "font-variant" || prop == "font-variant-ligatures"
            || prop == "font-display" || prop == "src"
            || prop == "text-size-adjust" || prop == "tab-size"
            || prop == "columns"
            || prop == "place-items" || prop == "place-content"
            || prop == "grid-area" || prop == "grid-auto-rows"
            || prop == "grid-auto-columns"
            || prop == "order" || prop == "counter-reset" || prop == "counter-increment"
            || prop == "quotes" || prop == "hyphens") {
        // Known properties parsed to prevent rule dropping. No visual effect yet.
    } else if (prop == "visibility") {
        std::string v = sLower(sTrim(val));
        out.visibilitySet = true;
        out.visibilityHidden = (v == "hidden" || v == "collapse");
    } else if (prop == "list-style" || prop == "list-style-type") {
        std::string v = sLower(sTrim(val));
        out.listStyleSet = true;
        out.listStyleNone = (v == "none" || v.find("none") != std::string::npos);
    } else if (prop == "border-spacing") {
        float f = ParseLength(val);
        if (f >= 0) out.borderSpacing = f;
    }
}

static void StoreDeclaration(const std::string& property, const std::string& value,
                             ComputedStyle& style) {
    std::string cleanValue = sTrim(value);
    std::string lowerValue = sLower(cleanValue);
    const size_t important = lowerValue.rfind("!important");
    if (important != std::string::npos) {
        const bool onlyWhitespaceAfter = sTrim(cleanValue.substr(important + 10)).empty();
        if (onlyWhitespaceAfter) cleanValue = sTrim(cleanValue.substr(0, important));
    }
    if (property.rfind("--", 0) == 0) {
        style.customProperties[property] = cleanValue;
    } else if (cleanValue.find("var(") != std::string::npos) {
        style.deferredDeclarations.emplace_back(property, cleanValue);
    } else {
        ApplyDeclaration(property, cleanValue, style);
    }
}

static bool ResolveVarValue(std::string& value,
                            const std::map<std::string, std::string>& properties) {
    for (int depth = 0; depth < 8; ++depth) {
        const size_t start = value.find("var(");
        if (start == std::string::npos) return true;
        const size_t end = value.find(')', start + 4);
        if (end == std::string::npos) return false;
        const std::string args = value.substr(start + 4, end - start - 4);
        const size_t comma = args.find(',');
        const std::string name = sTrim(args.substr(0, comma));
        auto it = properties.find(name);
        std::string replacement;
        if (it != properties.end()) replacement = it->second;
        else if (comma != std::string::npos) replacement = sTrim(args.substr(comma + 1));
        else return false;
        value.replace(start, end - start + 1, replacement);
    }
    return value.find("var(") == std::string::npos;
}

void ResolveStyleVariables(ComputedStyle& style) {
    for (const auto& [property, rawValue] : style.deferredDeclarations) {
        std::string value = rawValue;
        if (ResolveVarValue(value, style.customProperties))
            ApplyDeclaration(property, value, style);
    }
    style.deferredDeclarations.clear();
}

ComputedStyle ParseInlineStyle(const std::string& style) {
    static std::unordered_map<std::string, ComputedStyle> g_inlineStyleCache;
    constexpr size_t kMaxInlineStyleCacheEntries = 512;
    auto cached = g_inlineStyleCache.find(style);
    if (cached != g_inlineStyleCache.end()) return cached->second;
    if (g_inlineStyleCache.size() >= kMaxInlineStyleCacheEntries)
        g_inlineStyleCache.clear();

    ComputedStyle out;
    for (const auto& decl : SplitDeclarations(style)) {
        size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        StoreDeclaration(sLower(sTrim(decl.substr(0, colon))),
                         sTrim(decl.substr(colon+1)), out);
    }
    g_inlineStyleCache.emplace(style, out);
    return out;
}

// ─── selector matching ───────────────────────────────────────────────────────

static const Node* PreviousElementSibling(const Node* node);
static const Node* NextElementSibling(const Node* node);
static CssRule parseSelector(const std::string& sel);
static const CssRule& CachedCssSelector(const std::string& selector);

static std::vector<std::string> SplitCssList(const std::string& text) {
    std::vector<std::string> parts;
    size_t start = 0;
    int parens = 0;
    int brackets = 0;
    char quote = 0;
    bool escaped = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') ++parens;
        else if (c == ')' && parens > 0) --parens;
        else if (c == '[') ++brackets;
        else if (c == ']' && brackets > 0) --brackets;
        else if (c == ',' && parens == 0 && brackets == 0) {
            std::string part = sTrim(text.substr(start, i - start));
            if (!part.empty()) parts.push_back(part);
            start = i + 1;
        }
    }
    std::string part = sTrim(text.substr(start));
    if (!part.empty()) parts.push_back(part);
    return parts;
}

static bool SelectorListMatches(const std::vector<std::string>& selectors, const Node* node) {
    for (const auto& selector : selectors) {
        if (!selector.empty() && CachedCssSelector(selector).matches(node))
            return true;
    }
    return false;
}

static bool DescendantSelectorListMatches(const std::vector<std::string>& selectors, const Node* node) {
    if (!node) return false;
    for (const auto& child : node->children) {
        if (!child || child->type != NodeType::Element) continue;
        if (SelectorListMatches(selectors, child.get())) return true;
        if (DescendantSelectorListMatches(selectors, child.get())) return true;
    }
    return false;
}

static bool RelativeSelectorMatches(const std::string& rawSelector, const Node* node) {
    std::string selector = sTrim(rawSelector);
    if (!node || selector.empty()) return false;

    if (selector[0] == '>') {
        std::string childSelector = sTrim(selector.substr(1));
        for (const auto& child : node->children) {
            if (child && child->type == NodeType::Element
                && SelectorListMatches({ childSelector }, child.get()))
                return true;
        }
        return false;
    }
    if (selector[0] == '+') {
        const Node* next = NextElementSibling(node);
        return next && SelectorListMatches({ sTrim(selector.substr(1)) }, next);
    }
    if (selector[0] == '~') {
        std::string siblingSelector = sTrim(selector.substr(1));
        for (const Node* next = NextElementSibling(node); next; next = NextElementSibling(next)) {
            if (SelectorListMatches({ siblingSelector }, next)) return true;
        }
        return false;
    }

    return DescendantSelectorListMatches({ selector }, node);
}

static bool HasSelectorListMatches(const std::vector<std::string>& selectors, const Node* node) {
    for (const auto& selector : selectors) {
        if (RelativeSelectorMatches(selector, node)) return true;
    }
    return false;
}

static int SelectorListMaxSpecificity(const std::vector<std::string>& selectors) {
    int maxSpecificity = 0;
    for (const auto& selector : selectors) {
        if (!selector.empty())
            maxSpecificity = std::max(maxSpecificity, CachedCssSelector(selector).specificity());
    }
    return maxSpecificity;
}

static size_t FindTopLevelOfKeyword(const std::string& text) {
    int parens = 0;
    int brackets = 0;
    char quote = 0;
    bool escaped = false;
    for (size_t i = 0; i + 3 < text.size(); ++i) {
        const char c = text[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') { ++parens; continue; }
        if (c == ')' && parens > 0) { --parens; continue; }
        if (c == '[') { ++brackets; continue; }
        if (c == ']' && brackets > 0) { --brackets; continue; }
        if (parens == 0 && brackets == 0
            && std::isspace((unsigned char)c)
            && text.compare(i + 1, 2, "of") == 0
            && i + 3 < text.size()
            && std::isspace((unsigned char)text[i + 3])) {
            return i;
        }
    }
    return std::string::npos;
}

static int ElementChildIndexFiltered(const Node* node,
                                     const std::vector<std::string>& filters,
                                     bool fromEnd) {
    if (!node || !node->parent) return 0;
    std::vector<const Node*> matches;
    for (const auto& child : node->parent->children) {
        if (!child || child->type != NodeType::Element) continue;
        if (filters.empty() || SelectorListMatches(filters, child.get()))
            matches.push_back(child.get());
    }
    for (size_t i = 0; i < matches.size(); ++i) {
        if (matches[i] == node)
            return fromEnd ? (int)(matches.size() - i) : (int)i + 1;
    }
    return 0;
}

static int ElementChildIndex(const Node* node) {
    if (!node || !node->parent) return 0;
    int idx = 0;
    for (const auto& c : node->parent->children) {
        if (c->type == NodeType::Element) ++idx;
        if (c.get() == node) return idx;
    }
    return 0;
}

static int ElementChildCount(const Node* node) {
    if (!node || !node->parent) return 0;
    int count = 0;
    for (const auto& c : node->parent->children)
        if (c->type == NodeType::Element) ++count;
    return count;
}

static int ElementTypeIndex(const Node* node) {
    if (!node || !node->parent) return 0;
    int idx = 0;
    for (const auto& c : node->parent->children) {
        if (c->type == NodeType::Element && c->tagName == node->tagName) ++idx;
        if (c.get() == node) return idx;
    }
    return 0;
}

static int ElementTypeCount(const Node* node) {
    if (!node || !node->parent) return 0;
    int count = 0;
    for (const auto& c : node->parent->children)
        if (c->type == NodeType::Element && c->tagName == node->tagName) ++count;
    return count;
}

static bool IsFirstOfType(const Node* node) {
    if (!node || !node->parent) return false;
    for (const auto& c : node->parent->children) {
        if (c->type == NodeType::Element && c->tagName == node->tagName)
            return c.get() == node;
    }
    return false;
}

static bool IsLastOfType(const Node* node) {
    if (!node || !node->parent) return false;
    const Node* last = nullptr;
    for (const auto& c : node->parent->children)
        if (c->type == NodeType::Element && c->tagName == node->tagName) last = c.get();
    return last == node;
}

// Match An+B expression against a 1-based index.
static bool MatchesNth(int index, int a, int b) {
    if (a == 0) return index == b;
    int diff = index - b;
    if (a > 0) return diff >= 0 && diff % a == 0;
    return diff <= 0 && (-diff) % (-a) == 0;
}

// Parse "odd", "even", "3", "2n+1", "-n+3", etc. Returns (a,b).
static std::pair<int,int> ParseNthExpr(const std::string& expr) {
    std::string s; for (char c : expr) if (!std::isspace((unsigned char)c)) s += c;
    if (s == "odd") return {2, 1};
    if (s == "even") return {2, 0};
    size_t npos = s.find('n');
    if (npos == std::string::npos) {
        try { return {0, std::stoi(s)}; } catch (...) { return {0, 0}; }
    }
    int a = 1;
    if (npos > 0) {
        std::string as = s.substr(0, npos);
        if (as == "-") a = -1; else if (as == "+") a = 1;
        else { try { a = std::stoi(as); } catch (...) {} }
    }
    int b = 0;
    if (npos + 1 < s.size()) {
        try { b = std::stoi(s.substr(npos + 1)); } catch (...) {}
    }
    return {a, b};
}

static bool MatchesPseudoClass(const std::string& pseudo, const Node* node) {
    if (pseudo == "first-child") return PreviousElementSibling(node) == nullptr;
    if (pseudo == "last-child") return NextElementSibling(node) == nullptr;
    if (pseudo == "only-child") return PreviousElementSibling(node) == nullptr
                                      && NextElementSibling(node) == nullptr;
    if (pseudo == "empty") return node && node->children.empty();
    if (pseudo == "link") return node && node->tagName == "a"
                              && node->attrs.find("href") != node->attrs.end();
    if (pseudo == "root") return node && node->parent
                              && node->parent->type == NodeType::Document;
    if (pseudo == "first-of-type") return IsFirstOfType(node);
    if (pseudo == "last-of-type") return IsLastOfType(node);
    // :nth-child(An+B) — the argument is stored in the pseudo string itself.
    if (pseudo.rfind("nth-child(", 0) == 0) {
        std::string arg = pseudo.substr(10, pseudo.size() - 11);
        std::vector<std::string> filters;
        size_t ofPos = FindTopLevelOfKeyword(arg);
        if (ofPos != std::string::npos) {
            filters = SplitCssList(arg.substr(ofPos + 4));
            arg = sTrim(arg.substr(0, ofPos));
        }
        auto [a, b] = ParseNthExpr(arg);
        int index = filters.empty() ? ElementChildIndex(node)
                                    : ElementChildIndexFiltered(node, filters, false);
        return MatchesNth(index, a, b);
    }
    if (pseudo.rfind("nth-last-child(", 0) == 0) {
        std::string arg = pseudo.substr(15, pseudo.size() - 16);
        std::vector<std::string> filters;
        size_t ofPos = FindTopLevelOfKeyword(arg);
        if (ofPos != std::string::npos) {
            filters = SplitCssList(arg.substr(ofPos + 4));
            arg = sTrim(arg.substr(0, ofPos));
        }
        auto [a, b] = ParseNthExpr(arg);
        int fromEnd = filters.empty() ? ElementChildCount(node) - ElementChildIndex(node) + 1
                                      : ElementChildIndexFiltered(node, filters, true);
        return MatchesNth(fromEnd, a, b);
    }
    if (pseudo.rfind("nth-of-type(", 0) == 0) {
        std::string arg = pseudo.substr(12, pseudo.size() - 13);
        auto [a, b] = ParseNthExpr(arg);
        return MatchesNth(ElementTypeIndex(node), a, b);
    }
    if (pseudo.rfind("nth-last-of-type(", 0) == 0) {
        std::string arg = pseudo.substr(17, pseudo.size() - 18);
        auto [a, b] = ParseNthExpr(arg);
        int fromEnd = ElementTypeCount(node) - ElementTypeIndex(node) + 1;
        return MatchesNth(fromEnd, a, b);
    }
    // :hover — check if the node or any ancestor is the hovered element.
    if (pseudo == "hover") {
        if (!g_hoverNodeCss || !node) return false;
        const Node* cur = g_hoverNodeCss;
        while (cur) {
            if (cur == node) return true;
            cur = cur->parent;
        }
        return false;
    }
    // Form/document state pseudos with the state Vertex currently tracks.
    if (pseudo == "focus") return node && node == g_focusNodeCss;
    if (pseudo == "checked") {
        if (!node) return false;
        return node->attrs.find("checked") != node->attrs.end()
            || node->attrs.find("selected") != node->attrs.end();
    }
    if (pseudo == "disabled") return node && node->attrs.find("disabled") != node->attrs.end();
    if (pseudo == "enabled") {
        if (!node || node->attrs.find("disabled") != node->attrs.end()) return false;
        const std::string& t = node->tagName;
        return t == "button" || t == "input" || t == "select" || t == "textarea"
            || t == "option" || t == "optgroup" || t == "fieldset";
    }
    if (pseudo == "active" || pseudo == "visited") return false;
    return false;
}

static bool MatchesSimpleSelector(const CssSelectorPart& part, const Node* node) {
    if (!node || node->type != NodeType::Element) return false;
    if (part.neverMatch) return false;
    const std::string generated = node->attr("_vertex_pseudo");
    if (!part.pseudoElement.empty()) {
        if (generated != part.pseudoElement) return false;
    } else if (!generated.empty()) {
        return false;
    }
    if (!part.tag.empty() && node->tagName != part.tag) return false;
    if (!part.id.empty() && node->attr("id") != part.id) return false;
    if (!part.attrName.empty()) {
        std::string value = node->attr(part.attrName);
        if (value.empty() && node->attrs.find(part.attrName) == node->attrs.end()) return false;
        if (part.attrHasValue) {
            bool matches = false;
            switch (part.attrMatch) {
                case CssAttrMatch::Exact:
                    matches = (value == part.attrValue);
                    break;
                case CssAttrMatch::Includes: {
                    std::istringstream ss(value);
                    std::string token;
                    while (ss >> token) {
                        if (token == part.attrValue) {
                            matches = true;
                            break;
                        }
                    }
                    break;
                }
                case CssAttrMatch::DashPrefix:
                    matches = (value == part.attrValue)
                           || (value.size() > part.attrValue.size()
                            && value.compare(0, part.attrValue.size(), part.attrValue) == 0
                            && value[part.attrValue.size()] == '-');
                    break;
                case CssAttrMatch::Prefix:
                    matches = value.size() >= part.attrValue.size()
                           && value.compare(0, part.attrValue.size(), part.attrValue) == 0;
                    break;
                case CssAttrMatch::Suffix:
                    matches = value.size() >= part.attrValue.size()
                           && value.compare(value.size() - part.attrValue.size(), part.attrValue.size(), part.attrValue) == 0;
                    break;
                case CssAttrMatch::Substring:
                    matches = value.find(part.attrValue) != std::string::npos;
                    break;
                case CssAttrMatch::Exists:
                    matches = true;
                    break;
            }
            if (!matches) return false;
        }
    }
    for (const auto& required : part.classes) {
        auto ca = node->attr("class");
        bool found = false;
        std::istringstream ss(ca);
        std::string tok;
        while (ss >> tok) if (tok == required) { found = true; break; }
        if (!found) return false;
    }
    for (const auto& pseudo : part.pseudos) {
        if (!MatchesPseudoClass(pseudo, node)) return false;
    }
    for (const auto& selectorList : part.matchSelectorLists) {
        if (!SelectorListMatches(selectorList, node)) return false;
    }
    for (const auto& selectorList : part.hasSelectorLists) {
        if (!HasSelectorListMatches(selectorList, node)) return false;
    }
    // :not() — the node must NOT match the simple selector argument.
    for (const auto& notArg : part.notSelectors) {
        if (notArg.empty()) continue;
        if (notArg[0] == '.') {
            std::string cls = notArg.substr(1);
            std::string ca = node->attr("class");
            std::istringstream ss(ca); std::string tok;
            while (ss >> tok) if (tok == cls) return false;
        } else if (notArg[0] == '#') {
            if (node->attr("id") == notArg.substr(1)) return false;
        } else if (notArg[0] == '[') {
            // Simplified :not([attr]) — just check attribute existence.
            std::string attr = notArg.substr(1, notArg.size() - 2);
            if (!node->attr(attr).empty()) return false;
        } else {
            if (node->tagName == notArg) return false;
        }
    }
    return true;
}

static const Node* PreviousElementSibling(const Node* node) {
    if (!node || !node->parent) return nullptr;
    const Node* previous = nullptr;
    for (const auto& child : node->parent->children) {
        if (child.get() == node) return previous;
        if (child->type == NodeType::Element) previous = child.get();
    }
    return nullptr;
}

static const Node* NextElementSibling(const Node* node) {
    if (!node || !node->parent) return nullptr;
    bool seenCurrent = false;
    for (const auto& child : node->parent->children) {
        if (child.get() == node) {
            seenCurrent = true;
            continue;
        }
        if (seenCurrent && child->type == NodeType::Element) return child.get();
    }
    return nullptr;
}

int CssRule::specificity() const {
    if (selector.empty()) {
        return (!id.empty() ? 100 : 0)
             + (!cls.empty() ?  10 : 0)
             + (!tag.empty() ?   1 : 0);
    }

    int total = 0;
    for (const auto& part : selector) {
        total += (!part.id.empty() ? 100 : 0)
               + ((int)part.classes.size() * 10)
               + (!part.attrName.empty() ? 10 : 0)
               + (!part.tag.empty() ? 1 : 0)
               + part.functionalSpecificity;
    }
    return total;
}

bool CssRule::matches(const Node* node) const {
    if (!node || node->type != NodeType::Element) return false;
    if (!selector.empty()) {
        int i = (int)selector.size() - 1;
        const Node* current = node;
        if (!MatchesSimpleSelector(selector[i], current)) return false;

        for (; i > 0; --i) {
            char combinator = selector[i].combinator;
            const CssSelectorPart& wanted = selector[i - 1];

            if (combinator == '>') {
                current = current->parent;
                if (!MatchesSimpleSelector(wanted, current)) return false;
            } else if (combinator == '+') {
                current = PreviousElementSibling(current);
                if (!MatchesSimpleSelector(wanted, current)) return false;
            } else if (combinator == '~') {
                // General sibling: any preceding sibling element that matches.
                bool found = false;
                if (current && current->parent) {
                    for (const auto& c : current->parent->children) {
                        if (c.get() == current) break;
                        if (c->type == NodeType::Element && MatchesSimpleSelector(wanted, c.get())) {
                            current = c.get(); found = true;
                        }
                    }
                }
                if (!found) return false;
            } else {
                const Node* ancestor = current->parent;
                bool found = false;
                while (ancestor) {
                    if (MatchesSimpleSelector(wanted, ancestor)) {
                        current = ancestor;
                        found = true;
                        break;
                    }
                    ancestor = ancestor->parent;
                }
                if (!found) return false;
            }
        }
        return true;
    }

    if (!tag.empty() && node->tagName != tag) return false;
    if (!id.empty()  && node->attr("id") != id) return false;
    if (!cls.empty()) {
        // class attr is space-separated list
        auto ca = node->attr("class");
        bool found = false;
        std::istringstream ss(ca);
        std::string tok;
        while (ss >> tok) if (tok == cls) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

// ─── cascade resolution ──────────────────────────────────────────────────────

void Stylesheet::rebuildRuleBuckets() {
    idRuleBuckets.clear();
    classRuleBuckets.clear();
    tagRuleBuckets.clear();
    universalRuleBucket.clear();
    candidateRuleIndexCache.clear();

    for (size_t i = 0; i < rules.size(); ++i) {
        const CssRule& rule = rules[i];
        if (!rule.id.empty())
            idRuleBuckets[rule.id].push_back(i);
        else if (!rule.cls.empty())
            classRuleBuckets[rule.cls].push_back(i);
        else if (!rule.tag.empty())
            tagRuleBuckets[rule.tag].push_back(i);
        else
            universalRuleBucket.push_back(i);
    }
}

static void AppendCandidateIndices(std::vector<size_t>& out, const std::vector<size_t>* indices) {
    if (!indices) return;
    out.insert(out.end(), indices->begin(), indices->end());
}

static std::vector<std::string> ElementClassTokens(const Node* node) {
    std::vector<std::string> tokens;
    if (!node) return tokens;
    std::istringstream ss(node->attr("class"));
    std::string token;
    while (ss >> token)
        tokens.push_back(token);
    return tokens;
}

static std::string CandidateCacheKey(const Node* node) {
    if (!node || node->type != NodeType::Element) return "|doc||";
    std::vector<std::string> classes = ElementClassTokens(node);
    std::sort(classes.begin(), classes.end());
    classes.erase(std::unique(classes.begin(), classes.end()), classes.end());

    std::string key = node->tagName;
    key += "|";
    key += node->attr("id");
    key += "|";
    for (const auto& cls : classes) {
        key += cls;
        key += ".";
    }
    return key;
}

static const std::vector<size_t>& candidateRuleIndicesFor(const Stylesheet& sheet, const Node* node) {
    const std::string key = CandidateCacheKey(node);
    auto cached = sheet.candidateRuleIndexCache.find(key);
    if (cached != sheet.candidateRuleIndexCache.end())
        return cached->second;

    std::vector<size_t> indices;
    AppendCandidateIndices(indices, &sheet.universalRuleBucket);
    if (node && node->type == NodeType::Element) {
        const std::string id = node->attr("id");
        if (!id.empty()) {
            auto it = sheet.idRuleBuckets.find(id);
            if (it != sheet.idRuleBuckets.end())
                AppendCandidateIndices(indices, &it->second);
        }
        for (const auto& cls : ElementClassTokens(node)) {
            auto it = sheet.classRuleBuckets.find(cls);
            if (it != sheet.classRuleBuckets.end())
                AppendCandidateIndices(indices, &it->second);
        }
        auto tagIt = sheet.tagRuleBuckets.find(node->tagName);
        if (tagIt != sheet.tagRuleBuckets.end())
            AppendCandidateIndices(indices, &tagIt->second);
    }

    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    auto inserted = sheet.candidateRuleIndexCache.emplace(key, std::move(indices));
    return inserted.first->second;
}

static std::vector<const CssRule*> candidateRulesFor(const Stylesheet& sheet, const Node* node) {
    const std::vector<size_t>& indices = candidateRuleIndicesFor(sheet, node);
    std::vector<const CssRule*> candidates;
    candidates.reserve(indices.size());
    for (size_t idx : indices) {
        if (idx < sheet.rules.size())
            candidates.push_back(&sheet.rules[idx]);
    }
    return candidates;
}

ComputedStyle Stylesheet::resolve(const Node* node) const {
    g_remBase = rootRemBase;
    // Collect matching rules, sorted by specificity
    std::vector<const CssRule*> matched;
    for (auto* rule : candidateRulesFor(*this, node)) {
        const CssRule& r = *rule;
        const bool mediaMatches = r.media.empty() || std::any_of(r.media.begin(), r.media.end(),
            [&](const CssMediaCondition& condition) {
                return condition.matches(viewportWidth, viewportHeight);
            });
        if (mediaMatches && r.matches(node)) matched.push_back(rule);
    }
    std::stable_sort(matched.begin(), matched.end(),
        [](const CssRule* a, const CssRule* b) {
            return a->specificity() < b->specificity();
        });

    ComputedStyle out;
    for (auto* r : matched) out = out.inherit(r->style);

    // Inline style wins over everything
    auto inl = node->attr("style");
    if (!inl.empty()) out = out.inherit(ParseInlineStyle(inl));
    return out;
}

// ─── CSS text parser ─────────────────────────────────────────────────────────

// Strip /* ... */ comments
static std::string stripComments(const std::string& css) {
    std::string out; out.reserve(css.size());
    size_t i = 0;
    while (i < css.size()) {
        if (i+1 < css.size() && css[i]=='/' && css[i+1]=='*') {
            i += 2;
            while (i+1 < css.size() && !(css[i]=='*' && css[i+1]=='/')) i++;
            i += 2;
        } else {
            out += css[i++];
        }
    }
    return out;
}

// Parse one simple selector like "div", ".foo", "#bar", "div.foo", "a#id"
static CssSelectorPart parseSimpleSelectorPart(const std::string& sel) {
    CssSelectorPart part;
    std::string cur;
    char mode = 't'; // t=tag, c=class, i=id
    auto flush = [&]() {
        if (cur.empty()) return;
        std::string ident = CssUnescape(cur);
        if (mode == 't') part.tag = sLower(ident);
        if (mode == 'c') part.classes.push_back(ident);
        if (mode == 'i') part.id  = ident;
        cur.clear();
    };
    for (size_t i = 0; i < sel.size(); ++i) {
        char c = sel[i];
        if (c == '.') { flush(); mode = 'c'; }
        else if (c == '#') { flush(); mode = 'i'; }
        else if (c == '[') {
            flush();
            size_t end = sel.find(']', i + 1);
            if (end == std::string::npos) break;
            std::string body = sTrim(sel.substr(i + 1, end - i - 1));
            size_t opPos = std::string::npos;
            std::string op;
            for (const std::string candidate : { "~=", "|=", "^=", "$=", "*=", "=" }) {
                opPos = body.find(candidate);
                if (opPos != std::string::npos) {
                    op = candidate;
                    break;
                }
            }
            if (opPos == std::string::npos) {
                part.attrName = sLower(CssUnescape(sTrim(body)));
                part.attrHasValue = false;
                part.attrMatch = CssAttrMatch::Exists;
            } else {
                part.attrName = sLower(CssUnescape(sTrim(body.substr(0, opPos))));
                part.attrValue = CssUnescape(stripQuotes(body.substr(opPos + op.size())));
                part.attrHasValue = true;
                if (op == "~=") part.attrMatch = CssAttrMatch::Includes;
                else if (op == "|=") part.attrMatch = CssAttrMatch::DashPrefix;
                else if (op == "^=") part.attrMatch = CssAttrMatch::Prefix;
                else if (op == "$=") part.attrMatch = CssAttrMatch::Suffix;
                else if (op == "*=") part.attrMatch = CssAttrMatch::Substring;
                else part.attrMatch = CssAttrMatch::Exact;
            }
            i = end;
        }
        else if (c == ':') {
            flush();
            bool isDoubleColon = i + 1 < sel.size() && sel[i + 1] == ':';
            if (isDoubleColon) ++i;

            size_t nameStart = i + 1;
            size_t j = nameStart;
            while (j < sel.size()
                && (std::isalnum((unsigned char)sel[j]) || sel[j] == '-' || sel[j] == '_')) {
                ++j;
            }

            std::string pseudo = sLower(CssUnescape(sel.substr(nameStart, j - nameStart)));
            if (j < sel.size() && sel[j] == '(') {
                int depth = 1;
                ++j;
                bool escapedArg = false;
                char quoteArg = 0;
                while (j < sel.size() && depth > 0) {
                    char pc = sel[j];
                    if (escapedArg) {
                        escapedArg = false;
                    } else if (pc == '\\') {
                        escapedArg = true;
                    } else if (quoteArg) {
                        if (pc == quoteArg) quoteArg = 0;
                    } else if (pc == '"' || pc == '\'') {
                        quoteArg = pc;
                    } else if (pc == '(') {
                        ++depth;
                    } else if (pc == ')') {
                        --depth;
                    }
                    ++j;
                }
                // Functional pseudo-classes: :nth-child(...), :not(...), etc.
                // Extract the argument text (between the parens).
                size_t argStart = sel.find('(', nameStart) + 1;
                std::string argText = sTrim(sel.substr(argStart, j - 1 - argStart));
                if (pseudo == "nth-child" || pseudo == "nth-last-child"
                    || pseudo == "nth-of-type" || pseudo == "nth-last-of-type") {
                    // Store as "nth-child(2n+1)" so MatchesPseudoClass can parse it.
                    part.pseudos.push_back(pseudo + "(" + sLower(argText) + ")");
                } else if (pseudo == "not") {
                    // :not(selector) — parse the argument as a simple selector and
                    // store it as a negation pseudo. We support simple cases:
                    // :not(.class), :not(tag), :not(#id), :not([attr]).
                    auto selectorList = SplitCssList(argText);
                    if (selectorList.empty()) {
                        part.neverMatch = true;
                    } else {
                        for (auto& selector : selectorList)
                            part.notSelectors.push_back(sLower(selector));
                        part.functionalSpecificity += SelectorListMaxSpecificity(selectorList);
                    }
                } else if (pseudo == "is" || pseudo == "where") {
                    auto selectorList = SplitCssList(argText);
                    if (selectorList.empty()) {
                        part.neverMatch = true;
                    } else {
                        part.matchSelectorLists.push_back(selectorList);
                        if (pseudo == "is")
                            part.functionalSpecificity += SelectorListMaxSpecificity(selectorList);
                    }
                } else if (pseudo == "has") {
                    auto selectorList = SplitCssList(argText);
                    if (selectorList.empty()) {
                        part.neverMatch = true;
                    } else {
                        part.hasSelectorLists.push_back(selectorList);
                        part.functionalSpecificity += SelectorListMaxSpecificity(selectorList);
                    }
                } else {
                    part.neverMatch = true;
                }
                i = j - 1;
            } else {
                if (pseudo == "before" || pseudo == "after") {
                    part.pseudoElement = pseudo;
                } else if (!isDoubleColon && (pseudo == "first-child" || pseudo == "last-child"
                    || pseudo == "only-child" || pseudo == "empty"
                    || pseudo == "link" || pseudo == "root"
                    || pseudo == "first-of-type" || pseudo == "last-of-type"
                    || pseudo == "hover" || pseudo == "focus" || pseudo == "active"
                    || pseudo == "visited" || pseudo == "checked"
                    || pseudo == "disabled" || pseudo == "enabled")) {
                    part.pseudos.push_back(pseudo);
                } else {
                    part.neverMatch = true;
                }
                i = j - 1;
            }
        }
        else cur += c;
    }
    flush();
    // "*" or empty tag = universal
    if (part.tag == "*") part.tag.clear();
    return part;
}

static std::vector<CssSelectorPart> parseSelectorChain(std::string selector) {
    std::vector<CssSelectorPart> parts;
    std::string tok;
    char nextCombinator = 0;
    int bracketDepth = 0;
    int parenDepth = 0;
    char quote = 0;
    bool escaped = false;

    auto flushToken = [&]() {
        if (tok.empty()) return;
        CssSelectorPart part = parseSimpleSelectorPart(tok);
        part.combinator = parts.empty() ? 0 : (nextCombinator ? nextCombinator : ' ');
        parts.push_back(part);
        tok.clear();
        nextCombinator = 0;
    };

    for (char c : selector) {
        if (escaped) {
            tok += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            tok += c;
            escaped = true;
            continue;
        }
        if (quote) {
            tok += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            tok += c;
            quote = c;
            continue;
        }
        if (c == '[') {
            ++bracketDepth;
            tok += c;
            continue;
        }
        if (c == ']') {
            if (bracketDepth > 0) --bracketDepth;
            tok += c;
            continue;
        }
        if (c == '(') {
            ++parenDepth;
            tok += c;
            continue;
        }
        if (c == ')') {
            if (parenDepth > 0) --parenDepth;
            tok += c;
            continue;
        }
        if (bracketDepth == 0 && parenDepth == 0 && (c == '>' || c == '+' || c == '~')) {
            flushToken();
            nextCombinator = c;
            continue;
        }
        if (bracketDepth == 0 && parenDepth == 0 && std::isspace((unsigned char)c)) {
            flushToken();
            if (!parts.empty() && nextCombinator == 0) nextCombinator = ' ';
            continue;
        }
        tok += c;
    }
    flushToken();
    return parts;
}

static CssRule parseSelector(const std::string& sel) {
    CssRule rule;
    rule.selector = parseSelectorChain(sel);
    if (!rule.selector.empty()) {
        const auto& last = rule.selector.back();
        rule.tag = last.tag;
        rule.cls = !last.classes.empty() ? last.classes[0] : "";
        rule.id = last.id;
    }
    return rule;
}

static const CssRule& CachedCssSelector(const std::string& selector) {
    static std::unordered_map<std::string, CssRule> cache;
    constexpr size_t kMaxCssSelectorCacheEntries = 512;
    auto it = cache.find(selector);
    if (it != cache.end()) return it->second;
    if (cache.size() >= kMaxCssSelectorCacheEntries) cache.clear();
    auto inserted = cache.emplace(selector, parseSelector(selector));
    return inserted.first->second;
}

static size_t FindMatchingCssBrace(const std::string& css, size_t lbrace) {
    int depth = 0;
    char quote = 0;
    bool escaped = false;
    for (size_t i = lbrace; i < css.size(); ++i) {
        const char c = css[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '\'' || c == '"') { quote = c; continue; }
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i;
    }
    return std::string::npos;
}

static std::vector<std::string> SplitMediaList(const std::string& prelude) {
    std::vector<std::string> parts;
    size_t start = 0;
    int parens = 0;
    for (size_t i = 0; i < prelude.size(); ++i) {
        if (prelude[i] == '(') ++parens;
        else if (prelude[i] == ')' && parens > 0) --parens;
        else if (prelude[i] == ',' && parens == 0) {
            parts.push_back(sTrim(prelude.substr(start, i - start)));
            start = i + 1;
        }
    }
    parts.push_back(sTrim(prelude.substr(start)));
    return parts;
}

static bool ParseMediaLength(const std::string& raw, float& out) {
    const float value = ParseLength(raw, 16.f);
    if (value < 0.f) return false;
    out = value;
    return true;
}

static bool ApplyMediaRange(CssMediaCondition& condition,
                            const std::string& name,
                            const std::string& op,
                            float length,
                            bool featureOnLeft) {
    const float epsilon = 0.001f;
    auto applyMin = [&](float value) {
        condition.minWidth = name == "width" ? std::max(condition.minWidth, value) : condition.minWidth;
        condition.minHeight = name == "height" ? std::max(condition.minHeight, value) : condition.minHeight;
    };
    auto applyMax = [&](float value) {
        if (name == "width")
            condition.maxWidth = condition.maxWidth < 0.f ? value : std::min(condition.maxWidth, value);
        else if (name == "height")
            condition.maxHeight = condition.maxHeight < 0.f ? value : std::min(condition.maxHeight, value);
    };

    if (name != "width" && name != "height") return false;
    if (featureOnLeft) {
        if (op == "<=") { applyMax(length); return true; }
        if (op == "<")  { applyMax(length - epsilon); return true; }
        if (op == ">=") { applyMin(length); return true; }
        if (op == ">")  { applyMin(length + epsilon); return true; }
    } else {
        if (op == "<=") { applyMin(length); return true; }
        if (op == "<")  { applyMin(length + epsilon); return true; }
        if (op == ">=") { applyMax(length); return true; }
        if (op == ">")  { applyMax(length - epsilon); return true; }
    }
    return false;
}

static bool ParseMediaRangeFeature(const std::string& feature,
                                   CssMediaCondition& condition) {
    static const char* ops[] = { "<=", ">=", "<", ">" };
    for (const char* rawOp : ops) {
        const std::string op(rawOp);
        size_t pos = feature.find(op);
        if (pos == std::string::npos) continue;

        std::string left = sTrim(feature.substr(0, pos));
        std::string right = sTrim(feature.substr(pos + op.size()));
        if (left.empty() || right.empty()) return false;

        if (left == "width" || left == "height") {
            float length = 0.f;
            return ParseMediaLength(right, length)
                && ApplyMediaRange(condition, left, op, length, true);
        }
        if (right == "width" || right == "height") {
            float length = 0.f;
            return ParseMediaLength(left, length)
                && ApplyMediaRange(condition, right, op, length, false);
        }
        return false;
    }
    return false;
}

static std::vector<CssMediaCondition> ParseMediaConditions(const std::string& prelude) {
    std::vector<CssMediaCondition> conditions;
    for (const auto& part : SplitMediaList(prelude)) {
        CssMediaCondition condition;
        const std::string lower = sLower(part);
        if (lower.empty() || lower.find("not ") != std::string::npos
            || lower.find("print") != std::string::npos || lower.find("speech") != std::string::npos) {
            condition.supported = false;
        }

        size_t pos = 0;
        while (pos < lower.size()) {
            const size_t open = lower.find('(', pos);
            if (open == std::string::npos) break;
            const size_t close = lower.find(')', open + 1);
            if (close == std::string::npos) { condition.supported = false; break; }
            const std::string feature = sTrim(lower.substr(open + 1, close - open - 1));
            const size_t colon = feature.find(':');
            if (colon == std::string::npos) {
                if (!ParseMediaRangeFeature(feature, condition))
                    condition.supported = false;
            } else {
                const std::string name = sTrim(feature.substr(0, colon));
                float length = 0.f;
                if (!ParseMediaLength(sTrim(feature.substr(colon + 1)), length)) {
                    condition.supported = false;
                } else if (name == "min-width") {
                    condition.minWidth = std::max(condition.minWidth, length);
                } else if (name == "max-width") {
                    condition.maxWidth = condition.maxWidth < 0.f ? length : std::min(condition.maxWidth, length);
                } else if (name == "min-height") {
                    condition.minHeight = std::max(condition.minHeight, length);
                } else if (name == "max-height") {
                    condition.maxHeight = condition.maxHeight < 0.f ? length : std::min(condition.maxHeight, length);
                } else {
                    condition.supported = false;
                }
            }
            pos = close + 1;
        }
        conditions.push_back(condition);
    }
    return conditions;
}

static CssMediaCondition MergeMediaCondition(const CssMediaCondition& a,
                                              const CssMediaCondition& b) {
    CssMediaCondition merged;
    merged.minWidth = std::max(a.minWidth, b.minWidth);
    merged.minHeight = std::max(a.minHeight, b.minHeight);
    merged.maxWidth = a.maxWidth < 0.f ? b.maxWidth
        : (b.maxWidth < 0.f ? a.maxWidth : std::min(a.maxWidth, b.maxWidth));
    merged.maxHeight = a.maxHeight < 0.f ? b.maxHeight
        : (b.maxHeight < 0.f ? a.maxHeight : std::min(a.maxHeight, b.maxHeight));
    merged.supported = a.supported && b.supported
        && (merged.maxWidth < 0.f || merged.minWidth <= merged.maxWidth)
        && (merged.maxHeight < 0.f || merged.minHeight <= merged.maxHeight);
    return merged;
}

static std::vector<CssMediaCondition> CombineMediaConditions(
    const std::vector<CssMediaCondition>& outer,
    const std::vector<CssMediaCondition>& inner) {
    if (outer.empty()) return inner;
    if (inner.empty()) return outer;
    std::vector<CssMediaCondition> combined;
    for (const auto& a : outer)
        for (const auto& b : inner)
            combined.push_back(MergeMediaCondition(a, b));
    return combined;
}

static bool IsKnownDisplayValue(const std::string& value) {
    return value == "none" || value == "block" || value == "inline"
        || value == "flex" || value == "inline-flex"
        || value == "grid" || value == "inline-grid"
        || value == "table" || value == "inline-table"
        || value == "table-cell" || value == "inline-block"
        || value == "list-item" || value == "table-row"
        || value == "table-row-group" || value == "table-header-group"
        || value == "table-footer-group" || value == "table-caption"
        || value == "flow-root"
        || value == "contents";
}

static bool IsKnownPositionValue(const std::string& value) {
    return value == "static" || value == "relative" || value == "absolute"
        || value == "fixed" || value == "sticky";
}

static bool SupportsPropertyValue(const std::string& feature) {
    const size_t colon = feature.find(':');
    if (colon == std::string::npos) return false;
    const std::string property = sLower(sTrim(feature.substr(0, colon)));
    const std::string value = sLower(sTrim(feature.substr(colon + 1)));
    if (property.empty() || value.empty()) return false;

    if (property == "display") return IsKnownDisplayValue(value);
    if (property == "position") return IsKnownPositionValue(value);
    if (property == "width" || property == "height" || property == "min-width"
        || property == "max-width" || property == "min-height" || property == "max-height"
        || property == "margin" || property == "margin-left" || property == "margin-right"
        || property == "margin-top" || property == "margin-bottom" || property == "padding"
        || property == "padding-left" || property == "padding-right" || property == "padding-top"
        || property == "padding-bottom" || property == "top" || property == "right"
        || property == "bottom" || property == "left" || property == "gap") {
        return ParseLength(value) > -1e5f || value == "auto" || value == "max-content";
    }
    if (property == "color" || property == "background-color" || property == "border-color")
        return ParseCssColor(value).valid || value == "transparent" || value == "currentcolor";
    return false;
}

static bool SupportsSelectorFeature(const std::string& feature) {
    std::string value = sTrim(feature);
    if (value.rfind("selector(", 0) != 0 || value.back() != ')') return false;
    std::string selectorText = sTrim(value.substr(9, value.size() - 10));
    if (selectorText.empty()) return false;
    for (const auto& selector : SplitCssList(selectorText)) {
        const CssRule& rule = CachedCssSelector(selector);
        if (rule.selector.empty()) return false;
        for (const auto& part : rule.selector) {
            if (part.neverMatch) return false;
        }
    }
    return true;
}

static bool StripOuterParens(std::string& value) {
    value = sTrim(value);
    if (value.size() < 2 || value.front() != '(' || value.back() != ')') return false;
    int depth = 0;
    char quote = 0;
    bool escaped = false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') ++depth;
        else if (c == ')' && --depth == 0 && i + 1 != value.size()) return false;
    }
    value = sTrim(value.substr(1, value.size() - 2));
    return true;
}

static std::vector<std::string> SplitTopLevelKeyword(const std::string& value,
                                                     const std::string& keyword) {
    std::vector<std::string> parts;
    int parens = 0;
    char quote = 0;
    bool escaped = false;
    size_t start = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') { ++parens; continue; }
        if (c == ')' && parens > 0) { --parens; continue; }
        if (parens != 0 || i + keyword.size() > value.size()) continue;
        if (value.compare(i, keyword.size(), keyword) != 0) continue;
        const bool leftBoundary = i == 0 || std::isspace((unsigned char)value[i - 1]);
        const bool rightBoundary = i + keyword.size() >= value.size()
            || std::isspace((unsigned char)value[i + keyword.size()]);
        if (!leftBoundary || !rightBoundary) continue;
        parts.push_back(sTrim(value.substr(start, i - start)));
        start = i + keyword.size();
    }
    if (!parts.empty())
        parts.push_back(sTrim(value.substr(start)));
    return parts;
}

static bool SupportsConditionMatches(std::string raw) {
    std::string value = sLower(sTrim(raw));
    while (StripOuterParens(value)) {}
    if (value.rfind("not ", 0) == 0)
        return !SupportsConditionMatches(value.substr(4));

    auto orParts = SplitTopLevelKeyword(value, "or");
    if (!orParts.empty()) {
        return std::any_of(orParts.begin(), orParts.end(),
            [](const std::string& part) { return SupportsConditionMatches(part); });
    }

    auto andParts = SplitTopLevelKeyword(value, "and");
    if (!andParts.empty()) {
        return std::all_of(andParts.begin(), andParts.end(),
            [](const std::string& part) { return SupportsConditionMatches(part); });
    }

    while (StripOuterParens(value)) {}
    if (value.rfind("selector(", 0) == 0)
        return SupportsSelectorFeature(value);
    return SupportsPropertyValue(value);
}

Stylesheet ParseStylesheet(const std::string& rawCss) {
    const bool topLevelParse = (g_parseDepth++ == 0);
    if (topLevelParse) {
        g_emBase = 16.f;
        g_remBase = 16.f;
    }
    Stylesheet sheet;
    std::string css = stripComments(rawCss);

    size_t pos = 0;
    while (pos < css.size()) {
        // Skip whitespace
        while (pos < css.size() && std::isspace((unsigned char)css[pos])) pos++;
        if (pos >= css.size()) break;

        // Skip @-rules — they either end with ; or contain nested {} blocks
        if (css[pos] == '@') {
            size_t semiPos = css.find(';', pos);
            size_t lbPos   = css.find('{', pos);
            if (semiPos != std::string::npos
                && (lbPos == std::string::npos || semiPos < lbPos)) {
                // Statement @-rule (@charset, @import, @namespace)
                pos = semiPos + 1;
            } else if (lbPos != std::string::npos) {
                const size_t rbPos = FindMatchingCssBrace(css, lbPos);
                if (rbPos == std::string::npos) break;
                const std::string header = sLower(sTrim(css.substr(pos, lbPos - pos)));
                if (header.rfind("@media", 0) == 0) {
                    const auto conditions = ParseMediaConditions(header.substr(6));
                    const float outerEmBase = g_emBase;
                    const float outerRemBase = g_remBase;
                    Stylesheet nested = ParseStylesheet(css.substr(lbPos + 1, rbPos - lbPos - 1));
                    g_emBase = outerEmBase;
                    g_remBase = outerRemBase;
                    if (nested.rootRemBaseSet) {
                        sheet.rootRemBase = nested.rootRemBase;
                        sheet.rootRemBaseSet = true;
                    }
                    for (auto& rule : nested.rules) {
                        rule.media = CombineMediaConditions(conditions, rule.media);
                        sheet.rules.push_back(std::move(rule));
                    }
                } else if (header.rfind("@supports", 0) == 0) {
                    if (SupportsConditionMatches(header.substr(9))) {
                        const float outerEmBase = g_emBase;
                        const float outerRemBase = g_remBase;
                        Stylesheet nested = ParseStylesheet(css.substr(lbPos + 1, rbPos - lbPos - 1));
                        g_emBase = outerEmBase;
                        g_remBase = outerRemBase;
                        if (nested.rootRemBaseSet) {
                            sheet.rootRemBase = nested.rootRemBase;
                            sheet.rootRemBaseSet = true;
                        }
                        for (auto& rule : nested.rules)
                            sheet.rules.push_back(std::move(rule));
                    }
                } else if (header.rfind("@font-face", 0) == 0) {
                    // Parse @font-face { font-family: X; src: url(...); }
                    std::string body = css.substr(lbPos + 1, rbPos - lbPos - 1);
                    std::string family, srcUrl;
                    // Extract font-family and src url.
                    ComputedStyle ffs;
                    std::istringstream fss(body); std::string fdecl;
                    while (std::getline(fss, fdecl, ';')) {
                        size_t colon = fdecl.find(':');
                        if (colon == std::string::npos) continue;
                        std::string prop = sLower(sTrim(fdecl.substr(0, colon)));
                        std::string val = sTrim(fdecl.substr(colon + 1));
                        if (prop == "font-family") {
                            family = val;
                            // Strip quotes
                            while (!family.empty() && (family.front()=='"'||family.front()=='\'')) family.erase(family.begin());
                            while (!family.empty() && (family.back()=='"'||family.back()=='\'')) family.pop_back();
                        } else if (prop == "src") {
                            // Find url() in src
                            size_t u = val.find("url(");
                            if (u != std::string::npos) {
                                size_t e = val.find(')', u + 4);
                                if (e != std::string::npos) {
                                    srcUrl = sTrim(val.substr(u + 4, e - u - 4));
                                    while (!srcUrl.empty() && (srcUrl.front()=='"'||srcUrl.front()=='\'')) srcUrl.erase(srcUrl.begin());
                                    while (!srcUrl.empty() && (srcUrl.back()=='"'||srcUrl.back()=='\'')) srcUrl.pop_back();
                                }
                            }
                        }
                    }
                    if (!family.empty() && !srcUrl.empty()) {
                        sheet.fontFaces.push_back({family, srcUrl});
                    }
                }
                pos = rbPos + 1;
            } else {
                break;
            }
            continue;
        }

        // Find next '{'
        size_t lbrace = css.find('{', pos);
        if (lbrace == std::string::npos) break;

        // If another '@' appears before this '{', restart loop to handle it
        size_t atPos = css.find('@', pos);
        if (atPos != std::string::npos && atPos < lbrace) {
            pos = atPos;
            continue;
        }

        // Find the matching closing brace, skipping CSS escapes like \}
        size_t rbrace = std::string::npos;
        {
            bool esc = false;
            char inQuote = 0;
            for (size_t j = lbrace + 1; j < css.size(); ++j) {
                char c = css[j];
                if (esc) { esc = false; continue; }
                if (c == '\\') { esc = true; continue; }
                if (inQuote) { if (c == inQuote) inQuote = 0; continue; }
                if (c == '"' || c == '\'') { inQuote = c; continue; }
                if (c == '}') { rbrace = j; break; }
            }
        }
        if (rbrace == std::string::npos) break;

        std::string selectorBlock = sTrim(css.substr(pos, lbrace - pos));
        std::string declBlock     = css.substr(lbrace+1, rbrace - lbrace - 1);
        pos = rbrace + 1;

        if (selectorBlock.empty()) continue;

        // CSS declaration order must not decide the unit basis of other
        // properties.  In particular, `margin: 100em; font: 2em ...` uses the
        // element's computed font size, not the font that happened to be
        // current when the margin was parsed.  Resolve font declarations from
        // the inherited/root basis first, then resolve all remaining lengths
        // against that element-local basis.
        const float inheritedEmBase = g_emBase;
        const float inheritedRemBase = g_remBase;
        const bool rootSelector = (selectorBlock == "html" || selectorBlock == ":root");
        const auto declarations = SplitDeclarations(declBlock);
        ComputedStyle declStyle;
        g_emBase = inheritedEmBase;
        for (const auto& decl : declarations) {
            size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            const std::string property = sLower(sTrim(decl.substr(0, colon)));
            if (property == "font" || property == "font-size"
                || property == "font-family" || property == "font-weight"
                || property == "font-style") {
                StoreDeclaration(property, sTrim(decl.substr(colon+1)), declStyle);
            }
        }
        const float elementEmBase = declStyle.fontSize > 0 ? declStyle.fontSize : inheritedEmBase;
        const bool rootSetsFontSize = rootSelector && declStyle.fontSize > 0;
        if (rootSetsFontSize) g_remBase = elementEmBase;
        g_emBase = elementEmBase;
        for (const auto& decl : declarations) {
            size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            const std::string property = sLower(sTrim(decl.substr(0, colon)));
            if (property == "font" || property == "font-size"
                || property == "font-family" || property == "font-weight"
                || property == "font-style") continue;
            StoreDeclaration(property, sTrim(decl.substr(colon+1)), declStyle);
        }

        // Each top-level comma-separated selector becomes its own rule. Commas
        // inside :is()/:where() arguments are part of the selector.
        for (std::string selPart : SplitCssList(selectorBlock)) {
            selPart = sTrim(selPart);
            if (selPart.empty()) continue;

            CssRule rule = parseSelector(selPart);
            rule.style = declStyle;
            sheet.rules.push_back(rule);
        }

        // `html` provides the root inherited font size.  Other rules are
        // independent selectors and must not leak their local font basis.
        if (rootSelector) {
            g_emBase = elementEmBase;
            if (rootSetsFontSize) {
                g_remBase = elementEmBase;
                sheet.rootRemBase = elementEmBase;
                sheet.rootRemBaseSet = true;
            }
        } else {
            g_emBase = inheritedEmBase;
            g_remBase = inheritedRemBase;
        }
    }
    sheet.rebuildRuleBuckets();
    g_parseDepth--;
    return sheet;
}

static std::string BoolText(bool value) {
    return value ? "true" : "false";
}

static void AppendColor(std::ostringstream& out, const char* name, const CssColor& color) {
    if (!color.valid) return;
    out << name << "=" << color.r << "," << color.g << "," << color.b << "," << color.a << " ";
}

std::string SerializeComputedStyle(const ComputedStyle& style) {
    std::ostringstream out;
    AppendColor(out, "color", style.color);
    AppendColor(out, "bg", style.bgColor);
    if (style.fontSize > 0) out << "fontSize=" << style.fontSize << " ";
    if (style.boldSet) out << "bold=" << BoolText(style.bold) << " ";
    if (style.italicSet) out << "italic=" << BoolText(style.italic) << " ";
    if (style.underline) out << "underline=true ";
    if (style.display == 3) out << "display=none ";
    else if (style.display == 1) out << "display=block ";
    else if (style.display == 2) out << "display=inline ";
    else if (style.display == 4) out << "display=flex ";
    else if (style.display == 5) out << "display=table ";
    else if (style.display == 6) out << "display=table-cell ";
    else if (style.display == 11) out << "display=grid ";
    else if (style.display == 12) out << "display=flow-root ";
    else if (style.display == 13) out << "display=contents ";
    if (style.marginTopSet()) out << "marginTop="    << style.marginTop    << " ";
    if (style.marginRightSet()) out << "marginRight="  << style.marginRight  << " ";
    if (style.marginBottomSet()) out << "marginBottom=" << style.marginBottom << " ";
    if (style.marginLeftSet()) out << "marginLeft="   << style.marginLeft   << " ";
    if (style.paddingTop   >= 0) out << "paddingTop="   << style.paddingTop   << " ";
    if (style.paddingRight >= 0) out << "paddingRight=" << style.paddingRight << " ";
    if (style.paddingBottom>= 0) out << "paddingBottom="<< style.paddingBottom<< " ";
    if (style.paddingLeft  >= 0) out << "paddingLeft="  << style.paddingLeft  << " ";
    if (style.borderWidth  >= 0) out << "borderWidth="  << style.borderWidth  << " ";
    if (style.borderTopWidth >= 0) out << "borderTopWidth=" << style.borderTopWidth << " ";
    if (style.borderRightWidth >= 0) out << "borderRightWidth=" << style.borderRightWidth << " ";
    if (style.borderBottomWidth >= 0) out << "borderBottomWidth=" << style.borderBottomWidth << " ";
    if (style.borderLeftWidth >= 0) out << "borderLeftWidth=" << style.borderLeftWidth << " ";
    if (style.borderColor.valid) AppendColor(out, "borderColor", style.borderColor);
    AppendColor(out, "borderTopColor", style.borderTopColor);
    AppendColor(out, "borderRightColor", style.borderRightColor);
    AppendColor(out, "borderBottomColor", style.borderBottomColor);
    AppendColor(out, "borderLeftColor", style.borderLeftColor);
    if (style.borderRadius > 0)  out << "borderRadius=" << style.borderRadius << " ";
    if (style.lineHeight   > 0)  out << "lineHeight="   << style.lineHeight   << " ";
    if (style.textAlignSet)      out << "textAlign="    << style.textAlign    << " ";
    if (style.width        >= 0) out << "width="        << style.width        << " ";
    if (style.widthPercent >= 0) out << "widthPercent=" << style.widthPercent << " ";
    if (style.widthCalcPercent >= 0) out << "widthCalc=" << style.widthCalcPercent << "%+" << style.widthCalcOffset << " ";
    if (style.height       >= 0) out << "height="       << style.height       << " ";
    if (style.heightCalcPercent >= 0) out << "heightCalc=" << style.heightCalcPercent << "%+" << style.heightCalcOffset << " ";
    if (style.maxWidth     >= 0) out << "maxWidth="     << style.maxWidth     << " ";
    if (style.minWidth     >= 0) out << "minWidth="     << style.minWidth     << " ";
    if (style.minHeight    >= 0) out << "minHeight="    << style.minHeight    << " ";
    if (style.maxHeight    >= 0) out << "maxHeight="    << style.maxHeight    << " ";
    if (style.contentSet) out << "content=" << style.content << " ";
    if (!style.backgroundImage.empty()) out << "backgroundImage=" << style.backgroundImage << " ";
    if (style.floatMode == 1)    out << "float=left ";
    if (style.floatMode == 2)    out << "float=right ";
    if (style.clearMode == 1)    out << "clear=left ";
    if (style.clearMode == 2)    out << "clear=right ";
    if (style.clearMode == 3)    out << "clear=both ";
    if (style.positionMode == 1) out << "position=relative ";
    if (style.positionMode == 2) out << "position=absolute ";
    if (style.positionMode == 3) out << "position=fixed ";
    if (style.positionMode == 4) out << "position=sticky ";
    if (style.zIndexSet) out << "zIndex=" << style.zIndex << " ";
    if (style.overflowHidden)    out << "overflow=hidden ";
    if (style.topSet)    out << "top="    << style.top    << " ";
    if (style.rightSet)  out << "right="  << style.right  << " ";
    if (style.bottomSet) out << "bottom=" << style.bottom << " ";
    if (style.leftSet)   out << "left="   << style.left   << " ";
    out << "\n";
    return out.str();
}

std::string SerializeStylesheet(const Stylesheet& sheet) {
    std::ostringstream out;
    for (const auto& rule : sheet.rules) {
        out << "rule";
        if (!rule.tag.empty()) out << " tag=" << rule.tag;
        if (!rule.cls.empty()) out << " class=" << rule.cls;
        if (!rule.id.empty()) out << " id=" << rule.id;
        out << " specificity=" << rule.specificity() << " ";
        out << SerializeComputedStyle(rule.style);
    }
    return out.str();
}
