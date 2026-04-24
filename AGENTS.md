# Benchmarking Guide

Use this repo's benchmark scripts for any performance work. Do not compare ad hoc runs unless you are debugging a script problem.

## Rules

- Run benchmark backends serially. Never run `flat_sorted` and `optimized_bplus` at the same time when collecting numbers.
- Do not read `.json` or `.log` artifacts until the benchmark process has exited successfully.
- Treat partially written `.json` files as invalid.
- Use `RelWithDebInfo` for benchmark numbers.
- Use Tracy and `gprof` for hotspot direction only, not authoritative timings.
- For confirmation runs, prefer an exact benchmark filter over a broad suite.
- Record the artifact paths you used in any summary.
- Keep the machine idle for scene benchmarks. Avoid overlapping long benchmark jobs.
- Note that some project benchmark components declare explicit index types such as `FlatIndex` or `OptimizedUniqueIndex`; `--index-backend` is not always the whole story.

## Recommended Workflow

Baseline run for one backend:

```bash
bash scripts/bench/run.sh \
  --target ecs_benchmark_project \
  --index-backend flat_sorted \
  --min-time 0.2s
```

Run the other backend only after the first command finishes:

```bash
bash scripts/bench/run.sh \
  --target ecs_benchmark_project \
  --index-backend optimized_bplus \
  --min-time 0.2s
```

Focused single-case confirmation:

```bash
bash scripts/bench/run.sh \
  --target ecs_benchmark_project \
  --index-backend flat_sorted \
  --filter '^BM_ComplexSceneVisibilityQuery/16384$' \
  --min-time 0.2s
```

Focused multi-case comparison set:

```bash
bash scripts/bench/run.sh \
  --target ecs_benchmark_project \
  --index-backend flat_sorted \
  --filter '^(BM_IndexedViewWhereEq/65536|BM_IndexedViewPredicateComposeOr/65536|BM_IndexedViewWithSecondaryFetch/65536|BM_ComplexSceneFrame/16384|BM_ComplexSceneFrameGrouped/16384|BM_ComplexSceneFrameWithChurn/16384|BM_ComplexSceneVisibilityQuery/16384)$' \
  --min-time 0.2s
```

Compare-suite microbenchmarks:

```bash
bash scripts/bench/compare.sh --index-backend flat_sorted --min-time 0.2s
```

Tracy profiling:

```bash
bash scripts/bench/profile.sh \
  --index-backend flat_sorted \
  --target ecs_benchmark_project \
  --filter '^BM_ComplexSceneFrame/2048$' \
  --min-time 0.5s
```

`gprof` hotspot collection:

```bash
bash scripts/bench/gprof.sh --index-backend flat_sorted
```

## Comparison Discipline

- Compare the same benchmark names only.
- Prefer fresh before/after runs on the same machine state over historical artifacts.
- If a number looks extreme, rerun that exact case in isolation before treating it as real.
- If a suite is long, use isolated exact-case reruns to validate the interesting regressions or wins.
