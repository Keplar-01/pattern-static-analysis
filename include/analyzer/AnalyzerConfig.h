#pragma once

#include <string>

namespace analyzer {

enum class OutputFormat {
    JSON,
    JSONL
};

struct AnalyzerConfig {
    std::string inputFile;
    std::string outputFile;

    int maxLoopDepth = 4;
    int cacheLineBytes = 64;
    bool analyzeDependencies = true;
    bool analyzeScev = true;

    OutputFormat format = OutputFormat::JSON;

    bool verbose = false;
    bool dumpLoops = false;
    bool dumpScev = false;
    bool dumpMemoryAccesses = false;

    bool enableFingerprint = true;
    bool enableClassification = true;

    static AnalyzerConfig fromJsonFile(const std::string& path);
};

}  // namespace analyzer
