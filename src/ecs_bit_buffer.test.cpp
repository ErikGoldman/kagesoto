#include "ecs/bit_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>

TEST_CASE("bit buffer pushes and reads bits bytes and bools") {
    ecs::BitBuffer buffer;
    buffer.push_bool(true);
    buffer.push_bits(0b101, 3U);
    buffer.push_bytes("AZ", 2U);

    REQUIRE(buffer.bit_size() == 20U);
    REQUIRE(buffer.byte_size() == 3U);
    REQUIRE(buffer.read_bool());
    REQUIRE(buffer.read_bits(3U) == 0b101);

    char bytes[2]{};
    buffer.read_bytes(bytes, 2U);
    REQUIRE(bytes[0] == 'A');
    REQUIRE(bytes[1] == 'Z');
    REQUIRE_THROWS_AS(buffer.read_bool(), std::out_of_range);
}

TEST_CASE("bit buffer preserves exact non byte aligned frame lengths") {
    ecs::BitBuffer source;
    source.push_bits(0b101, 3U);
    source.push_unsigned_bits(0xcafebabeU, 32U);

    ecs::BitBuffer loaded;
    loaded.assign_bytes(source.bytes(), source.bit_size());

    REQUIRE(loaded.bit_size() == 35U);
    REQUIRE(loaded.read_bits(3U) == 0b101);
    REQUIRE(loaded.read_unsigned_bits(32U) == 0xcafebabeU);
    REQUIRE(loaded.remaining_bits() == 0U);
}

TEST_CASE("bit buffer validates invalid reads writes and assignment") {
    ecs::BitBuffer buffer;
    REQUIRE_THROWS_AS(buffer.push_bits(0, 65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_unsigned_bits(65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_bool(), std::out_of_range);
    REQUIRE_THROWS_AS(buffer.assign_bytes({}, 1U), std::invalid_argument);
}
