#include "ashiato_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("declared owned groups are used by matching views and track membership") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.declare_owned_group<Position, Velocity>();

    const ashiato::Entity position_only = registry.create();
    const ashiato::Entity both_a = registry.create();
    const ashiato::Entity both_b = registry.create();

    REQUIRE(registry.add<Position>(position_only, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Position>(both_a, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(both_a, Velocity{20.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Velocity>(both_b, Velocity{30.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(both_b, Position{3, 0}) != nullptr);

    registry.clear_all_dirty<Position>();
    registry.clear_all_dirty<Velocity>();

    std::vector<ashiato::Entity> visited;
    registry.view<Position, Velocity>().each([&](ashiato::Entity entity, Position& position, Velocity& velocity) {
        visited.push_back(entity);
        velocity.dy = static_cast<float>(position.x);
    });

    REQUIRE(visited.size() == 2);
    REQUIRE(registry.view<Position, Velocity>().get<Position>(both_a).x == 2);
    REQUIRE(registry.view<Position, Velocity>().write<Velocity>(both_b).dx == 30.0f);
    REQUIRE(registry.is_dirty<Position>(both_a));
    REQUIRE(registry.is_dirty<Velocity>(both_a));
    REQUIRE(registry.get<Velocity>(both_a).dy == 2.0f);
    REQUIRE(registry.get<Velocity>(both_b).dy == 3.0f);

    REQUIRE(registry.remove<Velocity>(both_a));
    REQUIRE(registry.add<Velocity>(position_only, Velocity{10.0f, 0.0f}) != nullptr);

    std::vector<ashiato::Entity> after_changes;
    registry.view<Position, Velocity>().each([&](ashiato::Entity entity, Position&, Velocity&) {
        after_changes.push_back(entity);
    });
    REQUIRE(after_changes.size() == 2);
    REQUIRE(std::find(after_changes.begin(), after_changes.end(), position_only) != after_changes.end());
    REQUIRE(std::find(after_changes.begin(), after_changes.end(), both_b) != after_changes.end());
}

TEST_CASE("owned groups declared before storage start empty and accept later matches") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.declare_owned_group<Position, Velocity>();

    int before_storage = 0;
    registry.view<Position, Velocity>().each([&](ashiato::Entity, Position&, Velocity&) {
        ++before_storage;
    });
    REQUIRE(before_storage == 0);

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{4, 5}) != nullptr);

    int position_only = 0;
    registry.view<Position, Velocity>().each([&](ashiato::Entity, Position&, Velocity&) {
        ++position_only;
    });
    REQUIRE(position_only == 0);

    REQUIRE(registry.add<Velocity>(entity, Velocity{6.0f, 7.0f}) != nullptr);

    std::vector<ashiato::Entity> after_match;
    registry.view<Position, Velocity>().each([&](ashiato::Entity visited, Position& position, Velocity& velocity) {
        after_match.push_back(visited);
        REQUIRE(position.x == 4);
        REQUIRE(velocity.dx == 6.0f);
    });
    REQUIRE(after_match == std::vector<ashiato::Entity>{entity});
}

TEST_CASE("owned group view iteration tolerates later entities gaining components") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.declare_owned_group<Position, Velocity>();

    const ashiato::Entity first = registry.create();
    const ashiato::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{2, 0}) != nullptr);

    std::vector<ashiato::Entity> visited;
    registry.view<Position, Velocity>().each([&](ashiato::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.add<Velocity>(later, Velocity{2.0f, 0.0f}) != nullptr);
        }
    });

    REQUIRE(visited.size() <= 2);
    std::vector<ashiato::Entity> after;
    registry.view<Position, Velocity>().each([&](ashiato::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) != after.end());
    REQUIRE(registry.get<Velocity>(later).dx == 2.0f);
}

TEST_CASE("owned group view iteration tolerates later entities losing components") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.declare_owned_group<Position, Velocity>();

    const ashiato::Entity first = registry.create();
    const ashiato::Entity second = registry.create();
    const ashiato::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(later, Velocity{3.0f, 0.0f}) != nullptr);

    std::vector<ashiato::Entity> visited;
    registry.view<Position, Velocity>().each([&](ashiato::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.remove<Velocity>(later));
        }
    });

    REQUIRE(visited.size() <= 3);
    std::vector<ashiato::Entity> after;
    registry.view<Position, Velocity>().each([&](ashiato::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), second) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) == after.end());
    REQUIRE(registry.contains<Position>(later));
    REQUIRE_FALSE(registry.contains<Velocity>(later));
}

TEST_CASE("owned group view iteration tolerates removing components from current entity") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.declare_owned_group<Position, Velocity>();

    const ashiato::Entity first = registry.create();
    const ashiato::Entity second = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);

    std::vector<ashiato::Entity> visited;
    registry.view<Position, Velocity>().each([&](ashiato::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        REQUIRE(registry.remove<Velocity>(entity));
    });

    REQUIRE_FALSE(visited.empty());
    REQUIRE(visited.size() <= 2);
    for (ashiato::Entity entity : visited) {
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE_FALSE(registry.contains<Velocity>(entity));
    }

    std::vector<ashiato::Entity> after;
    registry.view<Position, Velocity>().each([&](ashiato::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    for (ashiato::Entity entity : after) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
    }
}

TEST_CASE("owned group declarations allow identical groups but reject shared ownership") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Health>("Health");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{10}) != nullptr);

    registry.declare_owned_group<Position, Velocity>();
    registry.declare_owned_group<Velocity, Position>();

    REQUIRE_THROWS_AS((registry.declare_owned_group<Position>()), std::logic_error);
    REQUIRE_THROWS_AS((registry.declare_owned_group<Position, Velocity, Health>()), std::logic_error);
    REQUIRE_THROWS_AS((registry.declare_owned_group<Velocity, Health>()), std::logic_error);

    ashiato::Registry conflict;
    conflict.register_component<Position>("Position");
    conflict.register_component<Velocity>("Velocity");
    conflict.register_component<Health>("Health");
    conflict.declare_owned_group<Position, Velocity>();
    REQUIRE_THROWS_AS((conflict.declare_owned_group<Position, Health>()), std::logic_error);
}

TEST_CASE("destroying a component entity removes owned groups that include it") {
    ashiato::Registry registry;
    const ashiato::Entity position_component = registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 2}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{3.0f, 4.0f}) != nullptr);

    registry.declare_owned_group<Position, Velocity>();
    REQUIRE(registry.destroy(position_component));

    REQUIRE(registry.component_info(position_component) == nullptr);
    registry.declare_owned_group<Velocity>();

    std::vector<ashiato::Entity> velocity_group;
    registry.view<Velocity>().each([&](ashiato::Entity visited, Velocity&) {
        velocity_group.push_back(visited);
    });
    REQUIRE(velocity_group == std::vector<ashiato::Entity>{entity});
}

TEST_CASE("owned groups are rebuilt after registry snapshot restore") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity kept = registry.create();
    const ashiato::Entity later_removed = registry.create();
    REQUIRE(registry.add<Position>(kept, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(kept, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later_removed, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(later_removed, Velocity{2.0f, 0.0f}) != nullptr);

    registry.declare_owned_group<Position, Velocity>();
    auto snapshot = registry.create_snapshot();

    REQUIRE(registry.remove<Velocity>(kept));
    REQUIRE(registry.destroy(later_removed));

    registry.restore_snapshot(snapshot);

    std::vector<ashiato::Entity> grouped;
    registry.view<Position, Velocity>().each([&](ashiato::Entity entity, Position& position, Velocity& velocity) {
        grouped.push_back(entity);
        velocity.dx = static_cast<float>(position.x + 10);
    });

    REQUIRE(grouped.size() == 2);
    REQUIRE(std::find(grouped.begin(), grouped.end(), kept) != grouped.end());
    REQUIRE(std::find(grouped.begin(), grouped.end(), later_removed) != grouped.end());
    REQUIRE(registry.get<Velocity>(kept).dx == 11.0f);
    REQUIRE(registry.get<Velocity>(later_removed).dx == 12.0f);
}

TEST_CASE("owned groups preserve non-trivial components while packing") {
    TrackerCounts counts;

    {
        ashiato::Registry registry;
        registry.register_component<Tracker>("Tracker");
        registry.register_component<Position>("Position");

        const ashiato::Entity first = registry.create();
        const ashiato::Entity second = registry.create();
        const ashiato::Entity third = registry.create();

        REQUIRE(registry.add<Tracker>(first, counts, 3) != nullptr);
        REQUIRE(registry.add<Position>(first, Position{3, 0}) != nullptr);
        REQUIRE(registry.add<Position>(second, Position{1, 0}) != nullptr);
        REQUIRE(registry.add<Tracker>(second, counts, 1) != nullptr);
        REQUIRE(registry.add<Tracker>(third, counts, 2) != nullptr);
        REQUIRE(registry.add<Position>(third, Position{2, 0}) != nullptr);

        registry.declare_owned_group<Tracker, Position>();

        std::vector<int> tracker_values;
        registry.view<Tracker, Position>().each([&](ashiato::Entity, Tracker& tracker, Position& position) {
            tracker_values.push_back(tracker.value);
            REQUIRE(tracker.value == position.x);
        });

        REQUIRE(tracker_values.size() == 3);
        REQUIRE(std::find(tracker_values.begin(), tracker_values.end(), 1) != tracker_values.end());
        REQUIRE(std::find(tracker_values.begin(), tracker_values.end(), 2) != tracker_values.end());
        REQUIRE(std::find(tracker_values.begin(), tracker_values.end(), 3) != tracker_values.end());
        REQUIRE(registry.remove<Position>(second));
        int remaining = 0;
        registry.view<Tracker, Position>().each([&](ashiato::Entity, Tracker&, Position&) {
            ++remaining;
        });
        REQUIRE(remaining == 2);
    }

    REQUIRE(counts.constructed == counts.destroyed);
}
