#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
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

struct PreallocatedPosition {
    int x;
    int y;
};

struct GameClock {
    int tick = 123;
};

template <typename TransactionType, typename Component, typename = void>
struct can_try_get : std::false_type {};

template <typename TransactionType, typename Component>
struct can_try_get<TransactionType,
                   Component,
                   std::void_t<decltype(std::declval<TransactionType&>().template try_get<Component>(ecs::null_entity))>>
    : std::true_type {};

template <typename TransactionType, typename Component, typename = void>
struct can_write : std::false_type {};

template <typename TransactionType, typename Component>
struct can_write<TransactionType,
                 Component,
                 std::void_t<decltype(std::declval<TransactionType&>().template write<Component>(ecs::null_entity))>>
    : std::true_type {};

}  // namespace

namespace ecs {

template <>
struct ComponentSingletonTraits<GameClock> {
    static constexpr bool value = true;
};

template <>
struct ComponentTraceStorageTraits<PreallocatedPosition> {
    static constexpr ComponentTraceStorage value = ComponentTraceStorage::preallocated;
};

}  // namespace ecs

TEST_CASE("transactions write directly and commit visible state") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.write<Velocity>(entity, Velocity{3, 4});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
    REQUIRE(tx.get<Position>(entity).x == 1);
    REQUIRE(tx.get<Velocity>(entity).dy == 4);
}

TEST_CASE("direct transactions persist writes when closed without explicit commit") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{5, 6});
    }

    auto tx = registry.transaction<Position>();
    REQUIRE(tx.get<Position>(entity).x == 5);
}

TEST_CASE("direct transactions reject rollback with pending writes") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    auto tx = registry.transaction<Position>();
    tx.write<Position>(entity, Position{1, 2});
    REQUIRE_THROWS_AS(tx.rollback(), std::logic_error);
}

TEST_CASE("typed transactions enforce declared read and write access") {
    using Tx = ecs::Transaction<Position, const Velocity>;
    static_assert(can_try_get<Tx, Position>::value);
    static_assert(can_try_get<Tx, Velocity>::value);
    static_assert(can_write<Tx, Position>::value);
    static_assert(!can_write<Tx, Velocity>::value);
}

TEST_CASE("direct access guards reject overlapping snapshots and writers") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto writer = registry.transaction<Position>();
        writer.write<Position>(entity, Position{1, 2});
        REQUIRE_THROWS_AS(registry.snapshot(), std::logic_error);
    }

    auto snapshot = registry.snapshot();
    REQUIRE_THROWS_AS(registry.transaction<Position>(), std::logic_error);
}

TEST_CASE("copy-on-write tracing records baseline, writes, removals, and rollback") {
    ecs::Registry registry(4);
    registry.set_trace_max_history(8);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    registry.set_current_trace_time(1);
    registry.set_tracing_enabled(true);

    registry.set_current_trace_time(2);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{3, 4});
        tx.commit();
    }

    registry.set_current_trace_time(3);
    REQUIRE(registry.remove<Position>(entity));

    std::vector<ecs::TraceChangeInfo> changes;
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo info, const Position*) {
        changes.push_back(info);
    });
    REQUIRE(changes.size() == 3);
    REQUIRE(changes[0].timestamp == 1);
    REQUIRE_FALSE(changes[0].tombstone);
    REQUIRE(changes[2].timestamp == 3);
    REQUIRE(changes[2].tombstone);

    registry.set_current_trace_time(4);
    REQUIRE(registry.rollback_to_timestamp<Position>(entity, 1));

    auto tx = registry.transaction<Position>();
    REQUIRE(tx.get<Position>(entity).x == 1);
}

TEST_CASE("tracing disabled retains old history but records no new writes") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    registry.set_tracing_enabled(true);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    registry.set_tracing_enabled(false);
    registry.set_current_trace_time(2);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{3, 4});
        tx.commit();
    }

    std::vector<int> values;
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo, const Position* position) {
        if (position != nullptr) {
            values.push_back(position->x);
        }
    });
    REQUIRE(values == std::vector<int>{1});
}

TEST_CASE("preallocated tracing captures retained frames on trace time advance") {
    ecs::Registry registry(4);
    registry.set_trace_max_history(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<PreallocatedPosition>();
        tx.write<PreallocatedPosition>(entity, PreallocatedPosition{1, 2});
        tx.commit();
    }

    registry.set_current_trace_time(1);
    registry.set_tracing_enabled(true);
    registry.set_current_trace_time(2);

    {
        auto tx = registry.transaction<PreallocatedPosition>();
        tx.write<PreallocatedPosition>(entity, PreallocatedPosition{5, 6});
        tx.commit();
    }

    registry.set_current_trace_time(3);
    REQUIRE(registry.rollback_to_timestamp<PreallocatedPosition>(entity, 1));

    auto tx = registry.transaction<PreallocatedPosition>();
    REQUIRE(tx.get<PreallocatedPosition>(entity).x == 1);
}

TEST_CASE("singleton components use direct storage and trace history") {
    ecs::Registry registry(4);
    registry.set_tracing_enabled(true);
    registry.set_current_trace_time(1);

    {
        auto tx = registry.transaction<GameClock>();
        tx.write<GameClock>()->tick = 7;
        tx.commit();
    }

    registry.set_current_trace_time(2);
    {
        auto tx = registry.transaction<GameClock>();
        tx.write<GameClock>()->tick = 9;
        tx.commit();
    }

    REQUIRE(registry.rollback_to_timestamp<GameClock>(1));
    auto tx = registry.transaction<GameClock>();
    REQUIRE(tx.get<GameClock>().tick == 7);
}
