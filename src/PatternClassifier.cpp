#include "analyzer/PatternClassifier.h"

#include "analyzer/AnalysisResult.h"

#include <cstdlib>

namespace analyzer {

PatternType PatternClassifier::classify(const AccessPatternResult& pattern) const {
    if (!pattern.affine) {
        if (pattern.indexedByMemory) {
            return PatternType::GatherScatter;
        }
        return PatternType::Indirect;
    }

    // stride == 0 means the access is loop-invariant (same address every
    // iteration) and should be classified as broadcast even if the address
    // expression references outer-loop IVs.
    if (pattern.hasKnownStride && pattern.strideElements == 0) {
        return PatternType::Broadcast;
    }

    if (pattern.multidim && pattern.affine) {
        return PatternType::MultidimAffine;
    }

    if (pattern.hasKnownStride && std::llabs(pattern.strideElements) == 1) {
        return PatternType::UnitStride;
    }

    if (pattern.hasKnownStride && std::llabs(pattern.strideElements) > 1) {
        return PatternType::ConstantStride;
    }

    return PatternType::Unknown;
}

}  // namespace analyzer
