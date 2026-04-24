#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/build/prof"
target="ecs_benchmark_extended"
benchmark_filter='BM_CompareIterateTwoComponentsGrouped/(16384|32768)$'
min_time="0.5s"
build_type="${BUILD_TYPE:-RelWithDebInfo}"
index_backend="${ECS_INDEX_BACKEND:-flat_sorted}"
timestamp="$(date +%Y%m%d-%H%M%S)"
out_dir="${repo_root}/artifacts/bench/${timestamp}/${index_backend}"
capture_tool="${TRACY_CAPTURE_TOOL:-}"
capture_file=""
custom_build_dir="false"
custom_out_dir="false"

resolve_capture_tool() {
  if [[ -n "${capture_tool}" ]]; then
    return 0
  fi

  local bundled_capture="${build_dir}/_deps/tracy-build/capture/tracy-capture"
  if [[ -x "${bundled_capture}" ]]; then
    capture_tool="${bundled_capture}"
    return 0
  fi

  if command -v TracyCapture >/dev/null 2>&1; then
    capture_tool="$(command -v TracyCapture)"
    return 0
  fi

  if command -v tracy-capture >/dev/null 2>&1; then
    capture_tool="$(command -v tracy-capture)"
    return 0
  fi

  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      build_dir="$2"
      custom_build_dir="true"
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
      custom_out_dir="true"
      shift 2
      ;;
    --index-backend)
      index_backend="$2"
      shift 2
      ;;
    --capture-tool)
      capture_tool="$2"
      shift 2
      ;;
    --capture-file)
      capture_file="$2"
      shift 2
      ;;
    --help)
      cat <<'EOF'
Usage: profile.sh [--build-dir DIR] [--build-type TYPE] [--target NAME] [--filter REGEX] [--min-time TIME] [--out-dir DIR] [--capture-tool PATH] [--capture-file PATH] [--index-backend NAME]
EOF
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ "${custom_build_dir}" != "true" ]]; then
  build_dir="${repo_root}/build/prof-${index_backend}"
fi

if [[ "${custom_out_dir}" != "true" ]]; then
  out_dir="${repo_root}/artifacts/bench/${timestamp}/${index_backend}"
fi

bash "${repo_root}/scripts/bench/build.sh" \
  --profiling \
  --build-dir "${build_dir}" \
  --build-type "${build_type}" \
  --target "${target}" \
  --index-backend "${index_backend}"

mkdir -p "${out_dir}"

binary="${build_dir}/${target}"
json_out="${out_dir}/${target}.json"
log_out="${out_dir}/${target}.log"
instructions_out="${out_dir}/tracy.txt"
capture_status="manual"

if [[ -z "${capture_file}" ]]; then
  capture_file="${out_dir}/${target}.tracy"
fi

capture_pid=""
cleanup_capture() {
  if [[ -n "${capture_pid}" ]]; then
    wait "${capture_pid}" || true
    capture_pid=""
  fi
}

trap cleanup_capture EXIT

if resolve_capture_tool; then
  capture_status="automatic"
  "${capture_tool}" -o "${capture_file}" >/dev/null 2>&1 &
  capture_pid="$!"
fi

cat > "${instructions_out}" <<EOF
Profiling target: ${binary}
Benchmark filter: ${benchmark_filter}
Capture mode: ${capture_status}

This script builds with Tracy instrumentation and records the benchmark JSON/log artifacts beside this note.
EOF

if [[ "${capture_status}" == "automatic" ]]; then
  cat >> "${instructions_out}" <<EOF
Capture tool: ${capture_tool}
Capture file: ${capture_file}

The script started TracyCapture automatically and will wait for it to finish after the benchmark run.
EOF
else
  cat >> "${instructions_out}" <<EOF
Run Tracy Profiler or TracyCapture and connect to the process while the benchmark is running.
Set TRACY_CAPTURE_TOOL or pass --capture-tool PATH to save a capture automatically.
Suggested capture file: ${capture_file}
EOF
fi

"${binary}" \
  --benchmark_filter="${benchmark_filter}" \
  --benchmark_min_time="${min_time}" \
  --benchmark_out="${json_out}" \
  --benchmark_out_format=json | tee "${log_out}"

if [[ "${capture_status}" == "automatic" ]]; then
  cleanup_capture
fi

echo "Benchmark JSON: ${json_out}"
echo "Benchmark log: ${log_out}"
echo "Tracy note: ${instructions_out}"
if [[ "${capture_status}" == "automatic" ]]; then
  echo "Tracy capture: ${capture_file}"
fi
