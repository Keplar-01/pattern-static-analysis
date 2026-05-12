#include "analyzer/MemoryAccessAnalyzer.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/raw_ostream.h>

namespace analyzer {

namespace {

bool isInsideNestedSubloop(const llvm::Loop& loop, const llvm::BasicBlock* block) {
    for (const llvm::Loop* subLoop : loop.getSubLoops()) {
        if (subLoop->contains(block) || isInsideNestedSubloop(*subLoop, block)) {
            return true;
        }
    }
    return false;
}

bool hasLoadRec(const llvm::Value* value, llvm::SmallPtrSetImpl<const llvm::Value*>& visited) {
    if (!visited.insert(value).second) {
        return false;
    }
    if (llvm::isa<llvm::LoadInst>(value)) {
        return true;
    }
    if (const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value)) {
        for (const llvm::Value* operand : instruction->operand_values()) {
            if (hasLoadRec(operand, visited)) {
                return true;
            }
        }
    } else if (const auto* constantExpr = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
        for (const llvm::Value* operand : constantExpr->operand_values()) {
            if (hasLoadRec(operand, visited)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

bool MemoryAccessAnalyzer::isConditionallyExecuted(const llvm::Instruction& instruction,
                                                   const llvm::Loop& loop) const {
    const llvm::BasicBlock* block = instruction.getParent();
    if (block == loop.getHeader()) {
        return false;
    }

    for (const llvm::BasicBlock* pred : llvm::predecessors(block)) {
        if (!loop.contains(pred) || pred == loop.getHeader()) {
            continue;
        }

        const auto* branch = llvm::dyn_cast<llvm::BranchInst>(pred->getTerminator());
        if (!branch || !branch->isConditional()) {
            continue;
        }

        for (unsigned i = 0; i < branch->getNumSuccessors(); ++i) {
            if (branch->getSuccessor(i) != block) {
                return true;
            }
        }
    }

    return false;
}

bool MemoryAccessAnalyzer::hasIndexedAddressing(const llvm::Value* ptr) const {
    if (llvm::isa<llvm::GEPOperator>(ptr) || llvm::isa<llvm::GetElementPtrInst>(ptr)) {
        return true;
    }
    const llvm::Value* base = ptr->stripPointerCasts();
    return llvm::isa<llvm::GEPOperator>(base) || llvm::isa<llvm::GetElementPtrInst>(base);
}

const llvm::Value* getNormalizedBase(const llvm::Value* ptr) {
    const llvm::Value* base = ptr->stripPointerCasts();
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(base)) {
        base = gep->getPointerOperand()->stripPointerCasts();
    }
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(base)) {
        const llvm::Value* loadBase = load->getPointerOperand()->stripPointerCasts();
        if (const auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(loadBase)) {
            return allocaInst;
        }
    }
    return base;
}

bool shouldIncludeAccess(const std::string& baseKind, bool hasIndexedAddressing) {
    (void)baseKind;
    (void)hasIndexedAddressing;
    return true;
}

std::string MemoryAccessAnalyzer::detectBaseSymbol(const llvm::Value* ptr) const {
    const llvm::Value* base = getNormalizedBase(ptr);

    if (const auto* global = llvm::dyn_cast<llvm::GlobalValue>(base)) {
        const std::string name = std::string(global->getName());
        if (!name.empty()) {
            return name;
        }
        return "global";
    }

    if (const auto* argument = llvm::dyn_cast<llvm::Argument>(base)) {
        const std::string name = std::string(argument->getName());
        if (!name.empty()) {
            return name;
        }
        return "arg" + std::to_string(argument->getArgNo());
    }

    if (const auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(base)) {
        const std::string name = std::string(allocaInst->getName());
        if (!name.empty()) {
            return name;
        }
        return "stack_alloca";
    }

    std::string text;
    llvm::raw_string_ostream stream(text);
    base->printAsOperand(stream, false);
    stream.flush();
    if (!text.empty() && text[0] == '%') {
        return "ssa_" + text.substr(1);
    }
    return text;
}

std::string MemoryAccessAnalyzer::detectBaseKind(const llvm::Value* ptr) const {
    const llvm::Value* base = getNormalizedBase(ptr);

    if (const auto* global = llvm::dyn_cast<llvm::GlobalValue>(base)) {
        if (global->getValueType()->isArrayTy()) {
            return "global_array";
        }
        return "global_pointer";
    }

    if (llvm::isa<llvm::Argument>(base)) {
        return "pointer_arg";
    }

    if (llvm::isa<llvm::AllocaInst>(base)) {
        return "stack_pointer";
    }

    return "other";
}

bool MemoryAccessAnalyzer::indexDependsOnMemory(const llvm::Value* ptr) const {
    auto hasLoadInGepIndex = [](const llvm::Value* value) -> bool {
        llvm::SmallPtrSet<const llvm::Value*, 16> visited;
        if (const auto* gepOp = llvm::dyn_cast<llvm::GEPOperator>(value)) {
            for (const llvm::Use& idxUse : gepOp->indices()) {
                const llvm::Value* idx = idxUse.get();
                if (hasLoadRec(idx, visited)) {
                    return true;
                }
            }
            return false;
        }
        if (const auto* gepInst = llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
            for (auto idxIt = gepInst->idx_begin(); idxIt != gepInst->idx_end(); ++idxIt) {
                const llvm::Value* idx = *idxIt;
                if (hasLoadRec(idx, visited)) {
                    return true;
                }
            }
            return false;
        }
        return false;
    };

    if (hasLoadInGepIndex(ptr)) {
        return true;
    }
    const llvm::Value* stripped = ptr->stripPointerCasts();
    if (stripped != ptr && hasLoadInGepIndex(stripped)) {
        return true;
    }
    return false;
}

MemoryAccessInfo MemoryAccessAnalyzer::analyze(const llvm::Loop& loop) const {
    MemoryAccessInfo info;

    for (const llvm::BasicBlock* block : loop.blocks()) {
        if (isInsideNestedSubloop(loop, block)) {
            continue;
        }

        for (const llvm::Instruction& instruction : *block) {
            const llvm::Value* ptr = nullptr;
            bool isLoad = false;
            bool isStore = false;

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                ptr = load->getPointerOperand();
                isLoad = true;
                // Ignore service loads of pointer variables like "load ptr, ptr %a".
                // These are IR plumbing at -O0, not user memory-access patterns.
                if (load->getType()->isPointerTy() && !hasIndexedAddressing(ptr)) {
                    continue;
                }
            } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                ptr = store->getPointerOperand();
                isStore = true;
            }

            if (!ptr) {
                continue;
            }

            MemoryAccessPatternInfo pattern;
            pattern.instruction = &instruction;
            pattern.baseSymbol = detectBaseSymbol(ptr);
            pattern.baseKind = detectBaseKind(ptr);
            pattern.conditional = isConditionallyExecuted(instruction, loop);
            pattern.kind = isLoad ? AccessKind::Load : AccessKind::Store;
            pattern.hasIndexedAddressing = hasIndexedAddressing(ptr);
            pattern.indexedByMemory = indexDependsOnMemory(ptr);
            if (!shouldIncludeAccess(pattern.baseKind, pattern.hasIndexedAddressing)) {
                continue;
            }
            if (isLoad) {
                pattern.alignmentBytes =
                    static_cast<long long>(llvm::cast<llvm::LoadInst>(instruction).getAlign().value());
            } else if (isStore) {
                pattern.alignmentBytes =
                    static_cast<long long>(llvm::cast<llvm::StoreInst>(instruction).getAlign().value());
            }
            info.patterns.push_back(pattern);
        }
    }

    return info;
}

MemoryAccessInfo MemoryAccessAnalyzer::analyzeOutsideLoops(const llvm::Function& function,
                                                           const llvm::LoopInfo& loopInfo) const {
    MemoryAccessInfo info;

    for (const llvm::BasicBlock& block : function) {
        if (loopInfo.getLoopFor(&block) != nullptr) {
            continue;
        }

        for (const llvm::Instruction& instruction : block) {
            const llvm::Value* ptr = nullptr;
            bool isLoad = false;
            bool isStore = false;

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                ptr = load->getPointerOperand();
                isLoad = true;
                // Ignore service loads of pointer variables like "load ptr, ptr %a".
                // These are IR plumbing at -O0, not user memory-access patterns.
                if (load->getType()->isPointerTy() && !hasIndexedAddressing(ptr)) {
                    continue;
                }
            } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                ptr = store->getPointerOperand();
                isStore = true;
            }

            if (!ptr) {
                continue;
            }

            MemoryAccessPatternInfo pattern;
            pattern.instruction = &instruction;
            pattern.baseSymbol = detectBaseSymbol(ptr);
            pattern.baseKind = detectBaseKind(ptr);
            pattern.conditional = false;
            pattern.kind = isLoad ? AccessKind::Load : AccessKind::Store;
            pattern.hasIndexedAddressing = hasIndexedAddressing(ptr);
            pattern.indexedByMemory = indexDependsOnMemory(ptr);
            if (!shouldIncludeAccess(pattern.baseKind, pattern.hasIndexedAddressing)) {
                continue;
            }
            if (isLoad) {
                pattern.alignmentBytes =
                    static_cast<long long>(llvm::cast<llvm::LoadInst>(instruction).getAlign().value());
            } else if (isStore) {
                pattern.alignmentBytes =
                    static_cast<long long>(llvm::cast<llvm::StoreInst>(instruction).getAlign().value());
            }
            info.patterns.push_back(pattern);
        }
    }

    return info;
}

}  // namespace analyzer
