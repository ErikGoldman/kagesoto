#include "ashiato_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("jobs are persistent and use access views") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{2, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{1.0f, 0.0f}) != nullptr);

    registry.job<const Position>(0).access_other_entities<Velocity>().each(
        [&](auto& active_view, ashiato::Entity current, const Position& position) {
            Velocity& velocity = active_view.template write<Velocity>(current);
            velocity.dx += static_cast<float>(position.x);
        });

    registry.run_jobs();
    registry.run_jobs();

    REQUIRE(registry.get<Velocity>(entity).dx == 5.0f);
}

TEST_CASE("job optional components do not filter and are limited to current entity") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Health>("Health");

    const ashiato::Entity with_health = registry.create();
    const ashiato::Entity without_health = registry.create();
    REQUIRE(registry.add<Position>(with_health, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Health>(with_health, Health{10}) != nullptr);
    REQUIRE(registry.add<Position>(without_health, Position{2, 0}) != nullptr);

    int calls = 0;
    int health_sum = 0;
    registry.job<Position>(0).optional<Health>().each(
        [&](auto& view, ashiato::Entity, Position&) {
            ++calls;
            if (const Health* health = view.template try_get<Health>()) {
                health_sum += health->value;
            }
            static_assert(!HasViewTryGet<decltype(view), Health>::value);
        });

    registry.run_jobs();

    REQUIRE(calls == 2);
    REQUIRE(health_sum == 10);
}

TEST_CASE("optional jobs can be threaded but access other entities jobs are single threaded") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Health>("Health");

    for (int i = 0; i < 4; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
        if ((i % 2) == 0) {
            REQUIRE(registry.add<Health>(entity, Health{i}) != nullptr);
        }
    }

    registry.job<Position>(0).optional<Health>().max_threads(2).min_entities_per_thread(1).each(
        [](auto& view, ashiato::Entity, Position& position) {
            if (view.template contains<Health>()) {
                position.x += view.template write<Health>().value;
            }
        });
    registry.job<Position>(1).access_other_entities<Health>().each([](auto&, ashiato::Entity, Position&) {});

    std::vector<std::size_t> thread_counts;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        for (const ashiato::JobThreadTask& task : tasks) {
            thread_counts.push_back(task.thread_count);
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(thread_counts.size() == 3);
    REQUIRE(thread_counts[0] == 2);
    REQUIRE(thread_counts[1] == 2);
    REQUIRE(thread_counts[2] == 1);
    REQUIRE_THROWS_AS(registry.job<Position>(2).max_threads(2).access_other_entities<Health>(), std::logic_error);
}

TEST_CASE("job callback views create nested views from declared components") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Health>("Health");

    const ashiato::Entity first = registry.create();
    const ashiato::Entity second = registry.create();
    REQUIRE(registry.add<Position>(first, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(first, Velocity{2.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(first, Health{10}) != nullptr);
    REQUIRE(registry.add<Position>(second, Position{3, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(second, Velocity{4.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<Health>(second, Health{20}) != nullptr);

    int outer_calls = 0;
    registry.job<const Position>(0).access_other_entities<Velocity, Health>().single_thread().each(
        [&](auto& view, ashiato::Entity entity, const Position& position) {
            int nested_calls = 0;
            auto nested = view.template view<const Position, Velocity, Health>();
            nested.each([&](ashiato::Entity nested_entity, const Position& nested_position, Velocity& velocity, Health& health) {
                if (nested_entity == entity) {
                    velocity.dx += static_cast<float>(position.x + nested_position.x);
                    health.value += position.x;
                }
                ++nested_calls;
            });

            REQUIRE(nested_calls == 2);
            ++outer_calls;
        });

    registry.run_jobs();

    REQUIRE(outer_calls == 2);
    REQUIRE(registry.get<Velocity>(first).dx == 4.0f);
    REQUIRE(registry.get<Health>(first).value == 11);
    REQUIRE(registry.get<Velocity>(second).dx == 10.0f);
    REQUIRE(registry.get<Health>(second).value == 23);
}

TEST_CASE("registry access inside job callbacks throws in checked builds") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{2.0f, 0.0f}) != nullptr);

#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
    registry.job<Position>(0).each([&](ashiato::Entity current, Position&) {
        (void)registry.get<Position>(current);
    });
    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
#else
    registry.job<Position>(0).each([&](ashiato::Entity current, Position&) {
        REQUIRE(registry.get<Position>(current).x == 1);
    });
    registry.run_jobs();
#endif
}

TEST_CASE("registry view and add access inside job callbacks throws in checked builds") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
    registry.job<Position>(0).each([&](ashiato::Entity current, Position&) {
        (void)registry.add<Velocity>(current, Velocity{});
    });
    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);

    ashiato::Registry view_registry;
    view_registry.register_component<Position>("Position");
    const ashiato::Entity view_entity = view_registry.create();
    REQUIRE(view_registry.add<Position>(view_entity, Position{1, 0}) != nullptr);
    view_registry.job<Position>(0).each([&](ashiato::Entity, Position&) {
        (void)view_registry.view<Position>();
    });
    REQUIRE_THROWS_AS(view_registry.run_jobs(), std::logic_error);
#else
    registry.job<Position>(0).each([&](ashiato::Entity current, Position&) {
        REQUIRE(registry.add<Velocity>(current, Velocity{}) != nullptr);
    });
    registry.run_jobs();
#endif
}

TEST_CASE("threaded job callback registry access throws in checked builds") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 4; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
    registry.job<Position>(0).max_threads(2).min_entities_per_thread(1).each(
        [&](ashiato::Entity entity, Position&) {
            (void)registry.get<Position>(entity);
        });
    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>& tasks) {
        for (const ashiato::JobThreadTask& task : tasks) {
            task.run();
        }
    });
    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
#else
    registry.job<Position>(0).max_threads(2).min_entities_per_thread(1).each(
        [&](ashiato::Entity entity, Position&) {
            REQUIRE(registry.get<Position>(entity).x >= 0);
        });
    registry.run_jobs();
#endif
}

TEST_CASE("jobs added while jobs are running wait until the next run or throw in checked jobs") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int outer_calls = 0;
    int inner_calls = 0;
    registry.job<Position>(0).each([&](ashiato::Entity, Position&) {
        ++outer_calls;
        registry.job<Position>(-1).each([&](ashiato::Entity, Position&) {
            ++inner_calls;
        });
    });

#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
    REQUIRE(outer_calls == 1);
    REQUIRE(inner_calls == 0);
#else
    registry.run_jobs();
    REQUIRE(outer_calls == 1);
    REQUIRE(inner_calls == 0);

    registry.run_jobs();
    REQUIRE(outer_calls == 2);
    REQUIRE(inner_calls == 1);
#endif
}
