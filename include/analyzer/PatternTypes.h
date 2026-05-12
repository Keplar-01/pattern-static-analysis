#pragma once

namespace analyzer {

enum class PatternType {
    UnitStride,
    ConstantStride,
    MultidimAffine,
    GatherScatter,
    Indirect,
    Broadcast,
    Irregular,
    Unknown
};

const char* toString(PatternType type);

}  // namespace analyzer
