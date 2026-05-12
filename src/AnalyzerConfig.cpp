#include "analyzer/AnalyzerConfig.h"

#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>

#include <stdexcept>

namespace analyzer {

namespace {

const llvm::json::Object* getObject(const llvm::json::Object& root, llvm::StringRef key) {
    return root.getObject(key);
}

template <typename T>
void assignIfPresent(const llvm::json::Object&, llvm::StringRef, T&) {
}

template <>
void assignIfPresent<std::string>(const llvm::json::Object& obj,
                                  llvm::StringRef key,
                                  std::string& dst) {
    if (auto value = obj.getString(key)) {
        dst = value->str();
    }
}

template <>
void assignIfPresent<int>(const llvm::json::Object& obj, llvm::StringRef key, int& dst) {
    if (auto value = obj.getInteger(key)) {
        dst = static_cast<int>(*value);
    }
}

template <>
void assignIfPresent<bool>(const llvm::json::Object& obj, llvm::StringRef key, bool& dst) {
    if (auto value = obj.getBoolean(key)) {
        dst = *value;
    }
}

}  // namespace

AnalyzerConfig AnalyzerConfig::fromJsonFile(const std::string& path) {
    auto bufferOrError = llvm::MemoryBuffer::getFile(path);
    if (!bufferOrError) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    auto parsed = llvm::json::parse(bufferOrError.get()->getBuffer());
    if (!parsed) {
        throw std::runtime_error("Invalid JSON in config file: " + path);
    }

    auto* root = parsed->getAsObject();
    if (!root) {
        throw std::runtime_error("Config root must be a JSON object");
    }

    AnalyzerConfig config;

    assignIfPresent(*root, "input", config.inputFile);
    assignIfPresent(*root, "output", config.outputFile);

    if (const auto* analysis = getObject(*root, "analysis")) {
        assignIfPresent(*analysis, "max_loop_depth", config.maxLoopDepth);
        assignIfPresent(*analysis, "analyze_dependencies", config.analyzeDependencies);
        assignIfPresent(*analysis, "analyze_scev", config.analyzeScev);
    }

    if (auto format = root->getString("output_format")) {
        if (*format == "json") {
            config.format = OutputFormat::JSON;
        } else if (*format == "jsonl") {
            config.format = OutputFormat::JSONL;
        } else {
            throw std::runtime_error("Unknown output_format: " + std::string(format->str()));
        }
    }

    if (const auto* debug = getObject(*root, "debug")) {
        assignIfPresent(*debug, "verbose", config.verbose);
        assignIfPresent(*debug, "dump_loops", config.dumpLoops);
        assignIfPresent(*debug, "dump_scev", config.dumpScev);
        assignIfPresent(*debug, "dump_memory", config.dumpMemoryAccesses);
    }

    if (const auto* features = getObject(*root, "features")) {
        assignIfPresent(*features, "enable_fingerprint", config.enableFingerprint);
        assignIfPresent(*features, "enable_classification", config.enableClassification);
    }

    if (config.inputFile.empty()) {
        throw std::runtime_error("Config must contain 'input'");
    }

    if (config.outputFile.empty()) {
        config.outputFile = "output.jsonl";
    }

    return config;
}

}  // namespace analyzer
