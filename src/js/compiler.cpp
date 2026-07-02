#include "js/compiler.h"
#include <stdexcept>
#include <cassert>
#include <algorithm>
#include <cmath>

// ── BytecodeFunction helpers ──────────────────────────────────────────────────

uint16_t BytecodeFunction::addConst(JsValue v, const std::string& strKey) {
    constStrings.push_back(strKey);
    consts.push_back(v);
    return (uint16_t)(consts.size() - 1);
}

uint16_t BytecodeFunction::addConstString(const std::string& s) {
    // Deduplicate
    for (size_t i = 0; i < constStrings.size(); i++)
        if (constStrings[i] == s && consts[i].isNull())
            return (uint16_t)i;
    // Store as null with strKey; VM will intern at runtime
    constStrings.push_back(s);
    consts.push_back(JsValue::null()); // placeholder; VM resolves via constStrings[i]
    return (uint16_t)(consts.size() - 1);
}

int BytecodeFunction::emit(Opcode op, uint8_t a, uint8_t b, uint8_t c, int ln) {
    Instruction ins; ins.op = op; ins.a = a; ins.b = b; ins.c = c;
    code.push_back(ins);
    lines.push_back(ln > 0 ? (uint32_t)ln : (lines.empty() ? 1 : lines.back()));
    return (int)code.size() - 1;
}

int BytecodeFunction::emitJump(Opcode op, uint8_t a, int ln) {
    return emit(op, a, 0, 0, ln); // bc filled in by patchJump
}

void BytecodeFunction::patchJump(int idx) {
    int offset = (int)code.size() - idx - 1;
    code[idx].setsbc((int16_t)offset);
}

void BytecodeFunction::patchJumpTo(int idx, int target) {
    int offset = target - idx - 1;
    code[idx].setsbc((int16_t)offset);
}

// ── Compiler ──────────────────────────────────────────────────────────────────

Compiler::Compiler(BytecodeFunction* fn, Compiler* enc)
    : m_fn(fn), m_enclosing(enc) {}

uint8_t Compiler::allocReg() {
    if (!m_freeRegs.empty()) {
        uint8_t r = m_freeRegs.back(); m_freeRegs.pop_back(); return r;
    }
    uint8_t r = m_nextReg++;
    if (m_nextReg > m_fn->regCount) m_fn->regCount = m_nextReg;
    return r;
}

void Compiler::freeReg(uint8_t r) {
    if (r + 1 == m_nextReg) { m_nextReg--; return; }
    m_freeRegs.push_back(r);
}

uint8_t Compiler::loadConstString(const std::string& value, int line) {
    uint8_t dst = allocReg();
    uint16_t idx = m_fn->addConstString(value);
    Instruction ins;
    ins.op = OP_LOAD_CONST;
    ins.a = dst;
    ins.setbc(idx);
    m_fn->code.push_back(ins);
    m_fn->lines.push_back(line);
    return dst;
}

void Compiler::emitGetStaticProp(uint8_t dst, uint8_t obj, const std::string& key, int line) {
    uint8_t keyReg = loadConstString(key, line);
    emit(OP_GET_PROP, dst, obj, keyReg, line);
    freeReg(keyReg);
}

void Compiler::emitSetStaticProp(uint8_t obj, const std::string& key, uint8_t val, int line) {
    uint8_t keyReg = loadConstString(key, line);
    emit(OP_SET_PROP, obj, keyReg, val, line);
    freeReg(keyReg);
}

void Compiler::pushScope(bool isFn) {
    m_scopes.push_back({isFn, {}, {}, {}, -1, ""});
}

void Compiler::popScope() {
    // Close any upvalues captured from this scope
    if (!m_scopes.empty()) {
        for (auto& loc : m_scopes.back().locals) {
            if (loc.isCaptured) {
                emit(OP_CLOSE_UPVAL, (uint8_t)loc.reg, 0, 0, 0);
                freeReg((uint8_t)loc.reg);
            } else {
                freeReg((uint8_t)loc.reg);
            }
        }
    }
    if (!m_scopes.empty()) m_scopes.pop_back();
}

int Compiler::declareLocal(const std::string& name, bool isConst) {
    int r = allocReg();
    if (!m_scopes.empty())
        m_scopes.back().locals.push_back({name, r, isConst, false});
    return r;
}

std::optional<int> Compiler::resolveLocal(const std::string& name) const {
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        for (auto jt = it->locals.rbegin(); jt != it->locals.rend(); ++jt)
            if (jt->name == name) return jt->reg;
        if (it->isFn) break;
    }
    return {};
}

int Compiler::addUpval(bool inStack, uint8_t idx, const std::string& name) {
    for (int i = 0; i < (int)m_upvals.size(); i++)
        if (m_upvals[i].name == name) return i;
    m_upvals.push_back({name, inStack, idx});
    UpvalDesc d; d.name = name; d.inStack = inStack; d.idx = idx;
    m_fn->upvalDescs.push_back(d);
    return (int)m_upvals.size() - 1;
}

std::optional<uint8_t> Compiler::resolveUpval(const std::string& name) {
    if (!m_enclosing) return {};
    // Check enclosing locals
    if (auto local = m_enclosing->resolveLocal(name)) {
        // Mark as captured in enclosing
        for (auto& s : m_enclosing->m_scopes)
            for (auto& l : s.locals)
                if (l.name == name) l.isCaptured = true;
        return (uint8_t)addUpval(true, (uint8_t)*local, name);
    }
    // Check enclosing upvalues (transitive)
    if (auto upv = m_enclosing->resolveUpval(name)) {
        return (uint8_t)addUpval(false, *upv, name);
    }
    return {};
}

uint8_t Compiler::loadVar(const std::string& name, int hint, int ln) {
    if (auto local = resolveLocal(name)) {
        uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
        emit(OP_MOVE, dst, (uint8_t)*local, 0, ln);
        return dst;
    }
    if (auto uv = resolveUpval(name)) {
        uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
        emit(OP_GET_UPVAL, dst, *uv, 0, ln);
        return dst;
    }
    // Global
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    uint16_t idx = m_fn->addConstString(name);
    Instruction ins; ins.op = OP_GET_GLOBAL; ins.a = dst; ins.setbc(idx);
    m_fn->code.push_back(ins); m_fn->lines.push_back(ln);
    return dst;
}

void Compiler::storeVar(const std::string& name, uint8_t src, int ln) {
    if (auto local = resolveLocal(name)) {
        if ((uint8_t)*local != src) emit(OP_MOVE, (uint8_t)*local, src, 0, ln);
        return;
    }
    if (auto uv = resolveUpval(name)) {
        emit(OP_SET_UPVAL, src, *uv, 0, ln);
        return;
    }
    uint16_t idx = m_fn->addConstString(name);
    Instruction ins; ins.op = OP_SET_GLOBAL; ins.a = src; ins.setbc(idx);
    m_fn->code.push_back(ins); m_fn->lines.push_back(ln);
}

// ── Statement compilation ─────────────────────────────────────────────────────

void Compiler::compileStmt(const Stmt& s) {
    std::visit([this, &s](auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T,ExprStmt>)     { auto r = compileExpr(*v.expr); if (r >= m_fn->regCount || true) freeReg(r); }
        else if constexpr (std::is_same_v<T,BlockStmt>)    compileBlock(v);
        else if constexpr (std::is_same_v<T,EmptyStmt>)    {}
        else if constexpr (std::is_same_v<T,VarDecl>)      compileVarDecl(v);
        else if constexpr (std::is_same_v<T,FuncDecl>)     compileFuncDecl(v);
        else if constexpr (std::is_same_v<T,ClassDecl>)    compileClass(v);
        else if constexpr (std::is_same_v<T,IfStmt>)       compileIf(v);
        else if constexpr (std::is_same_v<T,WhileStmt>)    compileWhile(v);
        else if constexpr (std::is_same_v<T,DoWhileStmt>)  compileDoWhile(v);
        else if constexpr (std::is_same_v<T,ForStmt>)      compileFor(v);
        else if constexpr (std::is_same_v<T,ForInStmt>)    compileForIn(v);
        else if constexpr (std::is_same_v<T,ForOfStmt>)    compileForOf(v);
        else if constexpr (std::is_same_v<T,ReturnStmt>)   compileReturn(v);
        else if constexpr (std::is_same_v<T,ThrowStmt>)    compileThrow(v);
        else if constexpr (std::is_same_v<T,TryCatchStmt>) compileTryCatch(v);
        else if constexpr (std::is_same_v<T,SwitchStmt>)   compileSwitchStmt(v);
        else if constexpr (std::is_same_v<T,LabeledStmt>)  compileStmt(*v.body);
        else if constexpr (std::is_same_v<T,BreakStmt>) {
            // Patch break out of innermost loop
            for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
                if (it->loopStart >= 0 || (v.label.empty() ? false : it->label == v.label)) {
                    int j = m_fn->emitJump(OP_JUMP, 0, s.line);
                    it->breakPatches.push_back(j);
                    return;
                }
            }
        }
        else if constexpr (std::is_same_v<T,ContinueStmt>) {
            for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
                if (it->loopStart >= 0) {
                    int j = m_fn->emitJump(OP_JUMP, 0, s.line);
                    it->continuePatches.push_back(j);
                    return;
                }
            }
        }
        else if constexpr (std::is_same_v<T,DebuggerStmt>) emit(OP_DEBUGGER);
        // Import/Export: ignore at runtime (ES modules handled by engine)
    }, s.v);
}

void Compiler::compileBlock(const BlockStmt& s) {
    pushScope();
    for (auto& stmt : s.body) compileStmt(*stmt);
    popScope();
}

void Compiler::compileVarDecl(const VarDecl& s) {
    for (auto& d : s.decls) {
        if (d.target && d.target->is<IdentExpr>()) {
            std::string name = d.target->as<IdentExpr>().name;
            int r = declareLocal(name, s.kind == "const");
            if (d.init) {
                uint8_t src = compileExpr(*d.init, r);
                if (src != (uint8_t)r) emit(OP_MOVE, (uint8_t)r, src, 0, d.target->line);
                if (src != (uint8_t)r) freeReg(src);
            } else {
                emit(OP_LOAD_UNDEF, (uint8_t)r, 0, 0, 0);
            }
        } else if (d.target) {
            // Destructuring
            uint8_t src = allocReg();
            if (d.init) { uint8_t s2 = compileExpr(*d.init, src); if (s2 != src) { emit(OP_MOVE, src, s2); freeReg(s2); } }
            else emit(OP_LOAD_UNDEF, src);
            emitDestructure(*d.target, src);
            freeReg(src);
        }
    }
}

void Compiler::compileFuncDecl(const FuncDecl& s) {
    // Hoist: declare the local first, then compile and assign
    int r = declareLocal(s.name, false);
    uint8_t fnReg = compileFuncExpr(s.fn, r);
    if (fnReg != (uint8_t)r) { emit(OP_MOVE, (uint8_t)r, fnReg); freeReg(fnReg); }
}

void Compiler::compileIf(const IfStmt& s) {
    uint8_t cond = compileExpr(*s.cond);
    int jf = m_fn->emitJump(OP_JUMP_FALSE, cond, s.cond->line);
    freeReg(cond);
    compileStmt(*s.then);
    if (s.els) {
        int jEnd = m_fn->emitJump(OP_JUMP, 0, 0);
        m_fn->patchJump(jf);
        compileStmt(*s.els);
        m_fn->patchJump(jEnd);
    } else {
        m_fn->patchJump(jf);
    }
}

void Compiler::compileWhile(const WhileStmt& s) {
    pushScope(false);
    m_scopes.back().loopStart = m_fn->size();
    int loopStart = m_fn->size();
    uint8_t cond = compileExpr(*s.cond);
    int jf = m_fn->emitJump(OP_JUMP_FALSE, cond, s.cond->line);
    freeReg(cond);
    compileStmt(*s.body);
    // Patch continues
    for (int p : m_scopes.back().continuePatches) m_fn->patchJumpTo(p, loopStart);
    // Jump back
    int backOffset = loopStart - m_fn->size() - 1;
    Instruction jb; jb.op = OP_JUMP; jb.setsbc((int16_t)backOffset);
    m_fn->code.push_back(jb); m_fn->lines.push_back(0);
    m_fn->patchJump(jf);
    for (int p : m_scopes.back().breakPatches) m_fn->patchJump(p);
    popScope();
}

void Compiler::compileDoWhile(const DoWhileStmt& s) {
    pushScope(false);
    int loopStart = m_fn->size();
    m_scopes.back().loopStart = loopStart;
    compileStmt(*s.body);
    for (int p : m_scopes.back().continuePatches) m_fn->patchJump(p);
    uint8_t cond = compileExpr(*s.cond);
    int backOffset = loopStart - m_fn->size() - 1;
    Instruction jt; jt.op = OP_JUMP_TRUE; jt.a = cond; jt.setsbc((int16_t)backOffset);
    m_fn->code.push_back(jt); m_fn->lines.push_back(0);
    freeReg(cond);
    for (int p : m_scopes.back().breakPatches) m_fn->patchJump(p);
    popScope();
}

void Compiler::compileFor(const ForStmt& s) {
    pushScope(false);
    if (s.init) compileStmt(*s.init);
    int loopStart = m_fn->size();
    m_scopes.back().loopStart = loopStart;
    int jf = -1;
    if (s.cond) {
        uint8_t cond = compileExpr(*s.cond);
        jf = m_fn->emitJump(OP_JUMP_FALSE, cond, s.cond->line);
        freeReg(cond);
    }
    compileStmt(*s.body);
    int updatePos = m_fn->size();
    for (int p : m_scopes.back().continuePatches) m_fn->patchJumpTo(p, updatePos);
    if (s.update) { uint8_t r = compileExpr(*s.update); freeReg(r); }
    int backOffset = loopStart - m_fn->size() - 1;
    Instruction jb; jb.op = OP_JUMP; jb.setsbc((int16_t)backOffset);
    m_fn->code.push_back(jb); m_fn->lines.push_back(0);
    if (jf >= 0) m_fn->patchJump(jf);
    for (int p : m_scopes.back().breakPatches) m_fn->patchJump(p);
    popScope();
}

void Compiler::compileForIn(const ForInStmt& s) {
    pushScope(false);
    uint8_t obj  = compileExpr(*s.right);
    uint8_t iter = allocReg();
    emit(OP_FOR_IN_INIT, iter, obj);
    freeReg(obj);
    int loopStart = m_fn->size();
    m_scopes.back().loopStart = loopStart;
    uint8_t key = allocReg(), done = allocReg();
    emit(OP_FOR_IN_NEXT, key, done, iter);
    int jExit = m_fn->emitJump(OP_JUMP_TRUE, done);
    freeReg(done);
    // Assign key to loop variable
    if (s.left && s.left->is<IdentExpr>()) {
        std::string nm = s.left->as<IdentExpr>().name;
        if (!s.kind.empty()) declareLocal(nm); // var/let/const
        storeVar(nm, key, s.left->line);
    }
    freeReg(key);
    compileStmt(*s.body);
    for (int p : m_scopes.back().continuePatches) m_fn->patchJumpTo(p, loopStart);
    int back = loopStart - m_fn->size() - 1;
    Instruction jb; jb.op=OP_JUMP; jb.setsbc((int16_t)back); m_fn->code.push_back(jb); m_fn->lines.push_back(0);
    m_fn->patchJump(jExit);
    freeReg(iter);
    for (int p : m_scopes.back().breakPatches) m_fn->patchJump(p);
    popScope();
}

void Compiler::compileForOf(const ForOfStmt& s) {
    pushScope(false);
    uint8_t obj  = compileExpr(*s.right);
    uint8_t iter = allocReg();
    emit(OP_FOR_OF_INIT, iter, obj);
    freeReg(obj);
    int loopStart = m_fn->size();
    m_scopes.back().loopStart = loopStart;
    uint8_t val = allocReg(), done = allocReg();
    emit(OP_FOR_OF_NEXT, val, done, iter);
    int jExit = m_fn->emitJump(OP_JUMP_TRUE, done);
    freeReg(done);
    if (s.left) {
        if (s.left->is<IdentExpr>()) {
            std::string nm = s.left->as<IdentExpr>().name;
            if (!s.kind.empty()) { int r = declareLocal(nm); emit(OP_MOVE, (uint8_t)r, val); }
            else storeVar(nm, val, s.left->line);
        } else {
            emitDestructure(*s.left, val);
        }
    }
    freeReg(val);
    compileStmt(*s.body);
    for (int p : m_scopes.back().continuePatches) m_fn->patchJumpTo(p, loopStart);
    int back = loopStart - m_fn->size() - 1;
    Instruction jb; jb.op=OP_JUMP; jb.setsbc((int16_t)back); m_fn->code.push_back(jb); m_fn->lines.push_back(0);
    m_fn->patchJump(jExit);
    freeReg(iter);
    for (int p : m_scopes.back().breakPatches) m_fn->patchJump(p);
    popScope();
}

void Compiler::compileSwitchStmt(const SwitchStmt& s) {
    pushScope(false);
    m_scopes.back().loopStart = -2; // sentinel: has break but no continue
    uint8_t disc = compileExpr(*s.cond);
    uint8_t tmp  = allocReg();
    // Compile each case test + jump
    std::vector<int> caseJumps;
    int defaultJump = -1;
    for (auto& c : s.cases) {
        if (!c.test) { defaultJump = m_fn->size(); caseJumps.push_back(-1); continue; }
        uint8_t testR = compileExpr(*c.test);
        emit(OP_SEQ, tmp, disc, testR); freeReg(testR);
        caseJumps.push_back(m_fn->emitJump(OP_JUMP_TRUE, tmp));
    }
    freeReg(tmp); freeReg(disc);
    // Jump to default or past switch
    int skipAll = m_fn->emitJump(OP_JUMP);
    // Patch case jumps and compile bodies
    for (size_t i = 0; i < s.cases.size(); i++) {
        if (caseJumps[i] >= 0) m_fn->patchJump(caseJumps[i]);
        else { m_fn->patchJumpTo(skipAll, m_fn->size()); } // default
        for (auto& st : s.cases[i].body) compileStmt(*st);
    }
    if (defaultJump < 0) m_fn->patchJump(skipAll); // no default: patch past
    for (int p : m_scopes.back().breakPatches) m_fn->patchJump(p);
    popScope();
}

void Compiler::compileReturn(const ReturnStmt& s) {
    if (s.expr) {
        uint8_t r = compileExpr(*s.expr);
        emit(OP_RETURN, r, 0, 0, s.expr->line);
        freeReg(r);
    } else {
        emit(OP_RETURN_UNDEF);
    }
}

void Compiler::compileThrow(const ThrowStmt& s) {
    uint8_t r = compileExpr(*s.expr);
    emit(OP_THROW, r, 0, 0, s.expr->line);
    freeReg(r);
}

void Compiler::compileTryCatch(const TryCatchStmt& s) {
    int tryInstr = m_fn->emitJump(OP_ENTER_TRY); // bc=catch offset
    compileStmt(*s.tryBody);
    emit(OP_EXIT_TRY);
    int skipCatch = m_fn->emitJump(OP_JUMP);
    m_fn->patchJump(tryInstr); // catch starts here
    if (s.catchBody) {
        pushScope();
        uint8_t exReg = -1;
        if (!s.catchParam.empty()) {
            exReg = (uint8_t)declareLocal(s.catchParam);
        } else {
            exReg = allocReg(); // still need to load it somewhere
        }
        emit(OP_CATCH_LOAD, exReg);
        if (s.catchPattern) emitDestructure(*s.catchPattern, exReg);
        compileStmt(*s.catchBody);
        popScope();
    } else {
        uint8_t exReg = allocReg(); emit(OP_CATCH_LOAD, exReg); freeReg(exReg); // discard
    }
    m_fn->patchJump(skipCatch);
    if (s.finallyBody) compileStmt(*s.finallyBody);
}

void Compiler::compileClass(const ClassDecl& s) {
    uint8_t r = (uint8_t)declareLocal(s.name);
    uint8_t cr = compileClass(s.cls, r);
    if (cr != r) { emit(OP_MOVE, r, cr); freeReg(cr); }
}

// ── Expression compilation ────────────────────────────────────────────────────

uint8_t Compiler::compileExpr(const Expr& e, int hint) {
    return std::visit([this, &e, hint](auto& v) -> uint8_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T,LiteralExpr>)  return compileLiteral(v);
        else if constexpr (std::is_same_v<T,IdentExpr>)    return compileIdent(v, hint);
        else if constexpr (std::is_same_v<T,ThisExpr>)     { uint8_t r=allocReg(); emit(OP_GET_GLOBAL,r,(uint8_t)m_fn->addConstString("__this__")); return r; }
        else if constexpr (std::is_same_v<T,SuperExpr>)    { uint8_t r=allocReg(); emit(OP_GET_GLOBAL,r,(uint8_t)m_fn->addConstString("__super__")); return r; }
        else if constexpr (std::is_same_v<T,BinaryExpr>)   return compileBinary(v, hint);
        else if constexpr (std::is_same_v<T,LogicalExpr>)  return compileLogical(v, hint);
        else if constexpr (std::is_same_v<T,AssignExpr>)   return compileAssign(v, hint);
        else if constexpr (std::is_same_v<T,TernaryExpr>)  return compileTernary(v, hint);
        else if constexpr (std::is_same_v<T,MemberExpr>)   return compileMember(v, hint);
        else if constexpr (std::is_same_v<T,CallExpr>)     return compileCall(v, hint);
        else if constexpr (std::is_same_v<T,UnaryExpr>)    return compileUnary(v, hint);
        else if constexpr (std::is_same_v<T,ArrayExpr>)    return compileArray(v, hint);
        else if constexpr (std::is_same_v<T,ObjectExpr>)   return compileObject(v, hint);
        else if constexpr (std::is_same_v<T,FuncExpr>)     return compileFuncExpr(v, hint);
        else if constexpr (std::is_same_v<T,ClassExpr>)    return compileClass(v, hint);
        else if constexpr (std::is_same_v<T,TemplateExpr>) return compileTemplate(v, hint);
        else if constexpr (std::is_same_v<T,SequenceExpr>) return compileSequence(v, hint);
        else if constexpr (std::is_same_v<T,AwaitExpr>)    return compileAwait(v, hint);
        else if constexpr (std::is_same_v<T,YieldExpr>)    return compileYield(v, hint);
        else if constexpr (std::is_same_v<T,SpreadExpr>)   return compileExpr(*v.expr, hint);
        else { uint8_t r=allocReg(); emit(OP_LOAD_UNDEF,r); return r; }
    }, e.v);
}

uint8_t Compiler::compileLiteral(const LiteralExpr& e) {
    uint8_t dst = allocReg();
    if (e.isUndefined || (!e.isNum && !e.isStr && !e.isBool && !e.isNull)) {
        emit(OP_LOAD_UNDEF, dst);
    } else if (e.isNull) {
        emit(OP_LOAD_NULL, dst);
    } else if (e.isBool) {
        emit(e.boolVal ? OP_LOAD_TRUE : OP_LOAD_FALSE, dst);
    } else if (e.isNum) {
        double n = e.numVal;
        if (n == (double)(int16_t)n) {
            Instruction ins; ins.op = OP_LOAD_INT; ins.a = dst; ins.setsbc((int16_t)n);
            m_fn->code.push_back(ins); m_fn->lines.push_back(0);
        } else {
            uint16_t k = m_fn->addConst(JsValue::number(n));
            Instruction ins; ins.op = OP_LOAD_CONST; ins.a = dst; ins.setbc(k);
            m_fn->code.push_back(ins); m_fn->lines.push_back(0);
        }
    } else if (e.isStr) {
        if (e.strVal.substr(0, 9) == "__regex__") {
            // Regex literal: call new RegExp(src)
            // For now store as string; VM will handle
            uint16_t k = m_fn->addConstString(e.strVal);
            Instruction ins; ins.op=OP_LOAD_CONST; ins.a=dst; ins.setbc(k);
            m_fn->code.push_back(ins); m_fn->lines.push_back(0);
        } else {
            uint16_t k = m_fn->addConstString(e.strVal);
            Instruction ins; ins.op=OP_LOAD_CONST; ins.a=dst; ins.setbc(k);
            m_fn->code.push_back(ins); m_fn->lines.push_back(0);
        }
    }
    return dst;
}

uint8_t Compiler::compileIdent(const IdentExpr& e, int hint) {
    return loadVar(e.name, hint, 0);
}

uint8_t Compiler::compileBinary(const BinaryExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    uint8_t l = compileExpr(*e.left);
    uint8_t r = compileExpr(*e.right);
    static const std::unordered_map<std::string, Opcode> binOps = {
        {"+",OP_ADD},{"-",OP_SUB},{"*",OP_MUL},{"/",OP_DIV},{"%",OP_MOD},{"**",OP_POW},
        {"&",OP_BAND},{"|",OP_BOR},{"^",OP_BXOR},{"<<",OP_SHL},{">>",OP_SHR},{">>>",OP_USHR},
        {"==",OP_EQ},{"!=",OP_NEQ},{"===",OP_SEQ},{"!==",OP_SNEQ},
        {"<",OP_LT},{"<=",OP_LTE},{">",OP_GT},{">=",OP_GTE},
        {"instanceof",OP_INSTANCEOF},{"in",OP_IN},
    };
    auto it = binOps.find(e.op);
    if (it != binOps.end()) emit(it->second, dst, l, r);
    else emit(OP_ADD, dst, l, r); // fallback
    freeReg(l); freeReg(r);
    return dst;
}

uint8_t Compiler::compileLogical(const LogicalExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    uint8_t l = compileExpr(*e.left);
    emit(OP_MOVE, dst, l); freeReg(l);
    int j;
    if (e.op == "&&")      j = m_fn->emitJump(OP_JUMP_FALSE_POP, dst);
    else if (e.op == "||") j = m_fn->emitJump(OP_JUMP_TRUE_POP,  dst);
    else                   j = m_fn->emitJump(OP_JUMP_NULLISH,    dst);
    uint8_t r = compileExpr(*e.right);
    emit(OP_MOVE, dst, r); freeReg(r);
    m_fn->patchJump(j);
    return dst;
}

uint8_t Compiler::compileAssign(const AssignExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    if (e.op == "=") {
        uint8_t val = compileExpr(*e.value);
        emitStore(*e.target, val);
        emit(OP_MOVE, dst, val);
        freeReg(val);
    } else {
        // Compound: x += y  →  load x, compute, store back
        std::string baseOp = e.op.substr(0, e.op.size()-1); // strip '='
        uint8_t lhs = compileExpr(*e.target);
        uint8_t rhs = compileExpr(*e.value);
        static const std::unordered_map<std::string,Opcode> ops = {
            {"+",OP_ADD},{"-",OP_SUB},{"*",OP_MUL},{"/",OP_DIV},{"%",OP_MOD},{"**",OP_POW},
            {"&",OP_BAND},{"|",OP_BOR},{"^",OP_BXOR},{"<<",OP_SHL},{">>",OP_SHR},{">>>",OP_USHR},
            {"&&",OP_NOP},{"||",OP_NOP},{"??",OP_NOP},
        };
        auto it = ops.find(baseOp);
        if (it != ops.end() && it->second != OP_NOP)
            emit(it->second, dst, lhs, rhs);
        else emit(OP_ADD, dst, lhs, rhs); // fallback
        freeReg(lhs); freeReg(rhs);
        emitStore(*e.target, dst);
    }
    return dst;
}

void Compiler::emitStore(const Expr& target, uint8_t valReg) {
    if (target.is<IdentExpr>()) {
        storeVar(target.as<IdentExpr>().name, valReg, target.line);
    } else if (target.is<MemberExpr>()) {
        auto& m = target.as<MemberExpr>();
        uint8_t obj = compileExpr(*m.obj);
        if (m.computed) {
            uint8_t key = compileExpr(*m.prop);
            emit(OP_SET_PROP, obj, key, valReg);
            freeReg(key);
        } else {
            auto& propExpr = m.prop->as<LiteralExpr>();
            emitSetStaticProp(obj, propExpr.strVal, valReg, target.line);
        }
        freeReg(obj);
    } else if (target.is<ArrayPattern>() || target.is<ObjectPattern>()) {
        emitDestructure(target, valReg);
    }
}

void Compiler::emitDestructure(const Expr& pat, uint8_t src) {
    if (pat.is<ArrayPattern>()) {
        auto& ap = pat.as<ArrayPattern>();
        for (size_t i = 0; i < ap.elements.size(); i++) {
            if (!ap.elements[i]) continue;
            uint8_t idx = allocReg();
            Instruction ins; ins.op=OP_LOAD_INT; ins.a=idx; ins.setsbc((int16_t)i);
            m_fn->code.push_back(ins); m_fn->lines.push_back(0);
            uint8_t el = allocReg();
            emit(OP_GET_PROP, el, src, idx);
            freeReg(idx);
            emitStore(*ap.elements[i], el);
            freeReg(el);
        }
        if (!ap.rest.empty()) {
            uint8_t sliceFn = allocReg();
            uint8_t startArg = allocReg();
            uint8_t restArr = allocReg();
            emitGetStaticProp(sliceFn, src, "slice");
            Instruction start; start.op = OP_LOAD_INT; start.a = startArg; start.setsbc((int16_t)ap.elements.size());
            m_fn->code.push_back(start); m_fn->lines.push_back(0);
            emit(OP_CALL, restArr, src, sliceFn);
            emit(OP_NOP, 1);
            storeVar(ap.rest, restArr, 0);
            freeReg(restArr); freeReg(startArg); freeReg(sliceFn);
        }
    } else if (pat.is<ObjectPattern>()) {
        auto& op = pat.as<ObjectPattern>();
        for (auto& p : op.props) {
            if (p.rest) continue;
            uint8_t propReg = allocReg();
            emitGetStaticProp(propReg, src, p.key);
            if (p.value) emitStore(*p.value, propReg);
            freeReg(propReg);
        }
        for (auto& p : op.props) {
            if (!p.rest || p.key.empty()) continue;
            uint8_t objectReg = loadVar("Object", -1, 0);
            uint8_t assignFn = allocReg();
            emitGetStaticProp(assignFn, objectReg, "assign");
            uint8_t restObj = allocReg();
            uint8_t sourceArg = allocReg();
            emit(OP_NEW_OBJECT, restObj);
            emit(OP_MOVE, sourceArg, src);
            uint8_t ignored = allocReg();
            emit(OP_CALL, ignored, objectReg, assignFn);
            emit(OP_NOP, 2);
            freeReg(ignored);

            for (auto& bound : op.props) {
                if (bound.rest) continue;
                uint8_t key = allocReg();
                uint16_t ki = m_fn->addConstString(bound.key);
                Instruction keyLoad; keyLoad.op = OP_LOAD_CONST; keyLoad.a = key; keyLoad.setbc(ki);
                m_fn->code.push_back(keyLoad); m_fn->lines.push_back(0);
                uint8_t deleted = allocReg();
                emit(OP_DELETE, deleted, restObj, key);
                freeReg(deleted); freeReg(key);
            }

            storeVar(p.key, restObj, 0);
            freeReg(sourceArg); freeReg(restObj); freeReg(assignFn); freeReg(objectReg);
        }
    }
}

uint8_t Compiler::compileTernary(const TernaryExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    uint8_t cond = compileExpr(*e.cond);
    int jf = m_fn->emitJump(OP_JUMP_FALSE, cond); freeReg(cond);
    uint8_t yes = compileExpr(*e.yes); emit(OP_MOVE, dst, yes); freeReg(yes);
    int jEnd = m_fn->emitJump(OP_JUMP);
    m_fn->patchJump(jf);
    uint8_t no = compileExpr(*e.no); emit(OP_MOVE, dst, no); freeReg(no);
    m_fn->patchJump(jEnd);
    return dst;
}

uint8_t Compiler::compileMember(const MemberExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    uint8_t obj = compileExpr(*e.obj);
    // Optional chaining (?.) — if obj is null/undefined, produce undefined
    // instead of throwing a TypeError on property access.
    int skipJump = -1;
    if (e.optional) {
        // OP_JUMP_NULLISH jumps when NOT null/undefined (for ??).
        // For ?. we want: if NOT nullish → jump to property access; else load undef.
        emit(OP_MOVE, dst, obj);
        int notNullJump = m_fn->emitJump(OP_JUMP_NULLISH, dst);
        // IS nullish: load undefined, skip property access.
        emit(OP_LOAD_UNDEF, dst);
        skipJump = m_fn->emitJump(OP_JUMP);
        // NOT nullish: do the property access.
        m_fn->patchJump(notNullJump);
        if (e.computed) {
            uint8_t key = compileExpr(*e.prop);
            emit(OP_GET_PROP, dst, obj, key);
            freeReg(key);
        } else {
            auto& lit = e.prop->as<LiteralExpr>();
            emitGetStaticProp(dst, obj, lit.strVal, e.prop->line);
        }
        m_fn->patchJump(skipJump);
        freeReg(obj);
        return dst;
    }
    if (e.computed) {
        uint8_t key = compileExpr(*e.prop);
        emit(OP_GET_PROP, dst, obj, key);
        freeReg(key);
    } else {
        auto& lit = e.prop->as<LiteralExpr>();
        emitGetStaticProp(dst, obj, lit.strVal, e.prop->line);
    }
    freeReg(obj);
    return dst;
}

uint8_t Compiler::compileCall(const CallExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    // Determine this and fn
    uint8_t thisReg = allocReg();
    uint8_t fnReg   = allocReg();

    if (e.callee->is<MemberExpr>()) {
        auto& m = e.callee->as<MemberExpr>();
        uint8_t obj = compileExpr(*m.obj);
        emit(OP_MOVE, thisReg, obj);
        if (m.computed) {
            uint8_t key = compileExpr(*m.prop);
            emit(OP_GET_PROP, fnReg, obj, key); freeReg(key);
        } else {
            auto& lit = m.prop->as<LiteralExpr>();
            emitGetStaticProp(fnReg, obj, lit.strVal, m.prop->line);
        }
        freeReg(obj);
    } else if (e.callee->is<SuperExpr>()) {
        emit(OP_LOAD_UNDEF, thisReg);
        loadVar("__super__", fnReg, 0);
    } else {
        emit(OP_LOAD_UNDEF, thisReg);
        uint8_t fn = compileExpr(*e.callee);
        emit(OP_MOVE, fnReg, fn); freeReg(fn);
    }

    // Compile args
    uint8_t argc = 0;
    std::vector<uint8_t> argRegs;
    for (auto& a : e.args) {
        uint8_t ar = allocReg();
        if (a->is<SpreadExpr>()) {
            uint8_t spread = compileExpr(*a->as<SpreadExpr>().expr);
            emit(OP_SPREAD_CALL, ar, spread); freeReg(spread);
        } else {
            uint8_t av = compileExpr(*a);
            emit(OP_MOVE, ar, av); freeReg(av);
        }
        argRegs.push_back(ar);
        argc++;
    }

    if (e.isNew) {
        emit(OP_NEW, dst, fnReg, (uint8_t)argc);
    } else {
        emit(OP_CALL, dst, thisReg, fnReg);
        emit(OP_NOP, (uint8_t)argc); // argc follows call
    }

    for (auto ar : argRegs) freeReg(ar);
    freeReg(fnReg); freeReg(thisReg);
    return dst;
}

uint8_t Compiler::compileUnary(const UnaryExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    if (e.op == "typeof") {
        uint8_t src = compileExpr(*e.expr);
        emit(OP_TYPEOF, dst, src); freeReg(src);
        return dst;
    }
    if (e.op == "void") {
        uint8_t src = compileExpr(*e.expr); freeReg(src);
        emit(OP_LOAD_UNDEF, dst);
        return dst;
    }
    if (e.op == "delete") {
        if (e.expr->is<MemberExpr>()) {
            auto& m = e.expr->as<MemberExpr>();
            uint8_t obj = compileExpr(*m.obj);
            uint8_t key;
            if (m.computed) { key = compileExpr(*m.prop); }
            else { key = allocReg(); uint16_t ki = m_fn->addConstString(m.prop->as<LiteralExpr>().strVal);
                   Instruction ins; ins.op=OP_LOAD_CONST; ins.a=key; ins.setbc(ki); m_fn->code.push_back(ins); m_fn->lines.push_back(0); }
            emit(OP_DELETE, dst, obj, key);
            freeReg(key); freeReg(obj);
        } else { emit(OP_LOAD_TRUE, dst); }
        return dst;
    }
    if (e.postfix) {
        // x++ / x--
        uint8_t src = compileExpr(*e.expr);
        emit(OP_MOVE, dst, src); // return old value
        emit(e.op == "++" ? OP_INC : OP_DEC, src);
        emitStore(*e.expr, src);
        freeReg(src);
        return dst;
    }
    if (e.op == "++" || e.op == "--") {
        uint8_t src = compileExpr(*e.expr);
        emit(e.op == "++" ? OP_INC : OP_DEC, src);
        emitStore(*e.expr, src);
        emit(OP_MOVE, dst, src);
        freeReg(src);
        return dst;
    }
    uint8_t src = compileExpr(*e.expr);
    if      (e.op == "!")  emit(OP_NOT, dst, src);
    else if (e.op == "-")  emit(OP_NEG, dst, src);
    else if (e.op == "+")  emit(OP_PLUS, dst, src);
    else if (e.op == "~")  emit(OP_BNOT, dst, src);
    else                   emit(OP_MOVE, dst, src);
    freeReg(src);
    return dst;
}

uint8_t Compiler::compileArray(const ArrayExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    uint16_t cnt = (uint16_t)e.elements.size();
    Instruction ni; ni.op=OP_NEW_ARRAY; ni.a=dst; ni.setbc(cnt);
    m_fn->code.push_back(ni); m_fn->lines.push_back(0);
    for (auto& el : e.elements) {
        if (!el) continue; // hole
        if (el->is<SpreadExpr>()) {
            uint8_t spread = compileExpr(*el->as<SpreadExpr>().expr);
            emit(OP_SPREAD_CALL, dst, spread); freeReg(spread);
        } else {
            uint8_t v = compileExpr(*el);
            emit(OP_ARRAY_PUSH, dst, v); freeReg(v);
        }
    }
    return dst;
}

uint8_t Compiler::compileObject(const ObjectExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    emit(OP_NEW_OBJECT, dst);
    for (auto& p : e.props) {
        if (!p.key && p.value && p.value->is<SpreadExpr>()) {
            // Spread: Object.assign(dst, spread)
            uint8_t spread = compileExpr(*p.value->as<SpreadExpr>().expr);
            // Call Object.assign(dst, spread)
            uint8_t objAssign = loadVar("Object", -1, 0);
            uint8_t assignFn  = allocReg();
            emitGetStaticProp(assignFn, objAssign, "assign");
            uint8_t a1=allocReg(), a2=allocReg();
            emit(OP_MOVE,a1,dst); emit(OP_MOVE,a2,spread);
            uint8_t r=allocReg(); emit(OP_CALL,r,objAssign,assignFn); emit(OP_NOP,2);
            freeReg(r);freeReg(a2);freeReg(a1);freeReg(assignFn);freeReg(objAssign);freeReg(spread);
            continue;
        }
        uint8_t val = (p.isMethod || p.isGet || p.isSet) ? compileFuncExpr(p.value->as<FuncExpr>(), -1) : compileExpr(*p.value);
        if (p.isGet || p.isSet) {
            // Getter/setter: store as __get_key__ / __set_key__ for the VM to intercept.
            auto& lit = p.key->as<LiteralExpr>();
            std::string internalKey = (p.isGet ? "__get_" : "__set_") + lit.strVal + "__";
            emitSetStaticProp(dst, internalKey, val, p.key->line);
        } else if (p.computed) {
            uint8_t key = compileExpr(*p.key);
            emit(OP_SET_PROP, dst, key, val); freeReg(key);
        } else {
            auto& lit = p.key->as<LiteralExpr>();
            emitSetStaticProp(dst, lit.strVal, val, p.key->line);
        }
        freeReg(val);
    }
    return dst;
}

uint8_t Compiler::compileFuncExpr(const FuncExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    // Compile inner function
    auto inner = std::make_unique<BytecodeFunction>();
    inner->name       = e.name;
    inner->paramCount = (uint8_t)e.params.size();
    inner->hasRest    = !e.restParam.empty();
    inner->restParam  = e.restParam;
    inner->isArrow    = e.isArrow;
    inner->isAsync    = e.isAsync;
    inner->isGenerator= e.isStar;
    inner->regCount   = (uint8_t)e.params.size();

    Compiler inner_c(inner.get(), this);
    inner_c.pushScope(true);
    // Declare params as locals
    for (size_t i = 0; i < e.params.size(); i++) {
        int r = inner_c.declareLocal(e.params[i], false);
        if (i < e.defaults.size() && e.defaults[i]) {
            // emit default check: if param === undefined, assign default
            uint8_t undef = inner_c.allocReg();
            inner_c.emit(OP_LOAD_UNDEF, undef);
            uint8_t cmp = inner_c.allocReg();
            inner_c.emit(OP_SEQ, cmp, (uint8_t)r, undef);
            inner_c.freeReg(undef);
            int jf = inner_c.m_fn->emitJump(OP_JUMP_FALSE, cmp);
            inner_c.freeReg(cmp);
            uint8_t dv = inner_c.compileExpr(*e.defaults[i]);
            inner_c.emit(OP_MOVE, (uint8_t)r, dv);
            inner_c.freeReg(dv);
            inner_c.m_fn->patchJump(jf);
        }
    }
    if (e.isExprBody && e.body) {
        if (e.body->is<ExprStmt>()) {
            uint8_t r = inner_c.compileExpr(*e.body->as<ExprStmt>().expr);
            inner_c.emit(OP_RETURN, r);
            inner_c.freeReg(r);
        }
    } else if (e.body) {
        if (e.body->is<BlockStmt>()) {
            for (auto& s : e.body->as<BlockStmt>().body)
                inner_c.compileStmt(*s);
        } else {
            inner_c.compileStmt(*e.body);
        }
    }
    inner_c.emit(OP_RETURN_UNDEF);
    inner_c.popScope();
    // Copy upval descs
    inner->upvalDescs = std::vector<UpvalDesc>(inner_c.m_fn->upvalDescs);

    uint16_t fnIdx = (uint16_t)m_fn->innerFns.size();
    m_fn->innerFns.push_back(std::move(inner));
    Instruction ins; ins.op=OP_MAKE_FUNC; ins.a=dst; ins.setbc(fnIdx);
    m_fn->code.push_back(ins); m_fn->lines.push_back(0);
    return dst;
}

uint8_t Compiler::compileClass(const ClassExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    // Build a constructor function
    const FuncExpr* ctorFnPtr = nullptr;
    FuncExpr defaultCtor;
    defaultCtor.isArrow = false;
    // Find constructor method
    for (auto& m : e.methods) {
        if (!m.isStatic && m.key && m.key->is<LiteralExpr>() && m.key->as<LiteralExpr>().strVal == "constructor") {
            ctorFnPtr = &m.fn;
            break;
        }
    }
    if (!ctorFnPtr) ctorFnPtr = &defaultCtor;
    uint8_t ctorReg = compileFuncExpr(*ctorFnPtr, -1);
    emit(OP_MOVE, dst, ctorReg); freeReg(ctorReg);
    // Set prototype property
    uint8_t proto = allocReg(); emit(OP_NEW_OBJECT, proto);
    emitSetStaticProp(dst, "prototype", proto);
    freeReg(proto);
    // Add methods to prototype
    for (auto& m : e.methods) {
        if (m.isStatic || !m.key) continue;
        std::string nm;
        if (m.key->is<LiteralExpr>()) nm = m.key->as<LiteralExpr>().strVal;
        if (nm == "constructor") continue;
        uint8_t protoAccess = allocReg();
        emitGetStaticProp(protoAccess, dst, "prototype");
        uint8_t methReg = compileFuncExpr(m.fn, -1);
        emitSetStaticProp(protoAccess, nm, methReg);
        freeReg(methReg); freeReg(protoAccess);
    }
    return dst;
}

uint8_t Compiler::compileTemplate(const TemplateExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    if (e.cooked.empty()) { emit(OP_LOAD_CONST, dst); return dst; }
    // Build: cooked[0] + expr[0] + cooked[1] + expr[1] + ... + cooked[n]
    uint16_t ki = m_fn->addConstString(e.cooked[0]);
    Instruction ins; ins.op=OP_LOAD_CONST; ins.a=dst; ins.setbc(ki);
    m_fn->code.push_back(ins); m_fn->lines.push_back(0);
    for (size_t i = 0; i < e.exprs.size(); i++) {
        uint8_t ex = compileExpr(*e.exprs[i]);
        emit(OP_ADD, dst, dst, ex); freeReg(ex);
        if (i+1 < e.cooked.size()) {
            uint8_t s = allocReg();
            uint16_t k2 = m_fn->addConstString(e.cooked[i+1]);
            Instruction i2; i2.op=OP_LOAD_CONST; i2.a=s; i2.setbc(k2);
            m_fn->code.push_back(i2); m_fn->lines.push_back(0);
            emit(OP_ADD, dst, dst, s); freeReg(s);
        }
    }
    return dst;
}

uint8_t Compiler::compileSequence(const SequenceExpr& e, int hint) {
    uint8_t last = 0;
    for (size_t i = 0; i < e.exprs.size(); i++) {
        if (i > 0) freeReg(last);
        last = compileExpr(*e.exprs[i], i+1 == e.exprs.size() ? hint : -1);
    }
    return last;
}

uint8_t Compiler::compileAwait(const AwaitExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    uint8_t src = compileExpr(*e.expr);
    emit(OP_AWAIT, dst, src); freeReg(src);
    return dst;
}

uint8_t Compiler::compileYield(const YieldExpr& e, int hint) {
    uint8_t dst = (hint >= 0) ? (uint8_t)hint : allocReg();
    uint8_t src = e.expr ? compileExpr(*e.expr) : allocReg();
    if (!e.expr) emit(OP_LOAD_UNDEF, src);
    emit(OP_YIELD, src); emit(OP_MOVE, dst, src);
    freeReg(src);
    return dst;
}

void Compiler::compileProgram(const Program& prog) {
    pushScope(true);
    for (auto& s : prog.body) compileStmt(*s);
    emit(OP_RETURN_UNDEF);
    popScope();
}

std::unique_ptr<BytecodeFunction> Compiler::compile(const Program& prog) {
    auto fn = std::make_unique<BytecodeFunction>();
    fn->name = "<global>";
    Compiler c(fn.get(), nullptr);
    c.compileProgram(prog);
    return fn;
}

std::unique_ptr<BytecodeFunction> Compiler::compileFn(const FuncExpr& fe, const std::string& name) {
    auto fn = std::make_unique<BytecodeFunction>();
    fn->name = name.empty() ? fe.name : name;
    Compiler c(fn.get(), nullptr);
    // Directly compile
    c.pushScope(true);
    for (size_t i = 0; i < fe.params.size(); i++) {
        int r = c.declareLocal(fe.params[i], false);
        (void)r;
    }
    if (fe.isExprBody && fe.body && fe.body->is<ExprStmt>()) {
        uint8_t r = c.compileExpr(*fe.body->as<ExprStmt>().expr);
        c.emit(OP_RETURN, r); c.freeReg(r);
    } else if (fe.body) {
        if (fe.body->is<BlockStmt>())
            for (auto& s : fe.body->as<BlockStmt>().body) c.compileStmt(*s);
        else c.compileStmt(*fe.body);
    }
    c.emit(OP_RETURN_UNDEF);
    c.popScope();
    return fn;
}
