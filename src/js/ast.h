#pragma once
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>

// ── forward declarations ──────────────────────────────────────────────────────
struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// ══ Expression nodes ══════════════════════════════════════════════════════════

struct LiteralExpr   { double numVal = 0; std::string strVal; bool isBool = false; bool boolVal = false;
                       bool isNull = false; bool isUndefined = false; bool isNum = false; bool isStr = false; };
struct IdentExpr     { std::string name; };
struct ThisExpr      {};
struct SuperExpr     {};

struct UnaryExpr     { std::string op; ExprPtr expr; bool postfix = false; };
struct BinaryExpr    { std::string op; ExprPtr left, right; };
struct LogicalExpr   { std::string op; ExprPtr left, right; }; // &&, ||, ??
struct AssignExpr    { std::string op; ExprPtr target, value; };
struct TernaryExpr   { ExprPtr cond, yes, no; };

struct MemberExpr    { ExprPtr obj; ExprPtr prop; bool computed = false; bool optional = false; };
struct CallExpr      { ExprPtr callee; std::vector<ExprPtr> args; bool optional = false; bool isNew = false; };
struct SpreadExpr    { ExprPtr expr; };

struct ArrayExpr     { std::vector<ExprPtr> elements; }; // nullptr = hole/spread handled by SpreadExpr
struct ObjectExpr    {
    struct Prop {
        ExprPtr   key;     // can be LiteralExpr(string) or computed
        ExprPtr   value;
        bool      computed  = false;
        bool      shorthand = false;
        bool      isMethod  = false;
        bool      isGet     = false;
        bool      isSet     = false;
        bool      isAsync   = false;
        bool      isStar    = false;
    };
    std::vector<Prop> props;
};

struct FuncExpr {
    std::string name;
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults;   // same size as params, nullptr = no default
    std::string              restParam;  // "" = none
    StmtPtr                  body;
    bool                     isArrow  = false;
    bool                     isAsync  = false;
    bool                     isStar   = false; // generator
    bool                     isExprBody = false; // arrow: x => expr
};

struct ClassExpr {
    std::string name;
    ExprPtr     superClass;
    struct Method {
        ExprPtr   key;
        bool      computed = false;
        bool      isStatic = false;
        bool      isGet    = false;
        bool      isSet    = false;
        bool      isAsync  = false;
        bool      isStar   = false;
        bool      isPrivate= false;
        FuncExpr  fn;
    };
    std::vector<Method> methods;
};

struct TemplateExpr  {
    std::vector<std::string> cooked; // literal parts
    std::vector<ExprPtr>     exprs;  // expressions between parts
};

struct SequenceExpr  { std::vector<ExprPtr> exprs; };

struct YieldExpr     { ExprPtr expr; bool delegate = false; };
struct AwaitExpr     { ExprPtr expr; };

// Destructuring assignment targets
struct ArrayPattern  { std::vector<ExprPtr> elements; std::string rest; };
struct ObjectPattern {
    struct Prop { std::string key; ExprPtr value; ExprPtr defaultVal; bool rest = false; };
    std::vector<Prop> props;
};

// ── Expr sum type ─────────────────────────────────────────────────────────────
struct Expr {
    using V = std::variant<
        LiteralExpr, IdentExpr, ThisExpr, SuperExpr,
        UnaryExpr, BinaryExpr, LogicalExpr, AssignExpr, TernaryExpr,
        MemberExpr, CallExpr, SpreadExpr,
        ArrayExpr, ObjectExpr, FuncExpr, ClassExpr,
        TemplateExpr, SequenceExpr,
        YieldExpr, AwaitExpr,
        ArrayPattern, ObjectPattern
    >;
    V       v;
    int     line = 0;

    template<typename T>
    Expr(T&& t, int ln = 0) : v(std::forward<T>(t)), line(ln) {}

    template<typename T> bool is()       const { return std::holds_alternative<T>(v); }
    template<typename T> T&  as()              { return std::get<T>(v); }
    template<typename T> const T& as()   const { return std::get<T>(v); }
};

// ══ Statement nodes ═══════════════════════════════════════════════════════════

struct ExprStmt       { ExprPtr expr; };
struct BlockStmt      { std::vector<StmtPtr> body; };
struct EmptyStmt      {};

struct VarDecl {
    std::string kind; // "var","let","const"
    struct Decl { ExprPtr target; ExprPtr init; }; // target can be IdentExpr or pattern
    std::vector<Decl> decls;
};

struct FuncDecl {
    std::string name;
    FuncExpr    fn;
};

struct ClassDecl {
    std::string name;
    ClassExpr   cls;
};

struct IfStmt {
    ExprPtr cond;
    StmtPtr then, els; // els may be nullptr
};

struct WhileStmt    { ExprPtr cond; StmtPtr body; };
struct DoWhileStmt  { StmtPtr body; ExprPtr cond; };

struct ForStmt {
    StmtPtr init; // VarDecl or ExprStmt or nullptr
    ExprPtr cond, update;
    StmtPtr body;
};

struct ForInStmt   { std::string kind; ExprPtr left; ExprPtr right; StmtPtr body; };
struct ForOfStmt   { std::string kind; ExprPtr left; ExprPtr right; StmtPtr body; bool isAwait = false; };

struct ReturnStmt  { ExprPtr expr; }; // expr may be nullptr
struct ThrowStmt   { ExprPtr expr; };
struct BreakStmt   { std::string label; };
struct ContinueStmt{ std::string label; };
struct DebuggerStmt{};

struct TryCatchStmt {
    StmtPtr        tryBody;
    std::string    catchParam; // "" = no param (catch {})
    ExprPtr        catchPattern; // can be ObjectPattern or ArrayPattern
    StmtPtr        catchBody;   // nullptr = no catch
    StmtPtr        finallyBody; // nullptr = no finally
};

struct SwitchStmt {
    ExprPtr cond;
    struct Case { ExprPtr test; std::vector<StmtPtr> body; }; // test=nullptr→default
    std::vector<Case> cases;
};

struct LabeledStmt { std::string label; StmtPtr body; };

struct ImportDecl {
    std::string                                  source;
    std::vector<std::pair<std::string,std::string>> specifiers; // {imported,local}
    std::string                                  defaultName;
    bool                                         namespace_ = false;
    std::string                                  namespaceName;
};

struct ExportDecl {
    StmtPtr decl;   // can be VarDecl, FuncDecl, ClassDecl
    bool    default_ = false;
    ExprPtr defaultExpr;
};

// ── Stmt sum type ─────────────────────────────────────────────────────────────
struct Stmt {
    using V = std::variant<
        ExprStmt, BlockStmt, EmptyStmt,
        VarDecl, FuncDecl, ClassDecl,
        IfStmt, WhileStmt, DoWhileStmt,
        ForStmt, ForInStmt, ForOfStmt,
        ReturnStmt, ThrowStmt, BreakStmt, ContinueStmt,
        TryCatchStmt, SwitchStmt, LabeledStmt,
        ImportDecl, ExportDecl,
        DebuggerStmt
    >;
    V    v;
    int  line = 0;

    template<typename T>
    Stmt(T&& t, int ln = 0) : v(std::forward<T>(t)), line(ln) {}

    template<typename T> bool is()       const { return std::holds_alternative<T>(v); }
    template<typename T> T&  as()              { return std::get<T>(v); }
    template<typename T> const T& as()   const { return std::get<T>(v); }
};

// ── Program ───────────────────────────────────────────────────────────────────
struct Program {
    std::vector<StmtPtr> body;
    bool                 isModule = false;
};
