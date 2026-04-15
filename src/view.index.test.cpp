#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <vector>

#include "ecs/ecs.hpp"

namespace {

struct IndexedPosition {
    std::int32_t x;
    std::int32_t y;
};

using XIndex = ecs::Index<&IndexedPosition::x>;
using XYUniqueIndex = ecs::UniqueIndex<&IndexedPosition::x, &IndexedPosition::y>;

}  // namespace

namespace ecs {

template <>
struct ComponentIndices<IndexedPosition> {
    using type = std::tuple<XIndex, XYUniqueIndex>;
};

}  // namespace ecs

TEST_CASE("transaction storage supports indexed lookup on single and compound keys") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<IndexedPosition>(first, IndexedPosition{10, 1});
        tx.write<IndexedPosition>(second, IndexedPosition{10, 2});
        tx.write<IndexedPosition>(third, IndexedPosition{30, 3});
        tx.commit();
    }

    auto tx = registry.transaction();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find<XIndex>(10) == std::vector<ecs::Entity>{first, second});
    REQUIRE(positions.find<XYUniqueIndex>(10, 2) == std::vector<ecs::Entity>{second});
    REQUIRE(positions.find_one<XYUniqueIndex>(30, 3) == third);
    REQUIRE(positions.find_one<XYUniqueIndex>(99, 99) == ecs::null_entity);
}

TEST_CASE("component indexes enforce uniqueness constraints") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<IndexedPosition>(first, IndexedPosition{5, 7});
        tx.commit();
    }

    const ecs::Entity second = registry.create();
    auto tx = registry.transaction();
    tx.write<IndexedPosition>(second, IndexedPosition{5, 7});
    REQUIRE_THROWS_AS(tx.commit(), std::invalid_argument);
}

TEST_CASE("component indexes stay in sync for remove and replace") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<IndexedPosition>(entity, IndexedPosition{1, 2});
        tx.commit();
    }

    {
        auto tx = registry.transaction();
        auto positions = tx.storage<IndexedPosition>();
        REQUIRE(positions.find_one<XYUniqueIndex>(1, 2) == entity);
    }

    {
        auto tx = registry.transaction();
        tx.write<IndexedPosition>(entity, IndexedPosition{8, 9});
        tx.commit();
    }

    {
        auto tx = registry.transaction();
        auto positions = tx.storage<IndexedPosition>();
        REQUIRE(positions.find_one<XYUniqueIndex>(1, 2) == ecs::null_entity);
        REQUIRE(positions.find_one<XYUniqueIndex>(8, 9) == entity);
    }

    REQUIRE(registry.remove<IndexedPosition>(entity));

    auto tx = registry.transaction();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_one<XYUniqueIndex>(8, 9) == ecs::null_entity);
}

TEST_CASE("transaction storage exposes staged indexed inserts before commit") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();

    auto tx = registry.transaction();
    IndexedPosition* staged = tx.write<IndexedPosition>(entity, IndexedPosition{11, 22});
    REQUIRE(staged != nullptr);

    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find<XIndex>(11) == std::vector<ecs::Entity>{entity});
    REQUIRE(positions.find_one<XYUniqueIndex>(11, 22) == entity);

    tx.commit();

    auto read_tx = registry.transaction();
    auto committed_positions = read_tx.storage<IndexedPosition>();
    REQUIRE(committed_positions.find<XIndex>(11) == std::vector<ecs::Entity>{entity});
    REQUIRE(committed_positions.find_one<XYUniqueIndex>(11, 22) == entity);
}

TEST_CASE("transaction storage switches indexed keys immediately within the transaction") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<IndexedPosition>(entity, IndexedPosition{3, 4});
        tx.commit();
    }

    auto tx = registry.transaction();
    IndexedPosition* staged = tx.write<IndexedPosition>(entity);
    REQUIRE(staged != nullptr);
    staged->x = 30;
    staged->y = 40;

    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find<XIndex>(3).empty());
    REQUIRE(positions.find<XIndex>(30) == std::vector<ecs::Entity>{entity});
    REQUIRE(positions.find_one<XYUniqueIndex>(3, 4) == ecs::null_entity);
    REQUIRE(positions.find_one<XYUniqueIndex>(30, 40) == entity);

    tx.commit();

    auto read_tx = registry.transaction();
    auto committed_positions = read_tx.storage<IndexedPosition>();
    REQUIRE(committed_positions.find<XIndex>(3).empty());
    REQUIRE(committed_positions.find<XIndex>(30) == std::vector<ecs::Entity>{entity});
    REQUIRE(committed_positions.find_one<XYUniqueIndex>(3, 4) == ecs::null_entity);
    REQUIRE(committed_positions.find_one<XYUniqueIndex>(30, 40) == entity);
}

TEST_CASE("component row reuse after remove does not leak stale index state") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<IndexedPosition>(first, IndexedPosition{12, 34});
        tx.commit();
    }

    REQUIRE(registry.remove<IndexedPosition>(first));

    {
        auto tx = registry.transaction();
        tx.write<IndexedPosition>(second, IndexedPosition{56, 78});
        tx.commit();
    }

    auto tx = registry.transaction();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE_FALSE(tx.has<IndexedPosition>(first));
    REQUIRE(positions.find_one<XYUniqueIndex>(12, 34) == ecs::null_entity);
    REQUIRE(positions.find_one<XYUniqueIndex>(56, 78) == second);
}

TEST_CASE("concurrent transactions enforce unique indexes against newly committed rows") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    auto older = registry.transaction();
    auto newer = registry.transaction();

    older.write<IndexedPosition>(first, IndexedPosition{90, 91});
    newer.write<IndexedPosition>(second, IndexedPosition{90, 91});

    newer.commit();
    REQUIRE_THROWS_AS(older.commit(), std::invalid_argument);

    auto tx = registry.transaction();
    auto positions = tx.storage<IndexedPosition>();
    REQUIRE(positions.find_one<XYUniqueIndex>(90, 91) == second);
}
