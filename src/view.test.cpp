#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
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

struct GameClock {
    int tick = 7;
};

template <typename ViewType, typename = void>
struct can_where_singleton : std::false_type {};

template <typename ViewType>
struct can_where_singleton<ViewType,
                           std::void_t<decltype(std::declval<ViewType>().template where<&GameClock::tick>(
                               ecs::PredicateOperator::eq,
                               7))>>
    : std::true_type {};

}  // namespace

namespace ecs {

template <>
struct ComponentSingletonTraits<GameClock> {
    static constexpr bool value = true;
};

}  // namespace ecs

TEST_CASE("transaction views iterate matching entities with const component references") {
    ecs::Registry registry(4);

    const ecs::Entity mover = registry.create();
    const ecs::Entity stationary = registry.create();
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(mover, Position{10, 20});
        tx.write<Velocity>(mover, Velocity{3, 4});
        tx.write<Position>(stationary, Position{30, 40});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
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
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(mover, Position{1, 2});
        tx.write<Velocity>(mover, Velocity{3, 4});
        tx.write<Position>(staged, Position{5, 6});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
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
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.write<Velocity>(entity, Velocity{3, 4});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
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

TEST_CASE("transaction view explain reports the smallest visible component as the anchor scan") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity third = registry.create();
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(first, Position{1, 1});
        tx.write<Velocity>(first, Velocity{10, 10});
        tx.write<Position>(second, Position{2, 2});
        tx.write<Velocity>(second, Velocity{20, 20});
        tx.write<Position>(third, Position{3, 3});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
    const ecs::QueryExplain explain = tx.view<const Position, const Velocity>().explain();

    REQUIRE_FALSE(explain.empty);
    REQUIRE(explain.anchor_component == ecs::component_id<Velocity>());
    REQUIRE(explain.anchor_component_index == 1);
    REQUIRE(explain.candidate_rows == 2);
    REQUIRE(explain.estimated_entity_lookups == 2);
    REQUIRE(explain.steps.size() == 2);
    REQUIRE(explain.steps[0].component == ecs::component_id<Position>());
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::sparse_lookup);
    REQUIRE(explain.steps[0].visible_rows == 3);
    REQUIRE(explain.steps[1].component == ecs::component_id<Velocity>());
    REQUIRE(explain.steps[1].access == ecs::QueryAccessKind::anchor_scan);
    REQUIRE(explain.steps[1].visible_rows == 2);
}

TEST_CASE("transaction view explain accounts for staged writes and empty inputs") {
    ecs::Registry registry(4);

    const ecs::Entity entity = registry.create();
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{7, 8});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
    tx.write<Velocity>(entity, Velocity{9, 10});

    const ecs::QueryExplain explain = tx.view<const Position, const Velocity>().explain();
    REQUIRE_FALSE(explain.empty);
    REQUIRE(explain.anchor_component == ecs::component_id<Position>());
    REQUIRE(explain.candidate_rows == 1);
    REQUIRE(explain.steps[0].access == ecs::QueryAccessKind::anchor_scan);
    REQUIRE(explain.steps[1].access == ecs::QueryAccessKind::sparse_lookup);

    ecs::Registry empty_registry(4);
    const ecs::Entity other = empty_registry.create();
    auto empty_tx = empty_registry.transaction<Position, Velocity>();
    empty_tx.write<Position>(other, Position{1, 2});
    const ecs::QueryExplain empty_explain = empty_tx.view<const Position, const Velocity>().explain();
    REQUIRE(empty_explain.empty);
    REQUIRE(empty_explain.candidate_rows == 0);
    REQUIRE(empty_explain.estimated_entity_lookups == 0);
}

TEST_CASE("transaction view explain text describes scans and lack of component index usage") {
    ecs::Registry registry(4);

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(first, Position{1, 2});
        tx.write<Velocity>(first, Velocity{3, 4});
        tx.write<Velocity>(second, Velocity{5, 6});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
    const std::string explain = tx.view<const Position, const Velocity>().explain_text();

    REQUIRE(explain.find("EXPLAIN VIEW") != std::string::npos);
    REQUIRE(explain.find("Anchor: component[0] Position") != std::string::npos);
    REQUIRE(explain.find("via anchor scan") != std::string::npos);
    REQUIRE(explain.find("Entity probes: 1") != std::string::npos);
    REQUIRE(explain.find("component[1] Velocity: sparse entity probe") != std::string::npos);
    REQUIRE(explain.find("Component value indexes: not used by view execution") != std::string::npos);
}

TEST_CASE("transaction view explain text marks empty results") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    auto tx = registry.transaction<Position, Velocity>();
    tx.write<Position>(entity, Position{1, 2});

    const std::string explain = tx.view<const Position, const Velocity>().explain_text();
    REQUIRE(explain.find("Result: empty") != std::string::npos);
    REQUIRE(explain.find("component[0] Position") != std::string::npos);
    REQUIRE(explain.find("component[1] Velocity") != std::string::npos);
}

TEST_CASE("singleton-only views invoke once with null_entity") {
    ecs::Registry registry(4);
    auto tx = registry.transaction<GameClock>();

    int calls = 0;
    tx.view<const GameClock>().forEach([&](ecs::Entity entity, const GameClock& clock) {
        ++calls;
        REQUIRE(entity == ecs::null_entity);
        REQUIRE(clock.tick == 7);
    });

    REQUIRE(calls == 1);
}

TEST_CASE("mixed views pass singleton components alongside entity components") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, GameClock>();
        tx.write<Position>(entity, Position{3, 4});
        tx.write<GameClock>()->tick = 11;
        tx.commit();
    }

    auto tx = registry.transaction<Position, GameClock>();
    int calls = 0;
    tx.view<const Position, const GameClock>().forEach([&](ecs::Entity current, const Position& position, const GameClock& clock) {
        ++calls;
        REQUIRE(current == entity);
        REQUIRE(position.x == 3);
        REQUIRE(clock.tick == 11);
    });

    REQUIRE(calls == 1);
}

TEST_CASE("singleton-only view explain reports singleton reads without probes") {
    ecs::Registry registry(4);
    auto tx = registry.transaction<GameClock>();

    const ecs::QueryExplain explain = tx.view<const GameClock>().explain();
    REQUIRE_FALSE(explain.empty);
    REQUIRE(explain.anchor_component == ecs::null_component);
    REQUIRE(explain.candidate_rows == 1);
    REQUIRE(explain.estimated_entity_lookups == 0);
    REQUIRE(explain.steps.size() == 1);
    REQUIRE(explain.steps.front().access == ecs::QueryAccessKind::singleton_read);

    const std::string text = tx.view<const GameClock>().explain_text();
    REQUIRE(text.find("Anchor: singleton-only") != std::string::npos);
    REQUIRE(text.find("singleton read") != std::string::npos);
}

TEST_CASE("views do not allow predicates on singleton components") {
    using View = decltype(std::declval<ecs::Transaction<GameClock>&>().template view<const GameClock>());
    static_assert(!can_where_singleton<View>::value);
}
