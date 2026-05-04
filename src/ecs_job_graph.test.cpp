#include "ecs_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("compiled job graphs run only supplied jobs") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{0, 0}) != nullptr);

    const ecs::Entity first = registry.job<Position>(0).each([](ecs::Entity, Position& position) {
        position.x += 1;
    });
    registry.job<Position>(1).each([](ecs::Entity, Position& position) {
        position.y += 1;
    });

    ecs::JobGraph graph = registry.compile_job_graph({first});
    graph.tick(registry);

    REQUIRE(registry.get<Position>(entity).x == 1);
    REQUIRE(registry.get<Position>(entity).y == 0);
}

TEST_CASE("compiled job graphs schedule only supplied job dependencies") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.job<Position>(0).each([](ecs::Entity, Position&) {});
    const ecs::Entity position_reader = registry.job<const Position>(1).each([](ecs::Entity, const Position&) {});
    const ecs::Entity velocity_writer = registry.job<Velocity>(2).each([](ecs::Entity, Velocity&) {});

    const ecs::JobGraph full = registry.compile_all_jobs_graph();
    const ecs::JobGraph subset = registry.compile_job_graph({position_reader, velocity_writer});

    REQUIRE(full.schedule().stages.size() == 2);
    REQUIRE(subset.schedule().stages.size() == 1);
    REQUIRE(subset.schedule().stages[0].jobs == std::vector<ecs::Entity>{position_reader, velocity_writer});
}

TEST_CASE("compiled job graphs can run only target entities") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity first_entity = registry.create();
    const ecs::Entity second_entity = registry.create();
    REQUIRE(registry.add<Position>(first_entity, Position{0, 0}) != nullptr);
    REQUIRE(registry.add<Position>(second_entity, Position{0, 0}) != nullptr);

    const ecs::Entity job = registry.job<Position>(0).each([](ecs::Entity, Position& position) {
        position.x += 1;
    });

    ecs::JobGraph graph = registry.compile_job_graph({job});
    graph.tick_for_entities(registry, {second_entity});

    REQUIRE(registry.get<Position>(first_entity).x == 0);
    REQUIRE(registry.get<Position>(second_entity).x == 1);
}

TEST_CASE("orchestrator returns no stages when no jobs are registered") {
    ecs::Registry registry;

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.empty());
}

TEST_CASE("orchestrator batches read-only jobs for parallel execution") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity first = registry.job<const Position>(10).each([](ecs::Entity, const Position&) {});
    const ecs::Entity second = registry.job<const Position>(-1).each([](ecs::Entity, const Position&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{second, first});
}

TEST_CASE("orchestrator reuses stable schedules and invalidates them after job registration") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity reader = registry.job<const Position>(0).each([](ecs::Entity, const Position&) {});

    const ecs::JobSchedule first_schedule = ecs::Orchestrator(registry).schedule();
    const ecs::JobSchedule second_schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(first_schedule.stages.size() == 1);
    REQUIRE(first_schedule.stages[0].jobs == std::vector<ecs::Entity>{reader});
    REQUIRE(second_schedule.stages.size() == first_schedule.stages.size());
    REQUIRE(second_schedule.stages[0].jobs == first_schedule.stages[0].jobs);

    const ecs::Entity writer = registry.job<Position>(1).access_other_entities<Velocity>().each(
        [](auto&, ecs::Entity, Position&) {});

    const ecs::JobSchedule updated_schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(updated_schedule.stages.size() == 2);
    REQUIRE(updated_schedule.stages[0].jobs == std::vector<ecs::Entity>{reader});
    REQUIRE(updated_schedule.stages[1].jobs == std::vector<ecs::Entity>{writer});
}

TEST_CASE("orchestrator orders conflicting read and write jobs by canonical job order") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");

    const ecs::Entity writer = registry.job<Position>(10).each([](ecs::Entity, Position&) {});
    const ecs::Entity reader = registry.job<const Position>(20).each([](ecs::Entity, const Position&) {});
    const ecs::Entity later_writer = registry.job<Position>(20).each([](ecs::Entity, Position&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 3);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ecs::Entity>{reader});
    REQUIRE(schedule.stages[2].jobs == std::vector<ecs::Entity>{later_writer});
}

TEST_CASE("orchestrator batches independent writes") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity position_writer = registry.job<Position>(0).each([](ecs::Entity, Position&) {});
    const ecs::Entity velocity_writer = registry.job<Velocity>(1).each([](ecs::Entity, Velocity&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{position_writer, velocity_writer});
}

TEST_CASE("orchestrator includes access view components in job conflicts") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ecs::Entity velocity_writer = registry.job<const Position>(0).access_other_entities<Velocity>().each(
        [](auto&, ecs::Entity, const Position&) {});
    const ecs::Entity velocity_reader = registry.job<const Velocity>(1).each([](ecs::Entity, const Velocity&) {});
    const ecs::Entity position_reader = registry.job<const Position>(2).access_other_entities<const Velocity>().each(
        [](auto&, ecs::Entity, const Position&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{velocity_writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ecs::Entity>{velocity_reader, position_reader});
}

TEST_CASE("orchestrator includes optional components in job conflicts") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Health>("Health");

    const ecs::Entity health_optional_writer = registry.job<const Position>(0).optional<Health>().each(
        [](auto&, ecs::Entity, const Position&) {});
    const ecs::Entity health_reader = registry.job<const Health>(1).each([](ecs::Entity, const Health&) {});
    const ecs::Entity health_optional_reader = registry.job<const Position>(2).optional<const Health>().each(
        [](auto&, ecs::Entity, const Position&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{health_optional_writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ecs::Entity>{health_reader, health_optional_reader});
}

TEST_CASE("orchestrator includes typed tag filters in job conflicts") {
    ecs::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ecs::Entity tag_writer = registry.job<const Position>(0).structural<Disabled>().each(
        [](auto&, ecs::Entity, const Position&) {});
    const ecs::Entity tag_filtered_reader = registry.job<const Position>(1).without_tags<const Disabled>().each(
        [](ecs::Entity, const Position&) {});

    const ecs::JobSchedule schedule = ecs::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ecs::Entity>{tag_writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ecs::Entity>{tag_filtered_reader});
}
