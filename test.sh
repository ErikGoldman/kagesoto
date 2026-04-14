#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-"${repo_root}/build"}"
build_type="${BUILD_TYPE:-Debug}"

echo "Configuring in ${build_dir}..."
cmake -S "${repo_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DBUILD_TESTING=ON

echo "Building..."
cmake --build "${build_dir}" --config "${build_type}"

echo "Running tests..."
ctest --test-dir "${build_dir}" --output-on-failure -C "${build_type}"
