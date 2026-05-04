#include "ecs/bit_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <vector>

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

TEST_CASE("bit buffer clears stale trailing bits after assigning non byte aligned payloads") {
    ecs::BitBuffer buffer;
    buffer.assign_bytes(std::vector<std::uint8_t>{0xFEU}, 1U);

    buffer.push_bool(false);

    REQUIRE(buffer.bit_size() == 2U);
    REQUIRE(buffer.read_bool() == false);
    REQUIRE(buffer.read_bool() == false);
}

TEST_CASE("bit buffer copies only logical bits from non byte aligned source buffers") {
    ecs::BitBuffer source;
    source.assign_bytes(std::vector<std::uint8_t>{0xFEU}, 1U);

    ecs::BitBuffer copied;
    copied.push_buffer_bits(source);
    copied.push_bool(false);

    REQUIRE(copied.bit_size() == 2U);
    REQUIRE(copied.read_bool() == false);
    REQUIRE(copied.read_bool() == false);
}

TEST_CASE("bit buffer validates invalid reads writes and assignment") {
    ecs::BitBuffer buffer;
    REQUIRE_THROWS_AS(buffer.push_bits(0, 65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_unsigned_bits(65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_bool(), std::out_of_range);
    REQUIRE_THROWS_AS(buffer.assign_bytes({}, 1U), std::invalid_argument);
}

TEST_CASE("bit buffer supports byte aligned unsigned and byte operations") {
    ecs::BitBuffer buffer;
    buffer.reserve_bytes(8U);
    buffer.push_unsigned_bits(0x1234U, 16U);
    buffer.push_bytes("xy", 2U);

    REQUIRE_FALSE(buffer.empty());
    REQUIRE(buffer.size() == 4U);
    REQUIRE(buffer.data() == buffer.bytes().data());
    REQUIRE(buffer.remaining_bits() == 32U);
    REQUIRE(buffer.read_unsigned_bits(16U) == 0x1234U);

    char bytes[2]{};
    buffer.read_bytes(bytes, 2U);
    REQUIRE(bytes[0] == 'x');
    REQUIRE(bytes[1] == 'y');
    REQUIRE(buffer.remaining_bits() == 0U);

    buffer.clear();
    REQUIRE(buffer.empty());
    REQUIRE(buffer.bit_size() == 0U);
    REQUIRE(buffer.read_offset_bits() == 0U);
}

TEST_CASE("bit buffer supports unaligned unsigned byte and buffer operations") {
    ecs::BitBuffer buffer;
    buffer.push_bool(true);
    buffer.push_unsigned_bits(0x1ffU, 9U);
    buffer.push_bytes("A", 1U);

    REQUIRE(buffer.read_bool());
    REQUIRE(buffer.read_unsigned_bits(9U) == 0x1ffU);
    char byte{};
    buffer.read_bytes(&byte, 1U);
    REQUIRE(byte == 'A');
    REQUIRE(buffer.remaining_bits() == 0U);

    ecs::BitBuffer source;
    source.push_bool(false);
    source.push_bool(true);

    ecs::BitBuffer copied;
    copied.push_bool(true);
    copied.push_buffer_bits(source);
    REQUIRE(copied.bit_size() == 3U);
    REQUIRE(copied.read_bool());
    REQUIRE_FALSE(copied.read_bool());
    REQUIRE(copied.read_bool());

    ecs::BitBuffer frame;
    frame.push_bool(false);
    frame.push_bytes("B", 1U);
    ecs::BitBuffer extracted;
    frame.read_buffer_bits(extracted, 9U);
    REQUIRE_FALSE(extracted.read_bool());
    char extracted_byte{};
    extracted.read_bytes(&extracted_byte, 1U);
    REQUIRE(extracted_byte == 'B');
}

TEST_CASE("bit buffer overwrites aligned unaligned and wide bit ranges") {
    ecs::BitBuffer buffer;
    buffer.push_unsigned_bits(0U, 16U);
    buffer.overwrite_unsigned_bits(0U, 0xabcdU, 16U);
    REQUIRE(buffer.read_unsigned_bits(16U) == 0xabcdU);

    buffer.reset_read();
    buffer.overwrite_unsigned_bits(3U, 0x1fU, 5U);
    buffer.skip_bits(3U);
    REQUIRE(buffer.read_unsigned_bits(5U) == 0x1fU);

    ecs::BitBuffer wide;
    wide.push_bool(false);
    wide.push_unsigned_bits(0U, 64U);
    wide.overwrite_unsigned_bits(1U, 0xffffffffffffffffULL, 64U);
    wide.skip_bits(1U);
    REQUIRE(wide.read_unsigned_bits(64U) == 0xffffffffffffffffULL);
}

TEST_CASE("bit buffer accepts no-op operations and rejects null byte buffers") {
    ecs::BitBuffer buffer;
    buffer.push_unsigned_bits(123U, 0U);
    buffer.push_bytes(nullptr, 0U);
    buffer.read_bytes(nullptr, 0U);
    buffer.read_buffer_bits(buffer, 0U);
    REQUIRE(buffer.read_bits(0U) == 0);
    REQUIRE(buffer.bit_size() == 0U);

    REQUIRE_THROWS_AS(buffer.push_bytes(nullptr, 1U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_bytes(nullptr, 1U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.overwrite_unsigned_bits(0U, 0U, 65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.overwrite_unsigned_bits(1U, 0U, 1U), std::out_of_range);
}

TEST_CASE("bit buffer reads unaligned 64-bit values across a byte window") {
    ecs::BitBuffer buffer;
    buffer.push_bool(true);
    buffer.push_unsigned_bits(0x0123456789abcdefULL, 64U);
    buffer.push_bool(false);

    REQUIRE(buffer.read_bool());
    REQUIRE(buffer.read_unsigned_bits(64U) == 0x0123456789abcdefULL);
    REQUIRE_FALSE(buffer.read_bool());
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer masks overwritten tail bits without disturbing neighbors") {
    ecs::BitBuffer buffer;
    buffer.push_unsigned_bits(0xffffU, 16U);
    buffer.overwrite_unsigned_bits(5U, 0U, 6U);

    REQUIRE(buffer.read_unsigned_bits(5U) == 0x1fU);
    REQUIRE(buffer.read_unsigned_bits(6U) == 0U);
    REQUIRE(buffer.read_unsigned_bits(5U) == 0x1fU);
}
