#include "analyzer/AnalyzerConfig.h"
#include "analyzer/PatternExtractor.h"
#include "analyzer/PatternSerializer.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool endsWith(const std::string& value, const std::string& suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }
    return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string prepareInputForAnalysis(const std::string& inputPath) {
    if (endsWith(inputPath, ".c")) {
        const std::string irPath = "analyzer_input.bc";
        const std::string cmd =
            "clang-16 -O0 -Xclang -disable-O0-optnone -g "
            "-fno-inline -fno-inline-functions "
            "-fno-discard-value-names "
            "-emit-llvm -c " +
            shellQuote(inputPath) +
            " -o " + shellQuote(irPath);
        const int rc = std::system(cmd.c_str());
        if (rc != 0) {
            throw std::runtime_error("Failed to compile C source with clang-16: " + inputPath);
        }
        return irPath;
    }
    return inputPath;
}

}  // namespace

void applyCliOverrides(analyzer::AnalyzerConfig& config, int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--input" || arg == "-i") && i + 1 < argc) {
            config.inputFile = argv[++i];
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            config.outputFile = argv[++i];
        } else if (arg == "--stdout") {
            config.outputFile = "-";
        } else if (arg == "--quiet" || arg == "-q") {
            config.verbose = false;
        }
    }
}

int main(int argc, char** argv) {
    const std::string configPath = (argc > 1) ? argv[1] : "conf.json";

    try {
        analyzer::AnalyzerConfig config = analyzer::AnalyzerConfig::fromJsonFile(configPath);
        applyCliOverrides(config, argc, argv);

        const std::string analysisInput = prepareInputForAnalysis(config.inputFile);
        const std::string sourceFilterPath =
            endsWith(config.inputFile, ".c") ? config.inputFile : "";
        const analyzer::PatternExtractor extractor;
        const analyzer::AnalysisResult result =
            extractor.extractFromIR(analysisInput, config.maxLoopDepth, sourceFilterPath);

        const analyzer::PatternSerializer serializer;
        serializer.writeJson(config.outputFile, result);

        std::cerr << "Analysis finished\n";
        std::cerr << "Input: " << config.inputFile << "\n";
        std::cerr << "IR used: " << analysisInput << "\n";
        std::cerr << "Patterns found: " << result.patterns.size() << "\n";
        if (config.outputFile != "-") {
            std::cerr << "Output file: " << config.outputFile << "\n";
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
