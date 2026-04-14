#include "benchmark_common.hpp"

namespace {

ecs::benchmarks::BenchmarkHarness benchmark_suite;

void BM_SystemsUpdate(benchmark::State& state) {
    benchmark_suite.benchmark_systems_update(state, ecs::benchmarks::SystemFlavor::Basic, false);
}
BENCHMARK(BM_SystemsUpdate)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_SystemsUpdateMixedEntities(benchmark::State& state) {
    benchmark_suite.benchmark_systems_update(state, ecs::benchmarks::SystemFlavor::Basic, true);
}
BENCHMARK(BM_SystemsUpdateMixedEntities)->Apply(ecs::benchmarks::ApplyDefaultArguments);

}  // namespace

int main(int argc, char** argv) {
    ecs::benchmarks::AddCommonContext(false);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
