# Benchmarking Guide

Use this repo's benchmark target for performance work. The older `scripts/bench/*.sh` helpers are not present in this checkout; `benchmark.sh` currently delegates to that missing path, so do not rely on it unless the script tree is restored.

## Rules

- Build benchmark numbers with `RelWithDebInfo`.
- Run benchmark configurations serially. Never overlap long benchmark jobs when collecting numbers.
- For confirmation runs, prefer an exact benchmark filter over a broad suite.
- Keep the machine idle for benchmark runs.
- If writing benchmark output to `.json` or `.log`, do not read it until the process exits successfully.
- Treat partially written benchmark artifacts as invalid.
- Record the command and artifact path used in any summary.
- Direct benchmark executable runs are acceptable in this checkout because the benchmark scripts are absent. Call out when a run is only a smoke test rather than authoritative timing data.

## Recommended Workflow

Configure a `RelWithDebInfo` benchmark build:

```bash
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DECS_BUILD_BENCHMARKS=ON
```

Build the benchmark target:

```bash
cmake --build build-bench --target basic_operations_benchmark
```

Focused benchmark run:

```bash
build-bench/benchmarks/basic_operations_benchmark \
  --benchmark_filter='^BM_Snapshot/16384$' \
  --benchmark_min_time=0.2s
```

Focused snapshot comparison set:

```bash
build-bench/benchmarks/basic_operations_benchmark \
  --benchmark_filter='^(BM_Snapshot/16384|BM_DeltaSnapshotDirtyValues/16384|BM_DeltaSnapshotStructuralChanges/16384)$' \
  --benchmark_min_time=0.2s
```

## Comparison Discipline

- Compare the same benchmark names only.
- Prefer fresh before/after runs on the same machine state over historical artifacts.
- If a number looks extreme, rerun that exact case in isolation before treating it as real.
- If a suite is long, use isolated exact-case reruns to validate the interesting regressions or wins.
- Do not mix `Debug` smoke-test timings with `RelWithDebInfo` benchmark numbers.
