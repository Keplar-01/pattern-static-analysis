#include "analyzer/DependenceChecker.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/DependenceAnalysis.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <cstddef>
#include <optional>

namespace analyzer {

namespace {

std::optional<llvm::MemoryLocation> getMemoryLocation(llvm::Instruction* instruction) {
    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
        return llvm::MemoryLocation::get(load);
    }
    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(instruction)) {
        return llvm::MemoryLocation::get(store);
    }
    return std::nullopt;
}

}  // namespace

const char* DependenceChecker::check(const llvm::Loop&,
                                     llvm::Function& function,
                                     llvm::LoopInfo& loopInfo,
                                     llvm::ScalarEvolution& scalarEvolution,
                                     const std::vector<const llvm::Instruction*>& accesses) const {
    if (accesses.size() < 2) {
        return "no-dep";
    }

    llvm::AssumptionCache assumptionCache(function);
    llvm::TargetLibraryInfoImpl tliImpl(llvm::Triple(function.getParent()->getTargetTriple()));
    llvm::TargetLibraryInfo tli(tliImpl);
    llvm::AAResults aaResults(tli);
    llvm::BasicAAResult basicAA(function.getParent()->getDataLayout(),
                                function,
                                tli,
                                assumptionCache);
    aaResults.addAAResult(basicAA);

    llvm::DependenceInfo dependenceInfo(&function, &aaResults, &scalarEvolution, &loopInfo);

    bool hasUnresolvedDependence = false;

    for (std::size_t i = 0; i < accesses.size(); ++i) {
        for (std::size_t j = i + 1; j < accesses.size(); ++j) {
            auto* src = const_cast<llvm::Instruction*>(accesses[i]);
            auto* dst = const_cast<llvm::Instruction*>(accesses[j]);
            std::unique_ptr<llvm::Dependence> dep =
                dependenceInfo.depends(src, dst, /*PossiblyLoopIndependent=*/true);
            if (!dep) {
                continue;
            }

            if (dep->isConfused()) {
                auto srcLoc = getMemoryLocation(src);
                auto dstLoc = getMemoryLocation(dst);
                if (srcLoc && dstLoc) {
                    const llvm::AliasResult alias = aaResults.alias(*srcLoc, *dstLoc);
                    if (alias == llvm::AliasResult::NoAlias) {
                        continue;
                    }
                }
                hasUnresolvedDependence = true;
                continue;
            }

            if (!dep->isLoopIndependent()) {
                return "loop-carried-dep";
            }
        }
    }

    if (hasUnresolvedDependence) {
        return "unknown";
    }

    return "no-dep";
}

}  // namespace analyzer
