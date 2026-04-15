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

TEST_CASE("transaction views iterate matching entities with const component references") {
    ecs::Registry registry(4);

    const ecs::Entity mover = registry.create();
    const ecs::Entity stationary = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<Position>(mover, Position{10, 20});
        tx.write<Velocity>(mover, Velocity{3, 4});
        tx.write<Position>(stationary, Position{30, 40});
        tx.commit();
    }

    auto tx = registry.transaction();
    std::vector<ecs::Entity> seen;
    tx.view<Position, const Velocity>().forEach([&](ecs::Entity entity, auto& position, auto& velocity) {
        static_assert(std::is_same_v<decltype(position), const Position&>);
        static_assert(std::is_same_v<decltype(velocity), const Velocity&>);
        seen.push_back(entity);
    });

    REQUIRE(seen == std::vector<ecs::Entity>{mover});
    REQUIRE(tx.get<Position>(mover).x == 10);
    REQUIRE(tx.get<Position>(mover).y == 20);
    REQUIRE(tx.get<Position>(stationary).x == 30);
    REQUIRE(tx.get<Position>(stationary).y == 40);
}

TEST_CASE("transaction views include staged component writes before commit") {
    ecs::Registry registry(4);

    const ecs::Entity mover = registry.create();
    const ecs::Entity staged = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<Position>(mover, Position{1, 2});
        tx.write<Velocity>(mover, Velocity{3, 4});
        tx.write<Position>(staged, Position{5, 6});
        tx.commit();
    }

    auto tx = registry.transaction();
    tx.write<Velocity>(staged, Velocity{7, 8});

    std::vector<ecs::Entity> seen;
    tx.view<const Position, Velocity>().forEach([&](ecs::Entity entity, auto& position, auto& velocity) {
        static_assert(std::is_same_v<decltype(position), const Position&>);
        static_assert(std::is_same_v<decltype(velocity), const Velocity&>);
        REQUIRE(position.x + velocity.dx > 0);
        seen.push_back(entity);
    });

    REQUIRE(seen == std::vector<ecs::Entity>{mover, staged});
}

TEST_CASE("transaction views observe staged updates through the same transaction") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();
    {
        auto tx = registry.transaction();
        tx.write<Position>(entity, Position{1, 2});
        tx.write<Velocity>(entity, Velocity{3, 4});
        tx.commit();
    }

    auto tx = registry.transaction();
    Velocity* velocity = tx.write<Velocity>(entity);
    REQUIRE(velocity != nullptr);
    velocity->dx = 30;
    velocity->dy = 40;

    bool called = false;
    tx.view<const Position, const Velocity>().forEach([&](ecs::Entity current, auto& position, auto& updated_velocity) {
        called = true;
        REQUIRE(current == entity);
        REQUIRE(position.x == 1);
        REQUIRE(updated_velocity.dx == 30);
        REQUIRE(updated_velocity.dy == 40);
    });

    REQUIRE(called);
}
