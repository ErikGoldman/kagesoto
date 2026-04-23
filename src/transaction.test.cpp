#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>
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

ecs::Registry make_mvcc_registry() {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::mvcc);
    registry.set_storage_mode<Velocity>(ecs::ComponentStorageMode::mvcc);
    return registry;
}

}  // namespace

TEST_CASE("transactions stage copy-on-write updates and commit them atomically per component") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        Position* created = tx.write<Position>(entity, Position{1, 2});
        REQUIRE(created != nullptr);
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
    const Position* original = tx.try_get<Position>(entity);
    REQUIRE(original != nullptr);
    REQUIRE(original->x == 1);

    Position* staged = tx.write<Position>(entity);
    REQUIRE(staged != nullptr);
    REQUIRE(staged != original);
    staged->x = 9;
    staged->y = 8;

    REQUIRE(tx.get<Position>(entity).x == 9);
    REQUIRE(tx.get<Position>(entity).y == 8);

    tx.commit();

    auto read_tx = registry.transaction<Position, Velocity>();
    const Position* committed = read_tx.try_get<Position>(entity);
    REQUIRE(committed != nullptr);
    REQUIRE(committed->x == 9);
    REQUIRE(committed->y == 8);
}

TEST_CASE("typed transactions enforce declared read and write component access at compile time") {
    using ReadOnlyTx = ecs::Transaction<const Position>;
    using MixedTx = ecs::Transaction<Position, const Velocity>;

    static_assert(can_try_get<ReadOnlyTx, Position>::value);
    static_assert(!can_write<ReadOnlyTx, Position>::value);

    static_assert(can_try_get<MixedTx, Position>::value);
    static_assert(can_try_get<MixedTx, Velocity>::value);
    static_assert(can_write<MixedTx, Position>::value);
    static_assert(!can_write<MixedTx, Velocity>::value);
}

TEST_CASE("transactions roll back staged writes when not committed") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = 4;
        staged->y = 5;
        REQUIRE(tx.get<Position>(entity).x == 4);
    }

    auto read_tx = registry.transaction<Position, Velocity>();
    REQUIRE_FALSE(read_tx.has<Position>(entity));
}

TEST_CASE("transactions read back changed staged data before commit") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
    Position* staged = tx.write<Position>(entity);
    REQUIRE(staged != nullptr);
    staged->x = 41;
    staged->y = 99;

    const Position* observed = tx.try_get<Position>(entity);
    REQUIRE(observed != nullptr);
    REQUIRE(observed == staged);
    REQUIRE(observed->x == 41);
    REQUIRE(observed->y == 99);
    REQUIRE(tx.get<Position>(entity).x == 41);
    REQUIRE(tx.get<Position>(entity).y == 99);

    Position* reopened = tx.write<Position>(entity);
    REQUIRE(reopened == staged);
    REQUIRE(reopened->x == 41);
    REQUIRE(reopened->y == 99);
}

TEST_CASE("transactions discard changed staged data on rollback and preserve the committed value") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{7, 3});
        tx.commit();
    }

    {
        auto tx = registry.transaction<Position, Velocity>();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = -5;
        staged->y = 12;

        REQUIRE(tx.get<Position>(entity).x == -5);
        REQUIRE(tx.get<Position>(entity).y == 12);
    }

    auto read_tx = registry.transaction<Position, Velocity>();
    REQUIRE(read_tx.get<Position>(entity).x == 7);
    REQUIRE(read_tx.get<Position>(entity).y == 3);
}

TEST_CASE("transactions can stage a committed value even when revision value storage reallocates") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    for (int i = 0; i < 64; ++i) {
        auto tx = registry.transaction<Position, Velocity>();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x += 1;
        staged->y += 2;
        tx.commit();
    }

    auto read_tx = registry.transaction<Position, Velocity>();
    const Position* committed = read_tx.try_get<Position>(entity);
    REQUIRE(committed != nullptr);
    REQUIRE(committed->x == 65);
    REQUIRE(committed->y == 130);
}

TEST_CASE("transactions reuse the same staged object for multiple writes to one entity component pair") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    auto tx = registry.transaction<Position, Velocity>();

    Position* first = tx.write<Position>(entity, Position{1, 2});
    REQUIRE(first != nullptr);
    REQUIRE(first->x == 1);
    REQUIRE(first->y == 2);

    Position* second = tx.write<Position>(entity);
    REQUIRE(second == first);
    second->x = 7;
    second->y = 8;

    Position* third = tx.write<Position>(entity, Position{9, 10});
    REQUIRE(third == first);
    REQUIRE(third->x == 9);
    REQUIRE(third->y == 10);
    REQUIRE(tx.get<Position>(entity).x == 9);
    REQUIRE(tx.get<Position>(entity).y == 10);

    tx.commit();

    auto read_tx = registry.transaction<Position, Velocity>();
    REQUIRE(read_tx.get<Position>(entity).x == 9);
    REQUIRE(read_tx.get<Position>(entity).y == 10);
}

TEST_CASE("classic component storage writes in place and commits without MVCC copies") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::classic);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
    const Position* original = tx.try_get<Position>(entity);
    REQUIRE(original != nullptr);

    Position* staged = tx.write<Position>(entity);
    REQUIRE(staged != nullptr);
    REQUIRE(staged == original);
    staged->x = 9;
    staged->y = 8;
    tx.commit();

    auto read_tx = registry.transaction<Position, Velocity>();
    const Position* committed = read_tx.try_get<Position>(entity);
    REQUIRE(committed != nullptr);
    REQUIRE(committed->x == 9);
    REQUIRE(committed->y == 8);
}

TEST_CASE("classic component storage throws on explicit rollback and leaves visible state intact") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::classic);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{7, 3});
        tx.commit();
    }

    {
        auto tx = registry.transaction<Position, Velocity>();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = -5;
        staged->y = 12;
        REQUIRE_THROWS_AS(tx.rollback(), std::logic_error);
    }

    auto read_tx = registry.transaction<Position, Velocity>();
    REQUIRE(read_tx.get<Position>(entity).x == -5);
    REQUIRE(read_tx.get<Position>(entity).y == 12);
}

TEST_CASE("classic component storage persists writes when a transaction closes without commit") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::classic);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        Position* staged = tx.write<Position>(entity, Position{4, 6});
        REQUIRE(staged != nullptr);
    }

    auto read_tx = registry.transaction<Position, Velocity>();
    REQUIRE(read_tx.get<Position>(entity).x == 4);
    REQUIRE(read_tx.get<Position>(entity).y == 6);
}

TEST_CASE("trace component storage records timestamped history and can roll back to an earlier timestamp") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace);
    registry.set_trace_max_history(16);
    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    registry.set_current_trace_time(3);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{7, 8});
        tx.commit();
    }

    registry.set_current_trace_time(5);
    REQUIRE(registry.remove<Position>(entity));

    struct RecordedChange {
        std::uint32_t timestamp;
        std::uint16_t writer_id;
        bool tombstone;
        int x;
        int y;
    };

    std::vector<RecordedChange> changes;
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo info, const Position* value) {
        changes.push_back(RecordedChange{
            info.timestamp,
            info.writer_id,
            info.tombstone,
            value != nullptr ? value->x : 0,
            value != nullptr ? value->y : 0,
        });
    });

    REQUIRE(changes.size() == 3);
    REQUIRE(changes[0].timestamp == 1);
    REQUIRE(changes[0].writer_id == 0);
    REQUIRE_FALSE(changes[0].tombstone);
    REQUIRE(changes[0].x == 1);
    REQUIRE(changes[0].y == 2);
    REQUIRE(changes[1].timestamp == 3);
    REQUIRE_FALSE(changes[1].tombstone);
    REQUIRE(changes[1].x == 7);
    REQUIRE(changes[1].y == 8);
    REQUIRE(changes[2].timestamp == 5);
    REQUIRE(changes[2].tombstone);

    registry.set_current_trace_time(6);
    REQUIRE(registry.rollback_to_timestamp<Position>(entity, 3));

    auto read_tx = registry.transaction<Position>();
    REQUIRE(read_tx.get<Position>(entity).x == 7);
    REQUIRE(read_tx.get<Position>(entity).y == 8);

    changes.clear();
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo info, const Position* value) {
        changes.push_back(RecordedChange{
            info.timestamp,
            info.writer_id,
            info.tombstone,
            value != nullptr ? value->x : 0,
            value != nullptr ? value->y : 0,
        });
    });

    REQUIRE(changes.size() == 4);
    REQUIRE(changes.back().timestamp == 6);
    REQUIRE_FALSE(changes.back().tombstone);
    REQUIRE(changes.back().x == 7);
    REQUIRE(changes.back().y == 8);
}

TEST_CASE("trace component storage compacts history to the configured timestamp window") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace);
    registry.set_trace_max_history(2);
    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{1, 1});
        tx.commit();
    }

    registry.set_current_trace_time(2);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{2, 2});
        tx.commit();
    }

    registry.set_current_trace_time(4);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{4, 4});
        tx.commit();
    }

    std::vector<std::uint32_t> timestamps;
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo info, const Position*) {
        timestamps.push_back(info.timestamp);
    });

    REQUIRE(timestamps.size() == 2);
    REQUIRE(timestamps[0] == 2);
    REQUIRE(timestamps[1] == 4);

    registry.set_current_trace_time(5);
    REQUIRE(registry.rollback_to_timestamp<Position>(entity, 2));

    auto read_tx = registry.transaction<Position>();
    REQUIRE(read_tx.get<Position>(entity).x == 2);
    REQUIRE(read_tx.get<Position>(entity).y == 2);
}

TEST_CASE("trace rollback before the first change appends a tombstone and removes the current value") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace);
    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(3);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{3, 30});
        tx.commit();
    }

    registry.set_current_trace_time(4);
    REQUIRE(registry.rollback_to_timestamp<Position>(entity, 1));

    auto tx = registry.transaction<Position>();
    REQUIRE_FALSE(tx.has<Position>(entity));

    std::vector<ecs::TraceChangeInfo> changes;
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo info, const Position*) {
        changes.push_back(info);
    });

    REQUIRE(changes.size() == 2);
    REQUIRE(changes[0].timestamp == 3);
    REQUIRE_FALSE(changes[0].tombstone);
    REQUIRE(changes[1].timestamp == 4);
    REQUIRE(changes[1].tombstone);
}

TEST_CASE("trace rollback to a tombstone timestamp keeps the component absent") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace);
    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{1, 10});
        tx.commit();
    }

    registry.set_current_trace_time(2);
    REQUIRE(registry.remove<Position>(entity));

    registry.set_current_trace_time(3);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{3, 30});
        tx.commit();
    }

    registry.set_current_trace_time(4);
    REQUIRE(registry.rollback_to_timestamp<Position>(entity, 2));

    auto tx = registry.transaction<Position>();
    REQUIRE_FALSE(tx.has<Position>(entity));

    std::vector<ecs::TraceChangeInfo> changes;
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo info, const Position*) {
        changes.push_back(info);
    });

    REQUIRE(changes.size() == 4);
    REQUIRE(changes.back().timestamp == 4);
    REQUIRE(changes.back().tombstone);
}

TEST_CASE("trace configuration rejects out of range and non monotonic timestamps") {
    ecs::Registry registry(4);

    REQUIRE_THROWS_AS(
        registry.set_trace_max_history(ecs::RawPagedSparseArray::max_trace_time() + 1u),
        std::out_of_range);

    registry.set_current_trace_time(7);
    REQUIRE(registry.current_trace_time() == 7);

    REQUIRE_THROWS_AS(
        registry.set_current_trace_time(ecs::RawPagedSparseArray::max_trace_time() + 1u),
        std::out_of_range);
    REQUIRE_THROWS_AS(registry.set_current_trace_time(6), std::logic_error);
}

TEST_CASE("trace operations require all readers to be closed") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace);
    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    auto snapshot = registry.snapshot();
    REQUIRE_THROWS_AS(registry.set_current_trace_time(2), std::logic_error);
    REQUIRE_THROWS_AS(registry.rollback_to_timestamp<Position>(entity, 1), std::logic_error);
}

TEST_CASE("trace compaction and rollback work across multiple entities") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace);
    registry.set_trace_max_history(2);
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(second, Position{1, 20});
        tx.commit();
    }

    registry.set_current_trace_time(3);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(first, Position{3, 30});
        tx.commit();
    }

    registry.set_current_trace_time(4);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(first, Position{4, 40});
        tx.write<Position>(second, Position{4, 40});
        tx.commit();
    }

    auto read_tx = registry.transaction<Position>();
    REQUIRE(read_tx.get<Position>(first).x == 4);
    REQUIRE(read_tx.get<Position>(second).x == 4);
    read_tx.commit();

    std::vector<std::uint32_t> first_timestamps;
    registry.each_trace_change<Position>(first, [&](ecs::TraceChangeInfo info, const Position*) {
        first_timestamps.push_back(info.timestamp);
    });
    std::vector<std::uint32_t> second_timestamps;
    registry.each_trace_change<Position>(second, [&](ecs::TraceChangeInfo info, const Position*) {
        second_timestamps.push_back(info.timestamp);
    });

    REQUIRE(first_timestamps == std::vector<std::uint32_t>{3, 4});
    REQUIRE(second_timestamps == std::vector<std::uint32_t>{1, 4});

    registry.set_current_trace_time(5);
    REQUIRE(registry.rollback_to_timestamp<Position>(first, 3));
    REQUIRE(registry.rollback_to_timestamp<Position>(second, 4) == false);

    auto rolled_back = registry.transaction<Position>();
    REQUIRE(rolled_back.get<Position>(first).x == 3);
    REQUIRE(rolled_back.get<Position>(second).x == 4);
}

TEST_CASE("trace history survives tombstone rollback and row reuse") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::trace);
    const ecs::Entity entity = registry.create();

    registry.set_current_trace_time(1);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{1, 1});
        tx.commit();
    }

    registry.set_current_trace_time(2);
    REQUIRE(registry.remove<Position>(entity));

    registry.set_current_trace_time(3);
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{3, 3});
        tx.commit();
    }

    std::vector<std::uint32_t> timestamps;
    std::vector<bool> tombstones;
    registry.each_trace_change<Position>(entity, [&](ecs::TraceChangeInfo info, const Position*) {
        timestamps.push_back(info.timestamp);
        tombstones.push_back(info.tombstone);
    });

    REQUIRE(timestamps == std::vector<std::uint32_t>{1, 2, 3});
    REQUIRE(tombstones == std::vector<bool>{false, true, false});

    auto tx = registry.transaction<Position>();
    REQUIRE(tx.get<Position>(entity).x == 3);
    REQUIRE(tx.get<Position>(entity).y == 3);
}

TEST_CASE("classic component storage rejects concurrent snapshots and transactions") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::classic);

    const ecs::Entity entity = registry.create();
    {
        auto tx = registry.transaction<Position>();
        tx.write<Position>(entity, Position{1, 2});

        REQUIRE_THROWS_AS(registry.snapshot(), std::logic_error);
        REQUIRE_THROWS_AS(registry.transaction<Position>(), std::logic_error);

        bool rollback_threw = false;
        try {
            tx.rollback();
        } catch (const std::logic_error& error) {
            rollback_threw = true;
            REQUIRE(std::string(error.what()).find("does not support transaction rollback") != std::string::npos);
        }
        REQUIRE(rollback_threw);

        REQUIRE_THROWS_AS(registry.snapshot(), std::logic_error);
    }

    auto snapshot = registry.snapshot();
    REQUIRE_THROWS_AS(registry.transaction<Position>(), std::logic_error);
}

TEST_CASE("typed classic transactions use per-component reader and writer locks") {
    ecs::Registry registry(4);
    registry.set_storage_mode<Position>(ecs::ComponentStorageMode::classic);
    registry.set_storage_mode<Velocity>(ecs::ComponentStorageMode::classic);

    auto position_reader = registry.transaction<const Position>();
    auto second_position_reader = registry.transaction<const Position>();
    auto velocity_writer = registry.transaction<Velocity>();

    REQUIRE_THROWS_AS(registry.transaction<Position>(), std::logic_error);
    REQUIRE_THROWS_AS(registry.transaction<const Velocity>(), std::logic_error);

    velocity_writer.commit();
    second_position_reader.commit();
    position_reader.commit();

    auto position_writer = registry.transaction<Position>();
    REQUIRE_THROWS_AS(registry.transaction<const Position>(), std::logic_error);
    position_writer.commit();
}

TEST_CASE("concurrent transactions are isolated from each others uncommitted writes") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto seed = registry.transaction<Position, Velocity>();
        seed.write<Position>(entity, Position{1, 2});
        seed.commit();
    }

    auto first = registry.transaction<Position, Velocity>();
    auto second = registry.transaction<Position, Velocity>();

    Position* first_write = first.write<Position>(entity);
    REQUIRE(first_write != nullptr);
    first_write->x = 10;
    first_write->y = 20;

    REQUIRE(first.get<Position>(entity).x == 10);
    REQUIRE(first.get<Position>(entity).y == 20);
    REQUIRE(second.get<Position>(entity).x == 1);
    REQUIRE(second.get<Position>(entity).y == 2);

    Position* second_write = second.write<Position>(entity);
    REQUIRE(second_write != nullptr);
    second_write->x = 30;
    second_write->y = 40;

    REQUIRE(first.get<Position>(entity).x == 10);
    REQUIRE(second.get<Position>(entity).x == 30);
}

TEST_CASE("transactions can commit repeated updates after revision overflow storage reallocates") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{0, 0});
        tx.commit();
    }

    for (int i = 1; i <= 64; ++i) {
        auto tx = registry.transaction<Position, Velocity>();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = i;
        staged->y = i * 10;
        tx.commit();
    }

    auto read_tx = registry.transaction<Position, Velocity>();
    const Position* committed = read_tx.try_get<Position>(entity);
    REQUIRE(committed != nullptr);
    REQUIRE(committed->x == 64);
    REQUIRE(committed->y == 640);
}

TEST_CASE("later transactions only observe committed state from earlier transactions") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{1, 2});
        tx.commit();
    }

    {
        auto tx = registry.transaction<Position, Velocity>();
        Position* staged = tx.write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = 50;
        staged->y = 60;
    }

    auto next_tx = registry.transaction<Position, Velocity>();
    REQUIRE(next_tx.get<Position>(entity).x == 1);
    REQUIRE(next_tx.get<Position>(entity).y == 2);

    Position* committed = next_tx.write<Position>(entity);
    REQUIRE(committed != nullptr);
    committed->x = 70;
    committed->y = 80;
    next_tx.commit();

    auto final_tx = registry.transaction<Position, Velocity>();
    REQUIRE(final_tx.get<Position>(entity).x == 70);
    REQUIRE(final_tx.get<Position>(entity).y == 80);
}

TEST_CASE("snapshots keep their original visibility after later commits") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{5, 6});
        tx.commit();
    }

    auto snapshot = registry.snapshot();
    auto writer = registry.transaction<Position, Velocity>();
    Position* staged = writer.write<Position>(entity);
    REQUIRE(staged != nullptr);
    staged->x = 50;
    staged->y = 60;
    writer.commit();

    REQUIRE(snapshot.get<Position>(entity).x == 5);
    REQUIRE(snapshot.get<Position>(entity).y == 6);

    auto current = registry.transaction<Position, Velocity>();
    REQUIRE(current.get<Position>(entity).x == 50);
    REQUIRE(current.get<Position>(entity).y == 60);
}

TEST_CASE("snapshots exclude transactions that were active when the snapshot opened") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto seed = registry.transaction<Position, Velocity>();
        seed.write<Position>(entity, Position{1, 2});
        seed.commit();
    }

    auto writer = registry.transaction<Position, Velocity>();
    Position* staged = writer.write<Position>(entity);
    REQUIRE(staged != nullptr);
    staged->x = 10;
    staged->y = 20;

    auto snapshot = registry.snapshot();
    writer.commit();

    REQUIRE(snapshot.get<Position>(entity).x == 1);
    REQUIRE(snapshot.get<Position>(entity).y == 2);

    auto current = registry.transaction<Position, Velocity>();
    REQUIRE(current.get<Position>(entity).x == 10);
    REQUIRE(current.get<Position>(entity).y == 20);
}

TEST_CASE("later commit wins even when its transaction tsn is older") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto seed = registry.transaction<Position, Velocity>();
        seed.write<Position>(entity, Position{1, 2});
        seed.commit();
    }

    auto older = registry.transaction<Position, Velocity>();
    auto newer = registry.transaction<Position, Velocity>();

    Position* older_value = older.write<Position>(entity);
    REQUIRE(older_value != nullptr);
    older_value->x = 20;
    older_value->y = 21;

    Position* newer_value = newer.write<Position>(entity);
    REQUIRE(newer_value != nullptr);
    newer_value->x = 30;
    newer_value->y = 31;
    newer.commit();

    auto middle_snapshot = registry.snapshot();
    older.commit();

    REQUIRE(middle_snapshot.get<Position>(entity).x == 30);
    REQUIRE(middle_snapshot.get<Position>(entity).y == 31);

    auto current = registry.transaction<Position, Velocity>();
    REQUIRE(current.get<Position>(entity).x == 20);
    REQUIRE(current.get<Position>(entity).y == 21);
}

TEST_CASE("revision chains remain readable after overflowing inline and overflow storage") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{0, 0});
        tx.commit();
    }

    auto early_snapshot = registry.snapshot();

    for (int i = 1; i <= 8; ++i) {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{i, i * 10});
        tx.commit();
    }

    REQUIRE(early_snapshot.get<Position>(entity).x == 0);
    REQUIRE(early_snapshot.get<Position>(entity).y == 0);

    auto current = registry.transaction<Position, Velocity>();
    REQUIRE(current.get<Position>(entity).x == 8);
    REQUIRE(current.get<Position>(entity).y == 80);
}

TEST_CASE("transactions become unusable after commit or rollback") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    auto committed = registry.transaction<Position, Velocity>();
    committed.write<Position>(entity, Position{1, 2});
    committed.commit();
    REQUIRE_THROWS_AS(committed.get<Position>(entity), std::logic_error);
    REQUIRE_THROWS_AS(committed.write<Position>(entity), std::logic_error);

    auto rolled_back = registry.transaction<Position, Velocity>();
    rolled_back.write<Position>(entity, Position{3, 4});
    rolled_back.rollback();
    REQUIRE_THROWS_AS(rolled_back.get<Position>(entity), std::logic_error);
    REQUIRE_THROWS_AS(rolled_back.write<Position>(entity), std::logic_error);
}

TEST_CASE("empty transactions release registry guards on commit and rollback") {
    ecs::Registry registry(4);

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.commit();
    }

    const ecs::Entity created_after_commit = registry.create();
    REQUIRE(created_after_commit != ecs::null_entity);

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.rollback();
    }

    const ecs::Entity created_after_rollback = registry.create();
    REQUIRE(created_after_rollback != ecs::null_entity);
}

TEST_CASE("many concurrent isolated revisions remain readable through overflow chains") {
    auto registry = make_mvcc_registry();
    const ecs::Entity entity = registry.create();

    {
        auto seed = registry.transaction<Position, Velocity>();
        seed.write<Position>(entity, Position{0, 0});
        seed.commit();
    }

    std::array<std::unique_ptr<ecs::Transaction<Position, Velocity>>, 9> transactions{};
    for (std::size_t i = 0; i < transactions.size(); ++i) {
        transactions[i] = std::make_unique<ecs::Transaction<Position, Velocity>>(registry);
        Position* staged = transactions[i]->write<Position>(entity);
        REQUIRE(staged != nullptr);
        staged->x = static_cast<int>(i + 1);
        staged->y = static_cast<int>((i + 1) * 100);
    }

    auto snapshot = registry.snapshot();
    REQUIRE(snapshot.get<Position>(entity).x == 0);
    REQUIRE(snapshot.get<Position>(entity).y == 0);

    for (std::size_t i = 0; i < transactions.size(); ++i) {
        REQUIRE(transactions[i]->get<Position>(entity).x == static_cast<int>(i + 1));
        REQUIRE(transactions[i]->get<Position>(entity).y == static_cast<int>((i + 1) * 100));
    }

    for (std::size_t i = 0; i < transactions.size(); ++i) {
        transactions[i]->commit();
    }

    auto current = registry.transaction<Position, Velocity>();
    REQUIRE(current.get<Position>(entity).x == 9);
    REQUIRE(current.get<Position>(entity).y == 900);
}

TEST_CASE("transaction read APIs are const-only") {
    ecs::Registry registry(4);
    const ecs::Entity entity = registry.create();

    {
        auto tx = registry.transaction<Position, Velocity>();
        tx.write<Position>(entity, Position{7, 3});
        tx.commit();
    }

    auto tx = registry.transaction<Position, Velocity>();
    static_assert(std::is_same_v<decltype(tx.try_get<Position>(entity)), const Position*>);
    static_assert(std::is_same_v<decltype(tx.get<Position>(entity)), const Position&>);
    static_assert(std::is_same_v<decltype(tx.storage<Position>().get(entity)), const Position&>);
}
