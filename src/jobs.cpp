#include "ecs/ecs.hpp"

namespace ecs {

namespace {

struct JobTaskBatchState {
    explicit JobTaskBatchState(std::size_t expected_tasks)
        : expected(expected_tasks) {}

    const std::size_t expected = 0;
    std::atomic<std::size_t> started{0};
    std::atomic<std::size_t> finished{0};
    std::atomic<bool> closed{false};
    std::atomic<bool> contract_violated{false};
};

struct JobTaskRunState {
    std::atomic<bool> started{false};
};

void record_job_task_exception(
    std::exception_ptr exception,
    std::exception_ptr& first_exception,
    std::mutex& exception_mutex) {
    std::lock_guard<std::mutex> lock(exception_mutex);
    if (first_exception == nullptr) {
        first_exception = exception;
    }
}

void throw_job_executor_contract_violation(
    const std::shared_ptr<JobTaskBatchState>& batch,
    const char* message) {
    batch->contract_violated.store(true, std::memory_order_release);
    throw std::logic_error(message);
}

JobThreadTask make_guarded_job_task(
    JobThreadTask task,
    const std::shared_ptr<JobTaskBatchState>& batch,
    std::exception_ptr& first_exception,
    std::mutex& exception_mutex) {
    auto run_state = std::make_shared<JobTaskRunState>();
    task.run = [run = std::move(task.run), batch, run_state, &first_exception, &exception_mutex]() mutable {
        if (batch->closed.load(std::memory_order_acquire)) {
            throw_job_executor_contract_violation(batch, "ecs job task cannot run after the executor returns");
        }

        bool expected = false;
        if (!run_state->started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            throw_job_executor_contract_violation(batch, "ecs job task cannot run more than once");
        }

        batch->started.fetch_add(1, std::memory_order_acq_rel);
        try {
            run();
        } catch (...) {
            record_job_task_exception(std::current_exception(), first_exception, exception_mutex);
        }
        batch->finished.fetch_add(1, std::memory_order_acq_rel);
    };
    return task;
}

void close_and_validate_job_task_batch(const std::shared_ptr<JobTaskBatchState>& batch) {
    batch->closed.store(true, std::memory_order_release);
    if (batch->contract_violated.load(std::memory_order_acquire)) {
        throw std::logic_error("ecs job executor violated the task execution contract");
    }
    if (batch->started.load(std::memory_order_acquire) != batch->expected ||
        batch->finished.load(std::memory_order_acquire) != batch->expected) {
        throw std::logic_error("ecs job executor must run every task to completion before returning");
    }
}

}  // namespace

Orchestrator::Orchestrator(const Registry& registry)
    : registry_(&registry) {}

JobSchedule Orchestrator::schedule() const {
    if (registry_->job_registry_.schedule_cache_valid) {
        return registry_->job_registry_.cached_schedule;
    }

    JobSchedule result = build_schedule(registry_->ordered_job_indices());
    registry_->job_registry_.cached_schedule = result;
    registry_->job_registry_.schedule_cache_valid = true;
    return result;
}

JobSchedule Orchestrator::schedule_for_jobs(const std::vector<Entity>& jobs) const {
    std::vector<std::size_t> ordered_indices;
    ordered_indices.reserve(jobs.size());
    for (Entity job : jobs) {
        ordered_indices.push_back(registry_->job_index(job));
    }
    std::sort(
        ordered_indices.begin(),
        ordered_indices.end(),
        [&](std::size_t lhs, std::size_t rhs) {
            const Registry::JobRecord& left = registry_->job_registry_.jobs[lhs];
            const Registry::JobRecord& right = registry_->job_registry_.jobs[rhs];
            if (left.order != right.order) {
                return left.order < right.order;
            }
            return left.sequence < right.sequence;
        });
    ordered_indices.erase(std::unique(ordered_indices.begin(), ordered_indices.end()), ordered_indices.end());
    return build_schedule(ordered_indices);
}

JobSchedule Orchestrator::build_schedule(const std::vector<std::size_t>& ordered_indices) const {
    JobSchedule result;

    std::unordered_map<std::uint32_t, std::size_t> last_reader_stage;
    std::unordered_map<std::uint32_t, std::size_t> last_writer_stage;
    std::size_t latest_stage = 0;
    std::size_t structural_barrier_stage = 0;
    bool has_previous_job = false;
    bool has_structural_barrier = false;

    for (std::size_t job_index : ordered_indices) {
        const Registry::JobRecord& job = registry_->job_registry_.jobs[job_index];

        std::size_t stage_index = has_structural_barrier ? structural_barrier_stage + 1 : 0;
        if (job.structural) {
            stage_index = has_previous_job ? latest_stage + 1 : 0;
        } else {
            apply_read_dependencies(job.reads, last_writer_stage, stage_index);
            apply_write_dependencies(job.writes, last_reader_stage, last_writer_stage, stage_index);
        }

        if (result.stages.size() <= stage_index) {
            result.stages.resize(stage_index + 1);
        }
        result.stages[stage_index].jobs.push_back(job.entity);

        latest_stage = has_previous_job ? std::max(latest_stage, stage_index) : stage_index;
        has_previous_job = true;

        if (job.structural) {
            structural_barrier_stage = stage_index;
            has_structural_barrier = true;
            continue;
        }

        record_read_stage(job.reads, last_reader_stage, stage_index);
        record_write_stage(job.writes, last_writer_stage, stage_index);
    }

    return result;
}

void Orchestrator::apply_read_dependencies(
    const std::vector<std::uint32_t>& reads,
    const std::unordered_map<std::uint32_t, std::size_t>& last_writer_stage,
    std::size_t& stage_index) {
    for (std::uint32_t component : reads) {
        const auto found = last_writer_stage.find(component);
        if (found != last_writer_stage.end()) {
            stage_index = std::max(stage_index, found->second + 1);
        }
    }
}

void Orchestrator::apply_write_dependencies(
    const std::vector<std::uint32_t>& writes,
    const std::unordered_map<std::uint32_t, std::size_t>& last_reader_stage,
    const std::unordered_map<std::uint32_t, std::size_t>& last_writer_stage,
    std::size_t& stage_index) {
    for (std::uint32_t component : writes) {
        const auto found_reader = last_reader_stage.find(component);
        if (found_reader != last_reader_stage.end()) {
            stage_index = std::max(stage_index, found_reader->second + 1);
        }
        const auto found_writer = last_writer_stage.find(component);
        if (found_writer != last_writer_stage.end()) {
            stage_index = std::max(stage_index, found_writer->second + 1);
        }
    }
}

void Orchestrator::record_read_stage(
    const std::vector<std::uint32_t>& reads,
    std::unordered_map<std::uint32_t, std::size_t>& last_reader_stage,
    std::size_t stage_index) {
    for (std::uint32_t component : reads) {
        auto inserted = last_reader_stage.emplace(component, stage_index);
        if (!inserted.second) {
            inserted.first->second = std::max(inserted.first->second, stage_index);
        }
    }
}

void Orchestrator::record_write_stage(
    const std::vector<std::uint32_t>& writes,
    std::unordered_map<std::uint32_t, std::size_t>& last_writer_stage,
    std::size_t stage_index) {
    for (std::uint32_t component : writes) {
        last_writer_stage[component] = stage_index;
    }
}

void Registry::mark_component_dirty_range(
    const std::vector<std::uint32_t>& components,
    const std::vector<std::uint32_t>& indices,
    std::size_t begin,
    std::size_t end) {
    for (std::uint32_t component : components) {
        auto found = storage_registry_.storages.find(component);
        if (found == storage_registry_.storages.end()) {
            continue;
        }
        TypeErasedStorage& storage = *found->second;
        for (std::size_t position = begin; position < end; ++position) {
            storage.mark_dirty(indices[position]);
        }
    }
}

void Registry::merge_deferred_dirty_writes(const TypeErasedStorage::DeferredDirtyWrites& writes) {
    for (const TypeErasedStorage::DeferredDirtyWrite& write : writes) {
        if (write.storage != nullptr) {
            write.storage->mark_dirty(write.index);
        }
    }
}

void JobGraph::tick(Registry& registry, RunJobsOptions options) const {
    const std::size_t job_count = registry.job_registry_.jobs.size();
    if (options.force_single_threaded) {
        for (const JobScheduleStage& stage : schedule_.stages) {
            for (Entity job_entity : stage.jobs) {
                const std::size_t index = registry.job_index(job_entity);
                if (index >= job_count || registry.job_excluded_by_options(registry.job_registry_.jobs[index], options)) {
                    continue;
                }
                registry.job_registry_.jobs[index].run(registry);
            }
        }
        return;
    }

    std::exception_ptr first_exception;
    std::mutex exception_mutex;

    for (const JobScheduleStage& stage : schedule_.stages) {
        std::vector<JobThreadTask> tasks;
        struct DirtyRange {
            const std::vector<std::uint32_t>* components = nullptr;
            std::shared_ptr<std::vector<std::uint32_t>> indices;
            std::size_t begin = 0;
            std::size_t end = 0;
        };
        std::vector<DirtyRange> dirty_ranges;
        std::vector<std::shared_ptr<Registry::TypeErasedStorage::DeferredDirtyWrites>> dirty_writes;
        for (Entity job_entity : stage.jobs) {
            const std::size_t index = registry.job_index(job_entity);
            if (index >= job_count) {
                continue;
            }

            Registry::JobRecord& job = registry.job_registry_.jobs[index];
            if (registry.job_excluded_by_options(job, options)) {
                continue;
            }
            if (job.single_thread || job.structural || job.max_threads <= 1) {
                tasks.push_back(JobThreadTask{
                    job.entity,
                    0,
                    1,
                    [&registry, index]() {
                        registry.job_registry_.jobs[index].run(registry);
                    }});
                continue;
            }

            auto indices = std::make_shared<std::vector<std::uint32_t>>(job.collect_indices(registry));
            const std::size_t entity_count = indices->size();
            if (entity_count == 0) {
                tasks.push_back(JobThreadTask{
                    job.entity,
                    0,
                    1,
                    [&registry, index]() {
                        registry.job_registry_.jobs[index].run(registry);
                    }});
                continue;
            }

            const std::size_t desired_threads =
                std::max<std::size_t>(1, (entity_count + job.min_entities_per_thread - 1) / job.min_entities_per_thread);
            const std::size_t thread_count = std::min(job.max_threads, desired_threads);
            for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
                const std::size_t begin = entity_count * thread_index / thread_count;
                const std::size_t end = entity_count * (thread_index + 1) / thread_count;
                auto writes = std::make_shared<Registry::TypeErasedStorage::DeferredDirtyWrites>();
                dirty_writes.push_back(writes);
                dirty_ranges.push_back(DirtyRange{&job.range_dirty_writes, indices, begin, end});
                tasks.push_back(JobThreadTask{
                    job.entity,
                    thread_index,
                    thread_count,
                    [&registry, index, indices, begin, end, writes]() {
                        Registry::TypeErasedStorage::DeferredDirtyScope dirty_scope(*writes, true);
                        registry.job_registry_.jobs[index].run_range(registry, *indices, begin, end);
                    }});
            }
        }

        auto batch = std::make_shared<JobTaskBatchState>(tasks.size());
        for (JobThreadTask& task : tasks) {
            task = make_guarded_job_task(std::move(task), batch, first_exception, exception_mutex);
        }
        if (registry.job_registry_.thread_executor) {
            registry.job_registry_.thread_executor(tasks);
        } else {
            for (const JobThreadTask& task : tasks) {
                task.run();
            }
        }
        close_and_validate_job_task_batch(batch);

        for (const DirtyRange& range : dirty_ranges) {
            registry.mark_component_dirty_range(*range.components, *range.indices, range.begin, range.end);
        }
        for (const auto& writes : dirty_writes) {
            registry.merge_deferred_dirty_writes(*writes);
        }

        if (first_exception != nullptr) {
            std::rethrow_exception(first_exception);
        }
    }
}

void JobGraph::tick_for_entities(
    Registry& registry,
    const std::vector<Entity>& entities,
    RunJobsOptions options) const {
    if (entities.empty()) {
        return;
    }

    std::vector<std::uint32_t> target_indices;
    target_indices.reserve(entities.size());
    for (Entity entity : entities) {
        if (registry.alive(entity)) {
            target_indices.push_back(Registry::entity_index(entity));
        }
    }
    if (target_indices.empty()) {
        return;
    }
    std::sort(target_indices.begin(), target_indices.end());
    target_indices.erase(std::unique(target_indices.begin(), target_indices.end()), target_indices.end());

    const std::size_t job_count = registry.job_registry_.jobs.size();
    if (options.force_single_threaded) {
        for (const JobScheduleStage& stage : schedule_.stages) {
            for (Entity job_entity : stage.jobs) {
                const std::size_t index = registry.job_index(job_entity);
                if (index >= job_count || registry.job_excluded_by_options(registry.job_registry_.jobs[index], options)) {
                    continue;
                }
                std::vector<std::uint32_t> indices = registry.job_registry_.jobs[index].collect_indices(registry);
                indices.erase(
                    std::remove_if(
                        indices.begin(),
                        indices.end(),
                        [&](std::uint32_t entity_index) {
                            return !std::binary_search(target_indices.begin(), target_indices.end(), entity_index);
                        }),
                    indices.end());
                if (!indices.empty()) {
                    registry.job_registry_.jobs[index].run_range(registry, indices, 0, indices.size());
                }
            }
        }
        return;
    }

    std::exception_ptr first_exception;
    std::mutex exception_mutex;

    for (const JobScheduleStage& stage : schedule_.stages) {
        std::vector<JobThreadTask> tasks;
        struct DirtyRange {
            const std::vector<std::uint32_t>* components = nullptr;
            std::shared_ptr<std::vector<std::uint32_t>> indices;
            std::size_t begin = 0;
            std::size_t end = 0;
        };
        std::vector<DirtyRange> dirty_ranges;
        std::vector<std::shared_ptr<Registry::TypeErasedStorage::DeferredDirtyWrites>> dirty_writes;
        for (Entity job_entity : stage.jobs) {
            const std::size_t index = registry.job_index(job_entity);
            if (index >= job_count) {
                continue;
            }

            Registry::JobRecord& job = registry.job_registry_.jobs[index];
            if (registry.job_excluded_by_options(job, options)) {
                continue;
            }
            auto indices = std::make_shared<std::vector<std::uint32_t>>(job.collect_indices(registry));
            indices->erase(
                std::remove_if(
                    indices->begin(),
                    indices->end(),
                    [&](std::uint32_t entity_index) {
                        return !std::binary_search(target_indices.begin(), target_indices.end(), entity_index);
                    }),
                indices->end());
            if (indices->empty()) {
                continue;
            }

            if (job.single_thread || job.structural || job.max_threads <= 1) {
                tasks.push_back(JobThreadTask{
                    job.entity,
                    0,
                    1,
                    [&registry, index, indices]() {
                        registry.job_registry_.jobs[index].run_range(registry, *indices, 0, indices->size());
                    }});
                continue;
            }

            const std::size_t entity_count = indices->size();
            const std::size_t desired_threads =
                std::max<std::size_t>(1, (entity_count + job.min_entities_per_thread - 1) / job.min_entities_per_thread);
            const std::size_t thread_count = std::min(job.max_threads, desired_threads);
            for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
                const std::size_t begin = entity_count * thread_index / thread_count;
                const std::size_t end = entity_count * (thread_index + 1) / thread_count;
                auto writes = std::make_shared<Registry::TypeErasedStorage::DeferredDirtyWrites>();
                dirty_writes.push_back(writes);
                dirty_ranges.push_back(DirtyRange{&job.range_dirty_writes, indices, begin, end});
                tasks.push_back(JobThreadTask{
                    job.entity,
                    thread_index,
                    thread_count,
                    [&registry, index, indices, begin, end, writes]() {
                        Registry::TypeErasedStorage::DeferredDirtyScope dirty_scope(*writes, true);
                        registry.job_registry_.jobs[index].run_range(registry, *indices, begin, end);
                    }});
            }
        }

        auto batch = std::make_shared<JobTaskBatchState>(tasks.size());
        for (JobThreadTask& task : tasks) {
            task = make_guarded_job_task(std::move(task), batch, first_exception, exception_mutex);
        }
        if (registry.job_registry_.thread_executor) {
            registry.job_registry_.thread_executor(tasks);
        } else {
            for (const JobThreadTask& task : tasks) {
                task.run();
            }
        }
        close_and_validate_job_task_batch(batch);

        for (const DirtyRange& range : dirty_ranges) {
            registry.mark_component_dirty_range(*range.components, *range.indices, range.begin, range.end);
        }
        for (const auto& writes : dirty_writes) {
            registry.merge_deferred_dirty_writes(*writes);
        }

        if (first_exception != nullptr) {
            std::rethrow_exception(first_exception);
        }
    }
}

JobGraph Registry::compile_job_graph(const std::vector<Entity>& jobs) const {
    require_runtime_registry_access_allowed("compile_job_graph");
    return JobGraph(Orchestrator(*this).schedule_for_jobs(jobs));
}

JobGraph Registry::compile_all_jobs_graph() const {
    require_runtime_registry_access_allowed("compile_all_jobs_graph");
    return JobGraph(Orchestrator(*this).schedule());
}

const JobGraph& Registry::cached_all_jobs_graph() const {
    if (!job_registry_.graph_cache_valid) {
        job_registry_.cached_graph = compile_all_jobs_graph();
        job_registry_.graph_cache_valid = true;
    }
    return job_registry_.cached_graph;
}

void Registry::run_jobs(RunJobsOptions options) {
    require_runtime_registry_access_allowed("run_jobs");
    cached_all_jobs_graph().tick(*this, options);
}

void Registry::run_jobs_for_entities(const std::vector<Entity>& entities, RunJobsOptions options) {
    require_runtime_registry_access_allowed("run_jobs_for_entities");
    cached_all_jobs_graph().tick_for_entities(*this, entities, options);
}

}  // namespace ecs
