#version 450
#extension GL_GOOGLE_include_directive : require
// One of the game's sun cascades (WoW's 2048^2 depth maps, roadmap M10) reduced for the fog: each texel of the
// fogCascadeSize^2 target holds the NEAREST (smallest) depth of its block - an occluder's thin parts survive, a gap narrower
// than a block closes (shadows grow by at most a block; the fog's froxels are far coarser). Recorded at the composite, after
// the world sampled the maps; read by the next frame's fog_light.comp (fogGameShadow).
#include "fog_common.glsl"
layout(set = 0, binding = 4) uniform sampler2D cascade;
layout(location = 0) out float nearest;
void main() {
    ivec2 size = textureSize(cascade, 0), factor = max(size / fogCascadeSize, ivec2(1)), base = ivec2(gl_FragCoord.xy) * factor;
    float d = 1.0;
    for (int y = 0; y < factor.y; ++y)
        for (int x = 0; x < factor.x; ++x) d = min(d, texelFetch(cascade, min(base + ivec2(x, y), size - 1), 0).r);
    nearest = d;
}
