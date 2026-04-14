#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <type_traits>
#include <vector>

#include "ecs/ecs.hpp"

TEST_CASE("paged sparse set stores 64 bit entities across pages") {
    static_assert(std::is_same_v<ecs::Entity, std::uint64_t>);
    static_assert(ecs::entity_index_bits == 59);
    static_assert(ecs::entity_version_bits == 5);

    ecs::PagedSparseSet entities(4);

    REQUIRE(entities.insert(1));
    REQUIRE(entities.insert(9));
    REQUIRE_FALSE(entities.insert(1));

    REQUIRE(entities.size() == 2);
    REQUIRE(entities.contains(1));
    REQUIRE(entities.contains(9));
    REQUIRE(entities.page_size() == 4);

    REQUIRE(entities.erase(1));
    REQUIRE_FALSE(entities.contains(1));
    REQUIRE(entities.contains(9));
}

TEST_CASE("paged sparse set keys sparse slots by entity index but matches full entity ids") {
    ecs::PagedSparseSet entities(4);

    const ecs::Entity initial = ecs::make_entity(7, 0);
    const ecs::Entity recycled = ecs::make_entity(7, 1);

    REQUIRE(entities.insert(initial));
    REQUIRE(entities.contains(initial));
    REQUIRE_FALSE(entities.contains(recycled));
    REQUIRE(entities.index_of(recycled) == ecs::PagedSparseSet::npos);
    REQUIRE_FALSE(entities.insert(recycled));

    REQUIRE(entities.erase(initial));
    REQUIRE(entities.insert(recycled));
    REQUIRE_FALSE(entities.contains(initial));
    REQUIRE(entities.contains(recycled));
}

TEST_CASE("paged sparse set rejects non-power-of-two page sizes") {
    REQUIRE_THROWS_AS(ecs::PagedSparseSet(0), std::invalid_argument);
    REQUIRE_THROWS_AS(ecs::PagedSparseSet(3), std::invalid_argument);
    REQUIRE_THROWS_AS(ecs::PagedSparseSet(6), std::invalid_argument);
    REQUIRE_THROWS_AS(ecs::PagedSparseSet(10), std::invalid_argument);
}

TEST_CASE("paged sparse set reports indexes and preserves dense order until removal") {
    ecs::PagedSparseSet entities(4);

    REQUIRE(entities.empty());
    REQUIRE(entities.index_of(1) == ecs::PagedSparseSet::npos);

    REQUIRE(entities.insert(1));
    REQUIRE(entities.insert(4));
    REQUIRE(entities.insert(9));

    REQUIRE_FALSE(entities.empty());
    REQUIRE(entities.size() == 3);
    REQUIRE(entities.index_of(1) == 0);
    REQUIRE(entities.index_of(4) == 1);
    REQUIRE(entities.index_of(9) == 2);

    const std::vector<ecs::Entity>& dense = entities.entities();
    REQUIRE(dense == std::vector<ecs::Entity>{1, 4, 9});
}

TEST_CASE("paged sparse set compacts dense storage on erase across pages") {
    ecs::PagedSparseSet entities(4);

    REQUIRE(entities.insert(1));
    REQUIRE(entities.insert(4));
    REQUIRE(entities.insert(9));
    REQUIRE(entities.insert(15));

    REQUIRE(entities.erase(4));
    REQUIRE_FALSE(entities.contains(4));
    REQUIRE(entities.index_of(4) == ecs::PagedSparseSet::npos);
    REQUIRE(entities.size() == 3);

    const std::vector<ecs::Entity>& dense = entities.entities();
    REQUIRE(dense == std::vector<ecs::Entity>{1, 15, 9});
    REQUIRE(entities.index_of(1) == 0);
    REQUIRE(entities.index_of(15) == 1);
    REQUIRE(entities.index_of(9) == 2);

    REQUIRE_FALSE(entities.erase(4));
}

TEST_CASE("paged sparse set clear resets membership and can be reused") {
    ecs::PagedSparseSet entities(4);

    REQUIRE(entities.insert(2));
    REQUIRE(entities.insert(8));
    REQUIRE(entities.insert(17));

    entities.clear();

    REQUIRE(entities.empty());
    REQUIRE(entities.size() == 0);
    REQUIRE(entities.entities().empty());
    REQUIRE_FALSE(entities.contains(2));
    REQUIRE_FALSE(entities.contains(8));
    REQUIRE_FALSE(entities.contains(17));
    REQUIRE(entities.index_of(2) == ecs::PagedSparseSet::npos);
    REQUIRE(entities.index_of(8) == ecs::PagedSparseSet::npos);
    REQUIRE(entities.index_of(17) == ecs::PagedSparseSet::npos);

    REQUIRE(entities.insert(17));
    REQUIRE(entities.insert(2));
    REQUIRE(entities.entities() == std::vector<ecs::Entity>{17, 2});
    REQUIRE(entities.index_of(17) == 0);
    REQUIRE(entities.index_of(2) == 1);
}

TEST_CASE("paged sparse set supports move construction and assignment") {
    ecs::PagedSparseSet original(8);
    REQUIRE(original.insert(3));
    REQUIRE(original.insert(19));

    ecs::PagedSparseSet moved(std::move(original));
    REQUIRE(moved.page_size() == 8);
    REQUIRE(moved.size() == 2);
    REQUIRE(moved.contains(3));
    REQUIRE(moved.contains(19));
    REQUIRE(moved.entities() == std::vector<ecs::Entity>{3, 19});

    ecs::PagedSparseSet reassigned(4);
    REQUIRE(reassigned.insert(1));

    reassigned = std::move(moved);
    REQUIRE(reassigned.page_size() == 8);
    REQUIRE(reassigned.size() == 2);
    REQUIRE(reassigned.contains(3));
    REQUIRE(reassigned.contains(19));
    REQUIRE(reassigned.index_of(3) == 0);
    REQUIRE(reassigned.index_of(19) == 1);
}
