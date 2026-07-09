#include "fixture.h"

#include "css/stylesheet.h"
#include "html/parser.h"
#include "platform/browser_core.h"

#include <cstdio>
#include <sstream>

static const Node* FindFirstElement(const Node* node, const std::string& tag) {
    if (!node) return nullptr;
    if (node->type == NodeType::Element && node->tagName == tag) return node;
    for (const auto& child : node->children) {
        if (auto* found = FindFirstElement(child.get(), tag)) return found;
    }
    return nullptr;
}

static const Node* FindElementById(const Node* node, const std::string& id) {
    if (!node) return nullptr;
    if (node->type == NodeType::Element && node->attr("id") == id) return node;
    for (const auto& child : node->children) {
        if (auto* found = FindElementById(child.get(), id)) return found;
    }
    return nullptr;
}

static const Node* FindElementByClass(const Node* node, const std::string& cls) {
    if (!node) return nullptr;
    if (node->type == NodeType::Element) {
        std::istringstream ss(node->attr("class"));
        std::string token;
        while (ss >> token) {
            if (token == cls) return node;
        }
    }
    for (const auto& child : node->children) {
        if (auto* found = FindElementByClass(child.get(), cls)) return found;
    }
    return nullptr;
}

TestResult RunCssTests() {
    TestResult result;
    auto root = FindRepoRoot();

    {
        auto input = ReadTextFile(root / "tests/fixtures/css/rules/basic.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/rules/basic.expected.txt");
        auto actual = SerializeStylesheet(ParseStylesheet(input));
        ExpectEqual("css/rules/basic", actual, expected, result);
    }

    {
        const std::string html = HomePageHtml();
        const size_t styleStart = html.find("<style>");
        const size_t styleEnd = html.find("</style>");
        const std::string css = (styleStart != std::string::npos && styleEnd != std::string::npos)
            ? html.substr(styleStart + 7, styleEnd - styleStart - 7)
            : "";
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        const Node* wrap = FindElementByClass(dom.get(), "w");
        const Node* mark = FindElementByClass(dom.get(), "mark");
        const Node* links = FindElementByClass(dom.get(), "links");
        const Node* body = FindFirstElement(dom.get(), "body");
        const Node* firstLink = FindFirstElement(links, "a");
        ComputedStyle bodyStyle = body ? sheet.resolve(body) : ComputedStyle{};
        ComputedStyle wrapStyle = wrap ? sheet.resolve(wrap) : ComputedStyle{};
        ComputedStyle markStyle = mark ? sheet.resolve(mark) : ComputedStyle{};
        ComputedStyle linkStyle = firstLink ? sheet.resolve(firstLink) : ComputedStyle{};
        std::string actual =
            std::string("rules=") + std::to_string(sheet.rules.size())
            + " bodyBg=" + (bodyStyle.bgColorSet
                ? std::to_string((int)(bodyStyle.bgColor.r * 255.f + 0.5f)) + ","
                    + std::to_string((int)(bodyStyle.bgColor.g * 255.f + 0.5f)) + ","
                    + std::to_string((int)(bodyStyle.bgColor.b * 255.f + 0.5f))
                : "none")
            + " wrap=" + (wrapStyle.width >= 0 ? std::to_string((int)wrapStyle.width) : "auto")
            + " markDisplay=" + std::to_string((int)markStyle.display)
            + " markBg=" + (markStyle.bgColorSet ? "yes" : "no")
            + " linkDisplay=" + std::to_string((int)linkStyle.display)
            + " linkBorder=" + (linkStyle.borderWidth > 0 ? "yes" : "no")
            + "\n";
        ExpectEqual("css/homepage-stylesheet-applies",
            actual,
            "rules=18 bodyBg=242,232,214 wrap=720 markDisplay=1 markBg=yes linkDisplay=1 linkBorder=yes\n",
            result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/basic.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/basic.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/basic.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        auto* p = FindFirstElement(dom.get(), "p");
        std::string actual = p ? SerializeComputedStyle(sheet.resolve(p)) : "missing p\n";
        ExpectEqual("css/cascade/basic", actual, expected, result);
    }

    {
        auto dom = ParseHtml("<html><body><p id=\"target\"></p></body></html>");
        auto sheet = ParseStylesheet(
            "#target { --accent: red !important; color: var(--accent) !important; margin-left: 4px !important; }");
        auto* node = FindElementById(dom.get(), "target");
        ComputedStyle style = node ? sheet.resolve(node) : ComputedStyle{};
        ResolveStyleVariables(style);
        std::string actual = node ? SerializeComputedStyle(style) : "missing\n";
        ExpectEqual("css/cascade/important-marker-does-not-break-values",
            actual,
            "color=1,0,0,1 marginLeft=4 \n",
            result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/ancestor-combinators.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/ancestor-combinators.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/ancestor-combinators.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        std::string actual;
        for (const std::string id : { "article-desc", "section-desc", "section-child", "loose" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/ancestor-combinators", actual, expected, result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/attribute-selectors.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/attribute-selectors.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/attribute-selectors.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        std::string actual;
        for (const std::string id : {
            "ready", "busy", "div-ready", "missing",
            "token-hit", "token-miss",
            "dash-hit", "dash-exact", "dash-miss",
            "prefix-hit", "prefix-miss",
            "suffix-hit", "suffix-miss",
            "substring-hit", "substring-miss",
            "escaped-class"
        }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/attribute-selectors", actual, expected, result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/adjacent-sibling.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/adjacent-sibling.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/adjacent-sibling.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        std::string actual;
        for (const std::string id : { "lead", "later", "nested" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/adjacent-sibling", actual, expected, result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/pseudo-classes.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/pseudo-classes.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/pseudo-classes.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        std::string actual;
        for (const std::string id : {
            "first", "middle", "last", "only",
            "empty", "not-empty",
            "link", "not-link", "hover-target"
        }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/pseudo-classes", actual, expected, result);
    }

    {
        auto dom = ParseHtml(
            "<html><body>"
            "<input id=\"checked\" checked>"
            "<input id=\"disabled\" disabled>"
            "<input id=\"enabled\">"
            "<div id=\"plain\"></div>"
            "</body></html>");
        auto sheet = ParseStylesheet(
            "input:checked { color: red; }"
            "input:disabled { padding-left: 2px; }"
            "input:enabled { padding-right: 3px; }"
            "input:focus { margin-left: 4px; }"
            "div:enabled { margin-right: 9px; }");
        SetCssFocusNode(FindElementById(dom.get(), "enabled"));
        std::string actual;
        for (const std::string id : { "checked", "disabled", "enabled", "plain" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        SetCssFocusNode(nullptr);
        ExpectEqual("css/cascade/form-state-pseudo-classes",
            actual,
            "checked: color=1,0,0,1 paddingRight=3 \n"
            "disabled: paddingLeft=2 \n"
            "enabled: marginLeft=4 paddingRight=3 \n"
            "plain: \n",
            result);
    }

    {
        auto dom = ParseHtml(
            "<html><body>"
            "<p id=\"is-hit\" class=\"target primary\"></p>"
            "<p id=\"where-hit\" class=\"target secondary\"></p>"
            "<p id=\"miss\" class=\"target\"></p>"
            "<section id=\"ancestor\"><span id=\"desc\" class=\"child\"></span></section>"
            "</body></html>");
        auto sheet = ParseStylesheet(
            ".target:is(.primary, .secondary) { color: red; }"
            ".target:where(.secondary) { margin-left: 4px; color: green; }"
            ".target { color: blue; }"
            "#ancestor :is(.child, a) { padding-left: 3px; }"
            ".target:is(.absent) { padding-right: 9px; }");
        std::string actual;
        for (const std::string id : { "is-hit", "where-hit", "miss", "desc" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/is-where-selectors",
            actual,
            "is-hit: color=1,0,0,1 \n"
            "where-hit: color=1,0,0,1 marginLeft=4 \n"
            "miss: color=0,0,1,1 \n"
            "desc: paddingLeft=3 \n",
            result);
    }

    {
        auto dom = ParseHtml(
            "<html><body>"
            "<article id=\"a1\"></article><p id=\"p1\"></p><article id=\"a2\"></article>"
            "<p id=\"p2\"></p><article id=\"a3\"></article>"
            "</body></html>");
        auto sheet = ParseStylesheet(
            "article:nth-of-type(2n+1) { color: red; }"
            "p:nth-last-of-type(1) { margin-left: 7px; }");
        std::string actual;
        for (const std::string id : { "a1", "a2", "a3", "p1", "p2" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/nth-of-type-selectors",
            actual,
            "a1: color=1,0,0,1 \n"
            "a2: \n"
            "a3: color=1,0,0,1 \n"
            "p1: \n"
            "p2: marginLeft=7 \n",
            result);
    }

    {
        auto dom = ParseHtml(
            "<html><body>"
            "<section id=\"portal\" class=\"portal-card\"><a class=\"mw-link\" href=\"/wiki/Main_Page\">Main</a></section>"
            "<section id=\"plain\" class=\"portal-card\"><span>Plain</span></section>"
            "<ul>"
            "<li id=\"one\" class=\"entry featured\">One</li>"
            "<li id=\"two\" class=\"entry hidden\">Two</li>"
            "<li id=\"three\" class=\"entry\">Three</li>"
            "<li id=\"four\" class=\"other\">Four</li>"
            "</ul>"
            "</body></html>");
        auto sheet = ParseStylesheet(
            "section:has(a[href^=\"/wiki/\"]) { margin-left: 11px; }"
            "li.entry:not(.hidden, .featured) { padding-left: 13px; }"
            "li:nth-child(2n+1 of .entry) { margin-top: 17px; }");
        std::string actual;
        for (const std::string id : { "portal", "plain", "one", "two", "three", "four" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/relational-and-filtered-selectors",
            actual,
            "portal: marginLeft=11 \n"
            "plain: \n"
            "one: marginTop=17 \n"
            "two: \n"
            "three: marginTop=17 paddingLeft=13 \n"
            "four: \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"target\"></div></body></html>");
        auto* rootNode = FindFirstElement(dom.get(), "html");
        auto* target = FindElementById(dom.get(), "target");
        auto sheet = ParseStylesheet(
            ":root { --fallback-gap: 9px; }"
            "#target { padding-left: var(--missing, var(--fallback-gap, 3px));"
            "          color: var(--missing-color, rgb(1, 2, 3)); }");
        auto rootStyle = rootNode ? sheet.resolve(rootNode) : ComputedStyle{};
        ResolveStyleVariables(rootStyle);
        auto targetStyle = target ? rootStyle.inherit(sheet.resolve(target)) : ComputedStyle{};
        ResolveStyleVariables(targetStyle);
        ExpectEqual("css/custom-properties/nested-var-fallbacks",
            SerializeComputedStyle(targetStyle),
            "color=0.00392157,0.00784314,0.0117647,1 paddingLeft=9 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"target\"></div></body></html>");
        auto* target = FindElementById(dom.get(), "target");
        auto sheet = ParseStylesheet(
            "#target { color: red; }"
            "@supports (display: grid) { #target { color: blue; margin-left: 5px; } }"
            "@supports not (display: grid) { #target { padding-left: 9px; } }"
            "@supports (display: made-up) { #target { padding-right: 11px; } }"
            "@supports selector(:has(a[href])) { #target { margin-top: 7px; } }"
            "@supports not selector(:has(a[href])) { #target { margin-bottom: 13px; } }");
        std::string actual = target ? SerializeComputedStyle(sheet.resolve(target)) : "missing\n";
        ExpectEqual("css/supports/display-feature-queries",
            actual,
            "color=0,0,1,1 marginTop=7 marginLeft=5 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body class=\"category\"><div id=\"content-main\"></div><a id=\"skip\"></a></body></html>");
        auto sheet = ParseStylesheet(
            "body.category #content-main { float: right; width: 60%; overflow: hidden; }"
            "a#skip { display: block; position: absolute; top: 0; left: 0; width: 100%; }");
        std::string actual;
        for (const std::string id : { "content-main", "skip" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/wasp-layout-primitives",
            actual,
            "content-main: widthPercent=60 float=right overflow=hidden \n"
            "skip: display=block widthPercent=100 position=absolute top=0 left=0 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"face\"></div></body></html>");
        auto sheet = ParseStylesheet(
            "#face { background: red url(data:image/png;base64,QUJDRA%3D%3D); width: 4em; height: 2em; }");
        auto* node = FindElementById(dom.get(), "face");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/data-uri-background",
            actual,
            "bg=1,0,0,1 width=64 height=32 backgroundImage=data:image/png;base64,QUJDRA%3D%3D \n",
            result);
    }

    {
        // CSS sprite: background-position (negative offset), -size, -repeat parse.
        auto dom = ParseHtml("<html><body><i id=\"icon\"></i></body></html>");
        auto sheet = ParseStylesheet(
            "#icon { background-image: url(sprite.png); background-position: -40px -80px;"
            " background-size: 200px 100px; background-repeat: no-repeat; }");
        auto* node = FindElementById(dom.get(), "icon");
        auto cs = sheet.resolve(node);
        char buf[256];
        snprintf(buf, sizeof buf,
            "repeat=%d posX=%g posY=%g sizeMode=%d sizeW=%g sizeH=%g\n",
            cs.bgRepeat, cs.bgPosX, cs.bgPosY, cs.bgSizeMode, cs.bgSizeW, cs.bgSizeH);
        ExpectEqual("css/background/sprite-longhand",
            std::string(buf),
            "repeat=3 posX=-40 posY=-80 sizeMode=3 sizeW=200 sizeH=100\n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"logo\"></div></body></html>");
        auto sheet = ParseStylesheet("#logo { transform: translate(-50%, -25%) scale(1.2); }");
        auto* node = FindElementById(dom.get(), "logo");
        auto cs = sheet.resolve(node);
        char buf[256];
        snprintf(buf, sizeof buf,
            "tx=%g txPct=%d ty=%g tyPct=%d scale=%g\n",
            cs.transformTx, cs.transformTxPercent ? 1 : 0,
            cs.transformTy, cs.transformTyPercent ? 1 : 0,
            cs.transformScale);
        ExpectEqual("css/transform/translate-percent-preserved",
            std::string(buf),
            "tx=-50 txPct=1 ty=-25 tyPct=1 scale=1.2\n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"logo\"></div></body></html>");
        auto sheet = ParseStylesheet("#logo { transform: translate(10px 20px) scale(2, 3) rotate(15deg); }");
        auto* node = FindElementById(dom.get(), "logo");
        auto cs = sheet.resolve(node);
        char buf[256];
        snprintf(buf, sizeof buf,
            "tx=%g ty=%g scale=%g rotate=%g\n",
            cs.transformTx, cs.transformTy, cs.transformScale, cs.transformRotate);
        ExpectEqual("css/transform/space-separated-args-and-rotate",
            std::string(buf),
            "tx=10 ty=20 scale=2 rotate=15\n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"logo\"></div></body></html>");
        auto sheet = ParseStylesheet("#logo { translate: -50% -25%; scale: 1.5; rotate: -12deg; }");
        auto* node = FindElementById(dom.get(), "logo");
        auto cs = sheet.resolve(node);
        char buf[256];
        snprintf(buf, sizeof buf,
            "tx=%g txPct=%d ty=%g tyPct=%d scale=%g rotate=%g\n",
            cs.transformTx, cs.transformTxPercent ? 1 : 0,
            cs.transformTy, cs.transformTyPercent ? 1 : 0,
            cs.transformScale, cs.transformRotate);
        ExpectEqual("css/transform/individual-translate-scale-rotate",
            std::string(buf),
            "tx=-50 txPct=1 ty=-25 tyPct=1 scale=1.5 rotate=-12\n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><p id=\"copy\"></p></body></html>");
        auto sheet = ParseStylesheet("#copy { font: italic 700 18px/27px \"Times New Roman\", serif; }");
        auto* node = FindElementById(dom.get(), "copy");
        auto cs = sheet.resolve(node);
        char buf[256];
        snprintf(buf, sizeof buf,
            "italic=%d bold=%d size=%g line=%g family=%s\n",
            cs.italic ? 1 : 0, cs.bold ? 1 : 0, cs.fontSize, cs.lineHeight, cs.fontFamily.c_str());
        ExpectEqual("css/cascade/font-shorthand-quoted-family",
            std::string(buf),
            "italic=1 bold=1 size=18 line=27 family=Times New Roman\n",
            result);
    }

    {
        // Shorthand carries position + repeat: "url() <x> <y> no-repeat".
        auto dom = ParseHtml("<html><body><i id=\"s\"></i></body></html>");
        auto sheet = ParseStylesheet(
            "#s { background: url(sprite.png) -12px -34px no-repeat; }");
        auto* node = FindElementById(dom.get(), "s");
        auto cs = sheet.resolve(node);
        char buf[256];
        snprintf(buf, sizeof buf, "repeat=%d posX=%g posY=%g img=%d\n",
            cs.bgRepeat, cs.bgPosX, cs.bgPosY, cs.backgroundImageSet ? 1 : 0);
        ExpectEqual("css/background/sprite-shorthand",
            std::string(buf),
            "repeat=3 posX=-12 posY=-34 img=1\n",
            result);
    }

    {
        // clamp()/min()/max() resolve against the viewport. At 1000px wide:
        // clamp(200px,30vw,400px) = clamp(200,300,400) = 300.
        SetCssViewport(1000.f, 800.f);
        auto cs = ParseInlineStyle("width: clamp(200px, 30vw, 400px)");
        char buf[64]; snprintf(buf, sizeof buf, "width=%g\n", cs.width);
        ExpectEqual("css/values/clamp-resolves-against-viewport",
            std::string(buf), "width=300\n", result);
    }

    {
        auto cs = ParseInlineStyle("width: calc(100% - 56px)");
        char buf[96];
        snprintf(buf, sizeof buf, "calc=%g,%g width=%g pct=%g\n",
            cs.widthCalcPercent, cs.widthCalcOffset, cs.width, cs.widthPercent);
        ExpectEqual("css/values/calc-percent-minus-length-width",
            std::string(buf), "calc=100,-56 width=-1 pct=-1\n", result);
    }

    {
        auto dom = ParseHtml("<html><body><h2 id=\"top\">Hello World!</h2></body></html>");
        auto sheet = ParseStylesheet("#top { font: 2em/24px sans-serif; }");
        auto* node = FindElementById(dom.get(), "top");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/font-shorthand-absolute-line-height",
            actual,
            "fontSize=32 lineHeight=24 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div class=\"picture\"></div></body></html>");
        auto sheet = ParseStylesheet(".picture { background: red; } .picture { background: none; }");
        auto* node = FindFirstElement(dom.get(), "div");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/background-none-clears-background",
            actual,
            "\n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"band\"></div></body></html>");
        auto sheet = ParseStylesheet("#band { height: 8px; min-height: 1em; max-height: 2mm; }");
        auto* node = FindElementById(dom.get(), "band");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/min-max-height",
            actual,
            "height=8 minHeight=16 maxHeight=7.55906 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"nose\"></div></body></html>");
        auto* owner = FindElementById(dom.get(), "nose");
        auto pseudo = Node::makeElement("div");
        pseudo->attrs = owner->attrs;
        pseudo->attrs["_vertex_pseudo"] = "before";
        pseudo->parent = const_cast<Node*>(owner);
        auto sheet = ParseStylesheet(
            "#nose:before { display: block; border-style: none solid solid; "
            "border-color: red yellow black yellow; border-width: 1em; content: ''; height: 0; }");
        std::string actual = SerializeComputedStyle(sheet.resolve(pseudo.get()));
        ExpectEqual("css/cascade/generated-border-box",
            actual,
            "display=block borderWidth=16 borderTopWidth=0 borderTopColor=1,0,0,1 borderRightColor=1,1,0,1 borderBottomColor=0,0,0,1 borderLeftColor=1,1,0,1 height=0 content= \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"cover\"></div></body></html>");
        auto* node = FindElementById(dom.get(), "cover");
        auto sheet = ParseStylesheet("#cover { position: relative; z-index: 2; }");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/z-index",
            actual,
            "position=relative zIndex=2 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><a id=\"link\">English</a></body></html>");
        auto* node = FindElementById(dom.get(), "link");
        auto sheet = ParseStylesheet("#link { outline: 0; }");
        auto style = node ? sheet.resolve(node) : ComputedStyle{};
        std::string actual = node
            ? ((style.outlineSet && style.outlineWidth > 0) ? "visible\n" : "none\n")
            : "missing\n";
        ExpectEqual("css/cascade/outline-zero-is-none",
            actual,
            "none\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string header = ReadTextFile(root / "src/css/stylesheet.h");
        std::string source = ReadTextFile(root / "src/css/stylesheet.cpp");
        auto dom = ParseHtml("<html><body><article id=\"story\" class=\"featured lead\"><p id=\"child\"></p></article></body></html>");
        auto* story = FindElementById(dom.get(), "story");
        auto* child = FindElementById(dom.get(), "child");
        auto sheet = ParseStylesheet(
            "#story { color: red; }"
            ".featured { padding-left: 2px; }"
            "article { margin-left: 3px; }"
            "body .lead p { margin-right: 4px; }"
            "* { padding-right: 1px; }");
        const bool indexed =
            header.find("idRuleBuckets") != std::string::npos
            && header.find("classRuleBuckets") != std::string::npos
            && header.find("tagRuleBuckets") != std::string::npos
            && source.find("candidateRulesFor") != std::string::npos;
        std::string actual = indexed ? "indexed\n" : "flat\n";
        actual += "story: ";
        actual += story ? SerializeComputedStyle(sheet.resolve(story)) : "missing\n";
        actual += "child: ";
        actual += child ? SerializeComputedStyle(sheet.resolve(child)) : "missing\n";
        ExpectEqual("css/cascade/rule-buckets-preserve-cascade",
            actual,
            "indexed\n"
            "story: color=1,0,0,1 marginLeft=3 paddingRight=1 paddingLeft=2 \n"
            "child: marginRight=4 paddingRight=1 \n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string header = ReadTextFile(root / "src/css/stylesheet.h");
        std::string source = ReadTextFile(root / "src/css/stylesheet.cpp");
        auto dom = ParseHtml("<html><body><article id=\"story\" class=\"featured lead\"><p id=\"child\" class=\"lead\"></p></article></body></html>");
        auto* story = FindElementById(dom.get(), "story");
        auto* child = FindElementById(dom.get(), "child");
        auto sheet = ParseStylesheet(
            "#story { color: red; }"
            ".featured { padding-left: 2px; }"
            ".lead { padding-right: 5px; }"
            "article { margin-left: 3px; }"
            "body .lead p { margin-right: 4px; }"
            "* { margin-top: 1px; }");
        const bool cached =
            header.find("candidateRuleIndexCache") != std::string::npos
            && header.find("mutable std::unordered_map") != std::string::npos
            && source.find("CandidateCacheKey") != std::string::npos
            && source.find("candidateRuleIndexCache.clear()") != std::string::npos;
        std::string actual = cached ? "cached\n" : "uncached\n";
        actual += "story: ";
        actual += story ? SerializeComputedStyle(sheet.resolve(story)) : "missing\n";
        actual += "child: ";
        actual += child ? SerializeComputedStyle(sheet.resolve(child)) : "missing\n";
        ExpectEqual("css/cascade/candidate-rule-cache-preserves-style",
            actual,
            "cached\n"
            "story: color=1,0,0,1 marginTop=1 marginLeft=3 paddingRight=5 paddingLeft=2 \n"
            "child: marginTop=1 marginRight=4 paddingRight=5 \n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string source = ReadTextFile(root / "src/css/stylesheet.cpp");
        const bool cached =
            source.find("g_inlineStyleCache") != std::string::npos
            && source.find("kMaxInlineStyleCacheEntries") != std::string::npos
            && source.find("g_inlineStyleCache.find(style)") != std::string::npos;
        ComputedStyle first = ParseInlineStyle("color: #123456; margin-left: 7px; --gap: 3px; padding-left: var(--gap)");
        ResolveStyleVariables(first);
        ComputedStyle second = ParseInlineStyle("color: #123456; margin-left: 7px; --gap: 3px; padding-left: var(--gap)");
        ResolveStyleVariables(second);
        std::string actual = cached ? "cached\n" : "uncached\n";
        actual += SerializeComputedStyle(first);
        actual += SerializeComputedStyle(second);
        ExpectEqual("css/cascade/inline-style-parser-cache-preserves-vars",
            actual,
            "cached\n"
            "color=0.0705882,0.203922,0.337255,1 marginLeft=7 paddingLeft=3 \n"
            "color=0.0705882,0.203922,0.337255,1 marginLeft=7 paddingLeft=3 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div class=\"picture\"><p id=\"scalp\"></p></div></body></html>");
        auto* node = FindElementById(dom.get(), "scalp");
        auto sheet = ParseStylesheet(
            "html { font: 12px sans-serif; } "
            ".picture p { position: fixed; top: 9em; left: 11em; }");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/acid2-fixed-em-offsets",
            actual,
            "position=fixed top=108 left=132 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><h2 id=\"top\">Hello World!</h2></body></html>");
        auto* node = FindElementById(dom.get(), "top");
        auto sheet = ParseStylesheet(
            "html { font: 12px sans-serif; } "
            "#top { margin: 100em 3em 0; font: 2em/24px sans-serif; }");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/em-lengths-use-element-font-size",
            actual,
            "fontSize=24 marginTop=2400 marginRight=72 marginBottom=0 marginLeft=72 lineHeight=24 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><input id=\"search\"><select id=\"lang\"></select></body></html>");
        auto* search = FindElementById(dom.get(), "search");
        auto* lang = FindElementById(dom.get(), "lang");
        auto sheet = ParseStylesheet(
            ":root { --search-select-height: 2.4rem; } "
            "html { font-size: 62.5%; } "
            "#search { width: 9.6rem; height: 4.4rem; padding-left: 1.2rem; } "
            "#lang { height: var(--search-select-height); }");
        std::string actual = "search: ";
        actual += search ? SerializeComputedStyle(sheet.resolve(search)) : "missing\n";
        ComputedStyle rootStyle = sheet.resolve(FindFirstElement(dom.get(), "html"));
        ResolveStyleVariables(rootStyle);
        ComputedStyle langStyle = rootStyle.inherit(sheet.resolve(lang));
        ResolveStyleVariables(langStyle);
        actual += "lang: ";
        actual += lang ? SerializeComputedStyle(langStyle) : "missing\n";
        ExpectEqual("css/cascade/rem-lengths-use-root-font-size",
            actual,
            "search: paddingLeft=12 width=96 height=44 \n"
            "lang: fontSize=10 height=24 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"portal\"><form id=\"search\"></form><div id=\"picker\"></div></div></body></html>");
        auto* search = FindElementById(dom.get(), "search");
        auto* picker = FindElementById(dom.get(), "picker");
        auto sheet = ParseStylesheet(
            "#search { inline-size: calc(100% - 56px); block-size: 44px; "
            "margin-inline: auto 8px; padding-block: 4px 6px; padding-inline: calc(1rem + 2px) 20px; "
            "border-inline-width: 1px 2px; border-block-width: 3px 4px; }"
            "#picker { position:absolute; inset-inline-end: 0; inset-block-start: 50%; "
            "min-inline-size: 120px; max-inline-size: 180px; min-block-size: 24px; max-block-size: 44px; "
            "block-size: calc(100% - 10px); }");
        std::string actual = "search: ";
        actual += search ? SerializeComputedStyle(sheet.resolve(search)) : "missing\n";
        actual += "picker: ";
        actual += picker ? SerializeComputedStyle(sheet.resolve(picker)) : "missing\n";
        ExpectEqual("css/cascade/logical-sizing-spacing-and-insets",
            actual,
            "search: marginRight=8 marginLeft=-2 paddingTop=4 paddingRight=20 paddingBottom=6 paddingLeft=18 borderTopWidth=3 borderRightWidth=2 borderBottomWidth=4 borderLeftWidth=1 widthCalc=100%+-56 height=44 \n"
            "picker: heightCalc=100%+-10 maxWidth=180 minWidth=120 minHeight=24 maxHeight=44 position=absolute top=50 right=0 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"control\"></div></body></html>");
        auto* root = FindFirstElement(dom.get(), "html");
        auto* control = FindElementById(dom.get(), "control");
        auto base = ParseStylesheet(":root { --control-height: 2.4rem; } html { font-size: 62.5%; }");
        auto later = ParseStylesheet("#control { height: var(--control-height); }");
        Stylesheet sheet;
        if (base.rootRemBaseSet) {
            sheet.rootRemBase = base.rootRemBase;
            sheet.rootRemBaseSet = true;
        }
        for (auto& rule : base.rules) sheet.rules.push_back(rule);
        if (later.rootRemBaseSet) {
            sheet.rootRemBase = later.rootRemBase;
            sheet.rootRemBaseSet = true;
        }
        for (auto& rule : later.rules) sheet.rules.push_back(rule);
        sheet.rebuildRuleBuckets();
        ComputedStyle rootStyle = sheet.resolve(root);
        ResolveStyleVariables(rootStyle);
        ComputedStyle controlStyle = rootStyle.inherit(sheet.resolve(control));
        ResolveStyleVariables(controlStyle);
        ExpectEqual("css/cascade/rem-base-survives-multiple-stylesheets",
            SerializeComputedStyle(controlStyle),
            "fontSize=10 height=24 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"target\"></div></body></html>");
        auto* rootNode = FindFirstElement(dom.get(), "html");
        auto* target = FindElementById(dom.get(), "target");
        auto sheet = ParseStylesheet(
            ":root { --brand: #13579b; --gap: 12px; }"
            "#target { color: var(--brand); padding-left: var(--gap); }");
        auto rootStyle = rootNode ? sheet.resolve(rootNode) : ComputedStyle{};
        ResolveStyleVariables(rootStyle);
        auto targetStyle = target ? rootStyle.inherit(sheet.resolve(target)) : ComputedStyle{};
        ResolveStyleVariables(targetStyle);
        ExpectEqual("css/custom-properties/inherited-and-resolved",
            SerializeComputedStyle(targetStyle),
            "color=0.0745098,0.341176,0.607843,1 paddingLeft=12 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"target\"></div></body></html>");
        auto* target = FindElementById(dom.get(), "target");
        auto sheet = ParseStylesheet(
            "#target { color: red; }"
            "@media screen and (min-width: 600px) { #target { color: blue; } }");
        sheet.setViewport(599.f, 800.f);
        const std::string narrow = target ? SerializeComputedStyle(sheet.resolve(target)) : "missing\n";
        sheet.setViewport(600.f, 800.f);
        const std::string wide = target ? SerializeComputedStyle(sheet.resolve(target)) : "missing\n";
        ExpectEqual("css/media/min-width",
            "narrow: " + narrow + "wide: " + wide,
            "narrow: color=1,0,0,1 \nwide: color=0,0,1,1 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"target\"></div></body></html>");
        auto* target = FindElementById(dom.get(), "target");
        auto sheet = ParseStylesheet(
            "#target { color: red; }"
            "@media (width <= 700px) { #target { color: blue; } }"
            "@media (600px <= width) { #target { margin-left: 4px; } }"
            "@media (height > 500px) { #target { padding-left: 6px; } }");
        sheet.setViewport(500.f, 400.f);
        const std::string small = target ? SerializeComputedStyle(sheet.resolve(target)) : "missing\n";
        sheet.setViewport(650.f, 600.f);
        const std::string middle = target ? SerializeComputedStyle(sheet.resolve(target)) : "missing\n";
        sheet.setViewport(750.f, 600.f);
        const std::string wide = target ? SerializeComputedStyle(sheet.resolve(target)) : "missing\n";
        ExpectEqual("css/media/range-syntax",
            "small: " + small + "middle: " + middle + "wide: " + wide,
            "small: color=0,0,1,1 \n"
            "middle: color=0,0,1,1 marginLeft=4 paddingLeft=6 \n"
            "wide: color=1,0,0,1 marginLeft=4 paddingLeft=6 \n",
            result);
    }

    return result;
}
