#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/build/gprof_project"
target="ecs_benchmark_project"
build_type="${BUILD_TYPE:-RelWithDebInfo}"
min_time="2s"
timestamp="$(date +%Y%m%d-%H%M%S)"
out_dir="${repo_root}/artifacts/bench/${timestamp}"

benchmarks=(
  "BM_ComplexSceneFrame/2048"
  "BM_ComplexSceneFrameGrouped/2048"
  "BM_ComplexSceneFrameWithChurn/2048"
  "BM_ComplexSceneVisibilityQuery/16384"
  "BM_IndexedViewWhereEq/65536"
  "BM_IndexedViewPredicateComposeOr/65536"
  "BM_IndexedViewWithSecondaryFetch/65536"
)

sanitize_name() {
  sed 's#[^A-Za-z0-9_.-]#_#g' <<<"$1"
}

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
    --min-time)
      min_time="$2"
      shift 2
      ;;
    --out-dir)
      out_dir="$2"
      shift 2
      ;;
    --benchmark)
      benchmarks+=("$2")
      shift 2
      ;;
    --help)
      cat <<'EOF'
Usage: gprof.sh [--build-dir DIR] [--build-type TYPE] [--target NAME] [--min-time TIME] [--out-dir DIR] [--benchmark NAME]
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
  --gprof \
  --build-dir "${build_dir}" \
  --build-type "${build_type}" \
  --target "${target}"

mkdir -p "${out_dir}"

binary="${build_dir}/${target}"
summary_out="${out_dir}/${target}.summary.txt"
: > "${summary_out}"

for benchmark_name in "${benchmarks[@]}"; do
  safe_name="$(sanitize_name "${benchmark_name}")"
  log_out="${out_dir}/${safe_name}.log"
  gmon_out="${out_dir}/${safe_name}.gmon.out"
  gprof_out="${out_dir}/${safe_name}.gprof.txt"

  rm -f "${repo_root}/gmon.out"

  "${binary}" \
    --benchmark_filter="^${benchmark_name}$" \
    --benchmark_min_time="${min_time}" | tee "${log_out}"

  if [[ ! -f "${repo_root}/gmon.out" ]]; then
    echo "Missing gmon.out for ${benchmark_name}" >&2
    exit 1
  fi

  mv "${repo_root}/gmon.out" "${gmon_out}"
  gprof "${binary}" "${gmon_out}" > "${gprof_out}"

  {
    echo "=== ${benchmark_name} ==="
    sed -n '1,40p' "${log_out}"
    echo
    echo "--- Top Self Time ---"
    awk '
      /^Flat profile:/ { in_flat=1; next }
      in_flat && /^Each sample counts as/ { next }
      in_flat && /^$/ { blank_count++; if (blank_count >= 2) exit; next }
      in_flat && /^[[:space:]]*[0-9]/ { print; count++; if (count == 10) exit }
    ' "${gprof_out}"
    echo
  } >> "${summary_out}"
done

echo "Gprof summary: ${summary_out}"
echo "Artifacts: ${out_dir}"
