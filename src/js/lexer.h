#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ── Token types ───────────────────────────────────────────────────────────────
enum class TT : uint16_t {
    // Literals
    Number, String, TemplatePart, TemplateEnd, True, False, Null, Undefined,
    Regex,
    // Identifiers
    Ident,
    // Keywords
    Var, Let, Const, Function, Return, If, Else, While, For, Do, Break, Continue,
    Switch, Case, Default, Throw, Try, Catch, Finally, New, Delete, Typeof, Void,
    Instanceof, In, Of, Import, Export, Class, Extends, Super, This,
    Yield, Async, Await, Debugger, With, Static,
    // Operators
    Plus, Minus, Star, Slash, Percent, StarStar,        // + - * / % **
    Amp, Pipe, Caret, Tilde, LShift, RShift, URShift,  // & | ^ ~ << >> >>>
    EqEq, BangEq, EqEqEq, BangEqEq,                   // == != === !==
    Lt, LtEq, Gt, GtEq,                                // < <= > >=
    AmpAmp, PipePipe, Bang, Question, QuestionQuestion, // && || ! ? ??
    Eq, PlusEq, MinusEq, StarEq, SlashEq, PercentEq,  // = += -= *= /= %=
    StarStarEq, AmpEq, PipeEq, CaretEq,               // **= &= |= ^=
    LShiftEq, RShiftEq, URShiftEq,                    // <<= >>= >>>=
    AmpAmpEq, PipePipeEq, QuestionQuestionEq,          // &&= ||= ??=
    PlusPlus, MinusMinus,                              // ++ --
    Arrow,                                             // =>
    Dot, DotDotDot, Optional,                          // . ... ?.
    // Punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Semicolon, Colon, Comma,
    // Special
    Eof, Error
};

struct Token {
    TT          type  = TT::Eof;
    std::string value; // raw source text for the token
    uint32_t    line  = 1;

    bool is(TT t)               const { return type == t; }
    bool isKeyword()            const { return type >= TT::Var && type <= TT::Static; }
    bool isAssignOp()           const;
    std::string assignOpBase()  const; // "+=" → "+"
};

// ── Lexer ─────────────────────────────────────────────────────────────────────
class Lexer {
public:
    explicit Lexer(std::string src);

    // Tokenize the entire source. Returns the token stream.
    std::vector<Token> tokenize();

private:
    std::string m_src;
    size_t      m_pos  = 0;
    uint32_t    m_line = 1;

    char peek(int ahead = 0) const;
    char advance();
    bool match(char c);
    bool match(const char* s, int len);
    void skipWhitespace();
    void skipLineComment();
    void skipBlockComment();

    Token readNumber();
    Token readString(char quote);
    Token readTemplate();
    Token readIdOrKeyword();
    Token readRegex();

    Token makeToken(TT t, std::string val = {}) const;
};
