#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${TUFTY_TEST_BUILD_DIR:-$project_dir/build/host}"
cmake -S "$project_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNES_SANITIZERS=ON \
  "-DPython3_EXECUTABLE=${PYTHON:-$(command -v python3)}"
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
ctest --test-dir "$build_dir" --output-on-failure
