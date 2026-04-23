#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/build/bench"
target="ecs_benchmark_extended"
benchmark_filter="."
min_time="0.1s"
build_type="${BUILD_TYPE:-RelWithDebInfo}"
timestamp="$(date +%Y%m%d-%H%M%S)"
out_dir="${repo_root}/artifacts/bench/${timestamp}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      build_dir="$2"
      shift 2
      ;;
    --build-type)
      build_type="$2"
      shift 2
      ;;
    --target)
      target="$2"
      shift 2
      ;;
    --filter)
      benchmark_filter="$2"
      shift 2
      ;;
    --min-time)
      min_time="$2"
      shift 2
      ;;
    --out-dir)
      out_dir="$2"
      shift 2
      ;;
    --help)
      cat <<'EOF'
Usage: run.sh [--build-dir DIR] [--build-type TYPE] [--target NAME] [--filter REGEX] [--min-time TIME] [--out-dir DIR]
EOF
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

bash "${repo_root}/scripts/bench/build.sh" \
  --build-dir "${build_dir}" \
  --build-type "${build_type}" \
  --target "${target}"

mkdir -p "${out_dir}"

binary="${build_dir}/${target}"
json_out="${out_dir}/${target}.json"
log_out="${out_dir}/${target}.log"

"${binary}" \
  --benchmark_filter="${benchmark_filter}" \
  --benchmark_min_time="${min_time}" \
  --benchmark_out="${json_out}" \
  --benchmark_out_format=json | tee "${log_out}"

echo "Benchmark JSON: ${json_out}"
echo "Benchmark log: ${log_out}"
