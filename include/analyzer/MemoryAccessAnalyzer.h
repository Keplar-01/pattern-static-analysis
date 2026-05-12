#pragma once

#include <string>
#include <vector>

namespace llvm {
class Function;
class Instruction;
class Loop;
class Value;
class LoopInfo;
}

namespace analyzer {

enum class AccessKind {
    Load,
    Store
};

struct MemoryAccessPatternInfo {
    const llvm::Instruction* instruction = nullptr;
    bool conditional = false;
    std::string baseSymbol;
    std::string baseKind;
    AccessKind kind = AccessKind::Load;
    long long alignmentBytes = 0;
    bool indexedByMemory = false;
    bool hasIndexedAddressing = false;
};

struct MemoryAccessInfo {
    std::vector<MemoryAccessPatternInfo> patterns;
};

class MemoryAccessAnalyzer {
public:
    MemoryAccessInfo analyze(const llvm::Loop& loop) const;
    MemoryAccessInfo analyzeOutsideLoops(const llvm::Function& function,
                                         const llvm::LoopInfo& loopInfo) const;

private:
    bool isConditionallyExecuted(const llvm::Instruction& instruction,
                                 const llvm::Loop& loop) const;
    bool hasIndexedAddressing(const llvm::Value* ptr) const;
    std::string detectBaseSymbol(const llvm::Value* ptr) const;
    std::string detectBaseKind(const llvm::Value* ptr) const;
    bool indexDependsOnMemory(const llvm::Value* ptr) const;
};

}  // namespace analyzer
