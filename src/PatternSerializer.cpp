#include "analyzer/PatternSerializer.h"

#include "analyzer/PatternTypes.h"

#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace analyzer {

void PatternSerializer::writeJson(const std::string& outputPath,
                                  const AnalysisResult& result) const {
    llvm::json::Array patterns;
    for (const AccessPatternResult& pattern : result.patterns) {
        llvm::json::Object obj;
        obj["sequence_index"] = pattern.sequenceIndex;
        obj["function"] = pattern.functionName;
        obj["depth"] = pattern.depth;
        obj["access_kind"] = pattern.accessKind;
        obj["base_symbol"] = pattern.baseSymbol;
        obj["base_kind"] = pattern.baseKind;
        obj["indexed_by_memory"] = pattern.indexedByMemory;
        obj["has_indexed_addressing"] = pattern.hasIndexedAddressing;
        obj["load_count"] = pattern.loadCount;
        obj["store_count"] = pattern.storeCount;
        obj["affine"] = pattern.affine;
        obj["conditional"] = pattern.conditional;
        obj["fill_factor"] = pattern.fillFactor;
        obj["working_set_bytes"] = pattern.workingSetBytes;
        obj["dependence"] = pattern.dependence;
        obj["pattern_fingerprint"] = pattern.patternSignature;
        obj["pattern_type"] = toString(pattern.patternType);
        obj["pattern_signature"] = pattern.patternSignature;
        if (!pattern.sourceFile.empty()) {
            obj["source_file"] = pattern.sourceFile;
        } else {
            obj["source_file"] = nullptr;
        }
        if (pattern.sourceLine > 0) {
            obj["source_line"] = pattern.sourceLine;
            obj["source_column"] = pattern.sourceColumn;
        } else {
            obj["source_line"] = nullptr;
            obj["source_column"] = nullptr;
        }

        if (pattern.hasKnownStride) {
            obj["stride"] = pattern.strideElements;
        } else {
            obj["stride"] = nullptr;
        }
        if (pattern.contiguousBlockElements > 0) {
            obj["contiguous_block"] = pattern.contiguousBlockElements;
        } else {
            obj["contiguous_block"] = nullptr;
        }
        if (pattern.alignmentBytes > 0) {
            obj["alignment"] = pattern.alignmentBytes;
        } else {
            obj["alignment"] = nullptr;
        }
        patterns.push_back(std::move(obj));
    }

    std::string json;
    llvm::raw_string_ostream jsonOut(json);
    jsonOut << llvm::formatv("{0:2}", llvm::json::Value(std::move(patterns)));
    jsonOut.flush();

    if (outputPath == "-") {
        std::cout << json << '\n';
    } else {
        std::ofstream out(outputPath, std::ios::trunc);
        if (!out.is_open()) {
            throw std::runtime_error("Cannot open output file: " + outputPath);
        }
        out << json << '\n';
    }
}

}  // namespace analyzer
