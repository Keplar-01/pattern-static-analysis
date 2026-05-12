#pragma once

#include <vector>

namespace llvm {
class Function;
class Loop;
class LoopInfo;
}

namespace analyzer {

struct LoopContext {
    const llvm::Loop* loop = nullptr;
    const llvm::Function* function = nullptr;
    int depth = 0;
};

class LoopCollector {
public:
    std::vector<LoopContext> collect(const llvm::LoopInfo& loopInfo,
                                     const llvm::Function& function,
                                     int maxDepth) const;
};

}  // namespace analyzer
