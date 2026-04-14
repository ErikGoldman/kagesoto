#include <iostream>

#include "simplelib/simplelib.hpp"

int main() {
    simplelib::Greeter greeter("Welcome");

    std::cout << greeter.greet("cross-platform C++") << '\n';
    std::cout << "2 + 3 = " << simplelib::add(2, 3) << '\n';

    return 0;
}
