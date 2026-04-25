#include "benchmark_common.hpp"

#include <cstdint>
#include <vector>

namespace {

struct TracePosition {
    float x{0.0f};
    float y{0.0f};
};

struct PreallocatedTracePosition {
    float x{0.0f};
    float y{0.0f};
};

struct TraceVelocity {
    float x{1.0f};
    float y{1.0f};
};

constexpr std::uint32_t kTraceHistory = 64;

}  // namespace

namespace ecs {

template <>
struct ComponentTraceStorageTraits<PreallocatedTracePosition> {
    static constexpr ComponentTraceStorage value = ComponentTraceStorage::preallocated;
};

}  // namespace ecs

namespace {

template <typename PositionComponent>
void seed_entities(ecs::Registry& registry, std::int64_t entity_count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(entity_count));
    for (std::int64_t i = 0; i < entity_count; ++i) {
        entities.push_back(registry.create());
    }

    auto tx = registry.transaction<PositionComponent, TraceVelocity>();
    for (const ecs::Entity entity : entities) {
        tx.template write<PositionComponent>(entity, PositionComponent{0.0f, 0.0f});
        tx.template write<TraceVelocity>(entity, TraceVelocity{1.0f, 1.0f});
    }
    tx.commit();
}

template <typename PositionComponent>
void benchmark_high_frequency_position_trace(
    benchmark::State& state) {

    ecs::Registry registry;
    registry.set_trace_max_history(kTraceHistory);
    seed_entities<PositionComponent>(registry, state.range(0));
    registry.set_tracing_enabled(true);

    ecs::Timestamp trace_time = 1;
    float checksum = 0.0f;

    for (auto _ : state) {
        registry.set_current_trace_time(trace_time++);

        auto tx = registry.transaction<PositionComponent, const TraceVelocity>();
        auto positions = tx.template storage<PositionComponent>();
        positions.each([&](ecs::Entity entity, const PositionComponent&) {
            const TraceVelocity* velocity = tx.template try_get<TraceVelocity>(entity);
            PositionComponent* position = tx.template write<PositionComponent>(entity);
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

void BM_TraceCopyOnWriteHighFrequencyPosition(benchmark::State& state) {
    benchmark_high_frequency_position_trace<TracePosition>(state);
}
BENCHMARK(BM_TraceCopyOnWriteHighFrequencyPosition)->Apply(ecs::benchmarks::ApplySmallArguments);

void BM_TracePreallocateHighFrequencyPosition(benchmark::State& state) {
    benchmark_high_frequency_position_trace<PreallocatedTracePosition>(state);
}
BENCHMARK(BM_TracePreallocateHighFrequencyPosition)->Apply(ecs::benchmarks::ApplySmallArguments);

}  // namespace

int main(int argc, char** argv) {
    ecs::benchmarks::AddCommonContext(false);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
