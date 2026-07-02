#pragma once
#include "js/ast.h"
#include "js/value.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <optional>

// ── Opcodes ───────────────────────────────────────────────────────────────────
enum Opcode : uint8_t {
    // Loads
    OP_LOAD_CONST,   // a=dst, bc=const_idx(u16)
    OP_LOAD_UNDEF,   // a=dst
    OP_LOAD_NULL,    // a=dst
    OP_LOAD_TRUE,    // a=dst
    OP_LOAD_FALSE,   // a=dst
    OP_LOAD_INT,     // a=dst, bc=int16(signed)
    OP_MOVE,         // a=dst, b=src
    // Globals
    OP_GET_GLOBAL,   // a=dst, bc=name_const_idx
    OP_SET_GLOBAL,   // a=src, bc=name_const_idx
    OP_DEL_GLOBAL,   // a=dst(bool), bc=name_const_idx
    // Upvalues
    OP_GET_UPVAL,    // a=dst, b=upval_idx
    OP_SET_UPVAL,    // a=src, b=upval_idx
    OP_CLOSE_UPVAL,  // a=first_reg_to_close
    // Properties
    OP_GET_PROP,     // a=dst, b=obj, c=key_reg
    OP_SET_PROP,     // a=obj, b=key_reg, c=val
    OP_GET_PROP_S,   // a=dst, b=obj, bc=name_const_idx  (static string key)
    OP_SET_PROP_S,   // a=obj, bc=name_const_idx, then next instr a=val
    OP_DEL_PROP,     // a=dst, b=obj, c=key_reg
    OP_GET_THIS_PROP,// a=dst, bc=name_const_idx  (shortcut for this.prop)
    OP_SET_THIS_PROP,// bc=name_const_idx, a=val
    // Array/Object
    OP_NEW_ARRAY,    // a=dst, bc=count
    OP_NEW_OBJECT,   // a=dst
    OP_ARRAY_PUSH,   // a=arr, b=val
    OP_SPREAD_CALL,  // a=arr, b=iterable  (spread into call args array)
    // Arithmetic
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,  // a=dst,b=l,c=r
    OP_NEG, OP_PLUS, // a=dst, b=src  (unary - and +)
    OP_INC, OP_DEC,  // a=dst (in-place)
    // Bitwise
    OP_BAND, OP_BOR, OP_BXOR,                   // a=dst,b=l,c=r
    OP_BNOT, OP_SHL, OP_SHR, OP_USHR,           // a=dst,b=src (or b=l,c=r)
    // Comparison
    OP_EQ, OP_NEQ, OP_SEQ, OP_SNEQ,             // a=dst,b=l,c=r
    OP_LT, OP_LTE, OP_GT, OP_GTE,
    OP_INSTANCEOF, OP_IN,
    // Logical
    OP_NOT,       // a=dst, b=src
    OP_TYPEOF,    // a=dst, b=src
    OP_VOID_OP,   // a=dst (loads undefined)
    // Jumps: bc = signed int16 relative offset (from next instr)
    OP_JUMP,          // bc=offset
    OP_JUMP_TRUE,     // a=cond, bc=offset
    OP_JUMP_FALSE,    // a=cond, bc=offset
    OP_JUMP_NULLISH,  // a=val, bc=offset (jump if null/undefined)
    OP_JUMP_TRUE_POP, // a=cond, bc=offset (short-circuit &&)
    OP_JUMP_FALSE_POP,// a=cond, bc=offset (short-circuit ||)
    // Functions
    OP_MAKE_FUNC,  // a=dst, bc=inner_fn_idx
    OP_CALL,       // a=dst, b=this_reg, c=fn_reg  (args in fn_reg+1 onwards)
                   // next instr: a=argc
    OP_CALL_S,     // a=dst, b=obj, bc=method_name_idx (next: argc reg, first_arg_reg)
    OP_NEW,        // a=dst, b=fn_reg, c=argc  (args in fn_reg+1..fn_reg+argc)
    OP_RETURN,     // a=src
    OP_RETURN_UNDEF,
    // Exceptions
    OP_THROW,        // a=src
    OP_ENTER_TRY,    // bc=catch_offset (relative from this instr)
    OP_EXIT_TRY,     // (pop try scope)
    OP_CATCH_LOAD,   // a=dst (load caught exception into register)
    // Iteration
    OP_FOR_IN_INIT,  // a=iter_dst, b=obj
    OP_FOR_IN_NEXT,  // a=key_dst, b=done_dst, c=iter
    OP_FOR_OF_INIT,  // a=iter_dst, b=obj
    OP_FOR_OF_NEXT,  // a=val_dst, b=done_dst, c=iter
    // Misc
    OP_DELETE,       // a=dst(bool), b=obj, c=key_reg
    OP_AWAIT,        // a=dst, b=promise_reg
    OP_YIELD,        // a=val_reg
    OP_DEBUGGER,
    OP_NOP,

    _OP_COUNT
};

// ── Instruction ───────────────────────────────────────────────────────────────
struct Instruction {
    uint8_t op  = OP_NOP;
    uint8_t a   = 0;
    uint8_t b   = 0;
    uint8_t c   = 0;

    uint16_t bc() const { return (uint16_t)(b | (c << 8)); }
    int16_t  sbc()const { return (int16_t)bc(); }
    void     setbc(uint16_t v) { b = v & 0xFF; c = (v >> 8) & 0xFF; }
    void     setsbc(int16_t v) { setbc((uint16_t)v); }
};

// ── UpvalDesc ─────────────────────────────────────────────────────────────────
struct UpvalDesc {
    std::string name;
    bool        inStack; // true=capture from enclosing frame reg, false=from enclosing upvalue
    uint8_t     idx;     // register index or upvalue index in enclosing scope
};

// ── BytecodeFunction ──────────────────────────────────────────────────────────
struct BytecodeFunction {
    std::string              name;
    uint8_t                  paramCount  = 0;
    bool                     hasRest     = false;
    std::string              restParam;
    uint8_t                  regCount    = 0;
    bool                     isArrow     = false;
    bool                     isAsync     = false;
    bool                     isGenerator = false;

    std::vector<Instruction>                 code;
    std::vector<JsValue>                     consts;  // constant pool (strings stored as JsValue::string/null for strings)
    std::vector<std::string>                 constStrings; // parallel to consts for string keys
    std::vector<UpvalDesc>                   upvalDescs;
    std::vector<std::unique_ptr<BytecodeFunction>> innerFns;

    // Source lines (parallel to code)
    std::vector<uint32_t> lines;

    uint16_t addConst(JsValue v, const std::string& strKey = "");
    uint16_t addConstString(const std::string& s);
    int      emit(Opcode op, uint8_t a=0, uint8_t b=0, uint8_t c=0, int ln=0);
    int      emitJump(Opcode op, uint8_t a=0, int ln=0);
    void     patchJump(int jumpInstrIdx);
    void     patchJumpTo(int jumpInstrIdx, int targetIdx);

    int      size() const { return (int)code.size(); }
};

// ── Compiler ──────────────────────────────────────────────────────────────────
class Compiler {
public:
    // Compile a top-level Program. Returns ownership of the root BytecodeFunction.
    static std::unique_ptr<BytecodeFunction> compile(const Program& prog);
    // Compile a single FuncExpr (used for eval-style compilation).
    static std::unique_ptr<BytecodeFunction> compileFn(const FuncExpr& fe,
                                                        const std::string& name = "");

private:
    BytecodeFunction* m_fn;   // current function being compiled

    struct Scope {
        bool isFn = false;
        struct Local { std::string name; int reg; bool isConst; bool isCaptured; };
        std::vector<Local> locals;
        // Break/continue patch lists for current loop
        std::vector<int> breakPatches;
        std::vector<int> continuePatches;
        int loopStart = -1;
        std::string label;
    };
    std::vector<Scope> m_scopes;

    // Upvalue tracking for current function
    struct UpvalInfo { std::string name; bool inStack; uint8_t idx; };
    std::vector<UpvalInfo> m_upvals;

    // Enclosing compiler (for closures)
    Compiler* m_enclosing = nullptr;

    // Register allocation
    uint8_t m_nextReg = 0;
    std::vector<uint8_t> m_freeRegs;

    explicit Compiler(BytecodeFunction* fn, Compiler* enclosing = nullptr);

    uint8_t  allocReg();
    void     freeReg(uint8_t r);
    uint8_t  peekReg() const { return m_nextReg; }

    void     pushScope(bool isFn = false);
    void     popScope();
    int      declareLocal(const std::string& name, bool isConst = false);
    std::optional<int> resolveLocal(const std::string& name) const;
    std::optional<uint8_t> resolveUpval(const std::string& name);
    int      addUpval(bool inStack, uint8_t idx, const std::string& name);

    // Emit helpers
    int emit(Opcode op, uint8_t a=0, uint8_t b=0, uint8_t c=0, int ln=0) {
        return m_fn->emit(op, a, b, c, ln);
    }

    // Compile expressions: returns the register containing the result.
    uint8_t compileExpr(const Expr& e, int hint = -1);
    uint8_t compileLiteral (const LiteralExpr& e);
    uint8_t compileIdent   (const IdentExpr& e, int hint);
    uint8_t compileBinary  (const BinaryExpr& e, int hint);
    uint8_t compileLogical (const LogicalExpr& e, int hint);
    uint8_t compileAssign  (const AssignExpr& e, int hint);
    uint8_t compileTernary (const TernaryExpr& e, int hint);
    uint8_t compileMember  (const MemberExpr& e, int hint);
    uint8_t compileCall    (const CallExpr& e, int hint);
    uint8_t compileUnary   (const UnaryExpr& e, int hint);
    uint8_t compileArray   (const ArrayExpr& e, int hint);
    uint8_t compileObject  (const ObjectExpr& e, int hint);
    uint8_t compileFuncExpr(const FuncExpr& e, int hint);
    uint8_t compileClass   (const ClassExpr& e, int hint);
    uint8_t compileTemplate(const TemplateExpr& e, int hint);
    uint8_t compileSequence(const SequenceExpr& e, int hint);
    uint8_t compileAwait   (const AwaitExpr& e, int hint);
    uint8_t compileYield   (const YieldExpr& e, int hint);

    // Compile statements
    void compileStmt(const Stmt& s);
    void compileBlock  (const BlockStmt& s);
    void compileVarDecl(const VarDecl& s);
    void compileFuncDecl(const FuncDecl& s);
    void compileIf     (const IfStmt& s);
    void compileWhile  (const WhileStmt& s);
    void compileDoWhile(const DoWhileStmt& s);
    void compileFor    (const ForStmt& s);
    void compileForIn  (const ForInStmt& s);
    void compileForOf  (const ForOfStmt& s);
    void compileSwitchStmt(const SwitchStmt& s);
    void compileReturn (const ReturnStmt& s);
    void compileThrow  (const ThrowStmt& s);
    void compileTryCatch(const TryCatchStmt& s);
    void compileClass  (const ClassDecl& s);

    // Assignment target helpers
    void emitStore(const Expr& target, uint8_t valReg);
    void emitDestructure(const Expr& pattern, uint8_t srcReg);

    // Load a variable (local/upval/global) into a register
    uint8_t loadVar(const std::string& name, int hint, int ln);
    // Store a register value to a variable
    void    storeVar(const std::string& name, uint8_t src, int ln);

    void compileProgram(const Program& prog);

    uint8_t loadConstString(const std::string& value, int line = 0);
    void emitGetStaticProp(uint8_t dst, uint8_t obj, const std::string& key, int line = 0);
    void emitSetStaticProp(uint8_t obj, const std::string& key, uint8_t val, int line = 0);
};
