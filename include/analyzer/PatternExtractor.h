#pragma once

#include "analyzer/AnalysisResult.h"

#include <string>

namespace analyzer {

class PatternExtractor {
public:
    AnalysisResult extractFromIR(const std::string& inputPath,
                                 int maxLoopDepth,
                                 const std::string& sourceFilterPath = "") const;
};

}  // namespace analyzer
