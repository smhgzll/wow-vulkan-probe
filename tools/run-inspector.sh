#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if (($# == 0)); then
    printf 'Usage: %s <game command> [arguments...]\nSteam: %s %%command%%\n' "$0" "$0" >&2
    exit 2
fi
if [[ ! -r "$project_root/build32/libVkLayer_depth_probe.so" ]]; then
    printf 'Build the 32-bit layer first: cmake --build "%s/build32"\n' "$project_root" >&2
    exit 1
fi
for shader in depth.vert depth.frag menu.vert menu.frag inspector_pick.frag inspector_preview.frag inspector_export.frag water_camera.frag water_composite.frag water_sky.frag gizmo.vert gizmo.frag fog_inject.comp fog_light.comp fog_integrate.comp fog_composite.vert fog_glare.vert fog_composite.frag fog_shadow_depth.frag fog_memory_clear.comp fog_memory_scatter.comp fog_sun_find.comp fog_noise_bake.comp fog_ground.comp fog_exposure.comp fog_cascade_reduce.frag fog_rays_mask.frag fog_rays.frag fog_shade.frag; do
    if [[ ! -r "$project_root/shaders/$shader.spv" ]]; then
        printf 'Missing shader. Run "%s/tools/build-shaders.sh" first.\n' "$project_root" >&2
        exit 1
    fi
done
if [[ ! -r "$project_root/shaders/water_detail.bin" ]]; then
    # Water ripple/foam textures (synthesised, reproducible). Without the file the water still works, with flat textures.
    python3 "$project_root/tools/generate-water-textures.py" "$project_root/shaders/water_detail.bin" >/dev/null 2>&1 ||
        printf 'Note: shaders/water_detail.bin is missing (tools/generate-water-textures.py needs python3 + numpy); water uses flat textures.\n' >&2
fi
if [[ ! -r "$project_root/shaders/water_modern.bin" ]]; then
    # MODERN water textures (original, synthesised; tools/generate-modern-water-textures.py --crest DIR converts Crest's instead). ~10 s.
    python3 "$project_root/tools/generate-modern-water-textures.py" "$project_root/shaders/water_modern.bin" >/dev/null 2>&1 ||
        printf 'Note: shaders/water_modern.bin is missing (tools/generate-modern-water-textures.py needs python3 + numpy); MODERN water uses flat textures.\n' >&2
fi
export VK_ADD_LAYER_PATH="$project_root/build32${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
case ":${VK_INSTANCE_LAYERS:-}:" in
    *:VK_LAYER_depth_probe:*) ;;
    *) export VK_INSTANCE_LAYERS="VK_LAYER_depth_probe${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}" ;;
esac
export DEPTH_PROBE_DEPTH_PRESENT=1 DEPTH_PROBE_SURFACE_INSPECTOR=1
# The volumetric fog (with its scene shadows, AO and the soft game shadows it drives) starts on; DEPTH_PROBE_FOG_START=0 starts it off.
export DEPTH_PROBE_FOG_START="${DEPTH_PROBE_FOG_START:-1}"
export DEPTH_PROBE_SHADER_DIR="$project_root/shaders"
export DEPTH_PROBE_LAYER_LOG="${DEPTH_PROBE_LAYER_LOG:-$HOME/vulkan-depth-probe.log}"
export DEPTH_PROBE_EXPORT_DIR="${DEPTH_PROBE_EXPORT_DIR:-$project_root/exports}"
# Reflection research: save immutable pick-submit constants and shader snapshots
# on selection (including Next GPU Hit), at most four distinct draws per click.
export DEPTH_PROBE_INSPECTOR_AUTO_CONSTANTS="${DEPTH_PROBE_INSPECTOR_AUTO_CONSTANTS:-1}"
# Water reflection debugging: save the first replayed pass of every mode/view (up to 4, ~90 MB each) to
# $DEPTH_PROBE_EXPORT_DIR/water-capture-*; set DEPTH_PROBE_WATER_CAPTURE=0 to disable. The menu also has a capture button.
export DEPTH_PROBE_WATER_CAPTURE="${DEPTH_PROBE_WATER_CAPTURE:-1}"
# GPU time of the overlay pass in [FRAME-PERF] (overlayGpu=..); a couple of timestamp queries per frame. The Water Reflections menu has a PERFORMANCE TEST button (automatic A/B).
export DEPTH_PROBE_GPU_TIMING="${DEPTH_PROBE_GPU_TIMING:-1}"
# Inspector currently resolves descriptor sets, not opaque descriptor buffers.
# DXVK reads DXVK_CONFIG after its config file. Put this override before any
# per-executable sections and remove conflicting assignments from the inherited
# string, while preserving all other options (without evaluating shell text).
inspector_dxvk_remaining="${DXVK_CONFIG:-};"
export DXVK_CONFIG='dxvk.enableDescriptorBuffer = False'
while [[ -n "$inspector_dxvk_remaining" ]]; do
    inspector_dxvk_option="${inspector_dxvk_remaining%%;*}"
    inspector_dxvk_remaining="${inspector_dxvk_remaining#*;}"
    if [[ -n "$inspector_dxvk_option" && ! "$inspector_dxvk_option" =~ ^[[:space:]]*dxvk\.enableDescriptorBuffer[[:space:]]*= ]]; then
        DXVK_CONFIG+=";$inspector_dxvk_option"
    fi
done
printf '[depth-probe-inspector] DXVK compatibility: dxvk.enableDescriptorBuffer = False\n' >&2
exec "$@"
