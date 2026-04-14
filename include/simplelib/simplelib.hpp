#pragma once

#include <string>

#include "simplelib/export.hpp"

namespace simplelib {

class SIMPLELIB_API Greeter {
public:
    explicit Greeter(std::string prefix = "Hello");

    std::string greet(const std::string& name) const;

private:
    std::string prefix_;
};

SIMPLELIB_API int add(int lhs, int rhs);

}  // namespace simplelib
