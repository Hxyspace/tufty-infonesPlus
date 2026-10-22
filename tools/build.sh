#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${TUFTY_BUILD_DIR:-$project_dir/build/firmware}"
artifact_dir="${TUFTY_ARTIFACT_DIR:-$project_dir/dist}"
resources_dir="${TUFTY_RESOURCES_DIR:-$project_dir/resources}"
python_bin="${PYTHON:-$(command -v python3)}"
for program in git cmake ninja cc c++ arm-none-eabi-gcc arm-none-eabi-g++ mkfs.fat mmd mcopy; do
    command -v "$program" >/dev/null || { echo "Missing $program; see README.md" >&2; exit 1; }
done
case "$build_dir" in /*) ;; *) build_dir="$project_dir/$build_dir" ;; esac
case "$artifact_dir" in /*) ;; *) artifact_dir="$project_dir/$artifact_dir" ;; esac
case "$resources_dir" in /*) ;; *) resources_dir="$project_dir/$resources_dir" ;; esac
for partition in rom system state; do
    if [[ ! -d "$resources_dir/$partition" ]]; then
        echo "Missing resource directory: $resources_dir/$partition" >&2
        exit 1
    fi
done
echo "Resource input: $resources_dir"
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
"$python_bin" -B "$project_dir/tools/resource_images.py" "$build_dir/tufty-nes-flash.uf2" "$build_dir/resources" --resources "$resources_dir"
"$python_bin" -B "$project_dir/tools/package.py" "$build_dir" "$artifact_dir"
echo "Built firmware / sparse firmware+romfs / sparse firmware+filesystem images in $artifact_dir"
