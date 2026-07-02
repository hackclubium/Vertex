#include "html/tokenizer.h"
#include <cctype>
#include <map>
#include <stdexcept>
#include <set>

static std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string utf8(unsigned long cp) {
    std::string out;
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

static const std::map<std::string, unsigned long>& namedEntities() {
    static const std::map<std::string, unsigned long> entities = {
        {"amp", 38}, {"lt", 60}, {"gt", 62}, {"quot", 34}, {"apos", 39}, {"nbsp", 0x00A0},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"bull", 0x2022}, {"hellip", 0x2026},
        {"raquo", 0x00BB}, {"laquo", 0x00AB},
        {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201D}, {"ldquo", 0x201C},
        {"copy", 0x00A9}, {"reg", 0x00AE}, {"trade", 0x2122},
        {"times", 0x00D7}, {"divide", 0x00F7}, {"middot", 0x00B7},
        // Arrows
        {"rarr", 0x2192}, {"larr", 0x2190}, {"uarr", 0x2191}, {"darr", 0x2193},
        {"harr", 0x2194}, {"rArr", 0x21D2}, {"lArr", 0x21D0},
        // Spaces & dashes
        {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009}, {"shy", 0x00AD},
        // Common symbols & punctuation
        {"deg", 0x00B0}, {"plusmn", 0x00B1}, {"sup2", 0x00B2}, {"sup3", 0x00B3},
        {"frac12", 0x00BD}, {"frac14", 0x00BC}, {"frac34", 0x00BE},
        {"cent", 0x00A2}, {"pound", 0x00A3}, {"euro", 0x20AC}, {"yen", 0x00A5},
        {"sect", 0x00A7}, {"para", 0x00B6}, {"dagger", 0x2020}, {"Dagger", 0x2021},
        {"permil", 0x2030}, {"prime", 0x2032}, {"Prime", 0x2033},
        {"infin", 0x221E}, {"ne", 0x2260}, {"le", 0x2264}, {"ge", 0x2265},
        {"micro", 0x00B5}, {"sbquo", 0x201A}, {"bdquo", 0x201E},
        {"check", 0x2713}, {"cross", 0x2717}, {"star", 0x2605}, {"starf", 0x2605},
        // Latin accented letters (very common in European content)
        {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2}, {"atilde", 0xE3}, {"auml", 0xE4}, {"aring", 0xE5},
        {"ccedil", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecirc", 0xEA}, {"euml", 0xEB},
        {"igrave", 0xEC}, {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF},
        {"ntilde", 0xF1}, {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocirc", 0xF4}, {"otilde", 0xF5}, {"ouml", 0xF6},
        {"ugrave", 0xF9}, {"uacute", 0xFA}, {"ucirc", 0xFB}, {"uuml", 0xFC}, {"szlig", 0xDF},
        {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acirc", 0xC2}, {"Atilde", 0xC3}, {"Auml", 0xC4}, {"Aring", 0xC5},
        {"Ccedil", 0xC7}, {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"Ecirc", 0xCA}, {"Euml", 0xCB},
        {"Igrave", 0xCC}, {"Iacute", 0xCD}, {"Icirc", 0xCE}, {"Iuml", 0xCF},
        {"Ntilde", 0xD1}, {"Ograve", 0xD2}, {"Oacute", 0xD3}, {"Ocirc", 0xD4}, {"Otilde", 0xD5}, {"Ouml", 0xD6},
        {"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucirc", 0xDB}, {"Uuml", 0xDC},
        {"eth", 0xF0}, {"ETH", 0xD0}, {"thorn", 0xFE}, {"THORN", 0xDE}, {"yuml", 0xFF},
        // Greek letters (Wikipedia math articles)
        {"Alpha", 0x391}, {"Beta", 0x392}, {"Gamma", 0x393}, {"Delta", 0x394}, {"Epsilon", 0x395},
        {"Zeta", 0x396}, {"Eta", 0x397}, {"Theta", 0x398}, {"Iota", 0x399}, {"Kappa", 0x39A},
        {"Lambda", 0x39B}, {"Mu", 0x39C}, {"Nu", 0x39D}, {"Xi", 0x39E}, {"Omicron", 0x39F},
        {"Pi", 0x3A0}, {"Rho", 0x3A1}, {"Sigma", 0x3A3}, {"Tau", 0x3A4}, {"Upsilon", 0x3A5},
        {"Phi", 0x3A6}, {"Chi", 0x3A7}, {"Psi", 0x3A8}, {"Omega", 0x3A9},
        {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4}, {"epsilon", 0x3B5},
        {"zeta", 0x3B6}, {"eta", 0x3B7}, {"theta", 0x3B8}, {"iota", 0x3B9}, {"kappa", 0x3BA},
        {"lambda", 0x3BB}, {"mu", 0x3BC}, {"nu", 0x3BD}, {"xi", 0x3BE}, {"omicron", 0x3BF},
        {"pi", 0x3C0}, {"rho", 0x3C1}, {"sigma", 0x3C3}, {"tau", 0x3C4}, {"upsilon", 0x3C5},
        {"phi", 0x3C6}, {"chi", 0x3C7}, {"psi", 0x3C8}, {"omega", 0x3C9},
        // Math/misc symbols
        {"forall", 0x2200}, {"part", 0x2202}, {"exist", 0x2203}, {"empty", 0x2205},
        {"nabla", 0x2207}, {"isin", 0x2208}, {"notin", 0x2209}, {"ni", 0x220B},
        {"prod", 0x220F}, {"sum", 0x2211}, {"minus", 0x2212}, {"lowast", 0x2217},
        {"radic", 0x221A}, {"prop", 0x221D}, {"ang", 0x2220},
        {"and", 0x2227}, {"or", 0x2228}, {"cap", 0x2229}, {"cup", 0x222A}, {"int", 0x222B},
        {"there4", 0x2234}, {"sim", 0x223C}, {"cong", 0x2245}, {"asymp", 0x2248},
        {"equiv", 0x2261}, {"sub", 0x2282}, {"sup", 0x2283}, {"nsub", 0x2284},
        {"sube", 0x2286}, {"supe", 0x2287}, {"oplus", 0x2295}, {"otimes", 0x2297},
        {"perp", 0x22A5}, {"sdot", 0x22C5},
        // Card suits / misc
        {"spades", 0x2660}, {"clubs", 0x2663}, {"hearts", 0x2665}, {"diams", 0x2666},
        {"loz", 0x25CA}, {"crarr", 0x21B5}, {"lceil", 0x2308}, {"rceil", 0x2309},
        {"lfloor", 0x230A}, {"rfloor", 0x230B}, {"lang", 0x2329}, {"rang", 0x232A},
        // Spacing/formatting
        {"zwnj", 0x200C}, {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F},
        {"iexcl", 0xA1}, {"iquest", 0xBF}, {"ordf", 0xAA}, {"ordm", 0xBA},
        {"macr", 0xAF}, {"acute", 0xB4}, {"cedil", 0xB8}, {"uml", 0xA8},
        {"not", 0xAC}, {"brvbar", 0xA6}, {"curren", 0xA4}
    };
    return entities;
}

char HtmlTokenizer::peek(int off) const {
    size_t i = m_pos + off;
    return i < m_src->size() ? (*m_src)[i] : '\0';
}

char HtmlTokenizer::consume() {
    return m_pos < m_src->size() ? (*m_src)[m_pos++] : '\0';
}

bool HtmlTokenizer::startsWith(const std::string& s) const {
    return m_src->compare(m_pos, s.size(), s) == 0;
}

void HtmlTokenizer::skipWS() {
    while (m_pos < m_src->size() && std::isspace((unsigned char)(*m_src)[m_pos]))
        m_pos++;
}

std::string HtmlTokenizer::consumeName() {
    std::string name;
    while (m_pos < m_src->size()) {
        char c = (*m_src)[m_pos];
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':')
            name += consume();
        else break;
    }
    return toLower(name);
}

std::string HtmlTokenizer::decodeEntities(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ) {
        if (raw[i] != '&') { out += raw[i++]; continue; }
        size_t semi = raw.find(';', i + 1);
        if (semi == std::string::npos) { out += raw[i++]; continue; }
        std::string ent = raw.substr(i + 1, semi - i - 1);
        auto named = namedEntities().find(ent);
        if (named != namedEntities().end()) {
            out += utf8(named->second);
            i = semi + 1;
        }
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            try {
                size_t consumed = 0;
                if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                    cp = std::stoul(ent.substr(2), &consumed, 16);
                else
                    cp = std::stoul(ent.substr(1), &consumed, 10);

                size_t expected = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                    ? ent.size() - 2
                    : ent.size() - 1;
                if (consumed != expected || cp == 0 || cp > 0x10FFFF)
                    throw std::invalid_argument("invalid numeric entity");
            } catch (...) {
                out += raw.substr(i, semi - i + 1);
                i = semi + 1;
                continue;
            }
            out += utf8(cp);
            i = semi + 1;
        } else { out += raw[i++]; }
    }
    return out;
}

std::map<std::string, std::string> HtmlTokenizer::consumeAttrs() {
    std::map<std::string, std::string> attrs;
    while (m_pos < m_src->size() &&
           (*m_src)[m_pos] != '>' && !((*m_src)[m_pos] == '/' && peek(1) == '>')) {
        skipWS();
        if (m_pos >= m_src->size() || (*m_src)[m_pos] == '>'
            || ((*m_src)[m_pos] == '/' && peek(1) == '>')) break;
        // Attribute name: allow more characters than just alnum (data-*, aria-*, etc.)
        std::string name;
        while (m_pos < m_src->size()) {
            char c = (*m_src)[m_pos];
            if (c == '=' || c == '>' || c == '/' || std::isspace((unsigned char)c)) break;
            name += consume();
        }
        for (auto& c : name) c = (char)std::tolower((unsigned char)c);
        if (name.empty()) { consume(); continue; }
        std::string value;
        skipWS();
        if (m_pos < m_src->size() && (*m_src)[m_pos] == '=') {
            consume();
            skipWS();
            if (m_pos < m_src->size()) {
                char q = (*m_src)[m_pos];
                if (q == '"' || q == '\'') {
                    consume();
                    while (m_pos < m_src->size() && (*m_src)[m_pos] != q)
                        value += consume();
                    if (m_pos < m_src->size()) consume();
                } else {
                    while (m_pos < m_src->size()
                        && !std::isspace((unsigned char)(*m_src)[m_pos])
                        && (*m_src)[m_pos] != '>' && (*m_src)[m_pos] != '"'
                        && (*m_src)[m_pos] != '\'')
                        value += consume();
                }
            }
        }
        // Decode entities in attribute values.
        value = decodeEntities(value);
        // First attribute wins (HTML5 spec: duplicate attrs are ignored).
        if (attrs.find(name) == attrs.end())
            attrs[name] = value;
    }
    return attrs;
}

void HtmlTokenizer::skipUntil(const std::string& stop) {
    while (m_pos < m_src->size() && !startsWith(stop))
        m_pos++;
}

void HtmlTokenizer::tokenize(const std::string& html, Callback cb) {
    m_src = &html;
    m_pos = 0;

    // Tags whose content is raw text until the matching end tag.
    static const std::set<std::string> rawTags = { "script", "style", "noscript" };

    while (m_pos < m_src->size()) {
        if ((*m_src)[m_pos] != '<') {
            // Collect raw text up to next '<'
            std::string raw;
            while (m_pos < m_src->size() && (*m_src)[m_pos] != '<')
                raw += consume();
            HtmlToken t;
            t.type = TokenType::Text;
            t.data = decodeEntities(raw);
            cb(t);
            continue;
        }

        consume(); // '<'
        if (m_pos >= m_src->size()) break;

        // Comments — emit as Comment token (preserved in DOM for JS access).
        if (startsWith("!--")) {
            m_pos += 3;
            size_t commentStart = m_pos;
            skipUntil("-->");
            std::string commentData = m_src->substr(commentStart, m_pos - commentStart);
            m_pos += 3;
            HtmlToken ct; ct.type = TokenType::Comment; ct.data = commentData; cb(ct);
            continue;
        }
        // Bogus comment: <!anything> that isn't a comment or DOCTYPE.
        if ((*m_src)[m_pos] == '!' && !startsWith("![CDATA[")
            && toLower(m_src->substr(m_pos, 8)) != "!doctype") {
            skipUntil(">");
            if (m_pos < m_src->size()) consume();
            continue;
        }
        // DOCTYPE
        if (toLower(m_src->substr(m_pos, 8)) == "!doctype") {
            skipUntil(">");
            if (m_pos < m_src->size()) consume();
            HtmlToken t; t.type = TokenType::Doctype; cb(t);
            continue;
        }
        // CDATA
        if (startsWith("![CDATA[")) {
            m_pos += 8;
            skipUntil("]]>");
            m_pos += 3;
            continue;
        }

        bool isEnd = false;
        if ((*m_src)[m_pos] == '/') { isEnd = true; consume(); }

        skipWS();
        std::string tagName = consumeName();
        if (tagName.empty()) {
            // Malformed tag — skip to >. If it starts with '?', it's a processing
            // instruction (e.g. <?xml ...?>) — also skip.
            skipUntil(">");
            if (m_pos < m_src->size()) consume();
            continue;
        }

        // HTML5 tag name fixups: common misspellings and obsolete tags.
        if (tagName == "image") tagName = "img";       // <image> → <img>
        if (tagName == "listing") tagName = "pre";     // <listing> → <pre>
        if (tagName == "xmp") tagName = "pre";         // <xmp> → <pre>
        if (tagName == "plaintext") tagName = "pre";   // <plaintext> → <pre>
        if (tagName == "acronym") tagName = "abbr";    // <acronym> → <abbr>
        if (tagName == "dir") tagName = "ul";          // <dir> → <ul>
        if (tagName == "center") tagName = "div";      // <center> = block (styled by CSS)
        if (tagName == "nobr") tagName = "span";       // <nobr> → <span> (white-space handled by CSS)

        auto attrs = isEnd ? std::map<std::string,std::string>{}
                           : consumeAttrs();
        skipWS();
        bool selfClose = false;
        if (m_pos < m_src->size() && (*m_src)[m_pos] == '/') { selfClose = true; consume(); }
        if (m_pos < m_src->size() && (*m_src)[m_pos] == '>') consume();

        HtmlToken t;
        t.type        = isEnd ? TokenType::EndTag : TokenType::StartTag;
        t.name        = tagName;
        t.attrs       = std::move(attrs);
        t.selfClosing = selfClose;
        cb(t);

        // Preserve raw-content text without entity decoding or treating '<' as tags.
        if (!isEnd && rawTags.count(tagName)) {
            size_t rawStart = m_pos;
            skipUntil("</" + tagName);
            if (m_pos > rawStart) {
                HtmlToken rawText;
                rawText.type = TokenType::Text;
                rawText.data = m_src->substr(rawStart, m_pos - rawStart);
                cb(rawText);
            }
        }
    }

    HtmlToken eof; eof.type = TokenType::EOF_; cb(eof);
}
