#include "ecs_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

struct CountingPositionTraits {
    using Quantized = Position;
    static int serialize_calls;
    static int deserialize_calls;

    static Quantized quantize(const Position& value) {
        return value;
    }

    static Position dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* previous, const Quantized& current, ecs::BitBuffer& out) {
        ++serialize_calls;
        if (previous == nullptr) {
            out.push_bits(current.x, 32U);
            out.push_bits(current.y, 32U);
            return;
        }
        out.push_bits(current.x - previous->x, 16U);
        out.push_bits(current.y - previous->y, 16U);
    }

    static bool deserialize(ecs::BitBuffer& in, const Quantized* previous, Quantized& out) {
        ++deserialize_calls;
        if (previous == nullptr) {
            out.x = static_cast<int>(in.read_bits(32U));
            out.y = static_cast<int>(in.read_bits(32U));
            return true;
        }
        out.x = previous->x + static_cast<int>(in.read_bits(16U));
        out.y = previous->y + static_cast<int>(in.read_bits(16U));
        return true;
    }
};

int CountingPositionTraits::serialize_calls = 0;
int CountingPositionTraits::deserialize_calls = 0;

struct UnreadPositionTraits : CountingPositionTraits {
    static bool deserialize(ecs::BitBuffer&, const Quantized*, Quantized& out) {
        out = Position{};
        return true;
    }
};

}  // namespace

TEST_CASE("registry snapshots restore entities components metadata groups singletons and dirty bits") {
    ecs::Registry registry;
    const ecs::Entity position_component = registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    const ecs::Entity game_time_component = registry.register_component<GameTime>("GameTime");

    REQUIRE(registry.set_component_fields(
        position_component,
        {ecs::ComponentField{"x", offsetof(Position, x), registry.primitive_type(ecs::PrimitiveType::I32), 1}}));

    registry.declare_owned_group<Position, Velocity>();

    const ecs::Entity kept = registry.create();
    const ecs::Entity removed_before_snapshot = registry.create();
    REQUIRE(registry.add<Position>(kept, Position{1, 2}) != nullptr);
    REQUIRE(registry.add<Velocity>(kept, Velocity{3.0f, 4.0f}) != nullptr);
    REQUIRE(registry.clear_dirty<Position>(kept));
    registry.write<GameTime>().tick = 7;
    REQUIRE(registry.destroy(removed_before_snapshot));

    auto snapshot = registry.create_snapshot();

    const ecs::Entity reused_after_snapshot = registry.create();
    REQUIRE(ecs::Registry::entity_index(reused_after_snapshot) == ecs::Registry::entity_index(removed_before_snapshot));
    REQUIRE(registry.add<Position>(reused_after_snapshot, Position{9, 9}) != nullptr);
    REQUIRE(registry.remove<Velocity>(kept));
    registry.write<Position>(kept).x = 42;
    registry.write<GameTime>().tick = 99;
    registry.register_component<Health>("Health");

    registry.restore_snapshot(snapshot);

    REQUIRE(registry.alive(kept));
    REQUIRE_FALSE(registry.alive(reused_after_snapshot));
    REQUIRE(registry.get<Position>(kept).x == 1);
    REQUIRE(registry.get<Position>(kept).y == 2);
    REQUIRE(registry.get<Velocity>(kept).dx == 3.0f);
    REQUIRE(registry.get<Velocity>(kept).dy == 4.0f);
    REQUIRE_FALSE(registry.is_dirty<Position>(kept));
    REQUIRE(registry.is_dirty<Velocity>(kept));
    REQUIRE(registry.component<Position>() == position_component);
    REQUIRE(registry.component<GameTime>() == game_time_component);
    REQUIRE(registry.get<GameTime>().tick == 7);
    REQUIRE(registry.is_dirty<GameTime>());
    REQUIRE_THROWS_AS(registry.component<Health>(), std::logic_error);

    const std::vector<ecs::ComponentField>* fields = registry.component_fields(position_component);
    REQUIRE(fields != nullptr);
    REQUIRE(fields->size() == 1);
    REQUIRE((*fields)[0].name == "x");

    std::vector<ecs::Entity> grouped;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        grouped.push_back(entity);
    });
    REQUIRE(grouped == std::vector<ecs::Entity>{kept});

    const ecs::Entity reused_after_restore = registry.create();
    REQUIRE(reused_after_restore == reused_after_snapshot);
}

TEST_CASE("registry snapshots restore tag presence and dirty bits") {
    ecs::Registry registry;
    const ecs::Entity active_tag = registry.register_component<Active>("Active");
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Active>(entity));
    REQUIRE(registry.clear_dirty<Active>(entity));

    auto snapshot = registry.create_snapshot();

    REQUIRE(registry.remove<Active>(entity));
    REQUIRE_FALSE(registry.has<Active>(entity));

    registry.restore_snapshot(snapshot);

    REQUIRE(registry.component<Active>() == active_tag);
    REQUIRE(registry.has<Active>(entity));
    REQUIRE_FALSE(registry.is_dirty<Active>(entity));
}

TEST_CASE("registry snapshots deep copy copyable non-trivial component storage") {
    ecs::Registry registry;
    registry.register_component<CopyableName>("CopyableName");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<CopyableName>(entity, CopyableName{"before"}) != nullptr);

    auto snapshot = registry.create_snapshot();
    registry.write<CopyableName>(entity).value = "after";

    registry.restore_snapshot(snapshot);

    REQUIRE(registry.get<CopyableName>(entity).value == "before");
    registry.write<CopyableName>(entity).value = "restored";
    registry.restore_snapshot(snapshot);
    REQUIRE(registry.get<CopyableName>(entity).value == "before");
}

TEST_CASE("registry snapshots reject move-only non-trivial component storage") {
    ecs::Registry registry;
    registry.register_component<std::unique_ptr<int>>("OwnedInt");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<std::unique_ptr<int>>(entity, new int(5)) != nullptr);

    REQUIRE_THROWS_AS(registry.create_snapshot(), std::logic_error);
}

TEST_CASE("restoring a registry snapshot leaves registered jobs unchanged") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    registry.job<Position>(0).each([&](ecs::Registry::View<Position>& view, ecs::Entity current, Position&) {
        ++calls;
        view.write<Position>(current).x += 1;
    });

    auto snapshot = registry.create_snapshot();
    REQUIRE(registry.add<Position>(entity, Position{10, 0}) != nullptr);

    registry.restore_snapshot(snapshot);
    registry.run_jobs();

    REQUIRE(calls == 1);
    REQUIRE(registry.get<Position>(entity).x == 2);
}

TEST_CASE("registry snapshots exclude system-tagged job bookkeeping entities") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    const ecs::Entity job = registry.job<Position>(0).each([&](ecs::Entity, Position&) {
        ++calls;
    });
    REQUIRE(registry.add<Position>(job, Position{99, 0}) != nullptr);

    auto snapshot = registry.create_snapshot();

    (void)registry.write<Position>(entity);
    registry.write<Position>(entity).x = 7;
    registry.write<Position>(job).x = 100;

    registry.restore_snapshot(snapshot);

    REQUIRE(registry.alive(job));
    REQUIRE(registry.has(job, registry.system_tag()));
    REQUIRE(registry.get<Position>(entity).x == 1);
    REQUIRE_FALSE(registry.contains<Position>(job));

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();
    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{job});

    registry.run_jobs();
    REQUIRE(calls == 1);
}

TEST_CASE("registry full snapshots write read and restore from in-memory snapshot native format") {
    ecs::Registry source;
    const ecs::Entity position_component = source.register_component<Position>("Position");
    source.register_component<Velocity>("Velocity");
    source.register_component<Active>("Active");
    source.register_component<GameTime>("GameTime");
    REQUIRE(source.set_component_fields(
        position_component,
        {ecs::ComponentField{"x", offsetof(Position, x), source.primitive_type(ecs::PrimitiveType::I32), 1}}));

    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{3, 4}) != nullptr);
    REQUIRE(source.add<Velocity>(entity, Velocity{1.5f, 2.5f}) != nullptr);
    REQUIRE(source.add<Active>(entity));
    REQUIRE(source.clear_dirty<Position>(entity));
    source.write<GameTime>().tick = 99;

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    source.create_snapshot().write(stream);

    ecs::Registry::Snapshot loaded = ecs::Registry::Snapshot::read(stream);
    ecs::Registry restored;
    restored.restore_snapshot(loaded);

    REQUIRE(restored.alive(entity));
    REQUIRE(restored.component<Position>() == position_component);
    REQUIRE(restored.get<Position>(entity).x == 3);
    REQUIRE(restored.get<Position>(entity).y == 4);
    REQUIRE(restored.get<Velocity>(entity).dx == 1.5f);
    REQUIRE(restored.has<Active>(entity));
    REQUIRE_FALSE(restored.is_dirty<Position>(entity));
    REQUIRE(restored.is_dirty<Velocity>(entity));
    REQUIRE(restored.get<GameTime>().tick == 99);
    REQUIRE(restored.is_dirty<GameTime>());
    REQUIRE(restored.component_fields(position_component)->size() == 1);
}

TEST_CASE("registry delta snapshots write read and restore from in-memory snapshot native format") {
    ecs::Registry source;
    source.register_component<Position>("Position");
    source.register_component<Velocity>("Velocity");
    source.register_component<Active>("Active");

    const ecs::Entity updated = source.create();
    const ecs::Entity removed_component = source.create();
    const ecs::Entity destroyed = source.create();
    REQUIRE(source.add<Position>(updated, Position{1, 1}) != nullptr);
    REQUIRE(source.add<Velocity>(updated, Velocity{1.0f, 1.0f}) != nullptr);
    REQUIRE(source.add<Position>(removed_component, Position{2, 2}) != nullptr);
    REQUIRE(source.add<Velocity>(removed_component, Velocity{2.0f, 2.0f}) != nullptr);
    REQUIRE(source.add<Position>(destroyed, Position{3, 3}) != nullptr);
    REQUIRE(source.add<Active>(destroyed));
    source.clear_all_dirty<Position>();
    source.clear_all_dirty<Velocity>();
    source.clear_all_dirty<Active>();

    auto baseline = source.create_snapshot();
    ecs::Registry replay;
    replay.register_component<Position>("Position");
    replay.register_component<Velocity>("Velocity");
    replay.register_component<Active>("Active");
    replay.restore_snapshot(baseline);

    source.write<Position>(updated).x = 10;
    REQUIRE(source.remove<Velocity>(removed_component));
    REQUIRE(source.destroy(destroyed));

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    source.create_delta_snapshot(baseline).write(stream);

    ecs::Registry::DeltaSnapshot loaded = ecs::Registry::DeltaSnapshot::read(stream);
    replay.restore_delta_snapshot(loaded);

    REQUIRE(replay.get<Position>(updated).x == 10);
    REQUIRE(replay.get<Position>(updated).y == 1);
    REQUIRE_FALSE(replay.contains<Velocity>(removed_component));
    REQUIRE_FALSE(replay.alive(destroyed));
    REQUIRE_FALSE(replay.contains<Position>(destroyed));
    REQUIRE_FALSE(replay.has<Active>(destroyed));
    REQUIRE(replay.is_dirty<Position>(updated));
    REQUIRE(replay.is_dirty<Velocity>(removed_component));
}

TEST_CASE("registry in-memory snapshot native format component filters include and exclude storage") {
    ecs::Registry source;
    source.register_component<Position>("Position");
    source.register_component<Velocity>("Velocity");

    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{8, 9}) != nullptr);
    REQUIRE(source.add<Velocity>(entity, Velocity{3.0f, 4.0f}) != nullptr);

    ecs::SnapshotIoOptions options;
    options.include_components.push_back(source.component<Position>());
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    source.create_snapshot().write(stream, options);

    ecs::Registry restored;
    restored.restore_snapshot(ecs::Registry::Snapshot::read(stream));

    REQUIRE(restored.contains<Position>(entity));
    REQUIRE(restored.get<Position>(entity).x == 8);
    REQUIRE_THROWS_AS(restored.component<Velocity>(), std::logic_error);
}

TEST_CASE("registry in-memory snapshot native format rejects selected non-trivial component storage") {
    ecs::Registry source;
    source.register_component<Position>("Position");
    source.register_component<CopyableName>("CopyableName");

    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{1, 2}) != nullptr);
    REQUIRE(source.add<CopyableName>(entity, CopyableName{"not raw serializable"}) != nullptr);

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE_THROWS_AS(source.create_snapshot().write(stream), std::logic_error);

    ecs::SnapshotIoOptions options;
    options.include_components.push_back(source.component<Position>());
    std::stringstream filtered(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE_NOTHROW(source.create_snapshot().write(filtered, options));
}

TEST_CASE("registry in-memory snapshot native format read rejects invalid headers") {
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    stream.write("bad", 3);
    REQUIRE_THROWS_AS(ecs::Registry::Snapshot::read(stream), std::runtime_error);
}

TEST_CASE("persistent full snapshots use component codecs and restore through schema names") {
    ecs::SnapshotPersistenceCodecs codecs;
    ecs::Registry source;
    codecs.register_component<Position, CountingPositionTraits>(source, "Position");
    codecs.register_component<Velocity>(source, "Velocity");
    codecs.register_component<GameTime>(source, "GameTime");
    source.register_component<Active>("Active");

    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{3, 4}) != nullptr);
    REQUIRE(source.add<Velocity>(entity, Velocity{1.5f, 2.5f}) != nullptr);
    REQUIRE(source.add<Active>(entity));
    REQUIRE(source.clear_dirty<Position>(entity));
    source.write<GameTime>().tick = 77;

    ecs::Registry schema;
    codecs.register_component<Position, CountingPositionTraits>(schema, "Position");
    codecs.register_component<Velocity>(schema, "Velocity");
    codecs.register_component<GameTime>(schema, "GameTime");
    schema.register_component<Active>("Active");

    CountingPositionTraits::serialize_calls = 0;
    CountingPositionTraits::deserialize_calls = 0;
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    ecs::write_persistent_snapshot(stream, source.create_snapshot(), codecs);

    ecs::Registry::Snapshot loaded = ecs::read_persistent_snapshot(stream, schema, codecs);
    ecs::Registry restored;
    restored.restore_snapshot(loaded);

    REQUIRE(restored.alive(entity));
    REQUIRE(restored.get<Position>(entity).x == 3);
    REQUIRE(restored.get<Position>(entity).y == 4);
    REQUIRE(restored.get<Velocity>(entity).dx == 1.5f);
    REQUIRE(restored.has<Active>(entity));
    REQUIRE_FALSE(restored.is_dirty<Position>(entity));
    REQUIRE(restored.get<GameTime>().tick == 77);
    REQUIRE(CountingPositionTraits::serialize_calls == 1);
    REQUIRE(CountingPositionTraits::deserialize_calls == 1);
}

TEST_CASE("persistent delta snapshots handle values and tombstones separately from codecs") {
    ecs::SnapshotPersistenceCodecs codecs;
    ecs::Registry source;
    codecs.register_component<Position, CountingPositionTraits>(source, "Position");
    codecs.register_component<Velocity>(source, "Velocity");
    source.register_component<Active>("Active");

    const ecs::Entity updated = source.create();
    const ecs::Entity removed = source.create();
    const ecs::Entity destroyed = source.create();
    REQUIRE(source.add<Position>(updated, Position{1, 1}) != nullptr);
    REQUIRE(source.add<Position>(removed, Position{2, 2}) != nullptr);
    REQUIRE(source.add<Velocity>(removed, Velocity{3.0f, 4.0f}) != nullptr);
    REQUIRE(source.add<Position>(destroyed, Position{5, 5}) != nullptr);
    REQUIRE(source.add<Active>(destroyed));
    source.clear_all_dirty<Position>();
    source.clear_all_dirty<Velocity>();
    source.clear_all_dirty<Active>();

    auto baseline = source.create_snapshot();
    ecs::Registry replay;
    codecs.register_component<Position, CountingPositionTraits>(replay, "Position");
    codecs.register_component<Velocity>(replay, "Velocity");
    replay.register_component<Active>("Active");
    replay.restore_snapshot(baseline);

    source.write<Position>(updated).x = 10;
    REQUIRE(source.remove<Velocity>(removed));
    REQUIRE(source.destroy(destroyed));

    CountingPositionTraits::serialize_calls = 0;
    CountingPositionTraits::deserialize_calls = 0;
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    ecs::write_persistent_delta_snapshot(stream, source.create_delta_snapshot(baseline), baseline, codecs);

    ecs::Registry::DeltaSnapshot loaded = ecs::read_persistent_delta_snapshot(stream, replay, baseline, codecs);
    replay.restore_delta_snapshot(loaded);

    REQUIRE(replay.get<Position>(updated).x == 10);
    REQUIRE(replay.get<Position>(updated).y == 1);
    REQUIRE_FALSE(replay.contains<Velocity>(removed));
    REQUIRE_FALSE(replay.alive(destroyed));
    REQUIRE(CountingPositionTraits::serialize_calls == 1);
    REQUIRE(CountingPositionTraits::deserialize_calls == 1);
}

TEST_CASE("persistent delta tombstones do not call component codecs") {
    ecs::SnapshotPersistenceCodecs codecs;
    ecs::Registry source;
    codecs.register_component<Position, CountingPositionTraits>(source, "Position");

    const ecs::Entity removed = source.create();
    REQUIRE(source.add<Position>(removed, Position{2, 2}) != nullptr);
    source.clear_all_dirty<Position>();
    auto baseline = source.create_snapshot();
    REQUIRE(source.remove<Position>(removed));

    CountingPositionTraits::serialize_calls = 0;
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    ecs::write_persistent_delta_snapshot(stream, source.create_delta_snapshot(baseline), baseline, codecs);

    REQUIRE(CountingPositionTraits::serialize_calls == 0);
}

TEST_CASE("persistent snapshots reject missing codecs and duplicate component names") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    REQUIRE_THROWS_AS(registry.register_component<Velocity>("Position"), std::logic_error);

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 2}) != nullptr);

    ecs::SnapshotPersistenceCodecs codecs;
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE_THROWS_AS(ecs::write_persistent_snapshot(stream, registry.create_snapshot(), codecs), std::logic_error);
}

TEST_CASE("persistent snapshot frame bit length supports skipping frames") {
    ecs::SnapshotPersistenceCodecs codecs;
    ecs::Registry source;
    codecs.register_component<Position, CountingPositionTraits>(source, "Position");
    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{1, 1}) != nullptr);

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    ecs::write_persistent_snapshot(stream, source.create_snapshot(), codecs);
    source.write<Position>(entity).x = 9;
    ecs::write_persistent_snapshot(stream, source.create_snapshot(), codecs);

    const std::string bytes = stream.str();
    REQUIRE(bytes.size() >= 20U);
    auto read_u64 = [&](std::size_t offset) {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8U; ++index) {
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index])) << (index * 8U);
        }
        return value;
    };
    const std::uint64_t first_bits = read_u64(12U);
    const std::size_t second_offset = 20U + static_cast<std::size_t>((first_bits + 7U) / 8U);
    REQUIRE(second_offset < bytes.size());

    std::stringstream second(bytes.substr(second_offset), std::ios::in | std::ios::out | std::ios::binary);
    ecs::Registry::Snapshot loaded = ecs::read_persistent_snapshot(second, source, codecs);
    ecs::Registry restored;
    restored.restore_snapshot(loaded);
    REQUIRE(restored.get<Position>(entity).x == 9);
}

TEST_CASE("persistent snapshot read rejects unread codec payload bits") {
    ecs::SnapshotPersistenceCodecs writer_codecs;
    ecs::Registry source;
    writer_codecs.register_component<Position, CountingPositionTraits>(source, "Position");
    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{1, 2}) != nullptr);

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    ecs::write_persistent_snapshot(stream, source.create_snapshot(), writer_codecs);

    ecs::SnapshotPersistenceCodecs reader_codecs;
    ecs::Registry schema;
    reader_codecs.register_component<Position, UnreadPositionTraits>(schema, "Position");

    REQUIRE_THROWS_AS(ecs::read_persistent_snapshot(stream, schema, reader_codecs), std::runtime_error);
}

TEST_CASE("delta snapshots restore dirty values additions removals and destroyed entities") {
    ecs::Registry source;
    source.register_component<Position>("Position");
    source.register_component<Velocity>("Velocity");
    source.register_component<Health>("Health");
    source.declare_owned_group<Position, Velocity>();

    const ecs::Entity updated = source.create();
    const ecs::Entity removed_component = source.create();
    const ecs::Entity destroyed = source.create();
    REQUIRE(source.add<Position>(updated, Position{1, 1}) != nullptr);
    REQUIRE(source.add<Velocity>(updated, Velocity{1.0f, 1.0f}) != nullptr);
    REQUIRE(source.add<Position>(removed_component, Position{2, 2}) != nullptr);
    REQUIRE(source.add<Velocity>(removed_component, Velocity{2.0f, 2.0f}) != nullptr);
    REQUIRE(source.add<Position>(destroyed, Position{3, 3}) != nullptr);
    REQUIRE(source.add<Health>(destroyed, Health{30}) != nullptr);
    source.clear_all_dirty<Position>();
    source.clear_all_dirty<Velocity>();
    source.clear_all_dirty<Health>();

    auto baseline = source.create_snapshot();
    ecs::Registry replay;
    replay.register_component<Position>("Position");
    replay.register_component<Velocity>("Velocity");
    replay.register_component<Health>("Health");
    replay.declare_owned_group<Position, Velocity>();
    replay.restore_snapshot(baseline);

    source.write<Position>(updated).x = 10;
    const ecs::Entity added = source.create();
    REQUIRE(source.add<Position>(added, Position{4, 4}) != nullptr);
    REQUIRE(source.add<Velocity>(added, Velocity{4.0f, 4.0f}) != nullptr);
    REQUIRE(source.remove<Velocity>(removed_component));
    REQUIRE(source.destroy(destroyed));

    auto delta = source.create_delta_snapshot(baseline);
    replay.restore_delta_snapshot(delta);

    REQUIRE(replay.alive(updated));
    REQUIRE(replay.get<Position>(updated).x == 10);
    REQUIRE(replay.get<Position>(updated).y == 1);
    REQUIRE(replay.get<Velocity>(updated).dx == 1.0f);
    REQUIRE(replay.alive(added));
    REQUIRE(replay.get<Position>(added).x == 4);
    REQUIRE(replay.get<Velocity>(added).dx == 4.0f);
    REQUIRE(replay.alive(removed_component));
    REQUIRE(replay.get<Position>(removed_component).x == 2);
    REQUIRE_FALSE(replay.contains<Velocity>(removed_component));
    REQUIRE_FALSE(replay.alive(destroyed));
    REQUIRE_FALSE(replay.contains<Position>(destroyed));
    REQUIRE_FALSE(replay.contains<Health>(destroyed));
    REQUIRE(replay.is_dirty<Position>(updated));
    REQUIRE(replay.is_dirty<Position>(added));
    REQUIRE(replay.is_dirty<Velocity>(removed_component));

    std::vector<ecs::Entity> grouped;
    replay.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        grouped.push_back(entity);
    });
    REQUIRE(std::find(grouped.begin(), grouped.end(), updated) != grouped.end());
    REQUIRE(std::find(grouped.begin(), grouped.end(), added) != grouped.end());
    REQUIRE(std::find(grouped.begin(), grouped.end(), removed_component) == grouped.end());
}

TEST_CASE("delta snapshots restore tag additions and removals") {
    ecs::Registry source;
    source.register_component<Position>("Position");
    source.register_component<Active>("Active");

    const ecs::Entity kept = source.create();
    const ecs::Entity removed = source.create();
    REQUIRE(source.add<Position>(kept, Position{1, 0}) != nullptr);
    REQUIRE(source.add<Position>(removed, Position{2, 0}) != nullptr);
    REQUIRE(source.add<Active>(removed));
    source.clear_all_dirty<Active>();
    source.clear_all_dirty<Position>();

    auto baseline = source.create_snapshot();
    ecs::Registry replay;
    replay.register_component<Position>("Position");
    replay.register_component<Active>("Active");
    replay.restore_snapshot(baseline);

    REQUIRE(source.add<Active>(kept));
    REQUIRE(source.remove<Active>(removed));

    auto delta = source.create_delta_snapshot(baseline);
    replay.restore_delta_snapshot(delta);

    REQUIRE(replay.has<Active>(kept));
    REQUIRE_FALSE(replay.has<Active>(removed));
    REQUIRE(replay.is_dirty<Active>(kept));
    REQUIRE(replay.is_dirty<Active>(removed));

    std::vector<ecs::Entity> active_entities;
    replay.view<const Position>().with_tags<const Active>().each([&](ecs::Entity entity, const Position&) {
        active_entities.push_back(entity);
    });
    REQUIRE(active_entities == std::vector<ecs::Entity>{kept});
}

TEST_CASE("delta snapshots restore singleton dirty values") {
    ecs::Registry source;
    source.register_component<GameTime>("GameTime");
    source.clear_all_dirty<GameTime>();

    auto baseline = source.create_snapshot();
    ecs::Registry replay;
    replay.register_component<GameTime>("GameTime");
    replay.restore_snapshot(baseline);

    source.write<GameTime>().tick = 55;
    auto delta = source.create_delta_snapshot(baseline);
    replay.restore_delta_snapshot(delta);

    REQUIRE(replay.get<GameTime>().tick == 55);
    REQUIRE(replay.is_dirty<GameTime>());
}

TEST_CASE("delta snapshots exclude system-tagged job bookkeeping entities") {
    ecs::Registry source;
    source.register_component<Position>("Position");

    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{1, 0}) != nullptr);

    const ecs::Entity job = source.job<Position>(0).each([](ecs::Entity, Position&) {});
    REQUIRE(source.add<Position>(job, Position{99, 0}) != nullptr);
    source.clear_all_dirty<Position>();

    auto baseline = source.create_snapshot();
    ecs::Registry replay;
    replay.register_component<Position>("Position");
    replay.restore_snapshot(baseline);

    source.write<Position>(entity).x = 5;
    source.write<Position>(job).x = 100;

    auto delta = source.create_delta_snapshot(baseline);
    replay.restore_delta_snapshot(delta);

    REQUIRE(replay.get<Position>(entity).x == 5);
    REQUIRE_FALSE(replay.alive(job));
    REQUIRE_FALSE(replay.contains<Position>(job));
}

TEST_CASE("delta restore validates baseline token component metadata and removal state") {
    ecs::Registry source;
    source.register_component<Position>("Position");
    source.register_component<Velocity>("Velocity");
    const ecs::Entity entity = source.create();
    REQUIRE(source.add<Position>(entity, Position{1, 1}) != nullptr);
    REQUIRE(source.add<Velocity>(entity, Velocity{1.0f, 1.0f}) != nullptr);
    source.clear_all_dirty<Position>();
    source.clear_all_dirty<Velocity>();

    auto baseline = source.create_snapshot();
    source.write<Position>(entity).x = 2;
    REQUIRE(source.remove<Velocity>(entity));
    auto delta = source.create_delta_snapshot(baseline);

    ecs::Registry wrong_baseline;
    wrong_baseline.register_component<Position>("Position");
    wrong_baseline.register_component<Velocity>("Velocity");
    REQUIRE_THROWS_AS(wrong_baseline.restore_delta_snapshot(delta), std::logic_error);

    ecs::Registry missing_component;
    missing_component.register_component<Position>("Position");
    missing_component.restore_snapshot(baseline);
    REQUIRE(missing_component.destroy(missing_component.component<Velocity>()));
    REQUIRE_THROWS_AS(missing_component.restore_delta_snapshot(delta), std::logic_error);

    ecs::Registry missing_removed_value;
    missing_removed_value.register_component<Position>("Position");
    missing_removed_value.register_component<Velocity>("Velocity");
    missing_removed_value.restore_snapshot(baseline);
    REQUIRE(missing_removed_value.remove<Velocity>(entity));
    REQUIRE_THROWS_AS(missing_removed_value.restore_delta_snapshot(delta), std::logic_error);
}

TEST_CASE("delta snapshots reject dirty move-only non-trivial component storage") {
    ecs::Registry source;
    source.register_component<std::unique_ptr<int>>("OwnedInt");
    auto baseline = source.create_snapshot();

    const ecs::Entity entity = source.create();
    REQUIRE(source.add<std::unique_ptr<int>>(entity, new int(1)) != nullptr);

    REQUIRE_THROWS_AS(source.create_delta_snapshot(baseline), std::logic_error);
}
