#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/build/bench"
build_type="${BUILD_TYPE:-RelWithDebInfo}"
profiling="OFF"
gprof="OFF"
targets=(
  ecs_benchmark_entities
  ecs_benchmark_basic
  ecs_benchmark_extended
  ecs_benchmark_stress
  ecs_benchmark_trace
  ecs_benchmark_project
)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profiling)
      profiling="ON"
      build_dir="${repo_root}/build/prof"
      shift
      ;;
    --gprof)
      gprof="ON"
      build_dir="${repo_root}/build/gprof"
      shift
      ;;
    --build-dir)
      build_dir="$2"
      shift 2
      ;;
    --build-type)
      build_type="$2"
      shift 2
      ;;
    --target)
      targets=("$2")
      shift 2
      ;;
    --help)
      cat <<'EOF'
Usage: build.sh [--profiling] [--gprof] [--build-dir DIR] [--build-type TYPE] [--target NAME]
EOF
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

mkdir -p "${build_dir}"

cmake_args=(
  -S "${repo_root}"
  -B "${build_dir}"
  -DCMAKE_BUILD_TYPE="${build_type}"
  -DBUILD_SHARED_LIBS=OFF
  -DBUILD_TESTING=OFF
  -DECS_BUILD_BENCHMARKS=ON
  -DECS_BUILD_PROFILING="${profiling}"
)

if [[ "${gprof}" == "ON" ]]; then
  cmake_args+=("-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g -pg -fno-omit-frame-pointer")
fi

cmake "${cmake_args[@]}"

if [[ "${profiling}" == "ON" ]]; then
  targets+=(tracy-capture)
fi

cmake --build "${build_dir}" --target "${targets[@]}"
