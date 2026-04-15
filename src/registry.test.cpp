#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <vector>

#include "ecs/ecs.hpp"

namespace {

struct Position {
    int x;
    int y;
};

struct Velocity {
    int dx;
    int dy;
};

}  // namespace

TEST_CASE("component ids are stable and distinct per component type") {
    static_assert(std::is_same_v<ecs::ComponentId, std::uint32_t>);

    const ecs::ComponentId position_id = ecs::component_id<Position>();
    const ecs::ComponentId velocity_id = ecs::component_id<Velocity>();

    REQUIRE(position_id != ecs::null_component);
    REQUIRE(velocity_id != ecs::null_component);
    REQUIRE(position_id == ecs::component_id<Position>());
    REQUIRE(velocity_id == ecs::component_id<Velocity>());
    REQUIRE(position_id != velocity_id);
}

TEST_CASE("registry creates recycles and removes entities and components") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();
    REQUIRE(entity != ecs::null_entity);
    REQUIRE(ecs::entity_index(entity) == 0);
    REQUIRE(ecs::entity_version(entity) == 0);
    REQUIRE(registry.alive(entity));
    REQUIRE(registry.entity_count() == 1);

    auto tx = registry.transaction();
    Position* position = tx.write<Position>(entity, Position{5, 11});
    REQUIRE(position != nullptr);
    REQUIRE(position->x == 5);
    REQUIRE(tx.get<Position>(entity).y == 11);
    tx.commit();

    {
        auto read_tx = registry.transaction();
        REQUIRE(read_tx.has<Position>(entity));

        std::vector<ecs::Entity> seen;
        read_tx.storage<Position>().each([&seen](ecs::Entity current, const Position&) {
            seen.push_back(current);
        });
        REQUIRE(seen.size() == 1);
        REQUIRE(seen.front() == entity);
    }

    REQUIRE(registry.remove<Position>(entity));
    {
        auto after_remove_tx = registry.transaction();
        REQUIRE_FALSE(after_remove_tx.has<Position>(entity));
    }

    REQUIRE(registry.destroy(entity));
    REQUIRE_FALSE(registry.alive(entity));

    const ecs::Entity recycled = registry.create();
    REQUIRE(recycled != entity);
    REQUIRE(ecs::entity_index(recycled) == ecs::entity_index(entity));
    REQUIRE(ecs::entity_version(recycled) == 1);
    REQUIRE_FALSE(registry.alive(entity));
    REQUIRE(registry.alive(recycled));
}

TEST_CASE("registry stale entities fail liveness and component lookups after index reuse") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<Position>(first, Position{1, 2});
        tx.commit();
    }

    REQUIRE(registry.destroy(first));

    const ecs::Entity second = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<Position>(second, Position{3, 4});
        tx.commit();
    }

    auto read_tx = registry.transaction();
    REQUIRE(ecs::entity_index(second) == ecs::entity_index(first));
    REQUIRE(ecs::entity_version(second) != ecs::entity_version(first));
    REQUIRE_FALSE(registry.alive(first));
    REQUIRE_FALSE(read_tx.has<Position>(first));
    REQUIRE(read_tx.try_get<Position>(first) == nullptr);
    REQUIRE(read_tx.get<Position>(second).x == 3);
}

TEST_CASE("transactions support component lookups by explicit component id") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{7, 9});
        tx.commit();
    }

    const ecs::ComponentId position_id = ecs::component_id<Position>();
    const ecs::ComponentId velocity_id = ecs::component_id<Velocity>();

    {
        auto tx = registry.transaction();
        REQUIRE(tx.has(entity, position_id));
        REQUIRE_FALSE(tx.has(entity, velocity_id));

        const Position* position = static_cast<const Position*>(tx.try_get(entity, position_id));
        REQUIRE(position != nullptr);
        REQUIRE(position->x == 7);
        REQUIRE(tx.try_get(entity, velocity_id) == nullptr);
    }

    REQUIRE(registry.remove(entity, position_id));
    {
        auto after_remove_tx = registry.transaction();
        REQUIRE_FALSE(after_remove_tx.has(entity, position_id));
    }
    REQUIRE_FALSE(registry.remove(entity, position_id));
}

TEST_CASE("registry structural mutations throw while a snapshot is open") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    auto snapshot = registry.snapshot();
    REQUIRE_THROWS_AS(registry.create(), std::logic_error);
    REQUIRE_THROWS_AS(registry.destroy(entity), std::logic_error);
    REQUIRE_THROWS_AS(registry.remove<Position>(entity), std::logic_error);
    REQUIRE_THROWS_AS(registry.clear(), std::logic_error);
}

TEST_CASE("registry structural mutations throw while a transaction is open") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();
    {
        auto seed = registry.transaction();
        seed.write<Position>(entity, Position{1, 2});
        seed.commit();
    }

    auto tx = registry.transaction();
    REQUIRE_THROWS_AS(registry.create(), std::logic_error);
    REQUIRE_THROWS_AS(registry.destroy(entity), std::logic_error);
    REQUIRE_THROWS_AS(registry.remove<Position>(entity), std::logic_error);
    REQUIRE_THROWS_AS(registry.clear(), std::logic_error);
}

TEST_CASE("registry structural mutations are allowed again after readers close") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    {
        auto snapshot = registry.snapshot();
        REQUIRE_THROWS_AS(registry.create(), std::logic_error);
    }
    REQUIRE(registry.create() != ecs::null_entity);

    {
        auto tx = registry.transaction();
        REQUIRE_THROWS_AS(registry.remove<Position>(entity), std::logic_error);
        tx.rollback();
    }
    REQUIRE(registry.remove<Position>(entity));
}
