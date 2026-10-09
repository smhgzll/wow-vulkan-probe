#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
command -v glslc >/dev/null
python3 "$project_root/tools/generate_menu_font.py"
shader_tmp="$(mktemp -d "$project_root/shaders/.build.XXXXXX")"
trap 'rm -rf -- "$shader_tmp"' EXIT
# Finish all compilation before replacing any live file. Same filesystem makes
# each rename atomic. Descriptor/UBO interface changes still require a restart.
shaders=(depth.vert depth.frag menu.vert menu.frag inspector_pick.frag inspector_preview.frag inspector_export.frag water_camera.frag water_composite.frag water_sky.frag gizmo.vert gizmo.frag fog_inject.comp fog_light.comp fog_integrate.comp fog_composite.vert fog_glare.vert fog_composite.frag fog_shadow_depth.frag fog_memory_clear.comp fog_memory_scatter.comp fog_sun_find.comp fog_noise_bake.comp fog_ground.comp fog_exposure.comp fog_cascade_reduce.frag fog_rays_mask.frag fog_rays.frag fog_shade.frag)
for shader in "${shaders[@]}"; do
    glslc "$project_root/shaders/$shader" -o "$shader_tmp/$shader.spv"
    if command -v spirv-val >/dev/null; then
        spirv-val "$shader_tmp/$shader.spv"
    fi
done
for shader in "${shaders[@]}"; do
    mv -f -- "$shader_tmp/$shader.spv" "$project_root/shaders/$shader.spv"
done
printf '%s\n' "${#shaders[@]} SPIR-V files compiled and published. Restart the game after ABI changes."
