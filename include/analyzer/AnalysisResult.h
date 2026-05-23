#pragma once

#include "analyzer/PatternTypes.h"

#include <string>
#include <vector>

namespace analyzer {

struct AccessPatternResult {
    unsigned int sequenceIndex = 0;
    std::string functionName;
    int depth = 0;
    std::string baseSymbol;
    std::string baseKind;
    std::string accessKind;

    int loadCount = 0;
    int storeCount = 0;

    bool affine = false;
    bool conditional = false;
    bool indexedByMemory = false;
    bool hasIndexedAddressing = false;

    bool hasKnownStride = false;
    long long strideElements = 0;
    long long contiguousBlockElements = 0;
    double fillFactor = 0.0;

    bool multidim = false;
    bool hasKnownWorkingSet = false;
    long long workingSetBytes = 0;
    long long alignmentBytes = 0;

    std::string dependence = "unknown";
    PatternType patternType = PatternType::Unknown;
    std::string patternSignature;
    std::string sourceFile;
    int sourceLine = 0;
    int sourceColumn = 0;
};

struct AnalysisResult {
    std::vector<AccessPatternResult> patterns;
};

}  // namespace analyzer
