#include "ashiato_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>

TEST_CASE("run jobs batches independent jobs through the executor") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{2.0f, 0.0f}) != nullptr);

    registry.job<Position>(0).each([](ashiato::Entity, Position&) {});
    registry.job<Velocity>(1).each([](ashiato::Entity, Velocity&) {});

    std::vector<std::size_t> batch_sizes;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        batch_sizes.push_back(tasks.size());
        for (const ashiato::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(batch_sizes == std::vector<std::size_t>{2});
}

TEST_CASE("job executor must run every task before returning") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    registry.job<Position>(0).each([](ashiato::Entity, Position&) {});

    std::vector<ashiato::JobThreadTask> saved_tasks;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        saved_tasks = tasks;
    });

    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
    REQUIRE(saved_tasks.size() == 1);
    REQUIRE_THROWS_AS(saved_tasks.front().run(), std::logic_error);
}

TEST_CASE("job executor cannot run a task more than once") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    registry.job<Position>(0).each([&](ashiato::Entity, Position&) {
        ++calls;
    });

    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>& tasks) {
        REQUIRE(tasks.size() == 1);
        tasks.front().run();
        tasks.front().run();
    });

    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
    REQUIRE(calls == 1);
}

TEST_CASE("threaded jobs split entity ranges using max threads and minimum entity counts") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 5; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    registry.job<Position>(0).max_threads(3).min_entities_per_thread(2).each([](ashiato::Entity, Position& position) {
        position.y = position.x + 10;
    });

    std::vector<std::size_t> thread_indices;
    std::vector<std::size_t> thread_counts;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        for (const ashiato::JobThreadTask& task : tasks) {
            thread_indices.push_back(task.thread_index);
            thread_counts.push_back(task.thread_count);
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(thread_indices == std::vector<std::size_t>{0, 1, 2});
    REQUIRE(thread_counts == std::vector<std::size_t>{3, 3, 3});

    int visited = 0;
    registry.view<const Position>().each([&](ashiato::Entity, const Position& position) {
        REQUIRE(position.y == position.x + 10);
        ++visited;
    });
    REQUIRE(visited == 5);
}

TEST_CASE("threaded jobs defer dirty marking until split ranges complete") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    std::vector<ashiato::Entity> entities;
    for (int i = 0; i < 128; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
        entities.push_back(entity);
    }
    registry.clear_all_dirty<Position>();

    registry.job<Position>(0).max_threads(4).min_entities_per_thread(1).each(
        [](ashiato::Entity, Position& position) {
            position.y = position.x + 1000;
        });

    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>& tasks) {
        std::vector<std::thread> threads;
        threads.reserve(tasks.size());
        for (const ashiato::JobThreadTask& task : tasks) {
            const ashiato::JobThreadTask* task_ptr = &task;
            threads.emplace_back([task_ptr]() {
                task_ptr->run();
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    });

    registry.run_jobs();

    for (ashiato::Entity entity : entities) {
        const Position& position = registry.get<Position>(entity);
        REQUIRE(position.y == position.x + 1000);
        REQUIRE(registry.is_dirty<Position>(entity));
    }
}

TEST_CASE("threaded optional writes use deferred dirty logs without dirtying readonly optionals") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Health>("Health");

    std::vector<ashiato::Entity> with_health;
    std::vector<ashiato::Entity> without_health;
    for (int i = 0; i < 16; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
        if ((i % 2) == 0) {
            REQUIRE(registry.add<Health>(entity, Health{i}) != nullptr);
            with_health.push_back(entity);
        } else {
            without_health.push_back(entity);
        }
    }

    registry.clear_all_dirty<Position>();
    registry.clear_all_dirty<Health>();
    registry.job<Position>(0).optional<Health>().max_threads(4).min_entities_per_thread(1).each(
        [](auto& view, ashiato::Entity, Position& position) {
            position.y = position.x + 1;
            if (view.template contains<Health>()) {
                view.template write<Health>().value += 10;
            }
        });

    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>& tasks) {
        std::vector<std::thread> threads;
        threads.reserve(tasks.size());
        for (const ashiato::JobThreadTask& task : tasks) {
            const ashiato::JobThreadTask* task_ptr = &task;
            threads.emplace_back([task_ptr]() {
                task_ptr->run();
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    });

    registry.run_jobs();

    for (ashiato::Entity entity : with_health) {
        REQUIRE(registry.is_dirty<Position>(entity));
        REQUIRE(registry.is_dirty<Health>(entity));
        REQUIRE(registry.get<Health>(entity).value >= 10);
    }
    for (ashiato::Entity entity : without_health) {
        REQUIRE(registry.is_dirty<Position>(entity));
        REQUIRE_FALSE(registry.contains<Health>(entity));
        REQUIRE_FALSE(registry.is_dirty<Health>(entity));
    }
}

TEST_CASE("mutable singleton jobs stay single threaded even when max threads is requested") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<GameTime>("GameTime");

    for (int i = 0; i < 8; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }
    registry.clear_all_dirty<GameTime>();

    registry.job<Position, GameTime>(0).max_threads(4).min_entities_per_thread(1).each(
        [](ashiato::Entity, Position&, GameTime& time) {
            ++time.tick;
        });

    std::vector<std::size_t> thread_counts;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        for (const ashiato::JobThreadTask& task : tasks) {
            thread_counts.push_back(task.thread_count);
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(thread_counts == std::vector<std::size_t>{1});
    REQUIRE(registry.get<GameTime>().tick == 8);
    REQUIRE(registry.is_dirty<GameTime>());
}

TEST_CASE("force single threaded run ignores executor chunking") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 4; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    int calls = 0;
    registry.job<Position>(0).max_threads(4).min_entities_per_thread(1).each([&](ashiato::Entity, Position&) {
        ++calls;
    });

    int executor_calls = 0;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        ++executor_calls;
        for (const ashiato::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    registry.run_jobs(ashiato::RunJobsOptions{true});

    REQUIRE(calls == 4);
    REQUIRE(executor_calls == 0);
}

TEST_CASE("threaded job executor must run every task before returning") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 4; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    registry.job<Position>(0)
        .max_threads(4)
        .min_entities_per_thread(1)
        .each([](ashiato::Entity, Position& position) {
            position.x += 1;
        });
    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>&) {});

    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
}

TEST_CASE("threaded job executor cannot run a task after returning") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 2; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    std::vector<ashiato::JobThreadTask> captured;
    registry.job<Position>(0)
        .max_threads(2)
        .min_entities_per_thread(1)
        .each([](ashiato::Entity, Position&) {});
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        captured = tasks;
    });

    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
    REQUIRE_FALSE(captured.empty());
    REQUIRE_THROWS_AS(captured.front().run(), std::logic_error);
}

TEST_CASE("threaded job exceptions are rethrown after task completion") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    registry.job<Position>(0)
        .max_threads(2)
        .min_entities_per_thread(1)
        .each([](ashiato::Entity, Position&) {
            throw std::runtime_error("job failed");
        });
    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>& tasks) {
        for (const ashiato::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    REQUIRE_THROWS_AS(registry.run_jobs(), std::runtime_error);
}

TEST_CASE("structural jobs expose declared add and remove operations and stay single threaded") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    registry.job<const Position>(0).max_threads(4).min_entities_per_thread(1).structural<Disabled>().each(
        [](auto& view, ashiato::Entity current, const Position&) {
            REQUIRE(view.template add<Disabled>(current));
        });

    std::vector<std::size_t> batch_sizes;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        batch_sizes.push_back(tasks.size());
        for (const ashiato::JobThreadTask& task : tasks) {
            REQUIRE(task.thread_count == 1);
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(batch_sizes == std::vector<std::size_t>{1});
    REQUIRE(registry.has<Disabled>(entity));

    registry.job<const Position>(1).structural<Disabled>().each([](auto& view, ashiato::Entity current, const Position&) {
        REQUIRE(view.template remove<Disabled>(current));
    });

    registry.run_jobs();

    REQUIRE_FALSE(registry.has<Disabled>(entity));
}

TEST_CASE("structural jobs are isolated from otherwise independent jobs") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity structural =
        registry.job<const Position>(0).structural<Disabled>().each([](auto&, ashiato::Entity, const Position&) {});
    const ashiato::Entity independent = registry.job<Velocity>(1).each([](ashiato::Entity, Velocity&) {});

    const ashiato::JobSchedule schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ashiato::Entity>{structural});
    REQUIRE(schedule.stages[1].jobs == std::vector<ashiato::Entity>{independent});
}

TEST_CASE("structural access jobs can use access views and declared structural operations") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{2.0f, 0.0f}) != nullptr);

    registry.job<const Position>(0).access_other_entities<Velocity>().structural<Disabled>().each(
        [](auto& view, ashiato::Entity current, const Position& position) {
            Velocity& velocity = view.template write<Velocity>(current);
            velocity.dx += static_cast<float>(position.x);
            REQUIRE(view.template add<Disabled>(current));
        });

    registry.run_jobs();

    REQUIRE(registry.get<Velocity>(entity).dx == 3.0f);
    REQUIRE(registry.has<Disabled>(entity));
}
