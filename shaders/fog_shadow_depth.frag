#version 450
#extension GL_GOOGLE_include_directive : require
// Sun shadows, part 1 (recorded right before the composite, after the world was drawn): the scene depth reduced to the
// shadow image - per texel the NEAREST view depth of its factor x factor block (an occluder's thin parts survive the
// reduction; a gap narrower than a block closes, which only makes shadows slightly wider). Sky / cleared depth: far away.
// WoW's water writes no depth: the water replay's mask gives its surface (stored negative: "water"), so the memories see the sea level, not the seabed
// (fog_ground.comp takes the area's floor from them), and the sun finder does not take open sea for sky.
// The next frame's light pass marches toward the sun through this image (fog_light.comp fogSunVisibility).
#include "fog_common.glsl"
layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(set = 0, binding = 8) uniform sampler2D waterMask;
layout(location = 0) out float viewDepth;
void main() {
    int factor = int(fog.shadowImage.z);
    ivec2 base = ivec2(gl_FragCoord.xy) * factor, last = textureSize(sceneDepth, 0) - 1;
    float nearest = 1e6;
    for (int y = 0; y < factor; ++y)
        for (int x = 0; x < factor; ++x) {
            float raw = texelFetch(sceneDepth, min(base + ivec2(x, y), last), 0).r;
            if (raw < fog.depthRange.x || raw >= fog.depthRange.y) continue;   // sky (0.998..1) / cleared
            float nz = (raw - fog.depthRange.x) / (fog.depthRange.y - fog.depthRange.x);
            nearest = min(nearest, fog.proj.w / (nz - fog.proj.z));
        }
    float water = 1e6;
    if (fog.mode.y != 0u)
        for (int y = 0; y < factor; ++y)
            for (int x = 0; x < factor; ++x) {
                vec4 w = texelFetch(waterMask, min(base + ivec2(x, y), last), 0);
                if (w.a > 0.0 && w.z > 0.0) water = min(water, w.z);
            }
    // A water surface nearer than the world is stored NEGATIVE (readers take abs): the memory pass lifts the floor to it (the
    // seabed a frame without the mask showed is not the area's ground - Booty Bay, 2026-09-29).
    viewDepth = water < nearest ? -water : nearest;
}
