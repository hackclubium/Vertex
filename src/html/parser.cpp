#include "html/parser.h"
#include "html/tokenizer.h"
#include <set>
#include <algorithm>
#include <cctype>
#include <map>

// ── Element categories ──────────────────────────────────────────────────────

static const std::set<std::string> kVoidTags = {
    "area","base","br","col","embed","hr","img","input",
    "link","meta","param","source","track","wbr"
};

static const std::set<std::string> kBlockTags = {
    "address","article","aside","blockquote","center","details","dialog",
    "dd","div","dl","dt","fieldset","figcaption","figure","footer","form",
    "h1","h2","h3","h4","h5","h6","header","hgroup","hr","li","main",
    "nav","ol","p","pre","section","summary","table","ul",
    "thead","tbody","tfoot","tr","td","th",
    "menu","search","listing","xmp"
};

static const std::set<std::string> kFormattingTags = {
    "a","b","big","code","em","font","i","nobr","s","small",
    "strike","strong","tt","u","mark","ins","del","sub","sup"
};

static const std::set<std::string> kHeadTags = {
    "base","basefont","bgsound","link","meta","noframes",
    "script","style","template","title","noscript"
};

// Elements that can only contain specific children (phrasing content only).
static const std::set<std::string> kPhrasingOnly = {
    "p","h1","h2","h3","h4","h5","h6","pre","listing","summary"
};

// Elements that generate an implied end tag when encountered as end tags.
static const std::set<std::string> kImpliedEndTags = {
    "dd","dt","li","optgroup","option","p","rb","rp","rt","rtc"
};

// Elements that should NOT be reopened after being closed by a formatting
// element mismatch (adoption agency simplification).
static const std::set<std::string> kSpecialElements = {
    "address","applet","area","article","aside","base","basefont","bgsound",
    "blockquote","body","br","button","caption","center","col","colgroup",
    "dd","details","dialog","dir","div","dl","dt","embed","fieldset",
    "figcaption","figure","footer","form","frame","frameset","h1","h2","h3",
    "h4","h5","h6","head","header","hgroup","hr","html","iframe","img",
    "input","keygen","li","link","listing","main","marquee","menu","meta",
    "nav","noembed","noframes","noscript","object","ol","p","param","plaintext",
    "pre","script","section","select","source","style","summary","table",
    "tbody","td","template","textarea","tfoot","th","thead","title","tr",
    "track","ul","wbr","xmp"
};

// Tags that can only appear inside specific parents — if they appear
// outside, we need to create the parent implicitly.
static const std::map<std::string, std::string> kImplicitParent = {
    {"tr",    "tbody"},
    {"td",    "tr"},
    {"th",    "tr"},
    {"tbody", "table"},
    {"thead", "table"},
    {"tfoot", "table"},
    {"col",   "colgroup"},
    {"option","select"},
};

// Scope barriers: when looking for an element "in scope", these tags stop the search.
static const std::set<std::string> kScopeBarriers = {
    "applet","caption","html","table","td","th","marquee","object",
    "template","svg","math"
};

// ── Implied end tag rules ───────────────────────────────────────────────────

// Returns true if opening `newTag` should close `openTag`.
static bool implicitlyCloses(const std::string& newTag, const std::string& openTag) {
    // <p> is closed by any block-level element
    if (openTag == "p" && kBlockTags.count(newTag)) return true;
    // <p> is also closed by </p> (handled in end-tag), and by <address>, <article>, etc.
    if (openTag == "p" && (newTag == "address" || newTag == "article" || newTag == "aside"
        || newTag == "blockquote" || newTag == "details" || newTag == "dialog"
        || newTag == "fieldset" || newTag == "figcaption" || newTag == "figure"
        || newTag == "footer" || newTag == "header" || newTag == "hgroup"
        || newTag == "main" || newTag == "menu" || newTag == "nav" || newTag == "search"
        || newTag == "summary")) return true;

    // Table elements
    if (newTag == "td" || newTag == "th")
        return openTag == "td" || openTag == "th";
    if (newTag == "tr")
        return openTag == "td" || openTag == "th" || openTag == "tr";
    if (newTag == "thead" || newTag == "tbody" || newTag == "tfoot")
        return openTag == "td" || openTag == "th" || openTag == "tr"
            || openTag == "thead" || openTag == "tbody" || openTag == "tfoot";
    if (newTag == "caption" || newTag == "colgroup")
        return openTag == "td" || openTag == "th" || openTag == "tr"
            || openTag == "thead" || openTag == "tbody" || openTag == "tfoot"
            || openTag == "caption" || openTag == "colgroup";
    // <table> inside <table> closes the outer table's open elements
    if (newTag == "table")
        return openTag == "td" || openTag == "th" || openTag == "tr"
            || openTag == "thead" || openTag == "tbody" || openTag == "tfoot"
            || openTag == "caption";

    // Lists
    if (newTag == "li") return openTag == "li";
    if (newTag == "dt" || newTag == "dd") return openTag == "dt" || openTag == "dd";
    // <ul>/<ol> close <li> in the previous list
    if ((newTag == "ul" || newTag == "ol") && openTag == "li") return true;

    // Headings close other headings
    if (newTag.size() == 2 && newTag[0] == 'h' && newTag[1] >= '1' && newTag[1] <= '6')
        if (openTag.size() == 2 && openTag[0] == 'h' && openTag[1] >= '1' && openTag[1] <= '6')
            return true;

    // Options
    if (newTag == "option") return openTag == "option";
    if (newTag == "optgroup") return openTag == "option" || openTag == "optgroup";

    // Ruby
    if (newTag == "rt" || newTag == "rp")
        return openTag == "rt" || openTag == "rp" || openTag == "rb" || openTag == "rtc";
    if (newTag == "rb" || newTag == "rtc")
        return openTag == "rt" || openTag == "rp" || openTag == "rb" || openTag == "rtc";

    // <form> closes <form> (nested forms are invalid)
    if (newTag == "form" && openTag == "form") return true;

    // <button> closes <button> (nested buttons are invalid)
    if (newTag == "button" && openTag == "button") return true;

    // <a> closes <a> (nested anchors are invalid)
    if (newTag == "a" && openTag == "a") return true;

    // <select> closes <select>
    if (newTag == "select" && openTag == "select") return true;

    return false;
}

// Check if tag is "in scope" — i.e., there's an open element with that tag
// on the stack, and no scope barrier between it and the top.
static int findInScope(const std::vector<std::shared_ptr<Node>>& stack, const std::string& tag) {
    for (int i = (int)stack.size() - 1; i >= 1; --i) {
        if (stack[i]->tagName == tag) return i;
        if (kScopeBarriers.count(stack[i]->tagName)) return -1;
    }
    return -1;
}

// Check if the stack has a specific tag anywhere (ignoring scope).
static int findOnStack(const std::vector<std::shared_ptr<Node>>& stack, const std::string& tag) {
    for (int i = (int)stack.size() - 1; i >= 1; --i)
        if (stack[i]->tagName == tag) return i;
    return -1;
}

// Find in table scope (scope barriers: table, html, template).
static int findInTableScope(const std::vector<std::shared_ptr<Node>>& stack, const std::string& tag) {
    for (int i = (int)stack.size() - 1; i >= 1; --i) {
        if (stack[i]->tagName == tag) return i;
        const std::string& t = stack[i]->tagName;
        if (t == "table" || t == "html" || t == "template") return -1;
    }
    return -1;
}

// Find in select scope.
static int findInSelectScope(const std::vector<std::shared_ptr<Node>>& stack, const std::string& tag) {
    for (int i = (int)stack.size() - 1; i >= 1; --i) {
        if (stack[i]->tagName == tag) return i;
        const std::string& t = stack[i]->tagName;
        if (t != "optgroup" && t != "option") return -1;
    }
    return -1;
}

// Pop elements until we find the target tag or a scope barrier.
static void generateImpliedEndTags(std::vector<std::shared_ptr<Node>>& stack,
                                    const std::string& except = "") {
    while (stack.size() > 1) {
        const std::string& top = stack.back()->tagName;
        if (top == except) break;
        if (kImpliedEndTags.count(top))
            stack.pop_back();
        else break;
    }
}

// Is whitespace-only text?
static bool isWhitespace(const std::string& s) {
    for (char c : s) if (!std::isspace((unsigned char)c)) return false;
    return true;
}

// Is a heading tag?
static bool isHeading(const std::string& tag) {
    return tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6';
}

// ── Main parser ─────────────────────────────────────────────────────────────

std::shared_ptr<Node> ParseHtml(const std::string& html) {
    auto doc = Node::makeDocument();

    // Open-element stack: doc is always at index 0.
    std::vector<std::shared_ptr<Node>> stack;
    stack.push_back(doc);

    // Track whether we've seen <html>, <head>, <body> for implicit creation.
    bool seenHtml = false, seenHead = false, seenBody = false;
    bool inHead = false;

    auto current = [&]() -> Node* { return stack.back().get(); };

    auto ensureHtml = [&]() {
        if (!seenHtml) {
            auto html = Node::makeElement("html");
            doc->appendChild(html);
            stack.push_back(html);
            seenHtml = true;
        }
    };

    auto ensureBody = [&]() {
        ensureHtml();
        if (!seenBody) {
            // Close <head> if it's open.
            if (inHead) {
                int hi = findOnStack(stack, "head");
                if (hi > 0) stack.erase(stack.begin() + hi, stack.end());
                inHead = false;
            }
            auto body = Node::makeElement("body");
            // Find <html> and append body to it.
            for (int i = (int)stack.size() - 1; i >= 0; --i) {
                if (stack[i]->tagName == "html") {
                    stack[i]->appendChild(body);
                    stack.push_back(body);
                    break;
                }
            }
            seenBody = true;
        }
    };

    // Foster parenting: if we're inside a table and get non-table content,
    // it should go before the table (not inside it).
    auto shouldFosterParent = [&]() -> bool {
        const std::string& top = current()->tagName;
        return top == "table" || top == "tbody" || top == "thead"
            || top == "tfoot" || top == "tr";
    };

    auto fosterParent = [&](std::shared_ptr<Node> child) {
        // Find the table element on the stack and insert before it.
        for (int i = (int)stack.size() - 1; i >= 1; --i) {
            if (stack[i]->tagName == "table") {
                Node* tableParent = stack[i]->parent;
                if (tableParent) {
                    child->parent = tableParent;
                    // Insert before the table in the parent's children.
                    auto& kids = tableParent->children;
                    for (auto it = kids.begin(); it != kids.end(); ++it) {
                        if (it->get() == stack[i].get()) {
                            kids.insert(it, child);
                            return;
                        }
                    }
                    // Fallback: just append.
                    tableParent->appendChild(child);
                    return;
                }
                break;
            }
        }
        // Fallback.
        current()->appendChild(child);
    };

    // Whitespace preservation: track whether we're inside <pre>.
    auto inPre = [&]() -> bool {
        for (int i = (int)stack.size() - 1; i >= 1; --i)
            if (stack[i]->tagName == "pre" || stack[i]->tagName == "textarea"
                || stack[i]->tagName == "listing") return true;
        return false;
    };

    // Raw-text elements (<script>/<style>/<noscript>): their text is source
    // code/CSS, not display text, so it must be preserved byte-for-byte.
    // Collapsing newlines to spaces here (as we do for ordinary text nodes)
    // silently corrupts scripts; e.g. a `//` line comment with no `\n` left
    // to terminate it swallows the rest of the script.
    auto inRawText = [&]() -> bool {
        return !stack.empty() && (stack.back()->tagName == "script"
            || stack.back()->tagName == "style" || stack.back()->tagName == "noscript");
    };

    HtmlTokenizer tok;
    tok.tokenize(html, [&](const HtmlToken& t) {
        switch (t.type) {

        case TokenType::Doctype:
            // Ignore (we don't switch rendering modes).
            break;

        case TokenType::StartTag: {
            const std::string& tag = t.name;

            // <html>
            if (tag == "html") {
                if (!seenHtml) {
                    auto html = Node::makeElement("html");
                    html->attrs = t.attrs;
                    doc->appendChild(html);
                    stack.push_back(html);
                    seenHtml = true;
                } else {
                    // Merge attributes into existing <html>.
                    int hi = findOnStack(stack, "html");
                    if (hi > 0) for (auto& [k,v] : t.attrs)
                        if (stack[hi]->attrs.find(k) == stack[hi]->attrs.end())
                            stack[hi]->attrs[k] = v;
                }
                break;
            }

            // <head>
            if (tag == "head") {
                if (!seenHead) {
                    ensureHtml();
                    auto head = Node::makeElement("head");
                    head->attrs = t.attrs;
                    // Append to <html>.
                    for (int i = (int)stack.size()-1; i >= 0; --i)
                        if (stack[i]->tagName == "html") { stack[i]->appendChild(head); break; }
                    stack.push_back(head);
                    seenHead = true;
                    inHead = true;
                }
                break;
            }

            // <body>
            if (tag == "body") {
                if (!seenBody) {
                    ensureHtml();
                    if (inHead) {
                        int hi = findOnStack(stack, "head");
                        if (hi > 0) stack.erase(stack.begin() + hi, stack.end());
                        inHead = false;
                    }
                    auto body = Node::makeElement("body");
                    body->attrs = t.attrs;
                    for (int i = (int)stack.size()-1; i >= 0; --i)
                        if (stack[i]->tagName == "html") { stack[i]->appendChild(body); break; }
                    stack.push_back(body);
                    seenBody = true;
                } else {
                    // Merge attributes.
                    int bi = findOnStack(stack, "body");
                    if (bi > 0) for (auto& [k,v] : t.attrs)
                        if (stack[bi]->attrs.find(k) == stack[bi]->attrs.end())
                            stack[bi]->attrs[k] = v;
                }
                break;
            }

            // Head-only elements go into <head> even if we're in <body>.
            if (kHeadTags.count(tag) && tag != "script" && tag != "style" && tag != "template") {
                if (!seenHead) {
                    ensureHtml();
                    auto head = Node::makeElement("head");
                    for (int i = (int)stack.size()-1; i >= 0; --i)
                        if (stack[i]->tagName == "html") { stack[i]->appendChild(head); break; }
                    seenHead = true;
                }
                // Find <head> in the tree and add to it.
                std::function<Node*(Node*)> findHead = [&](Node* n) -> Node* {
                    if (n->tagName == "head") return n;
                    for (auto& c : n->children) if (auto* h = findHead(c.get())) return h;
                    return nullptr;
                };
                Node* head = findHead(doc.get());
                if (head) {
                    auto node = Node::makeElement(tag);
                    node->attrs = t.attrs;
                    head->appendChild(node);
                    break;
                }
            }

            // Everything else needs <body>.
            if (tag != "head" && tag != "html" && !inHead)
                ensureBody();

            // Implicit closing: pop open elements that the new tag closes.
            while (stack.size() > 1 && implicitlyCloses(tag, stack.back()->tagName))
                stack.pop_back();

            // Implicit parent creation for table elements.
            auto implIt = kImplicitParent.find(tag);
            if (implIt != kImplicitParent.end()) {
                const std::string& needed = implIt->second;
                if (current()->tagName != needed && findInScope(stack, needed) < 0) {
                    auto parent = Node::makeElement(needed);
                    current()->appendChild(parent);
                    stack.push_back(parent);
                }
            }

            auto node = Node::makeElement(tag);
            node->attrs = t.attrs;

            // Foster parenting: non-table content inside <table> goes before the table.
            if (shouldFosterParent() && !kBlockTags.count(tag)
                && tag != "table" && tag != "tbody" && tag != "thead"
                && tag != "tfoot" && tag != "tr" && tag != "td" && tag != "th"
                && tag != "caption" && tag != "colgroup" && tag != "col") {
                fosterParent(node);
            } else {
                current()->appendChild(node);
            }

            if (!kVoidTags.count(tag) && !t.selfClosing)
                stack.push_back(node);
            break;
        }

        case TokenType::EndTag: {
            const std::string& tag = t.name;

            // </body>, </html>: don't pop, just mark.
            if (tag == "html" || tag == "body") break;

            // </head>: close head, switch to body mode.
            if (tag == "head") {
                if (inHead) {
                    int hi = findOnStack(stack, "head");
                    if (hi > 0) stack.erase(stack.begin() + hi, stack.end());
                    inHead = false;
                }
                break;
            }

            // </template>: close to matching <template>.
            if (tag == "template") {
                int ti = findOnStack(stack, "template");
                if (ti > 0) stack.erase(stack.begin() + ti, stack.end());
                break;
            }

            // </p>: generate implied end tags (except p), then close <p>.
            if (tag == "p") {
                generateImpliedEndTags(stack, "p");
                int pi = findInScope(stack, "p");
                if (pi > 0) {
                    stack.erase(stack.begin() + pi, stack.end());
                } else {
                    ensureBody();
                    current()->appendChild(Node::makeElement("p"));
                }
                break;
            }

            // </li>: generate implied end tags (except li), then close <li>.
            if (tag == "li") {
                generateImpliedEndTags(stack, "li");
                int li = findInScope(stack, "li");
                if (li > 0) stack.erase(stack.begin() + li, stack.end());
                break;
            }

            // </dd>, </dt>: similar to </li>.
            if (tag == "dd" || tag == "dt") {
                generateImpliedEndTags(stack, tag);
                int di = findInScope(stack, tag);
                if (di > 0) stack.erase(stack.begin() + di, stack.end());
                break;
            }

            // Headings: </h1> through </h6> close any open heading.
            if (isHeading(tag)) {
                generateImpliedEndTags(stack);
                for (int i = (int)stack.size()-1; i >= 1; --i) {
                    if (isHeading(stack[i]->tagName)) {
                        stack.erase(stack.begin() + i, stack.end());
                        break;
                    }
                    if (kScopeBarriers.count(stack[i]->tagName)) break;
                }
                break;
            }

            // </br>: treated as <br> (common mistake in real HTML).
            if (tag == "br") {
                ensureBody();
                current()->appendChild(Node::makeElement("br"));
                break;
            }

            // </table>: close everything back to <table>.
            if (tag == "table") {
                int ti = findInTableScope(stack, "table");
                if (ti > 0) stack.erase(stack.begin() + ti, stack.end());
                break;
            }

            // </tr>: generate implied end tags, close to <tr>.
            if (tag == "tr") {
                generateImpliedEndTags(stack);
                int ri = findInTableScope(stack, "tr");
                if (ri > 0) stack.erase(stack.begin() + ri, stack.end());
                break;
            }

            // </td>, </th>: generate implied end tags, close to <td>/<th>.
            if (tag == "td" || tag == "th") {
                generateImpliedEndTags(stack);
                int ci = findInTableScope(stack, tag);
                if (ci > 0) stack.erase(stack.begin() + ci, stack.end());
                break;
            }

            // </thead>, </tbody>, </tfoot>: close to matching section.
            if (tag == "thead" || tag == "tbody" || tag == "tfoot") {
                generateImpliedEndTags(stack);
                int si = findInTableScope(stack, tag);
                if (si > 0) stack.erase(stack.begin() + si, stack.end());
                break;
            }

            // </caption>: close to <caption>.
            if (tag == "caption") {
                int ci = findInTableScope(stack, "caption");
                if (ci > 0) stack.erase(stack.begin() + ci, stack.end());
                break;
            }

            // </colgroup>: close to <colgroup>.
            if (tag == "colgroup") {
                if (stack.back()->tagName == "colgroup") stack.pop_back();
                break;
            }

            // </form>: close the form element.
            if (tag == "form") {
                int fi = findInScope(stack, "form");
                if (fi > 0) stack.erase(stack.begin() + fi, stack.end());
                break;
            }

            // </select>: close to <select>.
            if (tag == "select") {
                int si = findInSelectScope(stack, "select");
                if (si > 0) stack.erase(stack.begin() + si, stack.end());
                break;
            }

            // Formatting tags: simplified adoption agency algorithm.
            if (kFormattingTags.count(tag)) {
                // Try up to 8 times (spec says 8 iterations max).
                for (int iter = 0; iter < 8; ++iter) {
                    int fi = findInScope(stack, tag);
                    if (fi <= 0) break;
                    // Check if there are any special elements between fi and the top.
                    bool hasSpecial = false;
                    for (int j = fi + 1; j < (int)stack.size(); ++j)
                        if (kSpecialElements.count(stack[j]->tagName)) { hasSpecial = true; break; }
                    if (!hasSpecial) {
                        stack.erase(stack.begin() + fi, stack.end());
                        break;
                    }
                    // Complex case: just close the formatting element.
                    stack.erase(stack.begin() + fi, stack.end());
                    break;
                }
                break;
            }

            // General case: find the matching open tag and close it.
            // Walk the stack from top; if we hit a special element before
            // finding the target, ignore the end tag (error recovery).
            for (int i = (int)stack.size() - 1; i >= 1; --i) {
                if (stack[i]->tagName == tag) {
                    generateImpliedEndTags(stack, tag);
                    stack.erase(stack.begin() + i, stack.end());
                    break;
                }
                if (kSpecialElements.count(stack[i]->tagName)) break;
            }
            break;
        }

        case TokenType::Comment:
            // Comments are preserved in the DOM for JS access but don't affect layout.
            break;

        case TokenType::Text: {
            std::string txt = t.data;
            if (txt.empty()) break;

            // Text before <body>: if it's whitespace, skip it. Otherwise, ensure body.
            if (!seenBody && !inHead) {
                if (isWhitespace(txt)) break;
                ensureBody();
            }

            // Preserve whitespace inside <pre>/<textarea>, and preserve raw
            // source verbatim inside <script>/<style>/<noscript>.
            if (inPre() || inRawText()) {
                if (!txt.empty())
                    current()->appendChild(Node::makeText(txt));
                break;
            }

            // Normalise whitespace: newlines/tabs → spaces, collapse runs.
            for (auto& c : txt)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            std::string collapsed;
            bool lastSpace = false;
            for (char c : txt) {
                if (c == ' ') {
                    if (!lastSpace) collapsed += ' ';
                    lastSpace = true;
                } else {
                    collapsed += c;
                    lastSpace = false;
                }
            }
            if (collapsed.empty() || collapsed == " ") break;

            // Foster parenting for text inside tables.
            if (shouldFosterParent()) {
                auto textNode = Node::makeText(collapsed);
                fosterParent(textNode);
            } else {
                current()->appendChild(Node::makeText(collapsed));
            }
            break;
        }

        default: break;
        }
    });

    return doc;
}
