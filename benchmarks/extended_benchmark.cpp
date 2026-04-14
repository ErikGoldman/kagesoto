#include "benchmark_common.hpp"

namespace {

ecs::benchmarks::BenchmarkHarness benchmark_suite;

void BM_ComplexSystemsUpdate(benchmark::State& state) {
    benchmark_suite.benchmark_systems_update(state, ecs::benchmarks::SystemFlavor::Complex, false);
}
BENCHMARK(BM_ComplexSystemsUpdate)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_ComplexSystemsUpdateMixedEntities(benchmark::State& state) {
    benchmark_suite.benchmark_systems_update(state, ecs::benchmarks::SystemFlavor::Complex, true);
}
BENCHMARK(BM_ComplexSystemsUpdateMixedEntities)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_IterateSingleComponent(benchmark::State& state) {
    benchmark_suite.benchmark_iterate_single_component(state);
}
BENCHMARK(BM_IterateSingleComponent)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_IterateTwoComponents(benchmark::State& state) {
    benchmark_suite.benchmark_iterate_two_components(state);
}
BENCHMARK(BM_IterateTwoComponents)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_IterateThreeComponentsWithMixedEntities(benchmark::State& state) {
    benchmark_suite.benchmark_iterate_three_components_with_mixed_entities(state);
}
BENCHMARK(BM_IterateThreeComponentsWithMixedEntities)->Apply(ecs::benchmarks::ApplyDefaultArguments);

}  // namespace

int main(int argc, char** argv) {
    ecs::benchmarks::AddCommonContext(true);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
