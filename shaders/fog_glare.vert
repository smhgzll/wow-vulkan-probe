#version 450
#extension GL_GOOGLE_include_directive : require
// The composite's fullscreen triangle (as fog_composite.vert) plus sun_glare's visible share of the drawn disc, measured once
// per draw here instead of per pixel: every vertex computes the same value and hands it on flat.
#include "fog_common.glsl"
layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(location = 0) flat out float glareVisible;
// The share of a disc (centre, radius in pixels) this frame's depth leaves open (sky / cleared): 64 points on a spiral; points
// off the screen do not count. -1 when none is on the screen.
float discShare(vec2 centre, float radius, ivec2 size) {
    float seen = 0.0, counted = 0.0;
    for (int i = 0; i < 64; ++i) {
        float a = float(i) * 2.39996323, r = radius * sqrt((float(i) + .5) / 64.0);
        ivec2 p = ivec2(centre + r * vec2(cos(a), sin(a)));
        if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) continue;
        float raw = texelFetch(sceneDepth, p, 0).r;
        counted += 1.0;
        if (raw < fog.depthRange.x || raw >= fog.depthRange.y) seen += 1.0;
    }
    return counted > 0.0 ? seen / counted : -1.0;
}
// The drawn disc's direction is known to ~0.5 degrees (the arc it is snapped to; Booty Bay 2026-09-27: 8 px, a third of the
// disc's radius, into the mast that hid most of it - 1/24 of the disc measured where 18% showed): the most open of the
// predicted disc and eight shifted by 0.35 of its radius. A disc well behind a wide obstacle stays 0.
float glareShare() {
    vec3 d = normalize(fog.glare.xyz);
    vec3 v = vec3(dot(d, fog.right.xyz), dot(d, fog.up.xyz), dot(d, fog.forward.xyz));
    if (v.z <= .05) return -1.0;
    vec2 centre = fog.viewport.xy + (v.xy * fog.proj.xy / v.z * .5 + .5) * fog.viewport.zw;
    float radius = tan(fog.phase.z) * fog.proj.x * abs(fog.viewport.z) * .5 / v.z;
    ivec2 size = textureSize(sceneDepth, 0);
    float best = discShare(centre, radius, size);
    for (int k = 0; k < 8; ++k) {
        float a = float(k) * .785398163;
        best = max(best, discShare(centre + .35 * radius * vec2(cos(a), sin(a)), radius, size));
    }
    return best;
}
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    glareVisible = fog.glare.w > 0.0 ? glareShare() : -1.0;
}
