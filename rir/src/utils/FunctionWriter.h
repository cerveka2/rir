#ifndef RIR_FUNCTION_HANDLE_H
#define RIR_FUNCTION_HANDLE_H

#include "interpreter/interp.h"
#include "interpreter/runtime.h"
#include "ir/BC_inc.h"

#include "ir/CodeVerifier.h"
#include "runtime/Function.h"

#include <iostream>

namespace rir {

class FunctionWriter {
  public:
    typedef unsigned PcOffset;
    constexpr static unsigned initialSize = 2 * sizeof(Function);

    Function* function;

    size_t capacity;

    FunctionWriter() = delete;
    // also copy and move ctors = default ?

    static FunctionWriter create() {
        assert(initialSize > sizeof(Function));
        assert(initialSize % sizeof(int) == 0);
        SEXP store = Rf_allocVector(EXTERNALSXP, initialSize);
        void* payload = INTEGER(store);

        Function* function = new (payload) Function;
        FunctionWriter res(function, initialSize);
        return res;
    }

    ~FunctionWriter() { R_ReleaseObject(function->container()); }

    Code* writeCode(SEXP ast, void* bc, unsigned originalCodeSize,
                    const std::map<PcOffset, BC::PoolIdx>& sources,
                    const std::map<PcOffset, BC::Label>& patchpoints,
                    const std::map<PcOffset, std::vector<BC::Label>>& labels,
                    bool markDefaultArg, size_t localsCnt, size_t nops) {
        assert(function->size <= capacity);

        unsigned codeSize = originalCodeSize - nops;
        unsigned totalSize = Code::size(codeSize, sources.size());

        if (function->size + totalSize > capacity) {
            unsigned newCapacity = capacity;
            while (function->size + totalSize > newCapacity)
                newCapacity *= 1.5;
            newCapacity = pad4(newCapacity);

            assert(newCapacity % sizeof(int) == 0);
            assert(function->size + totalSize <= newCapacity);

            SEXP newStore = Rf_allocVector(EXTERNALSXP, newCapacity);
            void* newPayload = INTEGER(newStore);

            // it is ok to bypass write barrier here, since newPayload is a new
            // object
            memcpy(newPayload, function, capacity);

            R_PreserveObject(newStore);
            R_ReleaseObject(function->container());

            function = reinterpret_cast<Function*>(newPayload);
            capacity = newCapacity;
        }

        unsigned offset = function->size;
        void* insert = (void*)((uintptr_t)function + function->size);
        function->size += totalSize;
        assert(function->size <= capacity);

        Code* code = new (insert) Code(ast, codeSize, sources.size(), offset,
                                       markDefaultArg, localsCnt);

        assert(code->function() == function);

        size_t numberOfSources = 0;

        // Since we are removing instructions from the BC stream, we need to
        // update labels and patchpoint offsets.
        std::vector<PcOffset> updatedLabel2Pos;
        std::unordered_map<PcOffset, BC::Label> updatedPatchpoints;
        {
            Opcode* from = (Opcode*)bc;
            Opcode* to = code->code();
            const Opcode* from_start = (Opcode*)bc;
            const Opcode* to_start = code->code();
            const Opcode* from_end = from + originalCodeSize;
            const Opcode* to_end = to + codeSize;

            // Since those are ordered maps, the elements appear in order. Our
            // strategy is thus, to wait for the next element to show up in the
            // source stream, and transfer them to the updated maps for the
            // target code stream.
            auto source = sources.begin();
            auto patchpoint = patchpoints.begin();
            auto label = labels.begin();

            while (from != from_end) {
                assert(to < to_start + codeSize);

                unsigned bcSize = BC::size(from);
                PcOffset fromOffset = from - from_start;
                PcOffset fromOffsetAfter = fromOffset + bcSize;
                PcOffset toOffset = to - to_start;

                // Look for labels. If we have a label in the 'from' stream,
                // when we will need to note the position of that label in the
                // 'to' stream.
                if (label != labels.end()) {
                    auto nextLabelPos = label->first;
                    assert(nextLabelPos >= fromOffset);
                    if (nextLabelPos == fromOffset) {
                        for (auto labelNr : label->second) {
                            if ((unsigned)labelNr >= updatedLabel2Pos.size())
                                updatedLabel2Pos.resize(labelNr + 1, -1);
                            updatedLabel2Pos[labelNr] = toOffset;
                        }
                        label++;
                    }
                }

                // We skip nops (and maybe potentially other instructions)
                if (*from == Opcode::nop_) {
                    nops--;
                    from++;
                    continue;
                }

                // Copy the bytecode from 'from' to 'to'
                memcpy(to, from, bcSize);

                // The code stream stores sources after the instruction, but in
                // the BC we actually need the index before the instruction.
                // If the current BC in the code stream has a source attached,
                // we add it to the sources list of the code object.
                if (source != sources.end()) {
                    assert(source->first >= fromOffsetAfter);
                    if (source->first == fromOffsetAfter) {
                        code->srclist()[numberOfSources].pcOffset = toOffset;
                        code->srclist()[numberOfSources].srcIdx =
                            source->second;
                        numberOfSources++;
                        source++;
                    }
                }

                // Patchpoints can occur anywhere within BCs. If there is a
                // patchpoint in the 'from' BC, we need to update it, such
                // that it references the correct place in the 'to' BC.
                if (patchpoint != patchpoints.end()) {
                    auto patchpointPos = patchpoint->first;
                    assert(patchpointPos >= fromOffset);
                    auto patchpointDistance = patchpointPos - fromOffset;
                    if (patchpointDistance < bcSize) {
                        updatedPatchpoints[toOffset + patchpointDistance] =
                            patchpoint->second;
                        patchpoint++;
                    }
                }

                from += bcSize;
                to += bcSize;
            }

            // Make sure that there is no dangling garbage at the end, if we
            // skipped more instructions than anticipated
            while (to != to_end) {
                *to++ = Opcode::nop_;
            }

            assert(to == to_end);
        }
        assert(nops == 0 && "Client reported wrong number of nops");
        assert(patchpoints.size() == updatedPatchpoints.size());

        // Patch jumps with actual offset in bytes
        for (auto p : updatedPatchpoints) {
            unsigned pos = p.first;
            unsigned labelNr = p.second;
            assert(labelNr < updatedLabel2Pos.size() &&
                   "Jump to missing label");
            unsigned target = updatedLabel2Pos[labelNr];
            assert(target != (unsigned)-1 && "Jump to missing label");
            BC::Jmp j = target - pos - sizeof(BC::Jmp);
            *(BC::Jmp*)((uintptr_t)code->code() + pos) = j;
        }

        assert(numberOfSources == sources.size());
        function->codeLength++;

        // set the last code offset
        function->foffset = offset;

        return code;
    }

  private:
    explicit FunctionWriter(Function* function, size_t capacity)
        : function(function), capacity(capacity) {
        assert(function->info.magic == FUNCTION_MAGIC);
        assert(function->size <= capacity);
        R_PreserveObject(function->container());
    }
};
}

#endif
