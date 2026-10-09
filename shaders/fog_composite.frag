#version 450
#extension GL_GOOGLE_include_directive : require
// Pass 4: per pixel, at the view depth of the scene (sky: the fog volume's end), sample the integrated volume.
// Blend state is (ONE, SRC_ALPHA): result = inscatter + scene * transmittance. Debug views write alpha 0 (replace).
#include "fog_common.glsl"
layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(set = 0, binding = 5) uniform sampler3D mediumVolume;
layout(set = 0, binding = 6) uniform sampler3D lightingVolume;
layout(set = 0, binding = 7) uniform sampler3D integratedVolume;
layout(set = 0, binding = 8) uniform sampler2D waterMask; // water replay's mask: xyz view-space water surface, w alpha (0 = none)
layout(set = 0, binding = 10) uniform sampler2D sunRays;  // fog_rays.frag: the sun rays at a quarter of the resolution
layout(set = 0, binding = 12) uniform sampler2D sceneShade;  // fog_shade.frag: ambient occlusion, sun visibility, view depth (half resolution)
layout(location = 0) out vec4 outColor;
// Integrated value at slice coordinate t: texel k is the end of slice k, so t maps to texel coordinate t*Z - 1; in front of
// the first slice's end it blends from "no fog" at the camera. Linear in depth between slice ends, exact at t = 1.
vec4 fogSampleIntegrated(vec2 uv, float t) {
    float k = t * fog.grid.z - 1.0;
    if (k >= 0.0) return texture(integratedVolume, vec3(uv, (k + .5) / fog.grid.z));
    return mix(vec4(0, 0, 0, 1), texture(integratedVolume, vec3(uv, .5 / fog.grid.z)), clamp(t * fog.grid.z, 0.0, 1.0));
}
layout(location = 0) flat in float glareVisible; // fog_glare.vert: the share of the drawn sun disc left open (-1: off the screen)
// sun_rays: the beams at this pixel, added in the sun's colour after the fog (a look, not the air's physics). Only the beams'
// structure is added - the rays texture minus its mean over the same distance from the disc, turned about it by +-4 and +-8
// degrees - never its smooth glow: added whole (in Booty Bay's evening, capture 92693-5) it turned the sky round a hidden sun
// into a white-yellow blob WoW's glow blew out, and x the fog's opacity the beams vanished. Bent toward .8. Sky pixels over the
// disc itself (within its radius, faded out to twice it) take none: WoW draws it.
float fogRaysAt(vec2 p, vec2 size) { return texture(sunRays, p / (fogRaysFactor * size)).r; }
vec3 fogRaysLight(ivec2 pixel, vec2 uv, bool world) {
    vec3 d = normalize(fog.glare.xyz);
    vec3 v = vec3(dot(d, fog.right.xyz), dot(d, fog.up.xyz), dot(d, fog.forward.xyz));
    if (v.z <= .05) return vec3(0);
    vec2 disc = fog.viewport.xy + (v.xy * fog.proj.xy / v.z * .5 + .5) * fog.viewport.zw;
    vec2 size = vec2(textureSize(sunRays, 0)), p = vec2(pixel) + .5, r = p - disc;
    float around = 0.0;
    for (int i = 0; i < 4; ++i) {
        float a = radians(float(i < 2 ? i - 2 : i - 1) * 4.0);   // -8, -4, 4, 8 degrees
        around += fogRaysAt(disc + mat2(cos(a), sin(a), -sin(a), cos(a)) * r, size);
    }
    float beam = max(fogRaysAt(p, size) - .25 * around, 0.0);
    if (!world) {
        vec3 ray = normalize(fogWorldOffset(fogViewRay(uv * 2.0 - 1.0)));
        beam *= smoothstep(degrees(fog.phase.z), 2.0 * degrees(fog.phase.z), degrees(acos(clamp(dot(ray, d), -1.0, 1.0))));
    }
    float k = fogSoftLimit(4.0 * fog.rays.x * beam, .8);
    return k * fog.sunColor.rgb / max(max(fog.sunColor.r, max(fog.sunColor.g, fog.sunColor.b)), 1e-6);
}
// Scene shading at a full-resolution pixel of view depth z: the four nearest half-resolution texels, weighted bilinearly
// and by how close their depth is (an edge does not bleed the occlusion of the wall behind onto the character in front).
float fogSceneShade(ivec2 pixel, float z) {
    ivec2 size = textureSize(sceneShade, 0);
    vec2 t = (vec2(pixel) + .5) / fogShadeFactor - .5;
    ivec2 i0 = ivec2(floor(t));
    vec2 f = t - vec2(i0);
    float sum = 0.0, weight = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        vec4 s = texelFetch(sceneShade, clamp(i0 + o, ivec2(0), size - 1), 0);
        float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float depth = s.b > 0.0 ? exp(-abs(s.b - z) / (.05 * z + .1)) : 0.0;
        float w = bilinear * (depth + 1e-4);
        sum += w * s.r * s.g;
        weight += w;
    }
    float result = weight > 0.0 ? sum / weight : 1.0;
    if (fog.pointShadow.x > 0.0) {   // point-light shadows (a): a wider depth-aware tent over 4 x 4 texels - smooth, not grainy
        float ps = 0.0, pw = 0.0;
        for (int k = 0; k < 16; ++k) {
            ivec2 o = ivec2(k & 3, k >> 2) - 1;
            vec4 s = texelFetch(sceneShade, clamp(i0 + o, ivec2(0), size - 1), 0);
            vec2 away = abs(vec2(o) - f);
            float tent = max(2.0 - away.x, 0.0) * max(2.0 - away.y, 0.0);
            float w = tent * ((s.b > 0.0 ? exp(-abs(s.b - z) / (.05 * z + .1)) : 0.0) + 1e-4);
            ps += w * s.a;
            pw += w;
        }
        result *= pw > 0.0 ? ps / pw : 1.0;
    }
    return result;
}
vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float raw = texelFetch(sceneDepth, pixel, 0).r;
    bool world = raw >= fog.depthRange.x && raw < fog.depthRange.y;
    float z = fog.grid.w; // sky / cleared: the end of the fog volume, not the depth value
    float surface = 0.0;  // the scene's own view depth (not clamped to the fog's end): scene shading
    if (world) {
        float nz = (raw - fog.depthRange.x) / (fog.depthRange.y - fog.depthRange.x);
        surface = fog.proj.w / (nz - fog.proj.z);
        z = min(surface, fog.grid.w);
    }
    // WoW's water does not write depth: behind a water pixel the depth buffer holds the seabed - or nothing (the sky's depth,
    // open sea). Stop at the surface, and count it as world either way: sky_fog thinning the fog only where nothing lies
    // behind the water showed the seabed and the pier posts under it (Booty Bay, sky_fog .3).
    if (fog.mode.y != 0u) { vec4 w = texelFetch(waterMask, pixel, 0); if (w.a > 0.0 && w.z > 0.0) { z = min(z, w.z); world = true; } }
    vec2 uv = ((vec2(pixel) + .5) - fog.viewport.xy) / fog.viewport.zw; // = ndc * .5 + .5, v up
    vec3 uvt = vec3(uv, clamp(fogSliceOfDepth(z), 0.0, 1.0));
    uint mode = fog.mode.x;
    if (mode == 0u) {
        // fogShape: max_opacity, light_limit, and sky_fog over sky pixels (result = mix(sky, sky * T + S, k): the game's sky
        // already holds its haze and the sun's glow; Booty Bay morning: a weak open-sky fog greyed it, halo 197 -> 111).
        // Then auto exposure scales the whole fogged world (both terms of the blend), before WoW's glow and the UI.
        vec4 f = fogShape(fogSampleIntegrated(uv, uvt.z), world);
        // sun_glare: the fogged result pulled toward the sun's colour around the drawn disc, over whatever is in front of it too
        // (result = mix(scene * T + S, glare colour, k): both blend terms scaled by 1 - k). The fog itself is unchanged.
        if (fog.glare.w > 0.0) {
            vec3 ray = normalize(fogWorldOffset(fogViewRay(uv * 2.0 - 1.0)));
            float cosAngle = dot(ray, normalize(fog.glare.xyz));
            if (cosAngle > .94) {   // within ~20 degrees
                float visible = glareVisible;
                if (visible > 0.0) {
                    float deg = degrees(acos(min(cosAngle, 1.0)));
                    float k = fogGlareAmount(fog.glare.w, visible, deg);
                    // Not over the disc itself (sky pixels within its radius, faded out to twice it): a mix toward the sun's
                    // colour dimmed WoW's pale yellow disc to orange (Booty Bay evening: 255,255,165 -> 255,179,88).
                    if (!world) k *= smoothstep(degrees(fog.phase.z), 2.0 * degrees(fog.phase.z), deg);
                    vec3 c = fog.sunColor.rgb / max(max(fog.sunColor.r, max(fog.sunColor.g, fog.sunColor.b)), 1e-6);
                    f = vec4(f.rgb * (1.0 - k) + k * c, f.a * (1.0 - k));
                }
            }
        }
        if (fog.rays.x > 0.0) f.rgb += fogRaysLight(pixel, uv, world);
        // Scene shading darkens the scene behind the fog, not the fog's own light (the blend's second term: scene x f.a).
        if (surface > 0.0 && fog.shade.x + fog.shade.y + fog.shade.z + fog.pointShadow.x > 0.0) f.a *= fogSceneShade(pixel, surface);
        float e = fogExposure();
        outColor = vec4(f.rgb * e, f.a * e);
        return;
    }
    vec3 c = vec3(0);
    // DENSITY / EXTINCTION show the lighting volume's extinction: the temporally accumulated value the fog integrates. The
    // medium volume is one frame sampled at a jittered point inside each froxel (distant froxels are 10-20 yd deep), so
    // at smoke-clump edges it jumps between clump and clear air every frame - noise the fog itself never shows.
    if (mode == 1u) c = heat(texture(lightingVolume, uvt).a / max(2.0 * fog.medium.x, 1e-6));
    else if (mode == 2u) c = vec3(1.0 - exp(-texture(lightingVolume, uvt).a * 10.0));
    else if (mode == 3u) { vec4 l = texture(lightingVolume, uvt); c = l.a > 1e-6 ? l.rgb / l.a : vec3(0); }
    else if (mode == 6u) { vec4 l = texture(lightingVolume, uvt); c = vec3(l.r, .5 * (l.r + l.b), l.b); }   // sun (r) / sky (b) visibility
    else if (mode == 4u) c = fogSampleIntegrated(uv, uvt.z).rgb;
    else if (mode == 5u) c = vec3(fogSampleIntegrated(uv, uvt.z).a);
    outColor = vec4(c, 0.0);
}
