#include "simplelib/simplelib.hpp"

#include <utility>

namespace simplelib {

Greeter::Greeter(std::string prefix)
    : prefix_(std::move(prefix)) {}

std::string Greeter::greet(const std::string& name) const {
    return prefix_ + ", " + name + "!";
}

int add(int lhs, int rhs) {
    return lhs + rhs;
}

}  // namespace simplelib
