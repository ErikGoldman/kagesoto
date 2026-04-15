#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-"${repo_root}/build"}"
build_type="${BUILD_TYPE:-Release}"

echo "Configuring in ${build_dir}..."
 cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DECS_BUILD_BENCHMARKS=ON

echo "Building..."
cmake --build build/bench --target ecs_benchmark_entities ecs_benchmark_basic ecs_benchmark_extended ecs_benchmark_stress
