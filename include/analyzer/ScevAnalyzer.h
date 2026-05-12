#pragma once

#include <vector>
#include <unordered_map>

namespace llvm {
class DataLayout;
class Instruction;
class Loop;
class PHINode;
class ScalarEvolution;
class Value;
class AllocaInst;
}

namespace analyzer {

struct ScevFeatures {
    bool affine = false;
    bool hasKnownStride = false;
    long long strideElements = 0;
    long long contiguousBlockElements = 0;
    double fillFactor = 0.0;
    bool multidim = false;
    long long workingSetBytes = 0;
};

class ScevAnalyzer {
public:
    ScevFeatures analyze(const llvm::Loop& loop,
                         llvm::ScalarEvolution& scalarEvolution,
                         const llvm::DataLayout& dataLayout,
                         const std::vector<const llvm::Instruction*>& accesses) const;

private:
    long long getLinearCoeff(const llvm::Value* value,
                             const llvm::PHINode* inductionVariable,
                             const std::unordered_map<const llvm::AllocaInst*, long long>& allocaSteps,
                             bool& ok) const;
    long long getConstantIntValue(const llvm::Value* value, bool& ok) const;
};

}  // namespace analyzer
