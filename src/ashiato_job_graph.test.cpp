#include "ashiato_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("compiled job graphs run only supplied jobs") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{0, 0}) != nullptr);

    const ashiato::Entity first = registry.job<Position>(0).each([](ashiato::Entity, Position& position) {
        position.x += 1;
    });
    registry.job<Position>(1).each([](ashiato::Entity, Position& position) {
        position.y += 1;
    });

    ashiato::JobGraph graph = registry.compile_job_graph({first});
    graph.tick(registry);

    REQUIRE(registry.get<Position>(entity).x == 1);
    REQUIRE(registry.get<Position>(entity).y == 0);
}

TEST_CASE("compiled job graphs schedule only supplied job dependencies") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    registry.job<Position>(0).each([](ashiato::Entity, Position&) {});
    const ashiato::Entity position_reader = registry.job<const Position>(1).each([](ashiato::Entity, const Position&) {});
    const ashiato::Entity velocity_writer = registry.job<Velocity>(2).each([](ashiato::Entity, Velocity&) {});

    const ashiato::JobGraph full = registry.compile_all_jobs_graph();
    const ashiato::JobGraph subset = registry.compile_job_graph({position_reader, velocity_writer});

    REQUIRE(full.schedule().stages.size() == 2);
    REQUIRE(subset.schedule().stages.size() == 1);
    REQUIRE(subset.schedule().stages[0].jobs == std::vector<ashiato::Entity>{position_reader, velocity_writer});
}

TEST_CASE("compiled job graphs can run only target entities") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity first_entity = registry.create();
    const ashiato::Entity second_entity = registry.create();
    REQUIRE(registry.add<Position>(first_entity, Position{0, 0}) != nullptr);
    REQUIRE(registry.add<Position>(second_entity, Position{0, 0}) != nullptr);

    const ashiato::Entity job = registry.job<Position>(0).each([](ashiato::Entity, Position& position) {
        position.x += 1;
    });

    ashiato::JobGraph graph = registry.compile_job_graph({job});
    graph.tick_for_entities(registry, {second_entity});

    REQUIRE(registry.get<Position>(first_entity).x == 0);
    REQUIRE(registry.get<Position>(second_entity).x == 1);
}

TEST_CASE("compiled job graphs ignore empty stale and duplicate target entities") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity alive = registry.create();
    const ashiato::Entity stale = registry.create();
    REQUIRE(registry.add<Position>(alive, Position{0, 0}) != nullptr);
    REQUIRE(registry.add<Position>(stale, Position{0, 0}) != nullptr);
    REQUIRE(registry.destroy(stale));

    int calls = 0;
    const ashiato::Entity job = registry.job<Position>(0).each([&](ashiato::Entity entity, Position& position) {
        REQUIRE(entity == alive);
        ++calls;
        position.x += 1;
    });

    ashiato::JobGraph graph = registry.compile_job_graph({job});
    graph.tick_for_entities(registry, {});
    graph.tick_for_entities(registry, {stale});
    graph.tick_for_entities(registry, {alive, alive, stale});

    REQUIRE(calls == 1);
    REQUIRE(registry.get<Position>(alive).x == 1);
}

TEST_CASE("compiled job graphs honor excluded job tags for targeted runs") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{0, 0}) != nullptr);

    int first_calls = 0;
    int second_calls = 0;
    const ashiato::Entity first = registry.job<Position>(0).each([&](ashiato::Entity, Position& position) {
        ++first_calls;
        position.x += 1;
    });
    const ashiato::Entity second = registry.job<Position>(1).each([&](ashiato::Entity, Position& position) {
        ++second_calls;
        position.x += 10;
    });
    REQUIRE(registry.add<Disabled>(second));

    ashiato::RunJobsOptions options;
    options.excluded_job_tags = {registry.component<Disabled>()};
    registry.compile_job_graph({first, second}).tick_for_entities(registry, {entity}, options);

    REQUIRE(first_calls == 1);
    REQUIRE(second_calls == 0);
    REQUIRE(registry.get<Position>(entity).x == 1);
}

TEST_CASE("orchestrator returns no stages when no jobs are registered") {
    ashiato::Registry registry;

    const ashiato::JobSchedule schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.empty());
}

TEST_CASE("orchestrator batches read-only jobs for parallel execution") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity first = registry.job<const Position>(10).each([](ashiato::Entity, const Position&) {});
    const ashiato::Entity second = registry.job<const Position>(-1).each([](ashiato::Entity, const Position&) {});

    const ashiato::JobSchedule schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ashiato::Entity>{second, first});
}

TEST_CASE("orchestrator reuses stable schedules and invalidates them after job registration") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity reader = registry.job<const Position>(0).each([](ashiato::Entity, const Position&) {});

    const ashiato::JobSchedule first_schedule = ashiato::Orchestrator(registry).schedule();
    const ashiato::JobSchedule second_schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(first_schedule.stages.size() == 1);
    REQUIRE(first_schedule.stages[0].jobs == std::vector<ashiato::Entity>{reader});
    REQUIRE(second_schedule.stages.size() == first_schedule.stages.size());
    REQUIRE(second_schedule.stages[0].jobs == first_schedule.stages[0].jobs);

    const ashiato::Entity writer = registry.job<Position>(1).access_other_entities<Velocity>().each(
        [](auto&, ashiato::Entity, Position&) {});

    const ashiato::JobSchedule updated_schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(updated_schedule.stages.size() == 2);
    REQUIRE(updated_schedule.stages[0].jobs == std::vector<ashiato::Entity>{reader});
    REQUIRE(updated_schedule.stages[1].jobs == std::vector<ashiato::Entity>{writer});
}

TEST_CASE("orchestrator orders conflicting read and write jobs by canonical job order") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity writer = registry.job<Position>(10).each([](ashiato::Entity, Position&) {});
    const ashiato::Entity reader = registry.job<const Position>(20).each([](ashiato::Entity, const Position&) {});
    const ashiato::Entity later_writer = registry.job<Position>(20).each([](ashiato::Entity, Position&) {});

    const ashiato::JobSchedule schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 3);
    REQUIRE(schedule.stages[0].jobs == std::vector<ashiato::Entity>{writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ashiato::Entity>{reader});
    REQUIRE(schedule.stages[2].jobs == std::vector<ashiato::Entity>{later_writer});
}

TEST_CASE("orchestrator batches independent writes") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity position_writer = registry.job<Position>(0).each([](ashiato::Entity, Position&) {});
    const ashiato::Entity velocity_writer = registry.job<Velocity>(1).each([](ashiato::Entity, Velocity&) {});

    const ashiato::JobSchedule schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 1);
    REQUIRE(schedule.stages[0].jobs == std::vector<ashiato::Entity>{position_writer, velocity_writer});
}

TEST_CASE("orchestrator includes access view components in job conflicts") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity velocity_writer = registry.job<const Position>(0).access_other_entities<Velocity>().each(
        [](auto&, ashiato::Entity, const Position&) {});
    const ashiato::Entity velocity_reader = registry.job<const Velocity>(1).each([](ashiato::Entity, const Velocity&) {});
    const ashiato::Entity position_reader = registry.job<const Position>(2).access_other_entities<const Velocity>().each(
        [](auto&, ashiato::Entity, const Position&) {});

    const ashiato::JobSchedule schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ashiato::Entity>{velocity_writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ashiato::Entity>{velocity_reader, position_reader});
}

TEST_CASE("orchestrator includes optional components in job conflicts") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Health>("Health");

    const ashiato::Entity health_optional_writer = registry.job<const Position>(0).optional<Health>().each(
        [](auto&, ashiato::Entity, const Position&) {});
    const ashiato::Entity health_reader = registry.job<const Health>(1).each([](ashiato::Entity, const Health&) {});
    const ashiato::Entity health_optional_reader = registry.job<const Position>(2).optional<const Health>().each(
        [](auto&, ashiato::Entity, const Position&) {});

    const ashiato::JobSchedule schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ashiato::Entity>{health_optional_writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ashiato::Entity>{health_reader, health_optional_reader});
}

TEST_CASE("orchestrator includes typed tag filters in job conflicts") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity tag_writer = registry.job<const Position>(0).structural<Disabled>().each(
        [](auto&, ashiato::Entity, const Position&) {});
    const ashiato::Entity tag_filtered_reader = registry.job<const Position>(1).without_tags<const Disabled>().each(
        [](ashiato::Entity, const Position&) {});

    const ashiato::JobSchedule schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ashiato::Entity>{tag_writer});
    REQUIRE(schedule.stages[1].jobs == std::vector<ashiato::Entity>{tag_filtered_reader});
}
