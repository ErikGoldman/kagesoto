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

    Position& position = registry.emplace<Position>(entity, Position{5, 11});
    REQUIRE(position.x == 5);
    REQUIRE(registry.has<Position>(entity));
    REQUIRE(registry.get<Position>(entity).y == 11);

    std::vector<ecs::Entity> seen;
    registry.storage<Position>().each([&seen](ecs::Entity current, const Position&) {
        seen.push_back(current);
    });
    REQUIRE(seen.size() == 1);
    REQUIRE(seen.front() == entity);

    REQUIRE(registry.remove<Position>(entity));
    REQUIRE_FALSE(registry.has<Position>(entity));

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
    registry.emplace<Position>(first, Position{1, 2});

    REQUIRE(registry.destroy(first));

    const ecs::Entity second = registry.create();
    registry.emplace<Position>(second, Position{3, 4});

    REQUIRE(ecs::entity_index(second) == ecs::entity_index(first));
    REQUIRE(ecs::entity_version(second) != ecs::entity_version(first));
    REQUIRE_FALSE(registry.alive(first));
    REQUIRE_FALSE(registry.has<Position>(first));
    REQUIRE(registry.try_get<Position>(first) == nullptr);
    REQUIRE(registry.get<Position>(second).x == 3);
}

TEST_CASE("registry supports component lookups by explicit component id") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();
    registry.emplace<Position>(entity, Position{7, 9});

    const ecs::ComponentId position_id = ecs::component_id<Position>();
    const ecs::ComponentId velocity_id = ecs::component_id<Velocity>();

    REQUIRE(registry.has(entity, position_id));
    REQUIRE_FALSE(registry.has(entity, velocity_id));
    REQUIRE(registry.storage(position_id) != nullptr);
    REQUIRE(registry.storage(velocity_id) == nullptr);

    Position* position = static_cast<Position*>(registry.try_get(entity, position_id));
    REQUIRE(position != nullptr);
    REQUIRE(position->x == 7);
    REQUIRE(registry.try_get(entity, velocity_id) == nullptr);

    REQUIRE(registry.remove(entity, position_id));
    REQUIRE_FALSE(registry.has(entity, position_id));
    REQUIRE_FALSE(registry.remove(entity, position_id));
}
