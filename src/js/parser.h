#pragma once
#include "js/lexer.h"
#include "js/ast.h"
#include <stdexcept>

struct ParseError : std::runtime_error {
    int line;
    ParseError(const std::string& msg, int ln)
        : std::runtime_error(msg), line(ln) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    Program parse();

private:
    std::vector<Token> m_toks;
    size_t             m_pos = 0;

    // Token navigation
    const Token& cur()       const;
    const Token& peek(int ahead = 1) const;
    bool check(TT t)         const { return cur().is(t); }
    bool check2(TT a, TT b)  const { return check(a) && peek().is(b); }
    Token consume();
    Token expect(TT t, const char* msg);
    bool match(TT t);
    bool match(TT a, TT b);
    void semicolon(); // consume optional semicolon / ASI

    // Statement parsing
    StmtPtr parseStmt();
    StmtPtr parseBlock();
    StmtPtr parseVarDecl(const std::string& kind);
    StmtPtr parseFuncDecl(bool isAsync = false);
    StmtPtr parseClassDecl();
    StmtPtr parseIfStmt();
    StmtPtr parseWhileStmt();
    StmtPtr parseDoWhileStmt();
    StmtPtr parseForStmt();
    StmtPtr parseReturnStmt();
    StmtPtr parseThrowStmt();
    StmtPtr parseTryCatchStmt();
    StmtPtr parseSwitchStmt();
    StmtPtr parseImportDecl();
    StmtPtr parseExportDecl();

    // Expression parsing (Pratt / precedence climbing)
    ExprPtr parseExpr(int minPrec = 0);
    ExprPtr parseAssign();
    ExprPtr parseTernary();
    ExprPtr parseLogicalOr();
    ExprPtr parseLogicalAnd();
    ExprPtr parseBitwiseOr();
    ExprPtr parseBitwiseXor();
    ExprPtr parseBitwiseAnd();
    ExprPtr parseEquality();
    ExprPtr parseRelational();
    ExprPtr parseShift();
    ExprPtr parseAdditive();
    ExprPtr parseMultiplicative();
    ExprPtr parseExponent();
    ExprPtr parseUnary();
    ExprPtr parseUpdate();
    ExprPtr parseCallMember(ExprPtr base);
    ExprPtr parsePrimary();
    ExprPtr parseArrowOrParen();
    ExprPtr parseFuncExpr(bool isAsync, bool isStar, bool isArrow);
    ClassExpr parseClassBody(const std::string& name);
    ExprPtr parseTemplateLiteral();

    // Destructuring patterns
    ExprPtr parseArrayPattern();
    ExprPtr parseObjectPattern();

    // Helpers
    std::vector<std::string> parseParams(std::vector<ExprPtr>& defaults,
                                          std::string& restParam);
    int line() const { return (int)cur().line; }
};
