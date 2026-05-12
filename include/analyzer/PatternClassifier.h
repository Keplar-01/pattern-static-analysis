#pragma once

#include "analyzer/PatternTypes.h"

namespace analyzer {

struct AccessPatternResult;

class PatternClassifier {
public:
    PatternType classify(const AccessPatternResult& pattern) const;
};

}  // namespace analyzer
