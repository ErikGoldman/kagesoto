#include <cstddef>
#include <cstdint>
#include <vector>

#include "benchmark_common.hpp"

namespace {

using ecs::benchmarks::DataComponent;
using ecs::benchmarks::PositionComponent;
using ecs::benchmarks::RandomXoshiro128;
using ecs::benchmarks::VelocityComponent;

struct StressReadComponent {
    std::uint32_t value{0};
};

struct StressWriteComponent {
    std::uint64_t value{0};
};

constexpr std::uint32_t kRandomAddSeed = 0xA5C31F27u;
constexpr std::uint32_t kRandomReadSeed = 0x17D2B6C5u;
constexpr std::int64_t kStressMaxEntitiesRange = 262'144;

void ApplyStressArguments(benchmark::internal::Benchmark* benchmark_target) {
    ecs::benchmarks::ApplyArguments(benchmark_target, kStressMaxEntitiesRange);
}

std::uint32_t make_seeded_value(std::size_t index) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(index) * 2'654'435'761ull) ^ 0x9E3779B9u);
}

void create_entities_with_stress_components(ecs::Registry& registry,
                                            std::size_t entity_count,
                                            std::vector<ecs::Entity>& entities) {
    entities.clear();
    entities.reserve(entity_count);

    for (std::size_t i = 0; i < entity_count; ++i) {
        const ecs::Entity entity = registry.create();
        auto tx = registry.transaction();
        PositionComponent* position = tx.write<PositionComponent>(entity);
        VelocityComponent* velocity = tx.write<VelocityComponent>(entity);
        DataComponent* data = tx.write<DataComponent>(entity);
        StressReadComponent* read = tx.write<StressReadComponent>(entity);
        tx.write<StressWriteComponent>(entity);

        position->x = static_cast<float>(i % 1024u);
        position->y = static_cast<float>((i / 1024u) % 1024u);
        velocity->x = static_cast<float>((i % 17u) + 1u) * 0.25f;
        velocity->y = static_cast<float>((i % 23u) + 1u) * 0.125f;
        data->thingy = static_cast<int>(i % 65'537u);
        data->seed = make_seeded_value(i);
        data->rng.initialize(data->seed);
        data->numgy = data->rng();
        read->value = make_seeded_value(i);

        tx.commit();
        entities.push_back(entity);
    }
}

void create_entities_for_random_add(ecs::Registry& registry,
                                    std::size_t entity_count,
                                    std::vector<ecs::Entity>& entities) {
    entities.clear();
    entities.reserve(entity_count);

    for (std::size_t i = 0; i < entity_count; ++i) {
        const ecs::Entity entity = registry.create();
        auto tx = registry.transaction();
        PositionComponent* position = tx.write<PositionComponent>(entity);
        position->x = static_cast<float>(i);
        position->y = static_cast<float>(entity_count - i);
        tx.commit();
        entities.push_back(entity);
    }
}

std::vector<std::size_t> build_random_indices(std::size_t entity_count, std::uint32_t seed) {
    std::vector<std::size_t> indices(entity_count);
    if (entity_count == 0) {
        return indices;
    }

    RandomXoshiro128 rng(seed);
    for (std::size_t i = 0; i < entity_count; ++i) {
        indices[i] = static_cast<std::size_t>(rng.range(0u, static_cast<std::uint32_t>(entity_count - 1u)));
    }
    return indices;
}

void BM_CreateStressEntities(benchmark::State& state) {
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));

    for (auto _ : state) {
        ecs::Registry registry;
        std::vector<ecs::Entity> entities;
        create_entities_with_stress_components(registry, entity_count, entities);
        benchmark::DoNotOptimize(entities.data());
        benchmark::ClobberMemory();
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["components_per_entity"] = 5.0;
}
BENCHMARK(BM_CreateStressEntities)->Apply(ApplyStressArguments);

void BM_IterateAndWriteComponents(benchmark::State& state) {
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    create_entities_with_stress_components(registry, entity_count, entities);

    for (auto _ : state) {
        auto tx = registry.transaction();
        auto view = tx.view<PositionComponent, VelocityComponent, DataComponent>();
        view.forEach([&tx](ecs::Entity entity,
                           const PositionComponent&,
                           const VelocityComponent& velocity,
                           const DataComponent&) {
            PositionComponent* position = tx.write<PositionComponent>(entity);
            DataComponent* data = tx.write<DataComponent>(entity);
            position->x += velocity.x;
            position->y += velocity.y;
            data->thingy = (data->thingy + 3) % 1'000'003;
            data->numgy ^= static_cast<std::uint32_t>(data->thingy);
        });
        tx.commit();
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(entity_count));
}
BENCHMARK(BM_IterateAndWriteComponents)->Apply(ApplyStressArguments);

void BM_IterateAndRandomlyAddComponent(benchmark::State& state) {
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    std::vector<ecs::Entity> entities;

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        create_entities_for_random_add(registry, entity_count, entities);
        state.ResumeTiming();

        RandomXoshiro128 rng(kRandomAddSeed);
        std::size_t additions = 0;
        auto tx = registry.transaction();
        auto positions = tx.storage<PositionComponent>();
        positions.each([&](ecs::Entity entity, const PositionComponent& position) {
            if ((rng() & 3u) == 0u && !tx.has<VelocityComponent>(entity)) {
                VelocityComponent* velocity = tx.write<VelocityComponent>(entity);
                velocity->x = 0.5f + (position.x * 0.001f);
                velocity->y = -0.5f - (position.y * 0.001f);
                ++additions;
            }
        });
        tx.commit();

        benchmark::DoNotOptimize(additions);
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["seed"] = static_cast<double>(kRandomAddSeed);
}
BENCHMARK(BM_IterateAndRandomlyAddComponent)->Apply(ApplyStressArguments);

void BM_IndexedRandomReadsAndWrites(benchmark::State& state) {
    const std::size_t entity_count = static_cast<std::size_t>(state.range(0));
    ecs::Registry registry;
    std::vector<ecs::Entity> entities;
    create_entities_with_stress_components(registry, entity_count, entities);
    const std::vector<std::size_t> random_indices = build_random_indices(entity_count, kRandomReadSeed);

    for (auto _ : state) {
        auto tx = registry.transaction();
        for (const std::size_t index : random_indices) {
            const ecs::Entity entity = entities[index];
            const StressReadComponent* read = tx.try_get<StressReadComponent>(entity);
            StressWriteComponent* write = tx.write<StressWriteComponent>(entity);
            if (read != nullptr) {
                write->value = (write->value * 1'315'423'911ull) ^ static_cast<std::uint64_t>(read->value + index);
            }
        }
        tx.commit();
    }

    state.counters["entities"] = static_cast<double>(entity_count);
    state.counters["seed"] = static_cast<double>(kRandomReadSeed);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(random_indices.size()));
}
BENCHMARK(BM_IndexedRandomReadsAndWrites)->Apply(ApplyStressArguments);

}  // namespace

int main(int argc, char** argv) {
    ecs::benchmarks::AddCommonContext(true);
    benchmark::AddCustomContext("benchmark.kind", "stress");
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
