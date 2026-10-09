#!/usr/bin/env bash
set -euo pipefail

effect_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
for command in cmake ninja glslc tar; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf 'rainbow-wow: gerekli araç bulunamadı: %s\n' "$command" >&2
        exit 1
    fi
done

# Build the frozen source, not the evolving main project. Temporary absolute
# paths do not matter: run.sh supplies the package's shader directory at launch.
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/rainbow-wow-build.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
mkdir -p "$build_dir/source/shaders"
tar -xzf "$effect_dir/source/layer-source.tar.gz" -C "$build_dir/source"

glslc "$effect_dir/shaders/depth.vert" -o "$build_dir/source/shaders/depth.vert.spv"
glslc "$effect_dir/shaders/depth.frag" -o "$build_dir/source/shaders/depth.frag.spv"
cmake -S "$build_dir/source" -B "$build_dir/build32" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS=-m32 \
    -DCMAKE_SHARED_LINKER_FLAGS=-m32 \
    -DDEPTH_PROBE_BUILD_TESTS=OFF
cmake --build "$build_dir/build32"

# Publish only after both shaders and the 32-bit library built successfully.
mkdir -p "$effect_dir/layer32"
install -m 755 "$build_dir/build32/libVkLayer_depth_probe.so" "$effect_dir/layer32/"
install -m 644 "$build_dir/build32/VK_LAYER_depth_probe.json" "$effect_dir/layer32/"
install -m 644 "$build_dir/source/shaders/depth.vert.spv" "$effect_dir/shaders/"
install -m 644 "$build_dir/source/shaders/depth.frag.spv" "$effect_dir/shaders/"
printf '\nrainbow-wow hazır. Steam: "%s/run.sh" %%command%%\n' "$effect_dir"
