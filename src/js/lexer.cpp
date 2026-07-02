#include "js/lexer.h"
#include <stdexcept>
#include <unordered_map>
#include <cassert>
#include <algorithm>

static const std::unordered_map<std::string,TT> kKeywords = {
    {"var",TT::Var},{"let",TT::Let},{"const",TT::Const},
    {"function",TT::Function},{"return",TT::Return},
    {"if",TT::If},{"else",TT::Else},{"while",TT::While},
    {"for",TT::For},{"do",TT::Do},{"break",TT::Break},{"continue",TT::Continue},
    {"switch",TT::Switch},{"case",TT::Case},{"default",TT::Default},
    {"throw",TT::Throw},{"try",TT::Try},{"catch",TT::Catch},{"finally",TT::Finally},
    {"new",TT::New},{"delete",TT::Delete},{"typeof",TT::Typeof},{"void",TT::Void},
    {"instanceof",TT::Instanceof},{"in",TT::In},{"of",TT::Of},
    {"import",TT::Import},{"export",TT::Export},
    {"class",TT::Class},{"extends",TT::Extends},{"super",TT::Super},{"this",TT::This},
    {"yield",TT::Yield},{"async",TT::Async},{"await",TT::Await},
    {"debugger",TT::Debugger},{"with",TT::With},{"static",TT::Static},
    {"true",TT::True},{"false",TT::False},{"null",TT::Null},{"undefined",TT::Undefined},
};

Lexer::Lexer(std::string src) : m_src(std::move(src)) {}

char Lexer::peek(int ahead) const {
    size_t p = m_pos + ahead;
    return p < m_src.size() ? m_src[p] : '\0';
}

char Lexer::advance() {
    char c = m_src[m_pos++];
    if (c == '\n') m_line++;
    return c;
}

bool Lexer::match(char c) {
    if (m_pos < m_src.size() && m_src[m_pos] == c) { m_pos++; return true; }
    return false;
}

bool Lexer::match(const char* s, int len) {
    if (m_pos + len > m_src.size()) return false;
    if (m_src.compare(m_pos, len, s, len) != 0) return false;
    m_pos += len;
    return true;
}

void Lexer::skipWhitespace() {
    while (m_pos < m_src.size()) {
        char c = m_src[m_pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(); continue; }
        if (c == '/' && m_pos+1 < m_src.size()) {
            if (m_src[m_pos+1] == '/') { skipLineComment(); continue; }
            if (m_src[m_pos+1] == '*') { skipBlockComment(); continue; }
        }
        break;
    }
}

void Lexer::skipLineComment() {
    while (m_pos < m_src.size() && m_src[m_pos] != '\n') m_pos++;
}

void Lexer::skipBlockComment() {
    m_pos += 2; // skip /*
    while (m_pos + 1 < m_src.size()) {
        if (m_src[m_pos] == '\n') m_line++;
        if (m_src[m_pos] == '*' && m_src[m_pos+1] == '/') { m_pos += 2; return; }
        m_pos++;
    }
}

Token Lexer::makeToken(TT t, std::string val) const {
    return {t, std::move(val), m_line};
}

Token Lexer::readNumber() {
    size_t start = m_pos;
    // Hex
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        m_pos += 2;
        while (m_pos < m_src.size() && std::isxdigit(m_src[m_pos])) m_pos++;
        return makeToken(TT::Number, m_src.substr(start, m_pos - start));
    }
    // Octal
    if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
        m_pos += 2;
        while (m_pos < m_src.size() && m_src[m_pos] >= '0' && m_src[m_pos] <= '7') m_pos++;
        return makeToken(TT::Number, m_src.substr(start, m_pos - start));
    }
    // Binary
    if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        m_pos += 2;
        while (m_pos < m_src.size() && (m_src[m_pos] == '0' || m_src[m_pos] == '1')) m_pos++;
        return makeToken(TT::Number, m_src.substr(start, m_pos - start));
    }
    // Decimal
    while (m_pos < m_src.size() && (std::isdigit(m_src[m_pos]) || m_src[m_pos] == '_')) m_pos++;
    if (m_pos < m_src.size() && m_src[m_pos] == '.') {
        m_pos++;
        while (m_pos < m_src.size() && std::isdigit(m_src[m_pos])) m_pos++;
    }
    if (m_pos < m_src.size() && (m_src[m_pos] == 'e' || m_src[m_pos] == 'E')) {
        m_pos++;
        if (m_pos < m_src.size() && (m_src[m_pos] == '+' || m_src[m_pos] == '-')) m_pos++;
        while (m_pos < m_src.size() && std::isdigit(m_src[m_pos])) m_pos++;
    }
    std::string raw = m_src.substr(start, m_pos - start);
    // Remove numeric separators
    raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());
    return makeToken(TT::Number, raw);
}

static char hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

Token Lexer::readString(char quote) {
    std::string out;
    while (m_pos < m_src.size()) {
        char c = m_src[m_pos];
        if (c == quote) { m_pos++; break; }
        if (c == '\\') {
            m_pos++;
            if (m_pos >= m_src.size()) break;
            char esc = advance();
            switch (esc) {
            case 'n':  out += '\n'; break;
            case 't':  out += '\t'; break;
            case 'r':  out += '\r'; break;
            case '\\': out += '\\'; break;
            case '\'': out += '\''; break;
            case '"':  out += '"';  break;
            case '`':  out += '`';  break;
            case '0':  out += '\0'; break;
            case 'u': {
                // \uXXXX or \u{XXXX}
                if (m_pos < m_src.size() && m_src[m_pos] == '{') {
                    m_pos++;
                    uint32_t cp = 0;
                    while (m_pos < m_src.size() && m_src[m_pos] != '}')
                        cp = cp * 16 + hexDigit(advance());
                    if (m_pos < m_src.size()) m_pos++; // }
                    // Encode as UTF-8
                    if (cp < 0x80) { out += (char)cp; }
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                } else if (m_pos + 3 < m_src.size()) {
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; i++) cp = cp*16 + hexDigit(advance());
                    if (cp < 0x80) { out += (char)cp; }
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            }
            case 'x': {
                if (m_pos + 1 < m_src.size()) {
                    char hi = advance(), lo = advance();
                    out += (char)(hexDigit(hi) * 16 + hexDigit(lo));
                }
                break;
            }
            default: out += esc; break;
            }
        } else {
            if (c == '\n') m_line++;
            out += c; m_pos++;
        }
    }
    return makeToken(TT::String, out);
}

Token Lexer::readTemplate() {
    // We're after the opening ` or after }
    std::string out;
    while (m_pos < m_src.size()) {
        char c = m_src[m_pos];
        if (c == '`') {
            m_pos++;
            return makeToken(TT::TemplateEnd, out);
        }
        if (c == '$' && m_pos+1 < m_src.size() && m_src[m_pos+1] == '{') {
            m_pos += 2;
            return makeToken(TT::TemplatePart, out);
        }
        if (c == '\\') {
            m_pos++;
            if (m_pos < m_src.size()) {
                char esc = advance();
                switch(esc) {
                case 'n': out+='\n'; break; case 't': out+='\t'; break;
                case 'r': out+='\r'; break; case '\\': out+='\\'; break;
                case '`': out+='`'; break;  default: out+=esc; break;
                }
            }
        } else {
            if (c == '\n') m_line++;
            out += c; m_pos++;
        }
    }
    return makeToken(TT::TemplateEnd, out);
}

Token Lexer::readIdOrKeyword() {
    size_t start = m_pos;
    while (m_pos < m_src.size()) {
        char c = m_src[m_pos];
        if (std::isalnum(c) || c == '_' || c == '$' || (unsigned char)c > 127) m_pos++;
        else break;
    }
    std::string id = m_src.substr(start, m_pos - start);
    auto it = kKeywords.find(id);
    if (it != kKeywords.end()) return makeToken(it->second, id);
    return makeToken(TT::Ident, id);
}

Token Lexer::readRegex() {
    // Called when we know / starts a regex (after context check)
    size_t start = m_pos - 1; // include the /
    bool inClass = false;
    while (m_pos < m_src.size()) {
        char c = m_src[m_pos++];
        if (c == '\\') { if (m_pos < m_src.size()) m_pos++; continue; }
        if (c == '[') { inClass = true; continue; }
        if (c == ']') { inClass = false; continue; }
        if (!inClass && c == '/') break;
        if (c == '\n') break; // unterminated
    }
    // Flags
    while (m_pos < m_src.size() && std::isalpha(m_src[m_pos])) m_pos++;
    return makeToken(TT::Regex, m_src.substr(start, m_pos - start));
}

bool Token::isAssignOp() const {
    return type == TT::Eq       || type == TT::PlusEq    || type == TT::MinusEq ||
           type == TT::StarEq   || type == TT::SlashEq   || type == TT::PercentEq ||
           type == TT::StarStarEq || type == TT::AmpEq   || type == TT::PipeEq ||
           type == TT::CaretEq  || type == TT::LShiftEq  || type == TT::RShiftEq ||
           type == TT::URShiftEq || type == TT::AmpAmpEq || type == TT::PipePipeEq ||
           type == TT::QuestionQuestionEq;
}

std::string Token::assignOpBase() const {
    switch (type) {
    case TT::PlusEq:    return "+";  case TT::MinusEq:   return "-";
    case TT::StarEq:    return "*";  case TT::SlashEq:   return "/";
    case TT::PercentEq: return "%";  case TT::StarStarEq:return "**";
    case TT::AmpEq:     return "&";  case TT::PipeEq:    return "|";
    case TT::CaretEq:   return "^";  case TT::LShiftEq:  return "<<";
    case TT::RShiftEq:  return ">>";case TT::URShiftEq:  return ">>>";
    case TT::AmpAmpEq:  return "&&"; case TT::PipePipeEq:return "||";
    case TT::QuestionQuestionEq: return "??";
    default: return "";
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> toks;
    // Track last meaningful token for regex vs division disambiguation
    TT last = TT::Semicolon;

    auto canStartExpr = [](TT t) {
        return t == TT::Semicolon || t == TT::LParen  || t == TT::LBracket ||
               t == TT::LBrace   || t == TT::Comma    || t == TT::Colon    ||
               t == TT::Return   || t == TT::Typeof   || t == TT::New      ||
               t == TT::Delete   || t == TT::Throw    || t == TT::Void     ||
               t == TT::Bang     || t == TT::Tilde    || t == TT::Plus     ||
               t == TT::Minus    || t == TT::Arrow    || t == TT::Eq       ||
               t == TT::PlusEq   || t == TT::MinusEq  || t == TT::StarEq   ||
               t == TT::Case     || t == TT::Eof;
    };

    while (true) {
        skipWhitespace();
        if (m_pos >= m_src.size()) { toks.push_back(makeToken(TT::Eof)); break; }

        char c = m_src[m_pos++];
        Token tok;

        switch (c) {
        case '(': tok = makeToken(TT::LParen);   break;
        case ')': tok = makeToken(TT::RParen);   break;
        case '{': tok = makeToken(TT::LBrace);   break;
        case '}': tok = makeToken(TT::RBrace);   break;
        case '[': tok = makeToken(TT::LBracket); break;
        case ']': tok = makeToken(TT::RBracket); break;
        case ';': tok = makeToken(TT::Semicolon);break;
        case ',': tok = makeToken(TT::Comma);    break;
        case ':': tok = makeToken(TT::Colon);    break;
        case '~': tok = makeToken(TT::Tilde);    break;
        case '`': tok = readTemplate();          break;

        case '.':
            if (peek() == '.' && peek(1) == '.') { m_pos+=2; tok = makeToken(TT::DotDotDot); }
            else tok = makeToken(TT::Dot);
            break;

        case '+':
            if (match('+'))      tok = makeToken(TT::PlusPlus);
            else if (match('=')) tok = makeToken(TT::PlusEq);
            else                 tok = makeToken(TT::Plus);
            break;
        case '-':
            if (match('-'))      tok = makeToken(TT::MinusMinus);
            else if (match('=')) tok = makeToken(TT::MinusEq);
            else                 tok = makeToken(TT::Minus);
            break;
        case '*':
            if (match('*')) { tok = match('=') ? makeToken(TT::StarStarEq) : makeToken(TT::StarStar); }
            else              tok = match('=') ? makeToken(TT::StarEq)     : makeToken(TT::Star);
            break;
        case '/':
            if (canStartExpr(last)) { m_pos--; tok = readRegex(); break; }
            tok = match('=') ? makeToken(TT::SlashEq) : makeToken(TT::Slash);
            break;
        case '%': tok = match('=') ? makeToken(TT::PercentEq) : makeToken(TT::Percent); break;
        case '&':
            if (match('&')) { tok = match('=') ? makeToken(TT::AmpAmpEq)  : makeToken(TT::AmpAmp); }
            else              tok = match('=') ? makeToken(TT::AmpEq)      : makeToken(TT::Amp);
            break;
        case '|':
            if (match('|')) { tok = match('=') ? makeToken(TT::PipePipeEq): makeToken(TT::PipePipe); }
            else              tok = match('=') ? makeToken(TT::PipeEq)     : makeToken(TT::Pipe);
            break;
        case '^': tok = match('=') ? makeToken(TT::CaretEq) : makeToken(TT::Caret); break;
        case '!':
            if (match('=')) { tok = match('=') ? makeToken(TT::BangEqEq) : makeToken(TT::BangEq); }
            else              tok = makeToken(TT::Bang);
            break;
        case '=':
            if (match('>'))      tok = makeToken(TT::Arrow);
            else if (match('=')) tok = match('=') ? makeToken(TT::EqEqEq) : makeToken(TT::EqEq);
            else                 tok = makeToken(TT::Eq);
            break;
        case '<':
            if (match('<'))      tok = match('=') ? makeToken(TT::LShiftEq) : makeToken(TT::LShift);
            else                 tok = match('=') ? makeToken(TT::LtEq)     : makeToken(TT::Lt);
            break;
        case '>':
            if (match('>')) {
                if (match('>')) tok = match('=') ? makeToken(TT::URShiftEq) : makeToken(TT::URShift);
                else            tok = match('=') ? makeToken(TT::RShiftEq)  : makeToken(TT::RShift);
            } else              tok = match('=') ? makeToken(TT::GtEq)      : makeToken(TT::Gt);
            break;
        case '?':
            if (match('?')) { tok = match('=') ? makeToken(TT::QuestionQuestionEq) : makeToken(TT::QuestionQuestion); }
            else if (match('.')) tok = makeToken(TT::Optional);
            else                 tok = makeToken(TT::Question);
            break;

        case '\'': case '"': tok = readString(c); break;

        default:
            m_pos--; // put back
            if (std::isdigit(c) || (c == '.' && std::isdigit(peek(1)))) {
                tok = readNumber();
            } else if (std::isalpha(c) || c == '_' || c == '$' || (unsigned char)c > 127) {
                tok = readIdOrKeyword();
            } else {
                tok = makeToken(TT::Error, std::string(1, c));
                m_pos++;
            }
            break;
        }

        last = tok.type;
        toks.push_back(std::move(tok));
        if (toks.back().type == TT::Eof) break;
    }

    return toks;
}
