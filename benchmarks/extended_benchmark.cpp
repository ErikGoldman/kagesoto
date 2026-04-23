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

void BM_IterateTwoComponentsGrouped(benchmark::State& state) {
    benchmark_suite.benchmark_iterate_two_components_grouped(state);
}
BENCHMARK(BM_IterateTwoComponentsGrouped)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_CompareIterateTwoComponentsStandalone(benchmark::State& state) {
    benchmark_suite.benchmark_compare_iterate_two_components_standalone(state);
}
BENCHMARK(BM_CompareIterateTwoComponentsStandalone)->Apply(ecs::benchmarks::ApplySmallArguments);

void BM_CompareIterateTwoComponentsGrouped(benchmark::State& state) {
    benchmark_suite.benchmark_compare_iterate_two_components_grouped(state);
}
BENCHMARK(BM_CompareIterateTwoComponentsGrouped)->Apply(ecs::benchmarks::ApplySmallArguments);

void BM_IterateThreeComponentsWithMixedEntities(benchmark::State& state) {
    benchmark_suite.benchmark_iterate_three_components_with_mixed_entities(state);
}
BENCHMARK(BM_IterateThreeComponentsWithMixedEntities)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_IterateThreeComponentsWithSparseHealth(benchmark::State& state) {
    benchmark_suite.benchmark_iterate_three_components_with_sparse_health(state);
}
BENCHMARK(BM_IterateThreeComponentsWithSparseHealth)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_IterateThreeComponentsGroupedWithSparseHealth(benchmark::State& state) {
    benchmark_suite.benchmark_iterate_three_components_grouped_with_sparse_health(state);
}
BENCHMARK(BM_IterateThreeComponentsGroupedWithSparseHealth)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_CompareIterateThreeComponentsSparseHealthStandalone(benchmark::State& state) {
    benchmark_suite.benchmark_compare_iterate_three_components_sparse_health_standalone(state);
}
BENCHMARK(BM_CompareIterateThreeComponentsSparseHealthStandalone)->Apply(ecs::benchmarks::ApplySmallArguments);

void BM_CompareIterateThreeComponentsSparseHealthGrouped(benchmark::State& state) {
    benchmark_suite.benchmark_compare_iterate_three_components_sparse_health_grouped(state);
}
BENCHMARK(BM_CompareIterateThreeComponentsSparseHealthGrouped)->Apply(ecs::benchmarks::ApplySmallArguments);

}  // namespace

int main(int argc, char** argv) {
    ecs::benchmarks::AddCommonContext(true);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
