#ifndef RIR_OPTIMIZER_CLEANUP_H
#define RIR_OPTIMIZER_CLEANUP_H

#include "analysis_framework/dispatchers.h"

namespace rir {

class BCCleanup : public InstructionDispatcher::Receiver {
  public:
    InstructionDispatcher dispatcher;
    CodeEditor& code_;
    bool leaksEnvironment;

    BCCleanup(CodeEditor& code) : dispatcher(*this), code_(code) {}

    void nop_(CodeEditor::Iterator ins) override {
        CodeEditor::Cursor cur = ins.asCursor(code_);
        cur.remove();
        return;
    }

    void ldvar_(CodeEditor::Iterator ins) override {
        // double load elimination : ldvar a; ldvar a;
        if (ins != code_.begin()) {
            auto prev = ins - 1;
            if ((*prev).is(Opcode::ldvar_) && *ins == *prev) {
                CodeEditor::Cursor cur = ins.asCursor(code_);
                cur.remove();
                cur << BC::dup();
                return;
            }
        }
    }

    void invisible_(CodeEditor::Iterator ins) override {
        if ((ins + 1) != code_.end()) {
            if ((*(ins + 1)).is(Opcode::pop_) ||
                (*(ins + 1)).is(Opcode::visible_) ||
                (*(ins + 1)).is(Opcode::ldvar_)) {
                ins.asCursor(code_).remove();
            }
        }
    }

    void guard_fun_(CodeEditor::Iterator ins) override {
        SEXP name = Pool::get((*ins).immediate.guard_fun_args.expected);

        if (ins != code_.begin()) {
            CodeEditor::Iterator bubbleUp = ins;
            while (bubbleUp != code_.begin()) {
                bubbleUp = bubbleUp - 1;
                auto cur = *bubbleUp;
                // We cannot move the guard across those instructions
                if (cur.is(Opcode::label) || !cur.isPure() || cur.isReturn()) {
                    if (!cur.is(Opcode::stvar_)) {
                        break;
                    }
                    // stvar that does not interfere with the guard we can
                    // skip. Otherwise we treat it as a barrier. Note, this is a
                    // conservative approximation. Assigning to a variable with
                    // the same name does not guarantee that the guard fails.
                    // We could still:
                    // * override it with the same function
                    // * override it with a non-function value, which (due to
                    //   the amazing R lookup semantics) does not override
                    //   functions.
                    if (Pool::get(cur.immediate.pool) == name)
                        break;
                }
                if (cur == *ins) {
                    // This guard is redundant, remove it
                    ins.asCursor(code_).remove();
                    break;
                }
            }
        }
    }

    void pick_(CodeEditor::Iterator ins) override {
        // double pick elimination : pick 1; pick 1;
        if (ins != code_.begin() && ins.asCursor(code_).bc().immediate.i == 1) {
            auto prev = ins - 1;
            if ((*prev).is(Opcode::pick_) && *ins == *prev) {
                CodeEditor::Cursor cur = prev.asCursor(code_);
                cur.remove();
                cur.remove();
                return;
            }
        }
    }

    // TODO there is some brokennes when there is dead code
    // void br_(CodeEditor::Iterator ins) override {
    //     auto target = code_.target(*ins);
    //     auto cont = target+1;
    //     if ((*cont).isReturn()) {
    //         auto cur = ins.asCursor(code_);
    //         cur.remove();
    //         cur << *cont;
    //     }
    // }

    void run() {
        for (auto i = code_.begin(); i != code_.end(); ++i)
            dispatcher.dispatch(i);
    }
};
}
#endif