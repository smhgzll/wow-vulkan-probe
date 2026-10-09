#version 450
#extension GL_GOOGLE_include_directive : require
// sun_rays, pass 2 (a quarter of the resolution): every texel marches 64 steps toward the drawn disc on the screen (rays.y of
// the way) over pass 1's mask, each step weighing rays.z of the one before, and keeps the weighted mean: open sky near the disc
// along the way = light, what stands in front of it = its shadow, so beams fan out of the gaps (GPU Gems 3 ch. 13,
// "Volumetric Light Scattering as a Post-Process"). Off the screen counts as nothing. The start is jittered per texel (no
// banding); the composite's bilinear upsampling smooths it.
#include "fog_common.glsl"
layout(set = 0, binding = 9) uniform sampler2D raysMask;
layout(location = 0) out float outRays;
const int steps = 64;
void main() {
    vec3 d = normalize(fog.glare.xyz);
    vec3 v = vec3(dot(d, fog.right.xyz), dot(d, fog.up.xyz), dot(d, fog.forward.xyz));
    if (v.z <= .05) { outRays = 0.0; return; }   // the disc behind the camera
    vec2 size = vec2(textureSize(raysMask, 0));
    vec2 disc = (fog.viewport.xy + (v.xy * fog.proj.xy / v.z * .5 + .5) * fog.viewport.zw) / fogRaysFactor;   // in mask texels
    vec2 delta = (disc - gl_FragCoord.xy) * fog.rays.y / float(steps);
    float jitter = fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(.06711056, .00583715))));
    vec2 q = gl_FragCoord.xy + delta * jitter;
    float sum = 0.0, total = 0.0, w = 1.0;
    for (int i = 0; i < steps; ++i) {
        if (all(greaterThanEqual(q, vec2(0.0))) && all(lessThan(q, size))) sum += w * texture(raysMask, q / size).r;
        total += w;
        w *= fog.rays.z;
        q += delta;
    }
    outRays = sum / total;
}
