#include "ecs_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("stored views refresh storage pointers after construction") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    auto view_before_storage = registry.view<Position>();

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 2}) != nullptr);

    int refreshed_view_calls = 0;
    view_before_storage.each([&](ecs::Entity, Position&) {
        ++refreshed_view_calls;
    });
    REQUIRE(refreshed_view_calls == 1);

    auto view_after_storage = registry.view<Position>();
    int live_view_calls = 0;
    view_after_storage.each([&](ecs::Entity current, Position& position) {
        REQUIRE(current == entity);
        REQUIRE(position.x == 1);
        ++live_view_calls;
    });
    REQUIRE(live_view_calls == 1);
}

TEST_CASE("view iteration tolerates removing non-driver components from current entity") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        visited.push_back(entity);
        REQUIRE(registry.remove<Velocity>(entity));
    });

    REQUIRE(visited.size() == 2);
    REQUIRE(visited[0] == first);
    REQUIRE(visited[1] == second);
    REQUIRE(registry.contains<Position>(first));
    REQUIRE(registry.contains<Position>(second));
    REQUIRE_FALSE(registry.contains<Velocity>(first));
    REQUIRE_FALSE(registry.contains<Velocity>(second));
}

TEST_CASE("view iteration tolerates later entities gaining non-driver components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{2, 0}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.add<Velocity>(later, Velocity{2.0f, 0.0f}) != nullptr);
        }
    });

    REQUIRE(visited.size() <= 2);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) != after.end());
    REQUIRE(registry.get<Velocity>(later).dx == 2.0f);
}

TEST_CASE("stored view sees entities added after view construction") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    auto view = registry.view<Position>();
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    std::vector<ecs::Entity> visited;
    view.each([&](ecs::Entity current, Position& position) {
        visited.push_back(current);
        position.x = 7;
    });

    REQUIRE(visited == std::vector<ecs::Entity>{entity});
    REQUIRE(registry.get<Position>(entity).x == 7);
}

TEST_CASE("stored access and optional views see entities added after view construction") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Health>("Health");

    auto access_view = registry.view<Position>().access<Velocity>();
    auto optional_view = registry.view<Position>().optional<Health>();

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{3}) != nullptr);

    std::vector<ecs::Entity> access_visited;
    access_view.each([&](auto& view, ecs::Entity current, Position& position) {
        access_visited.push_back(current);
        view.template write<Velocity>(current).dx += static_cast<float>(position.x);
    });

    int optional_calls = 0;
    int health_sum = 0;
    optional_view.each([&](auto& view, ecs::Entity current, Position& position) {
        REQUIRE(current == entity);
        ++optional_calls;
        position.x = 5;
        if (const Health* health = view.template try_get<Health>()) {
            health_sum += health->value;
        }
    });

    REQUIRE(access_visited == std::vector<ecs::Entity>{entity});
    REQUIRE(registry.get<Velocity>(entity).dx == 3.0f);
    REQUIRE(optional_calls == 1);
    REQUIRE(health_sum == 3);
    REQUIRE(registry.get<Position>(entity).x == 5);
}

TEST_CASE("stored view matching indices sees entities added after view construction") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    auto view = registry.view<Position>();
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    const std::vector<std::uint32_t> indices = view.matching_indices();

    REQUIRE(indices.size() == 1);
}

TEST_CASE("stored tag filtered view sees entities tagged after view construction") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Active>("Active");

    auto typed_tag_view = registry.view<Position>().with_tags<const Active>();
    const ecs::Entity active_tag = registry.component<Active>();
    auto runtime_tag_view = registry.view<Position>().with_tags({active_tag});

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Active>(entity));

    std::vector<ecs::Entity> typed_visited;
    typed_tag_view.each([&](ecs::Entity current, Position& position) {
        typed_visited.push_back(current);
        position.x = 2;
    });

    std::vector<ecs::Entity> runtime_visited;
    runtime_tag_view.each([&](ecs::Entity current, Position& position) {
        runtime_visited.push_back(current);
        position.y = 3;
    });

    REQUIRE(typed_visited == std::vector<ecs::Entity>{entity});
    REQUIRE(runtime_visited == std::vector<ecs::Entity>{entity});
    REQUIRE(registry.get<Position>(entity).x == 2);
    REQUIRE(registry.get<Position>(entity).y == 3);
}

TEST_CASE("view iteration tolerates later entities losing non-driver components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(later, Velocity{3.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.remove<Velocity>(later));
        }
    });

    REQUIRE(visited.size() <= 3);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), second) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) == after.end());
    REQUIRE(registry.contains<Position>(later));
    REQUIRE_FALSE(registry.contains<Velocity>(later));
}

TEST_CASE("view iteration tolerates later entities gaining driver components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Velocity>(later, Velocity{2.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.add<Position>(later, Position{2, 0}) != nullptr);
        }
    });

    REQUIRE(visited.size() <= 2);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) != after.end());
    REQUIRE(registry.get<Position>(later).x == 2);
}

TEST_CASE("view iteration tolerates removing driver components from current entity") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        REQUIRE(registry.remove<Position>(entity));
    });

    REQUIRE_FALSE(visited.empty());
    REQUIRE(visited.size() <= 2);
    for (ecs::Entity entity : visited) {
        REQUIRE_FALSE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
    }

    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    for (ecs::Entity entity : after) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
    }
}

TEST_CASE("view iteration tolerates later entities losing driver components") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    const ecs::Entity later = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{1.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Position>(later, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(later, Velocity{3.0f, 0.0f}) != nullptr);

    std::vector<ecs::Entity> visited;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        REQUIRE(std::find(visited.begin(), visited.end(), entity) == visited.end());
        REQUIRE(registry.contains<Position>(entity));
        REQUIRE(registry.contains<Velocity>(entity));
        visited.push_back(entity);
        if (entity == first) {
            REQUIRE(registry.remove<Position>(later));
        }
    });

    REQUIRE(visited.size() <= 3);
    std::vector<ecs::Entity> after;
    registry.view<Position, Velocity>().each([&](ecs::Entity entity, Position&, Velocity&) {
        after.push_back(entity);
    });
    REQUIRE(after.size() == 2);
    REQUIRE(std::find(after.begin(), after.end(), first) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), second) != after.end());
    REQUIRE(std::find(after.begin(), after.end(), later) == after.end());
    REQUIRE_FALSE(registry.contains<Position>(later));
    REQUIRE(registry.contains<Velocity>(later));
}
