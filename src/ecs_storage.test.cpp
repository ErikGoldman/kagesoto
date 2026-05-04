#include "ecs_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("dirty component iteration exposes current values and removal tombstones") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity destroyed = registry.create();

    REQUIRE(registry.add<Position>(first, Position{1, 2}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{3, 4}) != nullptr);
    REQUIRE(registry.add<Position>(destroyed, Position{5, 6}) != nullptr);
    registry.clear_all_dirty<Position>();

    REQUIRE(registry.add<Position>(first, Position{7, 8}) != nullptr);
    REQUIRE(registry.remove<Position>(second));
    REQUIRE(registry.destroy(destroyed));

    std::vector<ecs::Entity> dirty_entities;
    std::vector<Position> dirty_values;
    registry.each_dirty<Position>([&](ecs::Entity entity, const void* value) {
        dirty_entities.push_back(entity);
        dirty_values.push_back(*static_cast<const Position*>(value));
    });

    REQUIRE(dirty_entities == std::vector<ecs::Entity>{first});
    REQUIRE(dirty_values.size() == 1);
    REQUIRE(dirty_values[0].x == 7);
    REQUIRE(dirty_values[0].y == 8);

    std::vector<ecs::Registry::ComponentRemoval> removals;
    registry.each_removed<Position>([&](ecs::Registry::ComponentRemoval removal) {
        removals.push_back(removal);
    });

    REQUIRE(removals.size() == 2);
    auto second_removal = std::find_if(removals.begin(), removals.end(), [&](const auto& removal) {
        return removal.entity_index == ecs::Registry::entity_index(second);
    });
    REQUIRE(second_removal != removals.end());
    REQUIRE_FALSE(second_removal->entity_destroyed);

    auto destroyed_removal = std::find_if(removals.begin(), removals.end(), [&](const auto& removal) {
        return removal.entity_index == ecs::Registry::entity_index(destroyed);
    });
    REQUIRE(destroyed_removal != removals.end());
    REQUIRE(destroyed_removal->entity_destroyed);

    registry.clear_all_dirty<Position>();

    int dirty_count = 0;
    int removal_count = 0;
    registry.each_dirty<Position>([&](ecs::Entity, const void*) {
        ++dirty_count;
    });
    registry.each_removed<Position>([&](ecs::Registry::ComponentRemoval) {
        ++removal_count;
    });
    REQUIRE(dirty_count == 0);
    REQUIRE(removal_count == 0);
}

TEST_CASE("runtime components use component entities and the shared write path") {
    ecs::Registry registry;

    ecs::ComponentDesc desc;
    desc.name = "Velocity";
    desc.size = sizeof(Velocity);
    desc.alignment = alignof(Velocity);
    const ecs::Entity velocity_component = registry.register_component(std::move(desc));

    const ecs::Entity entity = registry.create();
    const Velocity initial{1.5f, 2.5f};

    void* added = registry.add(entity, velocity_component, &initial);
    REQUIRE(added != nullptr);
    REQUIRE(static_cast<const Velocity*>(registry.get(entity, velocity_component))->dx == 1.5f);

    REQUIRE(registry.clear_dirty(entity, velocity_component));
    Velocity* writable = static_cast<Velocity*>(registry.write(entity, velocity_component));
    REQUIRE(writable != nullptr);
    writable->dy = 9.0f;
    REQUIRE(static_cast<const Velocity*>(registry.get(entity, velocity_component))->dy == 9.0f);

    REQUIRE(registry.remove(entity, velocity_component));
    REQUIRE(registry.get(entity, velocity_component) == nullptr);
}

TEST_CASE("runtime ensure creates zeroed components and returns existing storage") {
    ecs::Registry registry;

    ecs::ComponentDesc desc;
    desc.name = "Velocity";
    desc.size = sizeof(Velocity);
    desc.alignment = alignof(Velocity);
    const ecs::Entity velocity_component = registry.register_component(std::move(desc));

    const ecs::Entity invalid{};
    REQUIRE(registry.ensure(invalid, velocity_component) == nullptr);

    const ecs::Entity stale = registry.create();
    REQUIRE(registry.destroy(stale));
    REQUIRE(registry.ensure(stale, velocity_component) == nullptr);

    const ecs::Entity entity = registry.create();
    Velocity* ensured = static_cast<Velocity*>(registry.ensure(entity, velocity_component));
    REQUIRE(ensured != nullptr);
    REQUIRE(ensured->dx == 0.0f);
    REQUIRE(ensured->dy == 0.0f);
    REQUIRE(registry.is_dirty(entity, velocity_component));

    ensured->dx = 3.5f;
    REQUIRE(registry.clear_dirty(entity, velocity_component));

    Velocity* existing = static_cast<Velocity*>(registry.ensure(entity, velocity_component));
    REQUIRE(existing == ensured);
    REQUIRE(existing->dx == 3.5f);
    REQUIRE(registry.is_dirty(entity, velocity_component));
}

TEST_CASE("runtime ensure uses singleton storage for singleton component entities") {
    ecs::Registry registry;
    const ecs::Entity game_time_component = registry.register_component<GameTime>("GameTime");
    const ecs::Entity entity = registry.create();

    REQUIRE(registry.clear_dirty<GameTime>());

    GameTime* ensured = static_cast<GameTime*>(registry.ensure(entity, game_time_component));
    REQUIRE(ensured == &registry.write<GameTime>());
    ensured->tick = 17;

    REQUIRE(registry.get<GameTime>().tick == 17);
    REQUIRE(registry.is_dirty<GameTime>());
}

TEST_CASE("runtime byte add rejects non-trivial typed components") {
    ecs::Registry registry;
    const ecs::Entity tracker_component = registry.register_component<Tracker>("Tracker");
    const ecs::Entity entity = registry.create();
    TrackerCounts counts;
    Tracker value{counts, 1};

    REQUIRE_THROWS_AS(registry.add(entity, tracker_component, &value), std::logic_error);
}

TEST_CASE("dirty bits move with components in dense storage") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    REQUIRE(registry.add<Position>(first, Position{1, 1}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 2}) != nullptr);

    registry.clear_all_dirty<Position>();

    registry.write<Position>(second);

    REQUIRE(registry.remove<Position>(first));
    REQUIRE(registry.get<Position>(second).x == 2);
    REQUIRE(registry.is_dirty<Position>(second));
    REQUIRE(registry.is_dirty<Position>(first));
    REQUIRE(registry.clear_dirty<Position>(first));
    REQUIRE_FALSE(registry.is_dirty<Position>(first));
}

TEST_CASE("dirty bits are preserved for clean moved components during dense removal") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    REQUIRE(registry.add<Position>(first, Position{1, 1}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 2}) != nullptr);

    registry.clear_all_dirty<Position>();
    registry.write<Position>(first);

    REQUIRE(registry.remove<Position>(first));
    REQUIRE(registry.get<Position>(second).x == 2);
    REQUIRE_FALSE(registry.is_dirty<Position>(second));
}

TEST_CASE("non-trivial components are constructed, moved, and destroyed explicitly") {
    ecs::Registry registry;
    registry.register_component<Tracker>("Tracker");
    TrackerCounts counts;

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    REQUIRE(registry.add<Tracker>(first, counts, 1) != nullptr);
    REQUIRE(registry.add<Tracker>(second, counts, 2) != nullptr);
    REQUIRE(registry.get<Tracker>(first).value == 1);
    REQUIRE(registry.get<Tracker>(second).value == 2);

    REQUIRE(registry.remove<Tracker>(first));
    REQUIRE_FALSE(registry.contains<Tracker>(first));
    REQUIRE(registry.get<Tracker>(second).value == 2);

    REQUIRE(registry.destroy(second));
    REQUIRE(counts.constructed == counts.destroyed);
}

TEST_CASE("non-trivial component replacement destroys the old value") {
    ecs::Registry registry;
    registry.register_component<Tracker>("Tracker");
    TrackerCounts counts;

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Tracker>(entity, counts, 1) != nullptr);
    REQUIRE(registry.get<Tracker>(entity).value == 1);

    Tracker* replacement = registry.add<Tracker>(entity, counts, 2);
    REQUIRE(replacement != nullptr);
    REQUIRE(replacement == &registry.get<Tracker>(entity));
    REQUIRE(replacement->value == 2);
    REQUIRE(counts.constructed > counts.destroyed);

    REQUIRE(registry.remove<Tracker>(entity));
    REQUIRE(counts.constructed == counts.destroyed);
}

TEST_CASE("non-trivial storage growth and middle removal preserve live values") {
    TrackerCounts counts;
    std::vector<ecs::Entity> entities;

    {
        ecs::Registry registry;
        registry.register_component<Tracker>("Tracker");

        for (int i = 0; i < 10; ++i) {
            const ecs::Entity entity = registry.create();
            entities.push_back(entity);
            REQUIRE(registry.add<Tracker>(entity, counts, i) != nullptr);
        }

        for (int i = 0; i < 10; ++i) {
            REQUIRE(registry.get<Tracker>(entities[static_cast<std::size_t>(i)]).value == i);
        }

        REQUIRE(registry.remove<Tracker>(entities[4]));
        REQUIRE_FALSE(registry.contains<Tracker>(entities[4]));
        for (int i = 0; i < 10; ++i) {
            if (i == 4) {
                continue;
            }
            REQUIRE(registry.get<Tracker>(entities[static_cast<std::size_t>(i)]).value == i);
        }
    }

    REQUIRE(counts.constructed == counts.destroyed);
}

TEST_CASE("moved registries retain entities components metadata singletons and dirty bits") {
    ecs::Registry source;
    const ecs::Entity position_component = source.register_component<Position>("Position");
    const ecs::Entity game_time_component = source.register_component<GameTime>("GameTime");
    const ecs::Entity entity = source.create();

    REQUIRE(source.add<Position>(entity, Position{5, 6}) != nullptr);
    REQUIRE(source.clear_dirty<Position>(entity));
    (void)source.write<Position>(entity);
    REQUIRE(source.set_component_fields(
        position_component,
        {ecs::ComponentField{"x", offsetof(Position, x), source.primitive_type(ecs::PrimitiveType::I32), 1}}));
    source.write<GameTime>().tick = 12;

    ecs::Registry moved(std::move(source));

    REQUIRE(moved.alive(entity));
    REQUIRE(moved.component<Position>() == position_component);
    REQUIRE(moved.component<GameTime>() == game_time_component);
    REQUIRE(moved.get<Position>(entity).x == 5);
    REQUIRE(moved.get<Position>(entity).y == 6);
    REQUIRE(moved.is_dirty<Position>(entity));
    REQUIRE(moved.get<GameTime>().tick == 12);
    REQUIRE(moved.is_dirty<GameTime>());

    const std::vector<ecs::ComponentField>* fields = moved.component_fields(position_component);
    REQUIRE(fields != nullptr);
    REQUIRE(fields->size() == 1);
    REQUIRE((*fields)[0].name == "x");
}
