#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${TUFTY_BUILD_DIR:-$project_dir/build/firmware}"
artifact_dir="${TUFTY_ARTIFACT_DIR:-$project_dir/dist}"
python_bin="${PYTHON:-$(command -v python3)}"
for program in git cmake ninja cc c++ arm-none-eabi-gcc arm-none-eabi-g++; do
    command -v "$program" >/dev/null || { echo "Missing $program; see README.md" >&2; exit 1; }
done
case "$build_dir" in /*) ;; *) build_dir="$project_dir/$build_dir" ;; esac
case "$artifact_dir" in /*) ;; *) artifact_dir="$project_dir/$artifact_dir" ;; esac
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
"$python_bin" -B "$project_dir/tools/deps.py" --radio
sdk_dir="$project_dir/lib/pico-sdk"
cmake -S "$project_dir/lib/picotool" -B "$build_dir/host/picotool" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release "-DPICO_SDK_PATH=$sdk_dir" \
    -DPICOTOOL_NO_LIBUSB=ON -DPICOTOOL_FLAT_INSTALL=ON \
    "-DCMAKE_INSTALL_PREFIX=$build_dir/host/install"
cmake --build "$build_dir/host/picotool" --parallel "$jobs"
cmake --install "$build_dir/host/picotool"
cmake -S "$project_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DTUFTY_FIRMWARE=ON -DNES_SANITIZERS=OFF -DBUILD_TESTING=OFF \
    "-Dpicotool_DIR=$build_dir/host/install/picotool" \
    -DPICOTOOL_FORCE_FETCH_FROM_GIT=OFF \
    "-DPython3_EXECUTABLE=$python_bin" \
    "-DCMAKE_C_COMPILER=$(command -v arm-none-eabi-gcc)" \
    "-DCMAKE_CXX_COMPILER=$(command -v arm-none-eabi-g++)" \
    "-DCMAKE_ASM_COMPILER=$(command -v arm-none-eabi-gcc)"
cmake --build "$build_dir" --parallel "$jobs"
"$python_bin" -B "$project_dir/tools/check_uf2.py" "$build_dir/tufty-nes-flash.uf2"
"$python_bin" -B "$project_dir/tools/package.py" "$build_dir" "$artifact_dir"
echo "Built $artifact_dir/tufty-nes-flash.uf2"
