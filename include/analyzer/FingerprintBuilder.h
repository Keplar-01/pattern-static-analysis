#pragma once

#include <string>

namespace analyzer {

struct AccessPatternResult;

class FingerprintBuilder {
public:
    std::string build(const AccessPatternResult& pattern) const;
};

}  // namespace analyzer
