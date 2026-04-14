#include "benchmark_common.hpp"

namespace {

ecs::benchmarks::BenchmarkHarness benchmark_suite;

void BM_CreateNoEntities(benchmark::State& state) {
    benchmark_suite.benchmark_create_no_entities(state);
}
BENCHMARK(BM_CreateNoEntities);

void BM_CreateEmptyEntities(benchmark::State& state) {
    benchmark_suite.benchmark_create_empty_entities(state);
}
BENCHMARK(BM_CreateEmptyEntities)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_UnpackNoComponent(benchmark::State& state) {
    benchmark_suite.benchmark_unpack_no_component(state);
}
BENCHMARK(BM_UnpackNoComponent)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_CreateEntities(benchmark::State& state) {
    benchmark_suite.benchmark_create_entities(state);
}
BENCHMARK(BM_CreateEntities)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_UnpackOneComponent(benchmark::State& state) {
    benchmark_suite.benchmark_unpack_one_component(state);
}
BENCHMARK(BM_UnpackOneComponent)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_UnpackTwoComponents(benchmark::State& state) {
    benchmark_suite.benchmark_unpack_two_components(state);
}
BENCHMARK(BM_UnpackTwoComponents)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_UnpackThreeComponents(benchmark::State& state) {
    benchmark_suite.benchmark_unpack_three_components(state);
}
BENCHMARK(BM_UnpackThreeComponents)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_AddComponent(benchmark::State& state) {
    benchmark_suite.benchmark_add_component(state);
}
BENCHMARK(BM_AddComponent)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_RemoveAddComponent(benchmark::State& state) {
    benchmark_suite.benchmark_remove_add_component(state);
}
BENCHMARK(BM_RemoveAddComponent)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_DestroyEntities(benchmark::State& state) {
    benchmark_suite.benchmark_destroy_entities(state);
}
BENCHMARK(BM_DestroyEntities)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_CreateEntitiesInBulk(benchmark::State& state) {
    benchmark_suite.benchmark_create_entities_in_bulk(state);
}
BENCHMARK(BM_CreateEntitiesInBulk)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_CreateEmptyEntitiesInBulk(benchmark::State& state) {
    benchmark_suite.benchmark_create_empty_entities_in_bulk(state);
}
BENCHMARK(BM_CreateEmptyEntitiesInBulk)->Apply(ecs::benchmarks::ApplyDefaultArguments);

void BM_DestroyEntitiesInBulk(benchmark::State& state) {
    benchmark_suite.benchmark_destroy_entities_in_bulk(state);
}
BENCHMARK(BM_DestroyEntitiesInBulk)->Apply(ecs::benchmarks::ApplyDefaultArguments);

}  // namespace

int main(int argc, char** argv) {
    ecs::benchmarks::AddCommonContext(false);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
