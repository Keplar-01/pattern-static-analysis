#include "analyzer/LoopCollector.h"

#include <llvm/Analysis/LoopInfo.h>

namespace analyzer {

namespace {

void collectRecursively(const llvm::Loop* loop,
                        const llvm::Function& function,
                        int depth,
                        int maxDepth,
                        std::vector<LoopContext>& out) {
    if (depth > maxDepth) {
        return;
    }

    out.push_back(LoopContext{loop, &function, depth});

    for (const llvm::Loop* nested : loop->getSubLoops()) {
        collectRecursively(nested, function, depth + 1, maxDepth, out);
    }
}

}  // namespace

std::vector<LoopContext> LoopCollector::collect(const llvm::LoopInfo& loopInfo,
                                                const llvm::Function& function,
                                                int maxDepth) const {
    std::vector<LoopContext> loops;
    for (const llvm::Loop* loop : loopInfo) {
        collectRecursively(loop, function, 1, maxDepth < 1 ? 1 : maxDepth, loops);
    }
    return loops;
}

}  // namespace analyzer
