#include "ecs_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("jobs run views in order and preserve insertion order for ties") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    std::vector<int> calls;
    registry.job<Position>(10).each([&](ecs::Entity current, Position&) {
        REQUIRE(current == entity);
        calls.push_back(10);
    });
    registry.job<Position>(-1).each([&](ecs::Entity current, Position&) {
        REQUIRE(current == entity);
        calls.push_back(-1);
    });
    registry.job<Position>(10).each([&](ecs::Entity current, Position&) {
        REQUIRE(current == entity);
        calls.push_back(11);
    });

    REQUIRE(calls.empty());
    registry.run_jobs();

    REQUIRE(calls == std::vector<int>{-1, 10, 11});
}

TEST_CASE("job registration returns alive entities and the orchestrator schedules them") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity job = registry.job<const Position>(0).each([](ecs::Entity, const Position&) {});

    REQUIRE(registry.alive(job));
    REQUIRE(registry.has(job, registry.system_tag()));

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();
    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{job});
}

TEST_CASE("run jobs can exclude jobs by job entity tag") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    const ecs::Entity job = registry.job<Position>(0).each([&](ecs::Entity, Position&) {
        ++calls;
    });
    REQUIRE(registry.add<Disabled>(job));

    const ecs::Entity disabled = registry.component<Disabled>();
    ecs::RunJobsOptions options;
    options.excluded_job_tags = {disabled};
    registry.run_jobs(options);
    REQUIRE(calls == 0);

    registry.run_jobs();
    REQUIRE(calls == 1);
}

TEST_CASE("run jobs for entities can exclude jobs by job entity tag") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    const ecs::Entity job = registry.job<Position>(0).each([&](ecs::Entity, Position&) {
        ++calls;
    });
    REQUIRE(registry.add<Disabled>(job));

    const ecs::Entity disabled = registry.component<Disabled>();
    ecs::RunJobsOptions options;
    options.excluded_job_tags = {disabled};
    registry.run_jobs_for_entities({entity}, options);
    REQUIRE(calls == 0);

    registry.run_jobs_for_entities({entity});
    REQUIRE(calls == 1);
}

TEST_CASE("jobs can filter iterated entities by typed tags") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Health>("Health");
    registry.register_component<Active>("Active");
    registry.register_component<Disabled>("Disabled");

    std::vector<ecs::Entity> entities;
    for (int i = 0; i < 4; ++i) {
        const ecs::Entity entity = registry.create();
        entities.push_back(entity);
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
        REQUIRE(registry.add<Health>(entity, Health{0}) != nullptr);
    }
    REQUIRE(registry.add<Disabled>(entities[1]));
    REQUIRE(registry.add<Disabled>(entities[3]));

    registry.job<Position>(0)
        .optional<Health>()
        .without_tags<const Disabled>()
        .max_threads(4)
        .min_entities_per_thread(1)
        .each([](auto& view, ecs::Entity, Position& position) {
            position.x += 10;
            view.template write<Health>().value += 1;
        });
    registry.job<const Position>(1)
        .without_tags<const Disabled>()
        .structural<Active>()
        .each([](auto& view, ecs::Entity entity, const Position&) {
            REQUIRE(view.template add<Active>(entity));
        });

    registry.set_job_thread_executor([](const std::vector<ecs::JobThreadTask>& tasks) {
        for (const ecs::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(registry.get<Position>(entities[0]).x == 10);
    REQUIRE(registry.get<Position>(entities[1]).x == 1);
    REQUIRE(registry.get<Position>(entities[2]).x == 12);
    REQUIRE(registry.get<Position>(entities[3]).x == 3);
    REQUIRE(registry.get<Health>(entities[0]).value == 1);
    REQUIRE(registry.get<Health>(entities[1]).value == 0);
    REQUIRE(registry.get<Health>(entities[2]).value == 1);
    REQUIRE(registry.get<Health>(entities[3]).value == 0);
    REQUIRE(registry.has<Active>(entities[0]));
    REQUIRE_FALSE(registry.has<Active>(entities[1]));
    REQUIRE(registry.has<Active>(entities[2]));
    REQUIRE_FALSE(registry.has<Active>(entities[3]));
}

TEST_CASE("jobs use live views when they run") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    int calls = 0;
    registry.job<Position>(0).each([&](ecs::Entity, Position&) {
        ++calls;
    });

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    registry.run_jobs();
    REQUIRE(calls == 1);
}

TEST_CASE("mutable job views mark iterated components dirty") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{3, 0}) != nullptr);
    registry.clear_all_dirty<Position>();

    registry.job<Position>(0).each([&](ecs::Entity current, Position& position) {
        REQUIRE(current == entity);
        REQUIRE(position.x == 3);
    });

    REQUIRE_FALSE(registry.is_dirty<Position>(entity));
    registry.run_jobs();
    REQUIRE(registry.is_dirty<Position>(entity));
}
