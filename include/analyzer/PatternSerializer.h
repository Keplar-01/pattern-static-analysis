#pragma once

#include "analyzer/AnalysisResult.h"

#include <string>

namespace analyzer {

class PatternSerializer {
public:
    void writeJson(const std::string& outputPath, const AnalysisResult& result) const;
};

}  // namespace analyzer
