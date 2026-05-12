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

    if (pattern.multidim && pattern.affine) {
        return PatternType::MultidimAffine;
    }

    if (pattern.hasKnownStride && std::llabs(pattern.strideElements) == 1) {
        return PatternType::UnitStride;
    }

    if (pattern.hasKnownStride && std::llabs(pattern.strideElements) > 1) {
        return PatternType::ConstantStride;
    }

    // stride == 0 means the access is loop-invariant (same address every
    // iteration) — a broadcast pattern.
    if (pattern.hasKnownStride && pattern.strideElements == 0) {
        return PatternType::Broadcast;
    }

    return PatternType::Unknown;
}

}  // namespace analyzer
