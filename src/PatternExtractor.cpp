#include "analyzer/PatternExtractor.h"

#include "analyzer/DependenceChecker.h"
#include "analyzer/FingerprintBuilder.h"
#include "analyzer/LoopCollector.h"
#include "analyzer/MemoryAccessAnalyzer.h"
#include "analyzer/PatternClassifier.h"
#include "analyzer/ScevAnalyzer.h"

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
namespace analyzer {

namespace {

std::string getSourceLineText(const std::string& path, int lineNumber) {
    if (path.empty() || lineNumber <= 0) {
        return "";
    }
    std::ifstream in(path);
    if (!in.is_open()) {
        return "";
    }
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        if (n == lineNumber) {
            return line;
        }
    }
    return "";
}

bool looksIndexedSourceAccess(const std::string& line) {
    return line.find('[') != std::string::npos && line.find(']') != std::string::npos;
}

std::string normalizePath(const std::string& path) {
    if (path.empty()) {
        return "";
    }
    std::error_code ec;
    const std::filesystem::path weak = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return weak.generic_string();
    }
    return std::filesystem::path(path).lexically_normal().generic_string();
}

bool sameSourceFile(const std::string& sourceFromDebug, const std::string& sourceFilterPath) {
    if (sourceFilterPath.empty()) {
        return true;
    }
    if (sourceFromDebug.empty()) {
        return false;
    }
    const std::string sourceNorm = normalizePath(sourceFromDebug);
    const std::string filterNorm = normalizePath(sourceFilterPath);
    if (!sourceNorm.empty() && !filterNorm.empty() && sourceNorm == filterNorm) {
        return true;
    }
    const std::string sourceBase = std::filesystem::path(sourceFromDebug).filename().string();
    const std::string filterBase = std::filesystem::path(sourceFilterPath).filename().string();
    return !sourceBase.empty() && !filterBase.empty() && sourceBase == filterBase;
}

std::string getSourceLineTextAny(const std::string& sourceFromDebug,
                                 const std::string& sourceFilterPath,
                                 int lineNumber) {
    std::string line = getSourceLineText(sourceFromDebug, lineNumber);
    if (!line.empty()) {
        return line;
    }
    if (!sourceFilterPath.empty() && sourceFilterPath != sourceFromDebug) {
        line = getSourceLineText(sourceFilterPath, lineNumber);
    }
    return line;
}

void promoteAllocasToSSA(llvm::Function& function) {
    if (function.isDeclaration()) {
        return;
    }

    llvm::DominatorTree dominatorTree(function);
    llvm::AssumptionCache assumptionCache(function);
    std::vector<llvm::AllocaInst*> promotableAllocas;

    for (llvm::Instruction& instruction : function.getEntryBlock()) {
        auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
        if (!allocaInst) {
            continue;
        }
        if (llvm::isAllocaPromotable(allocaInst)) {
            promotableAllocas.push_back(allocaInst);
        }
    }

    if (!promotableAllocas.empty()) {
        llvm::PromoteMemToReg(promotableAllocas, dominatorTree, &assumptionCache);
    }
}

}  // namespace

AnalysisResult PatternExtractor::extractFromIR(const std::string& inputPath,
                                               int maxLoopDepth,
                                               const std::string& sourceFilterPath) const {
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    std::unique_ptr<llvm::Module> module = llvm::parseIRFile(inputPath, error, context);
    if (!module) {
        throw std::runtime_error("Cannot parse IR/bitcode file: " + inputPath);
    }

    LoopCollector loopCollector;
    MemoryAccessAnalyzer memoryAccessAnalyzer;
    ScevAnalyzer scevAnalyzer;
    DependenceChecker dependenceChecker;
    PatternClassifier patternClassifier;
    FingerprintBuilder fingerprintBuilder;

    AnalysisResult result;
    unsigned int nextSequenceIndex = 0;
    std::unordered_set<std::string> seenSourcePatterns;

    for (llvm::Function& function : *module) {
        if (function.isDeclaration()) {
            continue;
        }

        promoteAllocasToSSA(function);

        llvm::DominatorTree dominatorTree(function);
        llvm::LoopInfo loopInfo(dominatorTree);
        llvm::AssumptionCache assumptionCache(function);
        llvm::TargetLibraryInfoImpl tliImpl(llvm::Triple(module->getTargetTriple()));
        llvm::TargetLibraryInfo tli(tliImpl);
        llvm::ScalarEvolution scalarEvolution(function,
                                              tli,
                                              assumptionCache,
                                              dominatorTree,
                                              loopInfo);

        const std::vector<LoopContext> loops =
            loopCollector.collect(loopInfo, function, std::max(1, maxLoopDepth));

        for (const LoopContext& loopContext : loops) {
            const MemoryAccessInfo memoryInfo = memoryAccessAnalyzer.analyze(*loopContext.loop);
            std::vector<const llvm::Instruction*> loopAccesses;
            loopAccesses.reserve(memoryInfo.patterns.size());
            for (const MemoryAccessPatternInfo& access : memoryInfo.patterns) {
                loopAccesses.push_back(access.instruction);
            }
            const char* dependenceLabel = dependenceChecker.check(*loopContext.loop,
                                                                  function,
                                                                  loopInfo,
                                                                  scalarEvolution,
                                                                  loopAccesses);

            for (const MemoryAccessPatternInfo& access : memoryInfo.patterns) {
                AccessPatternResult pattern;
                pattern.functionName = std::string(loopContext.function->getName());
                pattern.depth = loopContext.depth;
                pattern.baseSymbol = access.baseSymbol;
                pattern.baseKind = access.baseKind;
                pattern.conditional = access.conditional;
                pattern.alignmentBytes = access.alignmentBytes;
                pattern.indexedByMemory = access.indexedByMemory;
                pattern.hasIndexedAddressing = access.hasIndexedAddressing;

                if (access.kind == AccessKind::Load) {
                    pattern.loadCount = 1;
                    pattern.storeCount = 0;
                    pattern.accessKind = "load";
                } else {
                    pattern.loadCount = 0;
                    pattern.storeCount = 1;
                    pattern.accessKind = "store";
                }

                const std::vector<const llvm::Instruction*> singleAccess = {access.instruction};
                const ScevFeatures scevFeatures =
                    scevAnalyzer.analyze(*loopContext.loop,
                                         scalarEvolution,
                                         module->getDataLayout(),
                                         singleAccess);

                pattern.affine = scevFeatures.affine;
                pattern.hasKnownStride = scevFeatures.hasKnownStride;
                pattern.strideElements = scevFeatures.strideElements;
                pattern.contiguousBlockElements = scevFeatures.contiguousBlockElements;
                pattern.fillFactor = scevFeatures.fillFactor;
                pattern.multidim = scevFeatures.multidim;
                pattern.workingSetBytes = scevFeatures.workingSetBytes;

                // Adjust fill factor for conditional accesses:
                // only a fraction of iterations actually execute the access.
                if (pattern.conditional && pattern.fillFactor > 0.0) {
                    pattern.fillFactor *= 0.5;
                }

                pattern.dependence = dependenceLabel;
                pattern.patternType = patternClassifier.classify(pattern);
                pattern.patternSignature = fingerprintBuilder.build(pattern);

                if (const llvm::DebugLoc& loc = access.instruction->getDebugLoc()) {
                    pattern.sourceLine = static_cast<int>(loc.getLine());
                    pattern.sourceColumn = static_cast<int>(loc.getCol());
                    if (auto* scope = llvm::dyn_cast<llvm::DIScope>(loc.getScope())) {
                        pattern.sourceFile = scope->getFilename().str();
                    }
                }

                // Keep only source-level, user-written patterns and de-duplicate
                // by exact source location + access kind + pattern signature.
                if (pattern.sourceLine <= 0 || pattern.sourceFile.empty()) {
                    continue;
                }
                if (!sameSourceFile(pattern.sourceFile, sourceFilterPath)) {
                    continue;
                }
                const std::string sourceLineText =
                    getSourceLineTextAny(pattern.sourceFile, sourceFilterPath, pattern.sourceLine);
                if (!sourceLineText.empty() && !looksIndexedSourceAccess(sourceLineText)) {
                    continue;
                }
                if (!pattern.hasIndexedAddressing && !pattern.indexedByMemory) {
                    continue;
                }
                const std::string sourceKey = pattern.sourceFile + "|" +
                                              std::to_string(pattern.sourceLine) + "|" +
                                              std::to_string(pattern.sourceColumn) + "|" +
                                              pattern.accessKind + "|" + pattern.patternSignature;
                if (!seenSourcePatterns.insert(sourceKey).second) {
                    continue;
                }

                pattern.sequenceIndex = nextSequenceIndex++;

                result.patterns.push_back(std::move(pattern));
            }
        }

        // Also capture memory access patterns outside loops as depth=0.
        const MemoryAccessInfo outsideLoopAccesses =
            memoryAccessAnalyzer.analyzeOutsideLoops(function, loopInfo);
        for (const MemoryAccessPatternInfo& access : outsideLoopAccesses.patterns) {
            AccessPatternResult pattern;
            pattern.functionName = std::string(function.getName());
            pattern.depth = 0;
            pattern.baseSymbol = access.baseSymbol;
            pattern.baseKind = access.baseKind;
            pattern.conditional = false;
            pattern.alignmentBytes = access.alignmentBytes;
            pattern.indexedByMemory = access.indexedByMemory;
            pattern.hasIndexedAddressing = access.hasIndexedAddressing;

            if (access.kind == AccessKind::Load) {
                pattern.loadCount = 1;
                pattern.storeCount = 0;
                pattern.accessKind = "load";
            } else {
                pattern.loadCount = 0;
                pattern.storeCount = 1;
                pattern.accessKind = "store";
            }

            pattern.affine = access.hasIndexedAddressing && !access.indexedByMemory;
            pattern.dependence = "no-dep";
            pattern.patternType = patternClassifier.classify(pattern);
            pattern.patternSignature = fingerprintBuilder.build(pattern);

            if (const llvm::DebugLoc& loc = access.instruction->getDebugLoc()) {
                pattern.sourceLine = static_cast<int>(loc.getLine());
                pattern.sourceColumn = static_cast<int>(loc.getCol());
                if (auto* scope = llvm::dyn_cast<llvm::DIScope>(loc.getScope())) {
                    pattern.sourceFile = scope->getFilename().str();
                }
            }
            if (pattern.sourceLine <= 0 || pattern.sourceFile.empty()) {
                continue;
            }
            if (!sameSourceFile(pattern.sourceFile, sourceFilterPath)) {
                continue;
            }
            const std::string sourceLineText =
                getSourceLineTextAny(pattern.sourceFile, sourceFilterPath, pattern.sourceLine);
            if (!sourceLineText.empty() && !looksIndexedSourceAccess(sourceLineText)) {
                continue;
            }
            if (!pattern.hasIndexedAddressing && !pattern.indexedByMemory) {
                continue;
            }

            const std::string sourceKey = pattern.sourceFile + "|" +
                                          std::to_string(pattern.sourceLine) + "|" +
                                          std::to_string(pattern.sourceColumn) + "|" +
                                          pattern.accessKind + "|" + pattern.patternSignature;
            if (!seenSourcePatterns.insert(sourceKey).second) {
                continue;
            }

            pattern.sequenceIndex = nextSequenceIndex++;
            result.patterns.push_back(std::move(pattern));
        }
    }

    return result;
}

}  // namespace analyzer
