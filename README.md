# ECS

Simple ECS library using 64-bit entity IDs and paged sparse arrays. Entity IDs use 59 low bits for the sparse-array index and 5 high bits for a recycled-version counter so stale handles can be detected after an index is reused. It builds as either a static library or a shared library on macOS, Windows, and Linux.

## Layout

- `include/ecs/`: public headers
- `src/`: library implementation
- `examples/`: small consumer executable
- `tests/`: Catch2 unit tests

## Build a Static Library

```bash
cmake -S . -B build/static -DBUILD_SHARED_LIBS=OFF
cmake --build build/static
```

## Build a Shared Library

```bash
cmake -S . -B build/shared -DBUILD_SHARED_LIBS=ON
cmake --build build/shared
```

## Build and Run Tests

```bash
cmake -S . -B build/test -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON
cmake --build build/test --target tests
ctest --test-dir build/test --output-on-failure
```

## Build and Run Benchmarks

The benchmark suite mirrors the data setup and benchmark shapes used in `abeimler/ecs_benchmark` so the results are directly comparable.

```bash
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DECS_BUILD_BENCHMARKS=ON
cmake --build build/bench --target ecs_benchmark_entities ecs_benchmark_basic ecs_benchmark_extended
```

Run the executables directly or emit Google Benchmark JSON:

```bash
./build/bench/ecs_benchmark_entities --benchmark_out=entities.json --benchmark_out_format=json
./build/bench/ecs_benchmark_basic --benchmark_out=basic.json --benchmark_out_format=json
./build/bench/ecs_benchmark_extended --benchmark_out=extended.json --benchmark_out_format=json
```

Suite mapping:

- `ecs_benchmark_entities`: entity creation, destruction, component unpack, and remove/add benchmarks
- `ecs_benchmark_basic`: the 2-system update benchmarks (`MovementSystem` and `DataSystem`)
- `ecs_benchmark_extended`: the 7-system update benchmarks plus iteration benchmarks

## Notes

- The same `CMakeLists.txt` works across macOS, Windows, and Linux.
- `BUILD_SHARED_LIBS` controls whether `ecs` is created as static or shared.
- The export macro in `include/ecs/export.hpp` handles symbol visibility for shared builds.
- `BUILD_TESTING=ON` enables Catch2-based unit tests and creates the `ecs_tests` and `tests` build targets.
