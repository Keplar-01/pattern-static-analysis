#pragma once

#include <vector>

namespace llvm {
class Function;
class Instruction;
class Loop;
class LoopInfo;
class ScalarEvolution;
}

namespace analyzer {

class DependenceChecker {
public:
    const char* check(const llvm::Loop& loop,
                      llvm::Function& function,
                      llvm::LoopInfo& loopInfo,
                      llvm::ScalarEvolution& scalarEvolution,
                      const std::vector<const llvm::Instruction*>& accesses) const;
};

}  // namespace analyzer
