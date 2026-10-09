#version 450
#extension GL_GOOGLE_include_directive : require
// sun_rays, pass 1 (a quarter of the resolution): how much light each 4x4 block of the screen sends toward the camera from
// the drawn disc's direction - its share of open sky (no surface, no water) x a weight falling off from the disc (the disc
// itself 1, the sky around it .4 / (1 + (degrees / falloff)^2), none beyond 40 degrees). Averaging the 16 texels lets thin
// ropes and leaves count by their coverage instead of flickering in and out.
#include "fog_common.glsl"
layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(set = 0, binding = 8) uniform sampler2D waterMask; // water replay's mask: w alpha (0 = none)
layout(location = 0) out float outMask;
float openShare(vec2 p, vec2 size) {   // the 2x2 texels around full-resolution position p
    vec2 uv = p / size;
    vec4 d = textureGather(sceneDepth, uv, 0);
    vec4 open = vec4(lessThan(d, vec4(fog.depthRange.x))) + vec4(greaterThanEqual(d, vec4(fog.depthRange.y)));
    if (fog.mode.y != 0u) open *= vec4(lessThanEqual(textureGather(waterMask, uv, 3), vec4(0.0)));
    return dot(open, vec4(.25));
}
void main() {
    vec2 size = vec2(textureSize(sceneDepth, 0));
    vec2 base = floor(gl_FragCoord.xy) * fogRaysFactor;
    float open = .25 * (openShare(base + vec2(1, 1), size) + openShare(base + vec2(3, 1), size) + openShare(base + vec2(1, 3), size) + openShare(base + vec2(3, 3), size));
    vec2 uv = ((base + 2.0) - fog.viewport.xy) / fog.viewport.zw;
    vec3 ray = normalize(fogWorldOffset(fogViewRay(uv * 2.0 - 1.0)));
    float deg = degrees(acos(clamp(dot(ray, normalize(fog.glare.xyz)), -1.0, 1.0)));
    float r = degrees(fog.phase.z), halo = deg / fog.rays.w;
    float weight = clamp(1.0 - clamp((deg - r) / (.5 * r), 0.0, 1.0) + .4 / (1.0 + halo * halo) * (1.0 - clamp((deg - 30.0) / 10.0, 0.0, 1.0)), 0.0, 1.0);
    outMask = open * weight;
}
