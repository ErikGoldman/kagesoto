#include <catch2/catch_test_macros.hpp>

#include "simplelib/simplelib.hpp"

TEST_CASE("add returns the sum of two integers") {
    REQUIRE(simplelib::add(2, 3) == 5);
    REQUIRE(simplelib::add(-4, 1) == -3);
}

TEST_CASE("Greeter formats a greeting with the configured prefix") {
    const simplelib::Greeter greeter("Hi");

    REQUIRE(greeter.greet("library") == "Hi, library!");
}
