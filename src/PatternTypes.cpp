#include "analyzer/PatternTypes.h"

namespace analyzer {

const char* toString(PatternType type) {
    switch (type) {
        case PatternType::UnitStride:
            return "unit_stride";
        case PatternType::ConstantStride:
            return "constant_stride";
        case PatternType::MultidimAffine:
            return "multidim_affine";
        case PatternType::GatherScatter:
            return "gather_scatter";
        case PatternType::Indirect:
            return "indirect";
        case PatternType::Broadcast:
            return "broadcast";
        case PatternType::Irregular:
            return "irregular";
        case PatternType::Unknown:
            return "unknown";
    }
    return "unknown";
}

}  // namespace analyzer
