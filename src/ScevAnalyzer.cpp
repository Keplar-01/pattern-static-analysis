#include "analyzer/ScevAnalyzer.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace analyzer {

namespace {

bool containsValueRec(const llvm::Value* root,
                      const llvm::Value* target,
                      std::unordered_set<const llvm::Value*>& visited) {
    if (root == target) {
        return true;
    }
    if (!visited.insert(root).second) {
        return false;
    }

    if (const auto* instruction = llvm::dyn_cast<llvm::Instruction>(root)) {
        for (const llvm::Value* operand : instruction->operand_values()) {
            if (containsValueRec(operand, target, visited)) {
                return true;
            }
        }
    } else if (const auto* constantExpr = llvm::dyn_cast<llvm::ConstantExpr>(root)) {
        for (const llvm::Value* operand : constantExpr->operand_values()) {
            if (containsValueRec(operand, target, visited)) {
                return true;
            }
        }
    }

    return false;
}

bool containsValue(const llvm::Value* root, const llvm::Value* target) {
    std::unordered_set<const llvm::Value*> visited;
    return containsValueRec(root, target, visited);
}

const llvm::AllocaInst* getAllocaFromPointerOperand(const llvm::Value* ptr) {
    if (!ptr) {
        return nullptr;
    }
    return llvm::dyn_cast<llvm::AllocaInst>(ptr->stripPointerCasts());
}

std::optional<long long> detectAllocaStepFromStore(const llvm::StoreInst& store) {
    const llvm::AllocaInst* targetAlloca = getAllocaFromPointerOperand(store.getPointerOperand());
    if (!targetAlloca) {
        return std::nullopt;
    }

    const llvm::Value* stored = store.getValueOperand();
    const auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(stored);
    if (!bin) {
        return std::nullopt;
    }

    const llvm::Value* lhs = bin->getOperand(0);
    const llvm::Value* rhs = bin->getOperand(1);

    auto extractLoadAlloca = [](const llvm::Value* value) -> const llvm::AllocaInst* {
        const auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
        if (!load) {
            return nullptr;
        }
        return getAllocaFromPointerOperand(load->getPointerOperand());
    };

    auto extractConst = [](const llvm::Value* value) -> std::optional<long long> {
        if (const auto* c = llvm::dyn_cast<llvm::ConstantInt>(value)) {
            return c->getSExtValue();
        }
        return std::nullopt;
    };

    if (bin->getOpcode() == llvm::Instruction::Add ||
        bin->getOpcode() == llvm::Instruction::Sub) {
        const llvm::AllocaInst* lhsAlloca = extractLoadAlloca(lhs);
        const llvm::AllocaInst* rhsAlloca = extractLoadAlloca(rhs);
        const std::optional<long long> lhsConst = extractConst(lhs);
        const std::optional<long long> rhsConst = extractConst(rhs);

        if (lhsAlloca == targetAlloca && rhsConst.has_value()) {
            return (bin->getOpcode() == llvm::Instruction::Add) ? *rhsConst : -*rhsConst;
        }
        if (bin->getOpcode() == llvm::Instruction::Add &&
            rhsAlloca == targetAlloca && lhsConst.has_value()) {
            return *lhsConst;
        }
    }

    return std::nullopt;
}

}  // namespace

long long ScevAnalyzer::getConstantIntValue(const llvm::Value* value, bool& ok) const {
    if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        ok = true;
        return constant->getSExtValue();
    }
    ok = false;
    return 0;
}

long long ScevAnalyzer::getLinearCoeff(const llvm::Value* value,
                                       const llvm::PHINode* inductionVariable,
                                       const std::unordered_map<const llvm::AllocaInst*, long long>& allocaSteps,
                                       bool& ok) const {
    if (inductionVariable && value == inductionVariable) {
        ok = true;
        return 1;
    }

    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value)) {
        if (const auto* allocaInst = getAllocaFromPointerOperand(load->getPointerOperand())) {
            const auto it = allocaSteps.find(allocaInst);
            if (it != allocaSteps.end()) {
                ok = true;
                return it->second;
            }
        }
    }

    if (llvm::isa<llvm::ConstantInt>(value)) {
        ok = true;
        return 0;
    }

    // Value that does not transitively reference the current IV is
    // loop-invariant — its coefficient with respect to the IV is zero.
    if (inductionVariable && !containsValue(value, inductionVariable)) {
        ok = true;
        return 0;
    }

    if (const auto* castInst = llvm::dyn_cast<llvm::CastInst>(value)) {
        return getLinearCoeff(castInst->getOperand(0), inductionVariable, allocaSteps, ok);
    }

    if (const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(value)) {
        bool okL = false;
        bool okR = false;
        const long long left =
            getLinearCoeff(binary->getOperand(0), inductionVariable, allocaSteps, okL);
        const long long right =
            getLinearCoeff(binary->getOperand(1), inductionVariable, allocaSteps, okR);

        if (binary->getOpcode() == llvm::Instruction::Add && okL && okR) {
            ok = true;
            return left + right;
        }

        if (binary->getOpcode() == llvm::Instruction::Sub && okL && okR) {
            ok = true;
            return left - right;
        }

        if (binary->getOpcode() == llvm::Instruction::Mul) {
            // If one operand is loop-invariant (coeff=0) and the other has a
            // known coefficient, the product coefficient is 0 * val + invariant * coeff.
            // Since we cannot evaluate the invariant value statically, use the
            // identity: if one factor's coeff is 0, the product coeff is
            // constant_factor * other_coeff — but only when the factor with
            // zero coeff is a compile-time constant.
            bool cOk = false;
            const long long c1 = getConstantIntValue(binary->getOperand(0), cOk);
            if (cOk && okR) {
                ok = true;
                return c1 * right;
            }
            const long long c2 = getConstantIntValue(binary->getOperand(1), cOk);
            if (cOk && okL) {
                ok = true;
                return c2 * left;
            }

            // Non-constant loop-invariant factor: coeff(a * b) where a is
            // loop-invariant equals val(a) * coeff(b). We cannot compute
            // val(a) statically, but if coeff(b) == 0 the result is 0.
            if (okL && okR && (left == 0 || right == 0)) {
                ok = true;
                return 0;
            }
        }
    }

    ok = false;
    return 0;
}

ScevFeatures ScevAnalyzer::analyze(const llvm::Loop& loop,
                                   llvm::ScalarEvolution& scalarEvolution,
                                   const llvm::DataLayout& dataLayout,
                                   const std::vector<const llvm::Instruction*>& accesses,
                                   int cacheLineBytes) const {
    ScevFeatures features;
    features.affine = !accesses.empty();

    constexpr long long unknownStride = std::numeric_limits<long long>::min();
    long long strideState = unknownStride;

    const llvm::PHINode* inductionVariable = loop.getCanonicalInductionVariable();
    if (!inductionVariable) {
        inductionVariable = loop.getInductionVariable(scalarEvolution);
    }

    std::vector<const llvm::PHINode*> loopIVs;
    for (const llvm::Loop* cursor = &loop; cursor != nullptr; cursor = cursor->getParentLoop()) {
        const llvm::PHINode* iv = cursor->getCanonicalInductionVariable();
        if (!iv) {
            iv = cursor->getInductionVariable(scalarEvolution);
        }
        if (iv) {
            loopIVs.push_back(iv);
        }
    }

    // getSmallConstantTripCount returns header-entry count (BTC + 1).
    // At -O0 (non-rotated loops) the body executes BTC times.
    // Use the backedge-taken count directly as the iteration count.
    unsigned bodyIterations = 0;
    if (const auto* exitConst = llvm::dyn_cast<llvm::SCEVConstant>(
            scalarEvolution.getBackedgeTakenCount(&loop))) {
        const llvm::APInt& btc = exitConst->getAPInt();
        if (btc.getActiveBits() <= 32) {
            bodyIterations = static_cast<unsigned>(btc.getZExtValue());
        }
    }
    llvm::SmallPtrSet<const llvm::PHINode*, 8> usedIVs;
    std::unordered_map<const llvm::AllocaInst*, long long> allocaSteps;

    for (const llvm::BasicBlock* block : loop.blocks()) {
        for (const llvm::Instruction& inst : *block) {
            const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst);
            if (!store) {
                continue;
            }
            const llvm::AllocaInst* targetAlloca =
                getAllocaFromPointerOperand(store->getPointerOperand());
            if (!targetAlloca) {
                continue;
            }
            const std::optional<long long> step = detectAllocaStepFromStore(*store);
            if (!step.has_value()) {
                continue;
            }
            auto it = allocaSteps.find(targetAlloca);
            if (it == allocaSteps.end()) {
                allocaSteps[targetAlloca] = *step;
            } else if (it->second != *step) {
                allocaSteps.erase(it);
            }
        }
    }

    for (const llvm::Instruction* instruction : accesses) {
        const llvm::Value* ptr = nullptr;
        llvm::Type* accessType = nullptr;

        if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
            ptr = load->getPointerOperand();
            accessType = load->getType();
        } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(instruction)) {
            ptr = store->getPointerOperand();
            accessType = store->getValueOperand()->getType();
        }

        if (!ptr || !accessType) {
            continue;
        }

        bool scevHandled = false;

        const llvm::SCEV* inLoopScev =
            scalarEvolution.getSCEVAtScope(const_cast<llvm::Value*>(ptr), &loop);
        if (const auto* addRec = llvm::dyn_cast<llvm::SCEVAddRecExpr>(inLoopScev)) {
            // Only use this AddRecExpr if it belongs to the current loop,
            // not an outer one.
            if (addRec->getLoop() == &loop) {
                scevHandled = true;
                features.affine = features.affine && addRec->isAffine();

                const llvm::SCEV* step = addRec->getStepRecurrence(scalarEvolution);
                if (const auto* stepConst = llvm::dyn_cast<llvm::SCEVConstant>(step)) {
                    const long long stepBytes = stepConst->getAPInt().getSExtValue();
                    const long long elemSize =
                        static_cast<long long>(dataLayout.getTypeStoreSize(accessType));
                    if (elemSize > 0 && stepBytes % elemSize == 0) {
                        const long long strideFromScev = stepBytes / elemSize;
                        if (strideState == unknownStride) {
                            strideState = strideFromScev;
                        } else if (strideState != strideFromScev) {
                            strideState = 0;
                            features.hasKnownStride = false;
                        }
                    }
                }
            }
            // If AddRecExpr is for an outer loop, the access is loop-invariant
            // in the current loop — affine stays unchanged (trivially affine).
        }

        bool accessDependsOnCurrentIV = scevHandled;

        if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(ptr->stripPointerCasts())) {
            int nonConstIndices = 0;
            long long stride = 0;
            bool strideOk = false;
            bool allLinear = true;
            bool dependsOnCurrentIV = false;

            for (const llvm::Use& idxUse : gep->indices()) {
                const llvm::Value* idx = idxUse.get();
                if (llvm::isa<llvm::ConstantInt>(idx)) {
                    continue;
                }

                ++nonConstIndices;
                for (const llvm::PHINode* ivCandidate : loopIVs) {
                    if (containsValue(idx, ivCandidate)) {
                        usedIVs.insert(ivCandidate);
                    }
                }

                // Check if this GEP index depends on the current loop's IV.
                if (inductionVariable && containsValue(idx, inductionVariable)) {
                    dependsOnCurrentIV = true;
                }

                if (!scevHandled) {
                    if (inductionVariable) {
                        bool coeffOk = false;
                        const long long coeff =
                            getLinearCoeff(idx, inductionVariable, allocaSteps, coeffOk);
                        if (coeffOk) {
                            stride += coeff;
                            strideOk = true;
                        } else {
                            allLinear = false;
                        }
                    } else {
                        bool coeffOk = false;
                        const long long coeff =
                            getLinearCoeff(idx, nullptr, allocaSteps, coeffOk);
                        if (coeffOk) {
                            stride += coeff;
                            strideOk = true;
                        } else {
                            allLinear = false;
                        }
                    }
                }
            }

            if (dependsOnCurrentIV) {
                accessDependsOnCurrentIV = true;
            }

            features.multidim = features.multidim || (nonConstIndices > 1);

            // Only let the GEP manual analysis override affine when:
            // 1. SCEV didn't already handle this access, AND
            // 2. The GEP index actually depends on the current loop's IV.
            // If the index only references outer IVs, the address is
            // loop-invariant in this loop — trivially affine.
            if (!scevHandled && nonConstIndices > 0 && dependsOnCurrentIV) {
                features.affine = features.affine && allLinear;
            }

            if (!scevHandled && strideOk) {
                if (strideState == unknownStride) {
                    strideState = stride;
                } else if (strideState != stride) {
                    strideState = 0;
                    features.hasKnownStride = false;
                }
            }
        }

        // If the access does not recur in the current loop AND the GEP
        // indices do not reference the current IV, the access is
        // loop-invariant — stride is 0 (same address every iteration).
        if (!accessDependsOnCurrentIV && strideState == unknownStride) {
            strideState = 0;
        }

        const long long elemSize = static_cast<long long>(dataLayout.getTypeStoreSize(accessType));
        if (bodyIterations > 0 && elemSize > 0 && strideState != unknownStride) {
            long long bytes;
            if (strideState == 0) {
                // Broadcast: single element accessed every iteration.
                bytes = elemSize;
            } else {
                bytes = static_cast<long long>(std::llabs(strideState)) *
                        static_cast<long long>(bodyIterations) *
                        elemSize;
            }
            if (bytes > features.workingSetBytes) {
                features.hasKnownWorkingSet = true;
                features.workingSetBytes = bytes;
            }
        }
    }

    if (strideState != unknownStride) {
        features.hasKnownStride = true;
        features.strideElements = strideState;

        const long long absStride = std::llabs(strideState);
        if (absStride > 0) {
            features.fillFactor = std::min(1.0, 1.0 / static_cast<double>(absStride));
        } else {
            // stride == 0: broadcast / loop-invariant access.
            // Same element every iteration — perfect cache reuse.
            features.fillFactor = 1.0;
        }


        if (absStride == 0) {
            // Broadcast: one element accessed repeatedly.
            features.contiguousBlockElements = 1;
        } else if (absStride == 1) {
            // Unit stride: all elements are contiguous.
            // If trip count is known, contiguous_block = iterations.
            // Otherwise leave 0 (unknown).
            features.contiguousBlockElements =
                bodyIterations > 0 ? static_cast<long long>(bodyIterations) : 0;
        } else {
            const llvm::Instruction* instruction = accesses.empty() ? nullptr : accesses.front();
            if (instruction) {
                llvm::Type* accessType = nullptr;
                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
                    accessType = load->getType();
                } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(instruction)) {
                    accessType = store->getValueOperand()->getType();
                }
                if (accessType) {
                    const long long elemSize =
                        static_cast<long long>(dataLayout.getTypeStoreSize(accessType));
                    if (elemSize > 0 && absStride > 0) {
                        const long long elemsPerLine = cacheLineBytes / elemSize;
                        features.contiguousBlockElements = std::max(1LL, elemsPerLine / absStride);
                    }
                }
            }
        }
    } else {
        features.fillFactor = 0.0;
        features.contiguousBlockElements = 0;
    }

    if (usedIVs.size() > 1) {
        features.multidim = true;
    }

    return features;
}

}  // namespace analyzer
