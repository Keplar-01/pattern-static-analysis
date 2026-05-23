#include "analyzer/FingerprintBuilder.h"

#include "analyzer/AnalysisResult.h"
#include "analyzer/PatternTypes.h"

namespace analyzer {

std::string FingerprintBuilder::build(const AccessPatternResult& pattern) const {
    const std::string strideText = pattern.hasKnownStride ? std::to_string(pattern.strideElements)
                                                          : std::string("na");
    const std::string workingSetText =
        pattern.hasKnownWorkingSet ? std::to_string(pattern.workingSetBytes) : std::string("na");

    return "k=" + pattern.accessKind +
           "|p=" + toString(pattern.patternType) +
           "|s=" + strideText +
            "|a=" + (pattern.affine ? "1" : "0") +
           "|c=" + (pattern.conditional ? "1" : "0") +
           "|m=" + (pattern.multidim ? "1" : "0") +
           "|im=" + (pattern.indexedByMemory ? "1" : "0") +
           "|ia=" + (pattern.hasIndexedAddressing ? "1" : "0") +
           "|wk=" + (pattern.hasKnownWorkingSet ? "1" : "0") +
           "|ws=" + workingSetText;
}

}  // namespace analyzer
