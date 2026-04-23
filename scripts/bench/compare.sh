#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/build/bench"
build_type="${BUILD_TYPE:-RelWithDebInfo}"
min_time="0.1s"
timestamp="$(date +%Y%m%d-%H%M%S)"
out_dir="${repo_root}/artifacts/bench/${timestamp}"
target="ecs_benchmark_extended"
filter='BM_CompareIterate(TwoComponents(Standalone|Grouped)|ThreeComponentsSparseHealth(Standalone|Grouped)).*'

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
Usage: compare.sh [--build-dir DIR] [--build-type TYPE] [--min-time TIME] [--out-dir DIR]
EOF
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

bash "${repo_root}/scripts/bench/run.sh" \
  --build-dir "${build_dir}" \
  --build-type "${build_type}" \
  --target "${target}" \
  --filter "${filter}" \
  --min-time "${min_time}" \
  --out-dir "${out_dir}"

echo
echo "Compare summary:"
grep '^BM_Compare' "${out_dir}/${target}.log" || true
echo "Artifacts: ${out_dir}"
