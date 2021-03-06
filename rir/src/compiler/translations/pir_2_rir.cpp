#include "pir_2_rir.h"
#include "../pir/pir_impl.h"
#include "../util/cfg.h"
#include "../util/visitor.h"
#include "interpreter/runtime.h"
#include "ir/CodeStream.h"
#include "ir/CodeVerifier.h"
#include "utils/FunctionWriter.h"

#include <algorithm>
#include <iomanip>

// #define DEBUGGING
#define ALLOC_DEBUG 1
#define PHI_REMOVE_DEBUG 1

#ifdef DEBUGGING
#define DEBUGCODE(flag, code)                                                  \
    if (flag)                                                                  \
    code
#else
#define DEBUGCODE(flag, code) /* nothing */
#endif

namespace rir {
namespace pir {

namespace {

/*
 * SSAAllocator assigns each instruction to a local variable number, or the
 * stack. It uses the following algorithm:
 *
 * 1. Split phis with moves. This translates the IR to CSSA (see toCSSA).
 * 2. Compute liveness (see computeLiveness):
 *    Liveness intervals are stored as:
 *        Instruction* -> BB id -> { start : pos, end : pos, live : bool}
 *    Two Instructions interfere iff there is a BB where they are both live
 *    and the start-end overlap.
 * 3. Use simple heuristics to detect Instructions that can stay on the RIR
 *    stack (see computeStackAllocation):
 *    1. Use stack slots for instructions which are used
 *       (i)   exactly once,
 *       (ii)  in stack order,
 *       (iii) within the same BB.
 *    2. Use stack slots for phi which are
 *       (i)  at the beginning of a BB, and
 *       (ii) all inputs are at the end of all immediate predecessor BBs.
 * 4. Assign the remaining Instructions to local RIR variable numbers
 *    (see computeAllocation):
 *    1. Coalesc all remaining phi with their inputs. This is save since we are
 *       already in CSSA. Directly allocate a register on the fly, such that.
 *    2. Traverse the dominance tree and eagerly allocate the remaining ones
 * 5. For debugging, verify the assignment with a static analysis that simulates
 *    the variable and stack usage (see verify).
 */
class SSAAllocator {
  public:
    CFG cfg;
    DominanceGraph dom;
    Code* code;
    size_t bbsSize;

    typedef size_t SlotNumber;
    const static SlotNumber unassignedSlot = 0;
    const static SlotNumber stackSlot = -1;

    std::unordered_map<Value*, SlotNumber> allocation;

    struct BBLiveness {
        uint8_t live = false;
        unsigned begin = -1;
        unsigned end = -1;
    };
    struct Liveness : public std::vector<BBLiveness> {
        bool interfere(const Liveness& other) const {
            assert(size() == other.size());
            for (size_t i = 0; i < size(); ++i) {
                const BBLiveness& mine = (*this)[i];
                const BBLiveness& their = other[i];
                if (mine.live && their.live) {
                    if (mine.begin == their.begin ||
                        (mine.begin < their.begin && mine.end >= their.begin) ||
                        (mine.begin > their.begin && their.end >= mine.begin))
                        return true;
                }
            }
            return false;
        }
    };
    std::unordered_map<Value*, Liveness> livenessInterval;

    SSAAllocator(Code* code, bool verbose)
        : cfg(code), dom(code), code(code), bbsSize(code->nextBBId) {
        computeLiveness(verbose);
        computeStackAllocation();
        computeAllocation();
    }

    // Run backwards analysis to compute livenessintervals
    void computeLiveness(bool verbose = 0) {
        // temp list of live out sets for every BB
        std::unordered_map<BB*, std::set<Value*>> liveAtEnd(bbsSize);

        std::set<BB*> todo;
        for (auto e : cfg.exits())
            todo.insert(e);

        while (!todo.empty()) {
            BB* bb = *todo.begin();
            todo.erase(todo.begin());

            // keep track of currently live variables
            std::set<Value*> accumulated;
            std::map<BB*, std::set<Value*>> accumulatedPhiInput;

            // Mark all (backwards) incoming live variables
            for (auto v : liveAtEnd[bb]) {
                assert(livenessInterval.count(v));
                auto& liveRange = livenessInterval.at(v)[bb->id];
                if (!liveRange.live || liveRange.end < bb->size()) {
                    liveRange.live = true;
                    liveRange.end = bb->size();
                    accumulated.insert(v);
                }
            }

            // Run BB in reverse
            size_t pos = bb->size();
            if (!bb->isEmpty()) {
                auto ip = bb->end();
                do {
                    --ip;
                    --pos;
                    Instruction* i = *ip;
                    Phi* phi = Phi::Cast(i);

                    auto markIfNotSeen = [&](Value* v) {
                        if (!livenessInterval.count(v)) {
                            // First time we see this variable, need to allocate
                            // vector of all livereanges
                            livenessInterval[v].resize(bbsSize);
                            assert(!livenessInterval[v][bb->id].live);
                        }
                        auto& liveRange = livenessInterval[v][bb->id];
                        if (!liveRange.live) {
                            liveRange.live = true;
                            liveRange.end = pos;
                            return true;
                        }
                        return false;
                    };

                    // First set all arguments to be live
                    if (phi)
                        phi->eachArg([&](BB* in, Value* v) {
                            if (markIfNotSeen(v))
                                accumulatedPhiInput[in].insert(v);
                        });
                    else
                        i->eachArg([&](Value* v) {
                            if (markIfNotSeen(v))
                                accumulated.insert(v);
                        });

                    // Mark the end of the current instructions liveness
                    if (accumulated.count(i)) {
                        assert(livenessInterval.count(i));
                        auto& liveRange = livenessInterval[i][bb->id];
                        assert(liveRange.live);
                        liveRange.begin = pos;
                        accumulated.erase(accumulated.find(i));
                    }
                } while (ip != bb->begin());
            }
            assert(pos == 0);

            // Mark everything that is live at the beginning of the BB.
            auto markLiveEntry = [&](Value* v) {
                assert(livenessInterval.count(v));
                auto& liveRange = livenessInterval[v][bb->id];
                assert(liveRange.live);
                liveRange.begin = 0;
            };

            for (auto v : accumulated)
                markLiveEntry(v);
            for (auto pi : accumulatedPhiInput)
                for (auto v : pi.second)
                    markLiveEntry(v);

            // Merge everything that is live at the beginning of the BB into the
            // incoming vars of all predecessors
            //
            // Phi inputs should only be merged to BB that are successors of the
            // input BBs
            auto merge = [&](BB* bb, const std::set<Value*>& live) {
                auto& liveOut = liveAtEnd[bb];
                if (!std::includes(liveOut.begin(), liveOut.end(), live.begin(),
                                   live.end())) {
                    liveOut.insert(live.begin(), live.end());
                    todo.insert(bb);
                }
            };
            auto mergePhiInp = [&](BB* bb) {
                for (auto in : accumulatedPhiInput) {
                    auto inBB = in.first;
                    auto inLive = in.second;
                    if (bb == inBB || cfg.isPredecessor(inBB, bb)) {
                        merge(bb, inLive);
                    }
                }
            };
            for (auto pre : cfg.immediatePredecessors(bb)) {
                bool firstTime = !liveAtEnd.count(pre);
                if (firstTime) {
                    liveAtEnd[pre] = accumulated;
                    mergePhiInp(pre);
                    todo.insert(pre);
                } else {
                    merge(pre, accumulated);
                    mergePhiInp(pre);
                }
            }
        }

        if (verbose) {
            std::cout << "======= Liveness ========\n";
            for (auto ll : livenessInterval) {
                auto& l = ll.second;
                ll.first->printRef(std::cout);
                std::cout << " is live : ";
                for (size_t i = 0; i < bbsSize; ++i) {
                    if (l[i].live) {
                        std::cout << "BB" << i << " [";
                        std::cout << l[i].begin << ",";
                        std::cout << l[i].end << "]  ";
                    }
                }
                std::cout << "\n";
            }
            std::cout << "======= End Liveness ========\n";
        }
    }

    void computeStackAllocation() {
        Visitor::run(code->entry, [&](BB* bb) {
            {
                // If a phi is at the beginning of a BB, and all inputs are at
                // the end of the immediate predecessors BB, we can allocate it
                // on the stack, since the stack is otherwise empty at the BB
                // boundaries.
                size_t pos = 1;
                for (auto i : *bb) {
                    Phi* phi = Phi::Cast(i);
                    if (!phi)
                        break;
                    bool argsInRightOrder = true;
                    phi->eachArg([&](BB* in, Value* v) {
                        if (in->next0 != bb || in->size() < pos ||
                            *(in->end() - pos) != v) {
                            argsInRightOrder = false;
                        }
                    });
                    if (!argsInRightOrder)
                        break;
                    phi->eachArg(
                        [&](BB*, Value* v) { allocation[v] = stackSlot; });
                    allocation[phi] = stackSlot;
                    pos++;
                }
            }

            // Precolor easy stack load-stores within one BB
            std::deque<Instruction*> stack;

            auto tryLoadingArgsFromStack = [&](Instruction* i) {
                if (i->nargs() == 0 || stack.size() < i->nargs())
                    return;

                // Match all args to stack slots.
                size_t newStackSize = stack.size();
                bool foundAll = true;
                auto check = stack.rbegin();
                i->eachArgRev([&](Value* arg) {
                    while (check != stack.rend() && *check != arg) {
                        ++check;
                        --newStackSize;
                    }

                    if (check == stack.rend()) {
                        foundAll = false;
                    } else {
                        // found arg!
                        ++check;
                        --newStackSize;
                    }
                });

                if (!foundAll)
                    return;

                // pop args from stack, discarding all unmatched values
                // in the process. For example if the stack contains
                // [xxx, A, B, C] and we match [A, C], then we will mark
                // A, C to be in a stack slot, discard B (it will become
                // a local variable later) and resize the stack to [xxx]
                stack.resize(newStackSize);
                i->eachArgRev([&](Value* arg) { allocation[arg] = stackSlot; });
            };

            for (auto i : *bb) {
                tryLoadingArgsFromStack(i);

                if (!allocation.count(i) && !(i->type == PirType::voyd()) &&
                    !Phi::Cast(i) && i->hasSingleUse()) {
                    stack.push_back(i);
                }
            }
        });
    }

    void computeAllocation() {
        std::unordered_map<SlotNumber, std::unordered_set<Value*>> reverseAlloc;
        auto slotIsAvailable = [&](SlotNumber slot, Value* i) {
            for (auto other : reverseAlloc[slot])
                if (livenessInterval.at(other).interfere(
                        livenessInterval.at(i)))
                    return false;
            return true;
        };

        // Precolor Phi
        Visitor::run(code->entry, [&](Instruction* i) {
            auto p = Phi::Cast(i);
            if (!p || allocation.count(p))
                return;
            SlotNumber slot = unassignedSlot;
            while (true) {
                ++slot;
                bool success = slotIsAvailable(slot, p);
                if (success) {
                    p->eachArg([&](BB*, Value* v) {
                        if (!slotIsAvailable(slot, v))
                            success = false;
                    });
                }
                if (success)
                    break;
            }
            allocation[i] = slot;
            reverseAlloc[slot].insert(i);
            p->eachArg([&](BB*, Value* v) {
                allocation[v] = slot;
                reverseAlloc[slot].insert(v);
            });
        });

        // Traverse the dominance graph in preorder and eagerly assign slots.
        // We assume that no critical paths exist, ie. we preprocessed the graph
        // such that every phi input is only used exactly once (by the phi).
        DominatorTreeVisitor<>(dom).run(code, [&](BB* bb) {
            auto findFreeSlot = [&](Instruction* i) {
                SlotNumber slot = unassignedSlot;
                for (;;) {
                    ++slot;
                    if (slotIsAvailable(slot, i)) {
                        allocation[i] = slot;
                        reverseAlloc[slot].insert(i);
                        break;
                    }
                };
            };

            size_t pos = 0;
            for (auto i : *bb) {
                ++pos;

                if (!allocation.count(i) && livenessInterval.count(i)) {
                    // Try to reuse input slot, to reduce moving
                    SlotNumber hint = unassignedSlot;
                    if (i->nargs() > 0) {
                        auto o = Instruction::Cast(i->arg(0).val());
                        if (o && allocation.count(o))
                            hint = allocation.at(o);
                    }
                    if (hint != unassignedSlot && hint != stackSlot &&
                        slotIsAvailable(hint, i)) {
                        allocation[i] = hint;
                        reverseAlloc[hint].insert(i);
                    } else {
                        findFreeSlot(i);
                    }
                }
            }
        });
    }

    void print(std::ostream& out = std::cout) {
        out << "======= Allocation ========\n";
        BreadthFirstVisitor::run(code->entry, [&](BB* bb) {
            out << "BB" << bb->id << ": ";
            for (auto a : allocation) {
                auto i = a.first;
                if (Instruction::Cast(i) && Instruction::Cast(i)->bb() != bb)
                    continue;
                i->printRef(out);
                out << "@";
                if (allocation.at(i) == stackSlot)
                    out << "s";
                else
                    out << a.second;
                out << "   ";
            }
            out << "\n";
        });
        out << "dead: ";
        BreadthFirstVisitor::run(code->entry, [&](BB* bb) {
            for (auto i : *bb) {
                if (allocation.count(i) == 0) {
                    i->printRef(out);
                    out << "   ";
                }
            }
        });
        out << "\nslots: " << slots() << "\n======= End Allocation ========\n";
    }

    void verify() {
        // Explore all possible traces and verify the allocation
        typedef std::pair<BB*, BB*> Jmp;
        typedef std::unordered_map<size_t, Instruction*> RegisterFile;
        typedef std::deque<Instruction*> Stack;
        typedef std::function<void(BB*, RegisterFile&, Stack&)> VerifyBB;
        std::set<Jmp> branchTaken;

        VerifyBB verifyBB = [&](BB* bb, RegisterFile& reg, Stack& stack) {
            for (auto i : *bb) {
                Phi* phi = Phi::Cast(i);
                if (phi) {
                    SlotNumber slot = allocation.at(phi);
                    phi->eachArg([&](BB*, Value* arg) {
                        auto i = Instruction::Cast(arg);
                        if (!i)
                            return;
                        if (allocation[i] != slot) {
                            std::cerr << "REG alloc fail: ";
                            phi->printRef(std::cerr);
                            std::cerr << " and it's input ";
                            i->printRef(std::cerr);
                            std::cerr << " have different allocations : ";
                            if (allocation[phi] == stackSlot)
                                std::cerr << "stack";
                            else
                                std::cerr << allocation[phi];
                            std::cerr << " vs ";
                            if (allocation[i] == stackSlot)
                                std::cerr << "stack";
                            else
                                std::cerr << allocation[i];
                            std::cerr << "\n";
                            assert(false);
                        }
                    });
                    if (slot == stackSlot)
                        stack.pop_back();
                } else {
                    // Make sure all our args are live
                    i->eachArgRev([&](Value* a) {
                        auto i = Instruction::Cast(a);
                        if (!i)
                            return;
                        if (!allocation.count(a)) {
                            std::cerr << "REG alloc fail: ";
                            i->printRef(std::cerr);
                            std::cerr << " needs ";
                            a->printRef(std::cerr);
                            std::cerr << " but is not allocated\n";
                            assert(false);
                        } else {
                            Instruction* given = nullptr;
                            SlotNumber slot = allocation.at(a);
                            if (slot == stackSlot) {
                                given = stack.back();
                                stack.pop_back();
                            } else {
                                given = reg.at(slot);
                            }
                            if (given != a) {
                                std::cerr << "REG alloc fail: ";
                                i->printRef(std::cerr);
                                std::cerr << " needs ";
                                a->printRef(std::cerr);
                                if (slot == stackSlot) {
                                    std::cerr << " the stack has ";
                                } else {
                                    std::cerr << " but slot " << slot
                                              << " was overridden by ";
                                }
                                given->printRef(std::cerr);
                                std::cerr << "\n";
                                assert(false);
                            }
                        }
                    });
                }

                // Remember this instruction if it writes to a slot
                if (allocation.count(i)) {
                    if (allocation.at(i) == stackSlot)
                        stack.push_back(i);
                    else
                        reg[allocation.at(i)] = i;
                }
            }

            if (!bb->next0 && !bb->next1) {
                if (stack.size() != 0) {
                    std::cerr << "REG alloc fail: BB " << bb->id
                              << " tries to return with " << stack.size()
                              << " elements on the stack\n";
                    assert(false);
                }
            }

            if (bb->next0 && !branchTaken.count(Jmp(bb, bb->next0))) {
                branchTaken.insert(Jmp(bb, bb->next0));
                if (!bb->next1) {
                    verifyBB(bb->next0, reg, stack);
                } else {
                    // Need to copy here, since we are gonna explore next1 next
                    RegisterFile regC = reg;
                    Stack stackC = stack;
                    verifyBB(bb->next0, regC, stackC);
                }
            }
            if (bb->next1 && !branchTaken.count(Jmp(bb, bb->next1))) {
                branchTaken.insert(Jmp(bb, bb->next1));
                verifyBB(bb->next1, reg, stack);
            }
        };

        {
            RegisterFile f;
            Stack s;
            verifyBB(code->entry, f, s);
        }
    }

    size_t operator[](Value* v) const {
        assert(allocation.at(v) != stackSlot);
        return allocation.at(v) - 1;
    }

    size_t slots() const {
        unsigned max = 0;
        for (auto a : allocation) {
            if (a.second != stackSlot && max < a.second)
                max = a.second;
        }
        return max;
    }

    bool onStack(Value* v) const { return allocation.at(v) == stackSlot; }

    bool hasSlot(Value* v) const { return allocation.count(v); }
};

class Context {
  public:
    std::stack<CodeStream*> css;
    FunctionWriter& fun;

    Context(FunctionWriter& fun) : fun(fun) {}
    ~Context() { assert(css.empty()); }

    CodeStream& cs() { return *css.top(); }

    void pushDefaultArg(SEXP ast) {
        defaultArg.push(true);
        push(ast);
    }
    void pushPromise(SEXP ast) {
        defaultArg.push(false);
        push(ast);
    }
    void pushBody(SEXP ast) {
        defaultArg.push(false);
        push(ast);
    }

    BC::FunIdx finalizeCode(size_t localsCnt) {
        auto idx = cs().finalize(defaultArg.top(), localsCnt);
        delete css.top();
        defaultArg.pop();
        css.pop();
        return idx;
    }

  private:
    std::stack<bool> defaultArg;
    void push(SEXP ast) { css.push(new CodeStream(fun, ast)); }
};

class Pir2Rir {
  public:
    Pir2Rir(Pir2RirCompiler& cmp, Closure* cls) : compiler(cmp), cls(cls) {}
    size_t compileCode(Context& ctx, Code* code);
    size_t getPromiseIdx(Context& ctx, Promise* code);
    void toCSSA(Code* code);
    rir::Function* finalize();

  private:
    Pir2RirCompiler& compiler;
    Closure* cls;
    std::unordered_map<Promise*, BC::FunIdx> promises;
    std::unordered_map<Promise*, SEXP> argNames;
};

size_t Pir2Rir::compileCode(Context& ctx, Code* code) {
    toCSSA(code);

    if (compiler.debug.includes(DebugFlag::PrintCSSA))
        code->print(std::cout);

    SSAAllocator alloc(code,
                       compiler.debug.includes(DebugFlag::DebugAllocator));

    if (compiler.debug.includes(DebugFlag::PrintLivenessIntervals))
        alloc.print();

    if (compiler.debug.includes(DebugFlag::PrintFinalPir))
        code->print(std::cout);

    alloc.verify();

    // create labels for all bbs
    std::unordered_map<BB*, BC::Label> bbLabels;
    BreadthFirstVisitor::run(code->entry, [&](BB* bb) {
        if (!bb->isEmpty())
            bbLabels[bb] = ctx.cs().mkLabel();
    });

    BreadthFirstVisitor::run(code->entry, [&](BB* bb) {
        if (bb->isEmpty())
            return;

        CodeStream& cs = ctx.cs();
        cs << bbLabels[bb];

        Value* currentEnv = nullptr;

        for (auto it = bb->begin(); it != bb->end(); ++it) {
            auto instr = *it;

            bool hasResult =
                instr->type != PirType::voyd() && !Phi::Cast(instr);

            auto explicitEnvValue = [](Instruction* instr) {
                return MkEnv::Cast(instr) || Deopt::Cast(instr);
            };

            // Load Arguments to the stack
            {
                auto loadEnv = [&](BB::Instrs::iterator it, Value* what) {
                    if (Env::isStaticEnv(what)) {
                        cs << BC::push(Env::Cast(what)->rho);
                    } else if (what == Env::notClosed()) {
                        cs << BC::parentEnv();
                    } else {
                        if (!alloc.hasSlot(what)) {
                            std::cerr << "Don't know how to load the env ";
                            what->printRef(std::cerr);
                            std::cerr << " (" << tagToStr(what->tag) << ")\n";
                            assert(false);
                        }
                        if (!alloc.onStack(what))
                            cs << BC::ldloc(alloc[what]);
                    }
                };

                auto loadArg = [&](BB::Instrs::iterator it, Instruction* instr,
                                   Value* what) {
                    if (what == Missing::instance()) {
                        // if missing flows into instructions with more than one
                        // arg we will need stack shuffling here
                        assert(MkArg::Cast(instr) &&
                               "only mkarg supports missing");
                        cs << BC::push(R_UnboundValue);
                    } else {
                        if (!alloc.hasSlot(what)) {
                            std::cerr << "Don't know how to load the arg ";
                            what->printRef(std::cerr);
                            std::cerr << " (" << tagToStr(what->tag) << ")\n";
                            assert(false);
                        }
                        if (!alloc.onStack(what))
                            cs << BC::ldloc(alloc[what]);
                    }
                };

                // Step one: load and set env
                if (!Phi::Cast(instr)) {
                    if (instr->hasEnv() && !explicitEnvValue(instr)) {
                        // If the env is passed on the stack, it needs
                        // to be TOS here. To relax this condition some
                        // stack shuffling would be needed.
                        assert(instr->envSlot() == instr->nargs() - 1);
                        auto env = instr->env();
                        if (currentEnv != env) {
                            loadEnv(it, env);
                            cs << BC::setEnv();
                            currentEnv = env;
                        } else {
                            if (alloc.hasSlot(env) && alloc.onStack(env))
                                cs << BC::pop();
                        }
                    }
                }

                // Step two: load the rest
                if (!Phi::Cast(instr)) {
                    instr->eachArg([&](Value* what) {
                        if (instr->hasEnv() && instr->env() == what) {
                            if (explicitEnvValue(instr))
                                loadEnv(it, what);
                        } else {
                            loadArg(it, instr, what);
                        }
                    });
                }
            }

            switch (instr->tag) {
            case Tag::LdConst: {
                cs << BC::push(LdConst::Cast(instr)->c);
                break;
            }
            case Tag::LdFun: {
                auto ldfun = LdFun::Cast(instr);
                cs << BC::ldfun(ldfun->varName);
                break;
            }
            case Tag::LdVar: {
                auto ldvar = LdVar::Cast(instr);
                cs << BC::ldvarNoForce(ldvar->varName);
                break;
            }
            case Tag::ForSeqSize: {
                cs << BC::forSeqSize();
                // TODO: currently we always pop the sequence, since we cannot
                // deal with instructions that do not pop the value after use.
                // If it is used in a later instruction, it will be loaded
                // from a local variable again.
                cs << BC::swap() << BC::pop();
                break;
            }
            case Tag::LdArg: {
                cs << BC::ldarg(LdArg::Cast(instr)->id);
                break;
            }
            case Tag::StVarSuper: {
                auto stvar = StVarSuper::Cast(instr);
                cs << BC::stvarSuper(stvar->varName);
                break;
            }
            case Tag::LdVarSuper: {
                auto ldvar = LdVarSuper::Cast(instr);
                cs << BC::ldvarNoForceSuper(ldvar->varName);
                break;
            }
            case Tag::StVar: {
                auto stvar = StVar::Cast(instr);
                cs << BC::stvar(stvar->varName);
                break;
            }
            case Tag::Branch: {
                // jump through empty blocks
                auto next0 = bb->next0;
                while (next0->isEmpty())
                    next0 = next0->next0;
                auto next1 = bb->next1;
                while (next1->isEmpty())
                    next1 = next1->next0;

                cs << BC::brfalse(bbLabels[next0]) << BC::br(bbLabels[next1]);

                // this is the end of this BB
                return;
            }
            case Tag::Return: {
                cs << BC::ret();

                // this is the end of this BB
                return;
            }
            case Tag::MkArg: {
                cs << BC::promise(getPromiseIdx(ctx, MkArg::Cast(instr)->prom));
                break;
            }
            case Tag::MkFunCls: {
                auto mkfuncls = MkFunCls::Cast(instr);

                auto dt = DispatchTable::unpack(mkfuncls->code);

                if (dt->capacity() > 1 && !dt->available(1)) {
                    Pir2Rir pir2rir(compiler, mkfuncls->fun);
                    auto rirFun = pir2rir.finalize();
                    if (!compiler.debug.includes(DebugFlag::DryRun))
                        dt->put(1, rirFun);
                }
                cs << BC::push(mkfuncls->fml) << BC::push(mkfuncls->code)
                   << BC::push(mkfuncls->src) << BC::close();
                break;
            }
            case Tag::Is: {
                auto is = Is::Cast(instr);
                cs << BC::is(is->sexpTag);
                break;
            }
            case Tag::Subassign2_1D: {
                auto res = Subassign2_1D::Cast(instr);
                cs << BC::subassign2(res->sym);
                break;
            }

#define EMPTY(Name)                                                            \
    case Tag::Name: {                                                          \
        break;                                                                 \
    }
                EMPTY(PirCopy);
                EMPTY(CastType);
#undef EMPTY

            case Tag::LdFunctionEnv: {
                // TODO: what should happen? For now get the current env (should
                // be the promise environment that the evaluator was called
                // with) and store it into local and leave it set as current
                cs << BC::getEnv();
                break;
            }

#define SIMPLE(Name, Factory)                                                  \
    case Tag::Name: {                                                          \
        cs << BC::Factory();                                                   \
        break;                                                                 \
    }
                SIMPLE(Identical, identical);
                SIMPLE(LOr, lglOr);
                SIMPLE(LAnd, lglAnd);
                SIMPLE(Inc, inc);
                SIMPLE(Force, force);
                SIMPLE(AsTest, asbool);
                SIMPLE(Length, length);
                SIMPLE(ChkMissing, checkMissing);
                SIMPLE(ChkClosure, isfun);
                SIMPLE(Seq, seq);
                SIMPLE(MkCls, close);
                SIMPLE(Subassign1_1D, subassign1);
                SIMPLE(IsObject, isObj);
                SIMPLE(Int3, int3);
#undef SIMPLE

#define SIMPLE_WITH_SRCIDX(Name, Factory)                                      \
    case Tag::Name: {                                                          \
        cs << BC::Factory();                                                   \
        cs.addSrcIdx(instr->srcIdx);                                           \
        break;                                                                 \
    }
                SIMPLE_WITH_SRCIDX(Add, add);
                SIMPLE_WITH_SRCIDX(Sub, sub);
                SIMPLE_WITH_SRCIDX(Mul, mul);
                SIMPLE_WITH_SRCIDX(Div, div);
                SIMPLE_WITH_SRCIDX(IDiv, idiv);
                SIMPLE_WITH_SRCIDX(Mod, mod);
                SIMPLE_WITH_SRCIDX(Pow, pow);
                SIMPLE_WITH_SRCIDX(Lt, lt);
                SIMPLE_WITH_SRCIDX(Gt, gt);
                SIMPLE_WITH_SRCIDX(Lte, ge);
                SIMPLE_WITH_SRCIDX(Gte, le);
                SIMPLE_WITH_SRCIDX(Eq, eq);
                SIMPLE_WITH_SRCIDX(Neq, ne);
                SIMPLE_WITH_SRCIDX(Colon, colon);
                SIMPLE_WITH_SRCIDX(AsLogical, asLogical);
                SIMPLE_WITH_SRCIDX(Plus, uplus);
                SIMPLE_WITH_SRCIDX(Minus, uminus);
                SIMPLE_WITH_SRCIDX(Not, Not);
                SIMPLE_WITH_SRCIDX(Extract1_1D, extract1_1);
                SIMPLE_WITH_SRCIDX(Extract2_1D, extract2_1);
                SIMPLE_WITH_SRCIDX(Extract1_2D, extract1_2);
                SIMPLE_WITH_SRCIDX(Extract2_2D, extract2_2);
#undef SIMPLE_WITH_SRCIDX

            case Tag::Call: {
                auto call = Call::Cast(instr);
                cs << BC::call(call->nCallArgs(), Pool::get(call->srcIdx));
                break;
            }
            case Tag::StaticCall: {
                auto call = StaticCall::Cast(instr);
                compiler.compile(call->cls(), call->origin());
                cs << BC::staticCall(call->nCallArgs(), Pool::get(call->srcIdx),
                                     call->origin());
                break;
            }
            case Tag::CallBuiltin: {
                // TODO(mhyee): all args have to be values, optimize here?
                auto blt = CallBuiltin::Cast(instr);
                cs << BC::staticCall(blt->nCallArgs(), Pool::get(blt->srcIdx),
                                     blt->blt);
                break;
            }
            case Tag::CallSafeBuiltin: {
                // TODO(mhyee): all args have to be values, optimize here?
                auto blt = CallSafeBuiltin::Cast(instr);
                cs << BC::staticCall(blt->nargs(), Pool::get(blt->srcIdx),
                                     blt->blt);
                break;
            }
            case Tag::MkEnv: {
                auto mkenv = MkEnv::Cast(instr);

                cs << BC::makeEnv();

                if (mkenv->nLocals() > 0) {
                    cs << BC::setEnv();
                    currentEnv = instr;

                    mkenv->eachLocalVarRev(
                        [&](SEXP name, Value* val) { cs << BC::stvar(name); });

                    cs << BC::getEnv();
                }
                break;
            }
            case Tag::Phi: {
                // Phi functions are no-ops, because after allocation on CSSA
                // form, all arguments and the funcion itself are allocated to
                // the same place
                auto phi = Phi::Cast(instr);
                phi->eachArg([&](BB*, Value* arg) {
                    assert(((alloc.onStack(phi) && alloc.onStack(arg)) ||
                            (alloc[phi] == alloc[arg])) &&
                           "Phi inputs must all be allocated in 1 slot");
                });
                break;
            }
            case Tag::Deopt: {
                Deopt::Cast(instr)->eachArg([&](Value*) { cs << BC::pop(); });
                // TODO
                cs << BC::int3() << BC::push(R_NilValue) << BC::ret();
                return;
            }
            // values, not instructions
            case Tag::Missing:
            case Tag::Env:
            case Tag::Nil:
                break;
            // dummy sentinel enum item
            case Tag::_UNUSED_:
                break;
            }

            // Store the result
            if (hasResult) {
                if (!alloc.hasSlot(instr))
                    cs << BC::pop();
                else if (!alloc.onStack(instr))
                    cs << BC::stloc(alloc[instr]);
            };
        }

        // this BB has exactly one successor, next0
        // jump through empty blocks
        assert(bb->next0);
        auto next = bb->next0;
        while (next->isEmpty())
            next = next->next0;
        cs << BC::br(bbLabels[next]);
    });

    return alloc.slots();
}

void Pir2Rir::toCSSA(Code* code) {

    // For each Phi, insert copies
    BreadthFirstVisitor::run(code->entry, [&](BB* bb) {
        // TODO: move all phi's to the beginning, then insert the copies not
        // after each phi but after all phi's
        for (auto it = bb->begin(); it != bb->end(); ++it) {
            auto instr = *it;
            Phi* phi = Phi::Cast(instr);
            if (phi) {
                for (size_t i = 0; i < phi->nargs(); ++i) {
                    BB* pred = phi->input[i];
                    // pred is either jump (insert copy at end) or branch
                    // (insert copy before the branch instr)
                    auto it = pred->isJmp() ? pred->end() : pred->end() - 1;
                    Instruction* iav = Instruction::Cast(phi->arg(i).val());
                    auto copy = pred->insert(it, new PirCopy(iav));
                    phi->arg(i).val() = *copy;
                }
                auto phiCopy = new PirCopy(phi);
                phi->replaceUsesWith(phiCopy);
                it = bb->insert(it + 1, phiCopy);
            }
        }
    });

    DEBUGCODE(PHI_REMOVE_DEBUG, {
        std::cout << "--- phi copies inserted ---\n";
        code->print(std::cout);
    });
}

size_t Pir2Rir::getPromiseIdx(Context& ctx, Promise* p) {
    if (!promises.count(p)) {
        ctx.pushPromise(src_pool_at(globalContext(), p->srcPoolIdx));
        size_t localsCnt = compileCode(ctx, p);
        promises[p] = ctx.finalizeCode(localsCnt);
    }
    return promises.at(p);
}

rir::Function* Pir2Rir::finalize() {

    // TODO: keep track of source ast indices in the source pool
    // (for now, calls, promises and operators do)
    // + how to deal with inlined stuff?

    FunctionWriter function = FunctionWriter::create();
    Context ctx(function);

    size_t i = 0;
    for (auto arg : cls->defaultArgs) {
        if (!arg)
            continue;
        getPromiseIdx(ctx, arg);
        argNames[arg] = cls->argNames[i++];
    }
    ctx.pushBody(R_NilValue);
    size_t localsCnt = compileCode(ctx, cls);
    ctx.finalizeCode(localsCnt);

#ifdef ENABLE_SLOWASSERT
    CodeVerifier::verifyFunctionLayout(function.function->container(),
                                       globalContext());
#endif

    return function.function;
}

} // namespace

void Pir2RirCompiler::compile(Closure* cls, SEXP origin) {
    if (done.count(cls))
        return;
    // Avoid recursivly compiling the same closure
    done.insert(cls);

    auto table = DispatchTable::unpack(BODY(origin));
    if (table->available(1))
        return;

    Pir2Rir pir2rir(*this, cls);
    auto fun = pir2rir.finalize();

    if (debug.includes(DebugFlag::PrintFinalRir)) {
        std::cout << "============= Final RIR Version ========\n";
        auto it = fun->begin();
        while (it != fun->end()) {
            (*it)->print();
            ++it;
        }
    }

    if (debug.includes(DebugFlag::DryRun))
        return;

    Protect p(fun->container());

    auto oldFun = table->first();

    fun->invocationCount = oldFun->invocationCount;
    // TODO: are these still needed / used?
    fun->envLeaked = oldFun->envLeaked;
    fun->envChanged = oldFun->envChanged;
    // TODO: signatures need a rework
    fun->signature = oldFun->signature;

    if (debug.intersects(PrintDebugPasses)) {
        std::cout << "\n*********** Finished compiling: " << std::setw(17)
                  << std::left << oldFun << " ************\n";
        std::cout << "*************************************************"
                  << "*************\n";
    }
    table->put(1, fun);
}

} // namespace pir
} // namespace rir
