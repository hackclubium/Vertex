#include "js/parser.h"
#include <stdexcept>
#include <cassert>

static std::string TokenText(const Token& token) {
    if (!token.value.empty()) return token.value;

    switch (token.type) {
    case TT::Plus: return "+";
    case TT::Minus: return "-";
    case TT::Star: return "*";
    case TT::Slash: return "/";
    case TT::Percent: return "%";
    case TT::StarStar: return "**";
    case TT::Amp: return "&";
    case TT::Pipe: return "|";
    case TT::Caret: return "^";
    case TT::Tilde: return "~";
    case TT::LShift: return "<<";
    case TT::RShift: return ">>";
    case TT::URShift: return ">>>";
    case TT::EqEq: return "==";
    case TT::BangEq: return "!=";
    case TT::EqEqEq: return "===";
    case TT::BangEqEq: return "!==";
    case TT::Lt: return "<";
    case TT::LtEq: return "<=";
    case TT::Gt: return ">";
    case TT::GtEq: return ">=";
    case TT::AmpAmp: return "&&";
    case TT::PipePipe: return "||";
    case TT::Bang: return "!";
    case TT::QuestionQuestion: return "??";
    case TT::Eq: return "=";
    case TT::PlusEq: return "+=";
    case TT::MinusEq: return "-=";
    case TT::StarEq: return "*=";
    case TT::SlashEq: return "/=";
    case TT::PercentEq: return "%=";
    case TT::StarStarEq: return "**=";
    case TT::AmpEq: return "&=";
    case TT::PipeEq: return "|=";
    case TT::CaretEq: return "^=";
    case TT::LShiftEq: return "<<=";
    case TT::RShiftEq: return ">>=";
    case TT::URShiftEq: return ">>>=";
    case TT::AmpAmpEq: return "&&=";
    case TT::PipePipeEq: return "||=";
    case TT::QuestionQuestionEq: return "??=";
    case TT::PlusPlus: return "++";
    case TT::MinusMinus: return "--";
    case TT::Instanceof: return "instanceof";
    case TT::In: return "in";
    case TT::Var: return "var";
    case TT::Let: return "let";
    case TT::Const: return "const";
    case TT::Function: return "function";
    case TT::Return: return "return";
    case TT::If: return "if";
    case TT::Else: return "else";
    case TT::While: return "while";
    case TT::For: return "for";
    case TT::Do: return "do";
    case TT::Break: return "break";
    case TT::Continue: return "continue";
    case TT::Switch: return "switch";
    case TT::Case: return "case";
    case TT::Default: return "default";
    case TT::Throw: return "throw";
    case TT::Try: return "try";
    case TT::Catch: return "catch";
    case TT::Finally: return "finally";
    case TT::New: return "new";
    case TT::Delete: return "delete";
    case TT::Typeof: return "typeof";
    case TT::Void: return "void";
    case TT::Of: return "of";
    case TT::Import: return "import";
    case TT::Export: return "export";
    case TT::Class: return "class";
    case TT::Extends: return "extends";
    case TT::Super: return "super";
    case TT::This: return "this";
    case TT::Yield: return "yield";
    case TT::Async: return "async";
    case TT::Await: return "await";
    case TT::Debugger: return "debugger";
    case TT::With: return "with";
    case TT::Static: return "static";
    case TT::True: return "true";
    case TT::False: return "false";
    case TT::Null: return "null";
    case TT::Undefined: return "undefined";
    default: return "";
    }
}

Parser::Parser(std::vector<Token> tokens) : m_toks(std::move(tokens)) {}

const Token& Parser::cur() const {
    static Token eof{TT::Eof};
    return m_pos < m_toks.size() ? m_toks[m_pos] : eof;
}
const Token& Parser::peek(int ahead) const {
    static Token eof{TT::Eof};
    size_t p = m_pos + ahead;
    return p < m_toks.size() ? m_toks[p] : eof;
}
Token Parser::consume() {
    return m_pos < m_toks.size() ? m_toks[m_pos++] : Token{TT::Eof};
}
Token Parser::expect(TT t, const char* msg) {
    if (!check(t)) throw ParseError(msg, line());
    return consume();
}
bool Parser::match(TT t) { if (check(t)) { consume(); return true; } return false; }
bool Parser::match(TT a, TT b) { if (check(a) && peek().is(b)) { consume(); consume(); return true; } return false; }

void Parser::semicolon() {
    if (match(TT::Semicolon)) return;
    // ASI: allowed before } or at EOF or after line break (tokens track lines)
    if (check(TT::RBrace) || check(TT::Eof)) return;
    if (m_pos > 0 && m_toks[m_pos-1].line < cur().line) return;
    // Optional: try to continue anyway
}

Program Parser::parse() {
    Program prog;
    while (!check(TT::Eof)) {
        size_t before = m_pos;
        try {
            prog.body.push_back(parseStmt());
        } catch (const ParseError& e) {
            // Skip to next semicolon/newline for recovery
            while (!check(TT::Eof) && !check(TT::Semicolon) && !check(TT::RBrace))
                consume();
            match(TT::Semicolon);
        }
        // Guarantee forward progress. Without this, a stray '}' (which the
        // recovery loop stops at but does not consume) makes parseStmt re-parse
        // the same token forever — an infinite loop on malformed input.
        if (m_pos == before) consume();
    }
    return prog;
}

// ── Statements ────────────────────────────────────────────────────────────────

StmtPtr Parser::parseStmt() {
    int ln = line();

    // Labeled statement
    if (check(TT::Ident) && peek().is(TT::Colon)) {
        std::string lbl = consume().value;
        consume(); // :
        auto body = parseStmt();
        return std::make_unique<Stmt>(LabeledStmt{lbl, std::move(body)}, ln);
    }

    if (check(TT::LBrace))    return parseBlock();
    if (check(TT::Semicolon)) { consume(); return std::make_unique<Stmt>(EmptyStmt{}, ln); }

    if (check(TT::Var) || check(TT::Let) || check(TT::Const))
        return parseVarDecl(consume().value);

    if (check(TT::Function)) return parseFuncDecl();
    if (check(TT::Async) && peek().is(TT::Function)) {
        consume(); // async
        return parseFuncDecl(true);
    }
    if (check(TT::Class))  return parseClassDecl();
    if (check(TT::If))     return parseIfStmt();
    if (check(TT::While))  return parseWhileStmt();
    if (check(TT::Do))     return parseDoWhileStmt();
    if (check(TT::For))    return parseForStmt();
    if (check(TT::Return)) return parseReturnStmt();
    if (check(TT::Throw))  return parseThrowStmt();
    if (check(TT::Try))    return parseTryCatchStmt();
    if (check(TT::Switch)) return parseSwitchStmt();
    if (check(TT::Import) && !peek().is(TT::LParen)) return parseImportDecl();
    if (check(TT::Export)) return parseExportDecl();

    if (check(TT::Break)) {
        consume();
        std::string lbl;
        if (check(TT::Ident) && m_toks[m_pos-1].line == cur().line) lbl = consume().value;
        semicolon();
        return std::make_unique<Stmt>(BreakStmt{lbl}, ln);
    }
    if (check(TT::Continue)) {
        consume();
        std::string lbl;
        if (check(TT::Ident) && m_toks[m_pos-1].line == cur().line) lbl = consume().value;
        semicolon();
        return std::make_unique<Stmt>(ContinueStmt{lbl}, ln);
    }
    if (check(TT::Debugger)) {
        consume(); semicolon();
        return std::make_unique<Stmt>(DebuggerStmt{}, ln);
    }

    // Expression statement
    auto expr = parseExpr();
    semicolon();
    return std::make_unique<Stmt>(ExprStmt{std::move(expr)}, ln);
}

StmtPtr Parser::parseBlock() {
    int ln = line();
    expect(TT::LBrace, "expected '{'");
    BlockStmt blk;
    while (!check(TT::RBrace) && !check(TT::Eof)) {
        size_t before = m_pos;
        blk.body.push_back(parseStmt());
        if (m_pos == before) consume();   // guarantee progress
    }
    expect(TT::RBrace, "expected '}'");
    return std::make_unique<Stmt>(std::move(blk), ln);
}

StmtPtr Parser::parseVarDecl(const std::string& kind) {
    int ln = line();
    VarDecl vd; vd.kind = kind;
    do {
        ExprPtr target;
        if (check(TT::LBracket)) target = parseArrayPattern();
        else if (check(TT::LBrace)) target = parseObjectPattern();
        else {
            std::string name = expect(TT::Ident, "expected variable name").value;
            target = std::make_unique<Expr>(IdentExpr{name}, ln);
        }
        ExprPtr init;
        if (match(TT::Eq)) init = parseAssign();
        vd.decls.push_back({std::move(target), std::move(init)});
    } while (match(TT::Comma));
    semicolon();
    return std::make_unique<Stmt>(std::move(vd), ln);
}

StmtPtr Parser::parseFuncDecl(bool isAsync) {
    int ln = line();
    expect(TT::Function, "expected 'function'");
    bool isStar = match(TT::Star);
    std::string name = expect(TT::Ident, "expected function name").value;
    auto fptr = parseFuncExpr(isAsync, isStar, false);
    auto& fnExpr = std::get<FuncExpr>(fptr->v);
    fnExpr.name = name;
    FuncDecl fd; fd.name = name; fd.fn = std::move(fnExpr);
    return std::make_unique<Stmt>(std::move(fd), ln);
}

StmtPtr Parser::parseClassDecl() {
    int ln = line();
    expect(TT::Class, "expected 'class'");
    std::string name = check(TT::Ident) ? consume().value : "";
    auto cls = parseClassBody(name);
    ClassDecl cd; cd.name = name; cd.cls = std::move(cls);
    return std::make_unique<Stmt>(std::move(cd), ln);
}

StmtPtr Parser::parseIfStmt() {
    int ln = line();
    expect(TT::If, "expected 'if'");
    expect(TT::LParen, "expected '('");
    auto cond = parseExpr();
    expect(TT::RParen, "expected ')'");
    auto then = parseStmt();
    StmtPtr els;
    if (match(TT::Else)) els = parseStmt();
    return std::make_unique<Stmt>(IfStmt{std::move(cond), std::move(then), std::move(els)}, ln);
}

StmtPtr Parser::parseWhileStmt() {
    int ln = line();
    expect(TT::While, "expected 'while'");
    expect(TT::LParen, "expected '('");
    auto cond = parseExpr();
    expect(TT::RParen, "expected ')'");
    auto body = parseStmt();
    return std::make_unique<Stmt>(WhileStmt{std::move(cond), std::move(body)}, ln);
}

StmtPtr Parser::parseDoWhileStmt() {
    int ln = line();
    expect(TT::Do, "expected 'do'");
    auto body = parseStmt();
    expect(TT::While, "expected 'while'");
    expect(TT::LParen, "expected '('");
    auto cond = parseExpr();
    expect(TT::RParen, "expected ')'");
    semicolon();
    return std::make_unique<Stmt>(DoWhileStmt{std::move(body), std::move(cond)}, ln);
}

StmtPtr Parser::parseForStmt() {
    int ln = line();
    expect(TT::For, "expected 'for'");
    bool isAwait = check(TT::Await) ? (consume(), true) : false;
    expect(TT::LParen, "expected '('");

    // Check for for-in / for-of
    if (check(TT::Var) || check(TT::Let) || check(TT::Const)) {
        std::string kind = consume().value;
        ExprPtr left;
        if (check(TT::LBracket)) left = parseArrayPattern();
        else if (check(TT::LBrace)) left = parseObjectPattern();
        else {
            std::string nm = expect(TT::Ident, "expected name").value;
            left = std::make_unique<Expr>(IdentExpr{nm}, ln);
        }
        if (check(TT::In)) {
            consume();
            auto right = parseExpr(); expect(TT::RParen,"expected ')'");
            auto body = parseStmt();
            return std::make_unique<Stmt>(ForInStmt{kind,std::move(left),std::move(right),std::move(body)},ln);
        }
        if (check(TT::Of)) {
            consume();
            auto right = parseAssign(); expect(TT::RParen,"expected ')'");
            auto body = parseStmt();
            return std::make_unique<Stmt>(ForOfStmt{kind,std::move(left),std::move(right),std::move(body),isAwait},ln);
        }
        // Normal for: wrap in VarDecl
        ExprPtr init_val;
        if (match(TT::Eq)) init_val = parseAssign();
        VarDecl vd; vd.kind = kind; vd.decls.push_back({std::move(left), std::move(init_val)});
        while (match(TT::Comma)) {
            std::string nm2 = expect(TT::Ident,"expected name").value;
            ExprPtr iv2; if (match(TT::Eq)) iv2 = parseAssign();
            vd.decls.push_back({std::make_unique<Expr>(IdentExpr{nm2},ln), std::move(iv2)});
        }
        expect(TT::Semicolon, "expected ';'");
        auto initStmt = std::make_unique<Stmt>(std::move(vd), ln);
        ExprPtr cond, update;
        if (!check(TT::Semicolon)) cond = parseExpr();
        expect(TT::Semicolon, "expected ';'");
        if (!check(TT::RParen)) update = parseExpr();
        expect(TT::RParen, "expected ')'");
        auto body = parseStmt();
        return std::make_unique<Stmt>(ForStmt{std::move(initStmt),std::move(cond),std::move(update),std::move(body)},ln);
    }

    // No var/let/const
    StmtPtr initStmt;
    if (!check(TT::Semicolon)) {
        auto initExpr = parseExpr();
        // Check for-in/of from expression
        if (check(TT::In)) {
            consume();
            auto right = parseExpr(); expect(TT::RParen,"expected ')'");
            auto body = parseStmt();
            return std::make_unique<Stmt>(ForInStmt{"",std::move(initExpr),std::move(right),std::move(body)},ln);
        }
        if (check(TT::Of)) {
            consume();
            auto right = parseAssign(); expect(TT::RParen,"expected ')'");
            auto body = parseStmt();
            return std::make_unique<Stmt>(ForOfStmt{"",std::move(initExpr),std::move(right),std::move(body),isAwait},ln);
        }
        initStmt = std::make_unique<Stmt>(ExprStmt{std::move(initExpr)}, ln);
    }
    expect(TT::Semicolon, "expected ';'");
    ExprPtr cond, update;
    if (!check(TT::Semicolon)) cond = parseExpr();
    expect(TT::Semicolon, "expected ';'");
    if (!check(TT::RParen)) update = parseExpr();
    expect(TT::RParen, "expected ')'");
    auto body = parseStmt();
    return std::make_unique<Stmt>(ForStmt{std::move(initStmt),std::move(cond),std::move(update),std::move(body)},ln);
}

StmtPtr Parser::parseReturnStmt() {
    int ln = line();
    expect(TT::Return, "expected 'return'");
    ExprPtr expr;
    // No expr if ; or } or newline
    if (!check(TT::Semicolon) && !check(TT::RBrace) && !check(TT::Eof)) {
        if (m_pos > 0 && m_toks[m_pos-1].line == cur().line)
            expr = parseExpr();
    }
    semicolon();
    return std::make_unique<Stmt>(ReturnStmt{std::move(expr)}, ln);
}

StmtPtr Parser::parseThrowStmt() {
    int ln = line();
    expect(TT::Throw, "expected 'throw'");
    auto expr = parseExpr();
    semicolon();
    return std::make_unique<Stmt>(ThrowStmt{std::move(expr)}, ln);
}

StmtPtr Parser::parseTryCatchStmt() {
    int ln = line();
    expect(TT::Try, "expected 'try'");
    auto tryBody = parseBlock();
    std::string catchParam;
    ExprPtr catchPattern;
    StmtPtr catchBody, finallyBody;
    if (check(TT::Catch)) {
        consume();
        if (match(TT::LParen)) {
            if (check(TT::LBracket)) catchPattern = parseArrayPattern();
            else if (check(TT::LBrace)) catchPattern = parseObjectPattern();
            else catchParam = expect(TT::Ident,"expected catch param").value;
            expect(TT::RParen,"expected ')'");
        }
        catchBody = parseBlock();
    }
    if (check(TT::Finally)) { consume(); finallyBody = parseBlock(); }
    return std::make_unique<Stmt>(TryCatchStmt{
        std::move(tryBody), catchParam, std::move(catchPattern),
        std::move(catchBody), std::move(finallyBody)}, ln);
}

StmtPtr Parser::parseSwitchStmt() {
    int ln = line();
    expect(TT::Switch,"expected 'switch'");
    expect(TT::LParen,"expected '('");
    auto cond = parseExpr();
    expect(TT::RParen,"expected ')'");
    expect(TT::LBrace,"expected '{'");
    SwitchStmt sw; sw.cond = std::move(cond);
    while (!check(TT::RBrace) && !check(TT::Eof)) {
        SwitchStmt::Case c;
        if (check(TT::Case)) {
            consume(); c.test = parseExpr(); expect(TT::Colon,"expected ':'");
        } else {
            expect(TT::Default,"expected 'default'"); expect(TT::Colon,"expected ':'");
        }
        while (!check(TT::Case) && !check(TT::Default) && !check(TT::RBrace) && !check(TT::Eof))
            c.body.push_back(parseStmt());
        sw.cases.push_back(std::move(c));
    }
    expect(TT::RBrace,"expected '}'");
    return std::make_unique<Stmt>(std::move(sw), ln);
}

StmtPtr Parser::parseImportDecl() {
    int ln = line();
    expect(TT::Import,"expected 'import'");
    ImportDecl imp;
    if (check(TT::String)) {
        imp.source = consume().value; semicolon();
        return std::make_unique<Stmt>(std::move(imp), ln);
    }
    if (check(TT::Ident)) {
        imp.defaultName = consume().value;
        if (!match(TT::Comma)) { imp.source = expect(TT::String,"").value; semicolon(); return std::make_unique<Stmt>(std::move(imp),ln); }
    }
    if (check(TT::Star)) {
        consume();
        // "as" is a contextual keyword, tokenized as Ident
        if (check(TT::Ident) && cur().value == "as") consume(); // consume 'as'
        imp.namespaceName = expect(TT::Ident,"").value; imp.namespace_ = true;
    } else if (check(TT::LBrace)) {
        consume();
        while (!check(TT::RBrace) && !check(TT::Eof)) {
            std::string imported = expect(TT::Ident,"").value;
            std::string local = imported;
            if (check(TT::Ident) && cur().value == "as") { consume(); local = expect(TT::Ident,"").value; }
            imp.specifiers.push_back({imported, local});
            match(TT::Comma);
        }
        expect(TT::RBrace,"expected '}'");
    }
    // "from"
    if (check(TT::Ident) && cur().value == "from") consume();
    imp.source = check(TT::String) ? consume().value : "";
    semicolon();
    return std::make_unique<Stmt>(std::move(imp), ln);
}

StmtPtr Parser::parseExportDecl() {
    int ln = line();
    expect(TT::Export,"expected 'export'");
    ExportDecl exp;
    if (match(TT::Default)) {
        exp.default_ = true;
        if (check(TT::Function) || (check(TT::Async) && peek().is(TT::Function)))
            exp.decl = parseFuncDecl(check(TT::Async) ? (consume(),true) : false);
        else if (check(TT::Class))
            exp.decl = parseClassDecl();
        else {
            exp.defaultExpr = parseAssign();
            semicolon();
        }
    } else if (check(TT::Var)||check(TT::Let)||check(TT::Const)) {
        exp.decl = parseVarDecl(consume().value);
    } else if (check(TT::Function)) {
        exp.decl = parseFuncDecl();
    } else if (check(TT::Class)) {
        exp.decl = parseClassDecl();
    }
    return std::make_unique<Stmt>(std::move(exp), ln);
}

// ── Expressions ───────────────────────────────────────────────────────────────

ExprPtr Parser::parseExpr(int) {
    auto e = parseAssign();
    if (!check(TT::Comma)) return e;
    SequenceExpr seq;
    seq.exprs.push_back(std::move(e));
    while (match(TT::Comma)) seq.exprs.push_back(parseAssign());
    return std::make_unique<Expr>(std::move(seq), line());
}

ExprPtr Parser::parseAssign() {
    int ln = line();
    auto left = parseTernary();

    if (cur().isAssignOp()) {
        std::string op = TokenText(consume());
        auto right = parseAssign();
        if (op == "=") return std::make_unique<Expr>(AssignExpr{"=", std::move(left), std::move(right)}, ln);
        // Compound: x += y  →  x = x + y
        std::string base = m_toks[m_pos-2].assignOpBase();
        // Clone left for the binary op (simple: wrap in a new BinaryExpr)
        // We can't easily clone, so store compound assign directly
        return std::make_unique<Expr>(AssignExpr{op, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ExprPtr Parser::parseTernary() {
    int ln = line();
    auto cond = parseLogicalOr();
    if (!match(TT::Question)) return cond;
    auto yes = parseAssign();
    expect(TT::Colon,"expected ':'");
    auto no = parseAssign();
    return std::make_unique<Expr>(TernaryExpr{std::move(cond),std::move(yes),std::move(no)}, ln);
}

ExprPtr Parser::parseLogicalOr() {
    int ln = line();
    auto left = parseLogicalAnd();
    while (check(TT::PipePipe) || check(TT::QuestionQuestion)) {
        std::string op = TokenText(consume());
        auto right = parseLogicalAnd();
        left = std::make_unique<Expr>(LogicalExpr{op, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ExprPtr Parser::parseLogicalAnd() {
    int ln = line();
    auto left = parseBitwiseOr();
    while (check(TT::AmpAmp)) {
        consume();
        auto right = parseBitwiseOr();
        left = std::make_unique<Expr>(LogicalExpr{"&&", std::move(left), std::move(right)}, ln);
    }
    return left;
}

ExprPtr Parser::parseBitwiseOr() {
    int ln = line();
    auto e = parseBitwiseXor();
    while (check(TT::Pipe)) {
        consume(); auto r = parseBitwiseXor();
        e = std::make_unique<Expr>(BinaryExpr{"|", std::move(e), std::move(r)}, ln);
    }
    return e;
}

ExprPtr Parser::parseBitwiseXor() {
    int ln = line();
    auto e = parseBitwiseAnd();
    while (check(TT::Caret)) {
        consume(); auto r = parseBitwiseAnd();
        e = std::make_unique<Expr>(BinaryExpr{"^", std::move(e), std::move(r)}, ln);
    }
    return e;
}

ExprPtr Parser::parseBitwiseAnd() {
    int ln = line();
    auto e = parseEquality();
    while (check(TT::Amp)) {
        consume(); auto r = parseEquality();
        e = std::make_unique<Expr>(BinaryExpr{"&", std::move(e), std::move(r)}, ln);
    }
    return e;
}

ExprPtr Parser::parseEquality() {
    int ln = line();
    auto e = parseRelational();
    while (check(TT::EqEq)||check(TT::BangEq)||check(TT::EqEqEq)||check(TT::BangEqEq)) {
        std::string op = TokenText(consume());
        auto r = parseRelational();
        e = std::make_unique<Expr>(BinaryExpr{op, std::move(e), std::move(r)}, ln);
    }
    return e;
}

ExprPtr Parser::parseRelational() {
    int ln = line();
    auto e = parseShift();
    while (check(TT::Lt)||check(TT::LtEq)||check(TT::Gt)||check(TT::GtEq)||
           check(TT::Instanceof)||check(TT::In)) {
        std::string op = TokenText(consume());
        auto r = parseShift();
        e = std::make_unique<Expr>(BinaryExpr{op, std::move(e), std::move(r)}, ln);
    }
    return e;
}

ExprPtr Parser::parseShift() {
    int ln = line();
    auto e = parseAdditive();
    while (check(TT::LShift)||check(TT::RShift)||check(TT::URShift)) {
        std::string op = TokenText(consume());
        auto r = parseAdditive();
        e = std::make_unique<Expr>(BinaryExpr{op, std::move(e), std::move(r)}, ln);
    }
    return e;
}

ExprPtr Parser::parseAdditive() {
    int ln = line();
    auto e = parseMultiplicative();
    while (check(TT::Plus)||check(TT::Minus)) {
        std::string op = TokenText(consume());
        auto r = parseMultiplicative();
        e = std::make_unique<Expr>(BinaryExpr{op, std::move(e), std::move(r)}, ln);
    }
    return e;
}

ExprPtr Parser::parseMultiplicative() {
    int ln = line();
    auto e = parseExponent();
    while (check(TT::Star)||check(TT::Slash)||check(TT::Percent)) {
        std::string op = TokenText(consume());
        auto r = parseExponent();
        e = std::make_unique<Expr>(BinaryExpr{op, std::move(e), std::move(r)}, ln);
    }
    return e;
}

ExprPtr Parser::parseExponent() {
    int ln = line();
    auto base = parseUnary();
    if (check(TT::StarStar)) {
        consume();
        auto exp = parseExponent(); // right-associative
        return std::make_unique<Expr>(BinaryExpr{"**", std::move(base), std::move(exp)}, ln);
    }
    return base;
}

ExprPtr Parser::parseUnary() {
    int ln = line();
    if (check(TT::Bang)||check(TT::Tilde)||check(TT::Minus)||check(TT::Plus)) {
        std::string op = TokenText(consume());
        auto e = parseUnary();
        return std::make_unique<Expr>(UnaryExpr{op, std::move(e)}, ln);
    }
    if (check(TT::Typeof)) { consume(); auto e = parseUnary(); return std::make_unique<Expr>(UnaryExpr{"typeof", std::move(e)}, ln); }
    if (check(TT::Void))   { consume(); auto e = parseUnary(); return std::make_unique<Expr>(UnaryExpr{"void", std::move(e)}, ln); }
    if (check(TT::Delete)) { consume(); auto e = parseUnary(); return std::make_unique<Expr>(UnaryExpr{"delete", std::move(e)}, ln); }
    if (check(TT::Await))  { consume(); auto e = parseUnary(); return std::make_unique<Expr>(AwaitExpr{std::move(e)}, ln); }
    if (check(TT::PlusPlus)||check(TT::MinusMinus)) {
        std::string op = TokenText(consume());
        auto e = parseUnary();
        return std::make_unique<Expr>(UnaryExpr{op, std::move(e), false}, ln);
    }
    return parseUpdate();
}

ExprPtr Parser::parseUpdate() {
    int ln = line();
    auto e = parseCallMember(parsePrimary());
    if ((check(TT::PlusPlus)||check(TT::MinusMinus)) &&
        m_toks[m_pos-1].line == cur().line) {
        std::string op = TokenText(consume());
        return std::make_unique<Expr>(UnaryExpr{op, std::move(e), true}, ln);
    }
    return e;
}

ExprPtr Parser::parseCallMember(ExprPtr base) {
    int ln = line();
    while (true) {
        if (check(TT::Dot) || check(TT::Optional)) {
            bool optional = cur().is(TT::Optional);
            consume();
            // ?.[expr] — optional computed access
            if (optional && check(TT::LBracket)) {
                consume(); // eat [
                auto idx = parseExpr();
                expect(TT::RBracket,"expected ']'");
                base = std::make_unique<Expr>(MemberExpr{std::move(base), std::move(idx), true, true}, ln);
                continue;
            }
            // ?.prop or .prop
            std::string prop;
            if (check(TT::Ident) || cur().isKeyword()) prop = TokenText(consume());
            else throw ParseError("expected property name", line());
            auto propExpr = std::make_unique<Expr>(LiteralExpr{0, prop, false,false,false,false,false,true}, ln);
            base = std::make_unique<Expr>(MemberExpr{std::move(base), std::move(propExpr), false, optional}, ln);
        } else if (check(TT::LBracket)) {
            consume();
            auto idx = parseExpr();
            expect(TT::RBracket,"expected ']'");
            base = std::make_unique<Expr>(MemberExpr{std::move(base), std::move(idx), true, false}, ln);
        } else if (check(TT::LParen)) {
            consume();
            std::vector<ExprPtr> args;
            while (!check(TT::RParen) && !check(TT::Eof)) {
                if (check(TT::DotDotDot)) { consume(); args.push_back(std::make_unique<Expr>(SpreadExpr{parseAssign()}, ln)); }
                else args.push_back(parseAssign());
                if (!match(TT::Comma)) break;
            }
            expect(TT::RParen,"expected ')'");
            base = std::make_unique<Expr>(CallExpr{std::move(base), std::move(args)}, ln);
        } else if (check(TT::TemplatePart) || check(TT::TemplateEnd)) {
            // Tagged template (treat as call)
            auto tmpl = parseTemplateLiteral();
            std::vector<ExprPtr> args;
            args.push_back(std::move(tmpl));
            base = std::make_unique<Expr>(CallExpr{std::move(base), std::move(args)}, ln);
        } else {
            break;
        }
    }
    return base;
}

ExprPtr Parser::parsePrimary() {
    int ln = line();

    if (check(TT::Number)) {
        std::string v = consume().value;
        double n;
        if (v.size() > 1 && v[0] == '0' && (v[1]=='x'||v[1]=='X')) n = (double)std::stoll(v, nullptr, 16);
        else if (v.size() > 1 && v[0] == '0' && (v[1]=='o'||v[1]=='O')) n = (double)std::stoll(v.substr(2), nullptr, 8);
        else if (v.size() > 1 && v[0] == '0' && (v[1]=='b'||v[1]=='B')) n = (double)std::stoll(v.substr(2), nullptr, 2);
        else n = std::stod(v);
        LiteralExpr lit; lit.isNum = true; lit.numVal = n;
        return std::make_unique<Expr>(lit, ln);
    }
    if (check(TT::String)) {
        std::string v = consume().value;
        LiteralExpr lit; lit.isStr = true; lit.strVal = v;
        return std::make_unique<Expr>(lit, ln);
    }
    if (check(TT::True))  { consume(); LiteralExpr lit; lit.isBool=true; lit.boolVal=true;  return std::make_unique<Expr>(lit,ln); }
    if (check(TT::False)) { consume(); LiteralExpr lit; lit.isBool=true; lit.boolVal=false; return std::make_unique<Expr>(lit,ln); }
    if (check(TT::Null))  { consume(); LiteralExpr lit; lit.isNull=true; return std::make_unique<Expr>(lit,ln); }
    if (check(TT::Undefined)) { consume(); LiteralExpr lit; lit.isUndefined=true; return std::make_unique<Expr>(lit,ln); }
    if (check(TT::This))  { consume(); return std::make_unique<Expr>(ThisExpr{}, ln); }
    if (check(TT::Super)) { consume(); return std::make_unique<Expr>(SuperExpr{}, ln); }
    if (check(TT::Import) && peek().is(TT::LParen)) {
        consume();
        return std::make_unique<Expr>(IdentExpr{"__dynamicImport"}, ln);
    }

    if (check(TT::Ident)) {
        std::string name = consume().value;
        return std::make_unique<Expr>(IdentExpr{name}, ln);
    }

    if (check(TT::Regex)) {
        std::string r = consume().value;
        // Store as a literal with special marker (use LiteralExpr isStr=true, strVal=source)
        // The compiler will recognize this and call RegExp constructor
        LiteralExpr lit; lit.isStr = true; lit.strVal = "__regex__" + r;
        return std::make_unique<Expr>(lit, ln);
    }

    if (check(TT::LParen)) return parseArrowOrParen();
    if (check(TT::LBracket)) {
        consume();
        ArrayExpr arr;
        while (!check(TT::RBracket) && !check(TT::Eof)) {
            if (check(TT::Comma)) { arr.elements.push_back(nullptr); consume(); continue; }
            if (check(TT::DotDotDot)) { consume(); arr.elements.push_back(std::make_unique<Expr>(SpreadExpr{parseAssign()},ln)); }
            else arr.elements.push_back(parseAssign());
            if (!match(TT::Comma)) break;
        }
        expect(TT::RBracket,"expected ']'");
        return std::make_unique<Expr>(std::move(arr), ln);
    }
    if (check(TT::LBrace)) {
        consume();
        ObjectExpr obj;
        while (!check(TT::RBrace) && !check(TT::Eof)) {
            ObjectExpr::Prop p;
            bool isGet = false, isSet = false, isAsync = false, isStar = false, isStatic = false;
            if (check(TT::Ident) && cur().value == "get" && !peek().is(TT::Colon) && !peek().is(TT::Comma) && !peek().is(TT::LParen)) { consume(); isGet = true; }
            else if (check(TT::Ident) && cur().value == "set" && !peek().is(TT::Colon) && !peek().is(TT::Comma) && !peek().is(TT::LParen)) { consume(); isSet = true; }
            else if (check(TT::Async)) { consume(); isAsync = true; }
            if (check(TT::Star)) { consume(); isStar = true; }
            if (check(TT::DotDotDot)) {
                consume();
                p.key = nullptr; p.value = std::make_unique<Expr>(SpreadExpr{parseAssign()},ln);
                obj.props.push_back(std::move(p)); match(TT::Comma); continue;
            }
            // key
            if (check(TT::LBracket)) { consume(); p.key = parseAssign(); expect(TT::RBracket,"expected ']'"); p.computed=true; }
            else if (check(TT::String)) { auto v=consume().value; LiteralExpr l; l.isStr=true; l.strVal=v; p.key=std::make_unique<Expr>(l,ln); }
            else if (check(TT::Number)) { auto v=consume().value; LiteralExpr l; l.isNum=true; l.numVal=std::stod(v); p.key=std::make_unique<Expr>(l,ln); }
            else { std::string nm = (check(TT::Ident)||cur().isKeyword()) ? consume().value : expect(TT::Ident,"expected key").value;
                   LiteralExpr l; l.isStr=true; l.strVal=nm; p.key=std::make_unique<Expr>(l,ln); }
            // value
            if (check(TT::LParen)) { // method shorthand
                p.isMethod=true; p.isGet=isGet; p.isSet=isSet; p.isAsync=isAsync; p.isStar=isStar;
                p.value = parseFuncExpr(isAsync, isStar, false);
            } else if (match(TT::Colon)) {
                p.value = parseAssign();
            } else {
                // shorthand: {name}
                p.shorthand = true;
                auto key_lit = p.key.get();
                std::string nm2;
                if (key_lit && key_lit->is<LiteralExpr>()) nm2 = key_lit->as<LiteralExpr>().strVal;
                p.value = std::make_unique<Expr>(IdentExpr{nm2}, ln);
                // Check for default: {name = default}
                if (match(TT::Eq)) {
                    auto def = parseAssign();
                    p.value = std::make_unique<Expr>(AssignExpr{"=", std::move(p.value), std::move(def)}, ln);
                }
            }
            obj.props.push_back(std::move(p));
            if (!match(TT::Comma)) break;
        }
        expect(TT::RBrace,"expected '}'");
        return std::make_unique<Expr>(std::move(obj), ln);
    }
    if (check(TT::Function)) { consume(); return parseFuncExpr(false, match(TT::Star), false); }
    if (check(TT::Async) && peek().is(TT::Function)) { consume(); consume(); return parseFuncExpr(true, match(TT::Star), false); }
    if (check(TT::Class)) {
        consume();
        std::string nm = check(TT::Ident) ? consume().value : "";
        return std::make_unique<Expr>(ClassExpr{nm, nullptr, {}}, ln);
    }
    if (check(TT::TemplatePart) || check(TT::TemplateEnd)) return parseTemplateLiteral();
    if (check(TT::New)) {
        consume();
        ExprPtr callee;
        if (check(TT::New)) callee = parseUnary(); // new new F()
        else callee = parsePrimary();
        // Consume member access before call
        while (check(TT::Dot) || check(TT::LBracket)) {
            if (check(TT::Dot)) {
                consume();
                std::string prop = (check(TT::Ident)||cur().isKeyword()) ? TokenText(consume()) : "";
                LiteralExpr l; l.isStr=true; l.strVal=prop;
                callee = std::make_unique<Expr>(MemberExpr{std::move(callee), std::make_unique<Expr>(l,ln), false}, ln);
            } else {
                consume();
                auto idx = parseExpr(); expect(TT::RBracket,"expected ']'");
                callee = std::make_unique<Expr>(MemberExpr{std::move(callee), std::move(idx), true}, ln);
            }
        }
        std::vector<ExprPtr> args;
        if (match(TT::LParen)) {
            while (!check(TT::RParen)&&!check(TT::Eof)) {
                if (check(TT::DotDotDot)) { consume(); args.push_back(std::make_unique<Expr>(SpreadExpr{parseAssign()},ln)); }
                else args.push_back(parseAssign());
                if (!match(TT::Comma)) break;
            }
            expect(TT::RParen,"expected ')'");
        }
        return std::make_unique<Expr>(CallExpr{std::move(callee), std::move(args), false, true}, ln);
    }
    if (check(TT::Yield)) {
        consume();
        bool delegate = match(TT::Star);
        ExprPtr expr;
        if (!check(TT::Semicolon)&&!check(TT::RBrace)&&!check(TT::Eof))
            expr = parseAssign();
        return std::make_unique<Expr>(YieldExpr{std::move(expr), delegate}, ln);
    }

    throw ParseError("unexpected token: " + cur().value, line());
}

ExprPtr Parser::parseArrowOrParen() {
    int ln = line();
    expect(TT::LParen,"expected '('");
    // Attempt to parse as arrow function params
    // Heuristic: if empty () or single ident => or pattern =>
    std::vector<Token> saved;
    size_t savedPos = m_pos;

    // Check for arrow after single ident
    if (check(TT::RParen) && peek().is(TT::Arrow)) {
        consume(); consume(); // ) =>
        return parseFuncExpr(false, false, true);
    }
    // Full param list check is complex; parse as expression then check for =>
    // For simplicity, check common cases
    if (check(TT::Ident) && peek().is(TT::RParen) && m_toks.size() > m_pos+2 && m_toks[m_pos+2].is(TT::Arrow)) {
        std::string pname = consume().value;
        consume(); consume(); // ) =>
        FuncExpr fe;
        fe.params.push_back(pname);
        fe.defaults.push_back(nullptr);
        fe.isArrow = true;
        if (!check(TT::LBrace)) { fe.isExprBody = true; fe.body = std::make_unique<Stmt>(ExprStmt{parseAssign()}, ln); }
        else fe.body = parseBlock();
        return std::make_unique<Expr>(std::move(fe), ln);
    }
    // Parse as grouped expression
    if (check(TT::RParen)) {
        consume();
        if (match(TT::Arrow)) return parseFuncExpr(false, false, true);
        LiteralExpr lit; lit.isUndefined = true;
        return std::make_unique<Expr>(lit, ln);
    }
    auto expr = parseExpr();
    expect(TT::RParen,"expected ')'");
    if (match(TT::Arrow)) {
        // Convert parsed expr to arrow params
        FuncExpr fe; fe.isArrow = true;
        if (expr->is<IdentExpr>()) fe.params.push_back(expr->as<IdentExpr>().name);
        if (!check(TT::LBrace)) { fe.isExprBody = true; fe.body = std::make_unique<Stmt>(ExprStmt{parseAssign()}, ln); }
        else fe.body = parseBlock();
        while (fe.params.size() > fe.defaults.size()) fe.defaults.push_back(nullptr);
        return std::make_unique<Expr>(std::move(fe), ln);
    }
    return expr;
}

ExprPtr Parser::parseFuncExpr(bool isAsync, bool isStar, bool isArrow) {
    int ln = line();
    std::string name;
    if (!isArrow && check(TT::Ident)) name = consume().value;
    std::vector<ExprPtr> defaults;
    std::string restParam;
    std::vector<std::string> params;
    if (!isArrow || check(TT::LParen)) params = parseParams(defaults, restParam);
    FuncExpr fe;
    fe.name       = name;
    fe.params     = params;
    fe.defaults   = std::move(defaults);
    fe.restParam  = restParam;
    fe.isArrow    = isArrow;
    fe.isAsync    = isAsync;
    fe.isStar     = isStar;
    if (isArrow && !check(TT::LBrace)) {
        fe.isExprBody = true;
        fe.body = std::make_unique<Stmt>(ExprStmt{parseAssign()}, ln);
    } else {
        fe.body = parseBlock();
    }
    return std::make_unique<Expr>(std::move(fe), ln);
}

std::vector<std::string> Parser::parseParams(std::vector<ExprPtr>& defaults,
                                              std::string& restParam) {
    std::vector<std::string> params;
    expect(TT::LParen,"expected '('");
    while (!check(TT::RParen) && !check(TT::Eof)) {
        if (check(TT::DotDotDot)) {
            consume();
            restParam = expect(TT::Ident,"expected rest param name").value;
            break;
        }
        if (check(TT::LBracket)||check(TT::LBrace)) {
            // Destructured param: store as placeholder name
            auto pat = check(TT::LBracket) ? parseArrayPattern() : parseObjectPattern();
            params.push_back("__destruct__");
            ExprPtr def;
            if (match(TT::Eq)) def = parseAssign();
            defaults.push_back(std::move(def));
        } else {
            std::string nm = expect(TT::Ident,"expected parameter").value;
            params.push_back(nm);
            ExprPtr def;
            if (match(TT::Eq)) def = parseAssign();
            defaults.push_back(std::move(def));
        }
        if (!match(TT::Comma)) break;
    }
    expect(TT::RParen,"expected ')'");
    return params;
}

ClassExpr Parser::parseClassBody(const std::string& name) {
    int ln = line();
    ClassExpr cls; cls.name = name;
    if (match(TT::Extends)) cls.superClass = parseAssign();
    expect(TT::LBrace,"expected '{'");
    while (!check(TT::RBrace) && !check(TT::Eof)) {
        if (match(TT::Semicolon)) continue;
        ClassExpr::Method m;
        bool isStatic = false;
        if (check(TT::Static) && !peek().is(TT::LParen)) { consume(); isStatic = true; m.isStatic = isStatic; }
        bool isGet = false, isSet = false, isAsync = false, isStar = false;
        if (check(TT::Ident) && cur().value == "get" && !peek().is(TT::LParen)) { consume(); isGet = true; }
        else if (check(TT::Ident) && cur().value == "set" && !peek().is(TT::LParen)) { consume(); isSet = true; }
        else if (check(TT::Async) && !peek().is(TT::LParen)) { consume(); isAsync = true; }
        if (check(TT::Star)) { consume(); isStar = true; }
        m.isGet = isGet; m.isSet = isSet; m.isAsync = isAsync; m.isStar = isStar;
        if (check(TT::LBracket)) { consume(); m.key = parseAssign(); expect(TT::RBracket,"expected ']'"); m.computed=true; }
        else { LiteralExpr l; l.isStr=true; l.strVal=(check(TT::Ident)||cur().isKeyword()) ? consume().value : ""; m.key=std::make_unique<Expr>(l,ln); }
        {
            auto fptr2 = parseFuncExpr(isAsync, isStar, false);
            m.fn = std::move(std::get<FuncExpr>(fptr2->v));
        }
        cls.methods.push_back(std::move(m));
    }
    expect(TT::RBrace,"expected '}'");
    return cls;
}

ExprPtr Parser::parseTemplateLiteral() {
    int ln = line();
    TemplateExpr tmpl;
    while (true) {
        if (check(TT::TemplateEnd)) { tmpl.cooked.push_back(consume().value); break; }
        if (check(TT::TemplatePart)) {
            tmpl.cooked.push_back(consume().value);
            tmpl.exprs.push_back(parseExpr());
            expect(TT::RBrace,"expected '}'");
            // After }, lexer should pick up more template
            // We re-read template continuation
            if (m_pos < m_toks.size() && !m_toks[m_pos].is(TT::TemplatePart) && !m_toks[m_pos].is(TT::TemplateEnd)) break;
        } else break;
    }
    return std::make_unique<Expr>(std::move(tmpl), ln);
}

ExprPtr Parser::parseArrayPattern() {
    int ln = line();
    expect(TT::LBracket,"expected '['");
    ArrayPattern pat;
    while (!check(TT::RBracket) && !check(TT::Eof)) {
        if (check(TT::Comma)) { pat.elements.push_back(nullptr); consume(); continue; }
        if (check(TT::DotDotDot)) {
            consume();
            pat.rest = check(TT::Ident) ? consume().value : "";
            break;
        }
        pat.elements.push_back(parseAssign());
        if (!match(TT::Comma)) break;
    }
    expect(TT::RBracket,"expected ']'");
    return std::make_unique<Expr>(std::move(pat), ln);
}

ExprPtr Parser::parseObjectPattern() {
    int ln = line();
    expect(TT::LBrace,"expected '{'");
    ObjectPattern pat;
    while (!check(TT::RBrace) && !check(TT::Eof)) {
        if (check(TT::DotDotDot)) {
            consume();
            ObjectPattern::Prop p; p.rest = true; p.key = check(TT::Ident)?consume().value:"";
            pat.props.push_back(std::move(p)); break;
        }
        ObjectPattern::Prop p;
        p.key = (check(TT::Ident)||cur().isKeyword()) ? consume().value : consume().value;
        if (match(TT::Colon)) p.value = parseAssign();
        else { p.value = std::make_unique<Expr>(IdentExpr{p.key}, ln); }
        if (match(TT::Eq)) p.defaultVal = parseAssign();
        pat.props.push_back(std::move(p));
        if (!match(TT::Comma)) break;
    }
    expect(TT::RBrace,"expected '}'");
    return std::make_unique<Expr>(std::move(pat), ln);
}
