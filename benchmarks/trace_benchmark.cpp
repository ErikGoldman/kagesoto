#include "benchmark_common.hpp"

#include <cstdint>
#include <vector>

namespace {

struct TracePosition {
    float x{0.0f};
    float y{0.0f};
};

struct TraceVelocity {
    float x{1.0f};
    float y{1.0f};
};

constexpr std::uint32_t kTraceHistory = 64;

void seed_entities(ecs::Registry& registry, std::int64_t entity_count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(entity_count));
    for (std::int64_t i = 0; i < entity_count; ++i) {
        entities.push_back(registry.create());
    }

    auto tx = registry.transaction<TracePosition, TraceVelocity>();
    for (const ecs::Entity entity : entities) {
        tx.write<TracePosition>(entity, TracePosition{0.0f, 0.0f});
        tx.write<TraceVelocity>(entity, TraceVelocity{1.0f, 1.0f});
    }
    tx.commit();
}

void benchmark_high_frequency_position_trace(
    benchmark::State& state,
    ecs::ComponentStorageMode position_mode) {

    ecs::Registry registry;
    registry.set_trace_max_history(kTraceHistory);
    registry.set_storage_mode<TracePosition>(position_mode);
    registry.set_storage_mode<TraceVelocity>(ecs::ComponentStorageMode::mvcc);
    seed_entities(registry, state.range(0));

    ecs::Timestamp trace_time = 1;
    float checksum = 0.0f;

    for (auto _ : state) {
        registry.set_current_trace_time(trace_time++);

        auto tx = registry.transaction<TracePosition, const TraceVelocity>();
        auto positions = tx.storage<TracePosition>();
        positions.each([&](ecs::Entity entity, const TracePosition&) {
            const TraceVelocity* velocity = tx.try_get<TraceVelocity>(entity);
            TracePosition* position = tx.write<TracePosition>(entity);
            position->x += velocity->x;
            position->y += velocity->y;
            checksum += position->x;
        });
        tx.commit();
    }

    benchmark::DoNotOptimize(checksum);
    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.counters["components"] = static_cast<double>(state.range(0));
    state.counters["trace_history"] = static_cast<double>(kTraceHistory);
}

void BM_TraceOndemandHighFrequencyPosition(benchmark::State& state) {
    benchmark_high_frequency_position_trace(state, ecs::ComponentStorageMode::trace_ondemand);
}
BENCHMARK(BM_TraceOndemandHighFrequencyPosition)->Apply(ecs::benchmarks::ApplySmallArguments);

void BM_TracePreallocateHighFrequencyPosition(benchmark::State& state) {
    benchmark_high_frequency_position_trace(state, ecs::ComponentStorageMode::trace_preallocate);
}
BENCHMARK(BM_TracePreallocateHighFrequencyPosition)->Apply(ecs::benchmarks::ApplySmallArguments);

}  // namespace

int main(int argc, char** argv) {
    ecs::benchmarks::AddCommonContext(false);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
