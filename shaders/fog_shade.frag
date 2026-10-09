#version 450
#extension GL_GOOGLE_include_directive : require
// Scene shading (half resolution, before the composite that applies it to what is behind the fog): what the game does not
// shadow, from this frame's depth. r = ambient occlusion, g = sun visibility (contact + terrain), a = point-light shadows (the
// composite filters it wider), b = the view depth (for
// the composite's depth-aware upsampling; 0 = sky), strengths already applied (1 = untouched).
//  - ambient occlusion: around each pixel, 6 directions x 4 steps within 1.5 yd - how much of the hemisphere over the
//    surface (normal from the depth) nearby geometry takes; corners, wall feet, under trees.
//  - contact shadows: 16 steps toward the game's sun over .4-3 yd (longer farther away) - grass, stones, feet, sills the
//    2048 cascades miss.
//  - terrain shadows: 24 steps over up to 100 yd - hills and cliffs, which WoW draws into no cascade, from terrain_shadows_from
//    yards on (faded in); only land blocks
//    (what the game's maps hold - characters, models, buildings - casts its own shadow already).
// Both sun terms only where the game's own cascades (this frame's rows and reduced maps) see the sun: never a shadow on a
// shadow. A step is blocked when the depth there is in front of it by more than a bias and less than a thickness (thin
// things do not cast endless shadows).
#include "fog_common.glsl"
layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(set = 0, binding = 8) uniform sampler2D waterMask;
layout(set = 0, binding = 11) uniform sampler2DArray cascadeMaps;   // the game's cascades reduced (nearest depth per block)
layout(set = 0, binding = 13, r32ui) uniform readonly uimage2D floorMap;   // the fog's floor memory (lowest surface per cell)
layout(location = 0) out vec4 outShade;

bool viewAt(vec2 p, out vec3 v) {   // p: full-resolution pixel coordinates
    ivec2 size = textureSize(sceneDepth, 0), i = ivec2(floor(p));
    if (any(lessThan(i, ivec2(0))) || any(greaterThanEqual(i, size))) return false;
    float raw = texelFetch(sceneDepth, i, 0).r;
    if (raw < fog.depthRange.x || raw >= fog.depthRange.y) return false;
    float nz = (raw - fog.depthRange.x) / (fog.depthRange.y - fog.depthRange.x);
    float z = fog.proj.w / (nz - fog.proj.z);
    vec2 uv = ((vec2(i) + .5) - fog.viewport.xy) / fog.viewport.zw;
    v = fogViewRay(uv * 2.0 - 1.0) * z;
    return true;
}
vec2 pixelOf(vec3 v) { return fog.viewport.xy + (v.xy * fog.proj.xy / v.z * .5 + .5) * fog.viewport.zw; }

// The game's own sun cascades at a view position: stored depth - the position's light depth (> 0: nothing between it and the
// sun in the maps; about 0: the surface of a caster; < 0: behind one). A large value outside the cascades or without them.
float gameDepthGap(vec3 v) {
    if (fog.shade.w <= 0.0) return 1e9;
    vec4 p = vec4(v, 1.0);
    float n = float(textureSize(cascadeMaps, 0).x), edge = 1.0 - 2.0 / n;
    for (int k = 0; k < 4; ++k) {
        vec3 c = vec3(dot(fog.frameCascade[3 * k], p), dot(fog.frameCascade[3 * k + 1], p), dot(fog.frameCascade[3 * k + 2], p));
        if (abs(c.x) > edge || abs(c.y) > edge) continue;
        float stored = texture(cascadeMaps, vec3(.5 + .5 * c.x, .5 + .5 * c.y, float(k))).r;   // WoW: (.5 + .5x, .5 + .5y), reference z
        return stored - c.z;
    }
    return 1e9;
}
bool gameLit(vec3 v) { return gameDepthGap(v) >= -1e-4; }   // the game's own sun shadows (lit outside its cascades)
// A caster the game shadows itself (models, buildings - they are in its maps; land is not): no blocker for the land march,
// else characters threw long streaks across the ground (2026-09-28).
bool gameCaster(vec3 v) { return gameDepthGap(v) <= 2e-4; }

// Yards over the lowest surface the fog's floor memory saw in this point's cell (0 when it saw none there).
float overFloor(vec3 v) {
    if ((fog.memoryB.w & 1) == 0) return 0.0;
    vec3 offset = fogWorldOffset(v);
    ivec2 cell = fogMemoryCell(offset);
    if (!fogMemoryInside(cell)) return 0.0;
    uint f = imageLoad(floorMap, fogMemoryTexel(cell)).r;
    return f == fogFloorEmpty ? 0.0 : fog.memoryA.z + offset.z - fogHeightDecode(f);
}
// Nothing holds it up: the points .2, .5, 1 and 1.5 yd straight below it (world) - where it would continue if it stood on
// something - see, on screen, something farther than them (or the sky). A name or an icon over a character floats; the
// character's own head has its body under it, a wall or a ledge its own face (2026-09-28: a depth-edge test made the
// characters' upper bodies and the Den's ledges "floating" indoors, where the game draws no cascades, and cut their shadows).
bool unsupported(vec3 v) {
    vec3 down = -vec3(fog.right.z, fog.up.z, fog.forward.z);   // world down in view space
    ivec2 size = textureSize(sceneDepth, 0);
    for (int i = 0; i < 4; ++i) {
        vec3 q = v + down * (i == 0 ? .2 : i == 1 ? .5 : i == 2 ? 1.0 : 1.5);
        if (q.z < .3) continue;
        vec2 pq = pixelOf(q);
        if (any(lessThan(pq, vec2(0))) || any(greaterThanEqual(pq, vec2(size)))) continue;   // off screen: no evidence
        vec3 hit;
        if (!viewAt(pq, hit) || hit.z > q.z + .6 + .02 * q.z) return true;
    }
    return false;
}
// Something floating over the ground that casts no shadow in the game: names, icons, effects that write depth ("Lazy Peon"
// threw a long streak): over 1.5 yd above the floor memory, not in the game's maps, nothing under it.
bool floating(vec3 v) { return overFloor(v) > 1.5 + .02 * v.z && !gameCaster(v) && unsupported(v); }

// The game's point lights (torches, fires; fog.pointLights, view space): toward each light within range that this surface
// faces, a march over the depth (steps of .4 yd at most, 8..40); blocked = in its shadow, darker by the strength x its weight x a smooth falloff to the
// range (the game's own falloff is not in its constants). The weight (fewer shadows in daylight) counts only where the sun
// reaches: in a cave, under a roof or in the game's sun shadow a torch is the light. Nothing within pointShadow.w yards of
// the light blocks it (its brazier, lantern or candlestick, which the light is not always centred in). The light is a small
// sphere (.3 yd, a point per pixel): with the composite's wider filter of this channel, soft edges instead of grain.
// The shadow fades out behind its caster (pointShadowLength: a low fire would throw a head's shadow endlessly).
// Names and icons (floating) block nothing; characters, models and buildings do. 1 = no point shadow here.
// A normal from +-8 pixels (the smaller difference on each axis): smooth across the terrain's triangles, which the 2-pixel
// one follows exactly - a torch at ground height meets them at grazing angles, each on another side of "facing".
vec3 wideNormal(vec2 p, vec3 v, vec3 fallback) {
    vec3 a, b, c, d;
    bool ha = viewAt(p + vec2(8, 0), a), hb = viewAt(p - vec2(8, 0), b), hc = viewAt(p + vec2(0, 8), c), hd = viewAt(p - vec2(0, 8), d);
    if (!(ha || hb) || !(hc || hd)) return fallback;
    vec3 dx = ha && (!hb || abs(a.z - v.z) < abs(v.z - b.z)) ? a - v : v - b;
    vec3 dy = hc && (!hd || abs(c.z - v.z) < abs(v.z - d.z)) ? c - v : v - d;
    vec3 n = normalize(cross(dx, dy));
    return dot(n, v) > 0.0 ? -n : n;
}
float pointShadows(vec2 p, vec3 v, vec3 n, float jitter) {
    bool haveWide = false;
    float lit = 1.0, range = fog.pointShadow.y, ignore = fog.pointShadow.w;
    int count = int(fog.pointShadow.z);
    int sunlit = -1;   // gameLit(v), when first needed
    vec2 area = vec2(fract(jitter * 1.6180339 + .31), fract(jitter * 2.6180339 + .71)) * 2.0 - 1.0;
    for (int i = 0; i < count && i < 8; ++i) {
        vec3 center = fog.pointLights[i].xyz, toLight = center - v;
        float d = length(toLight);
        if (d > range || d < ignore) continue;   // inside the ignore sphere: lit by it, nothing between
        vec3 l0 = toLight / d;
        if (!haveWide) { n = wideNormal(p, v, n); haveWide = true; }
        // Facing: only what clearly turns its back to the light (a wall's far side) takes none - the ground around a torch
        // at its own height faces it at grazing angles, and a hard cut there drew the terrain's triangles (2026-09-28).
        float facing = smoothstep(-.5, -.2, dot(n, l0));
        if (facing <= 0.0) continue;
        float weight = fog.pointLights[i].w;
        if (weight < 1.0) { if (sunlit < 0) sunlit = gameLit(v) ? 1 : 0; if (sunlit == 0) weight = 1.0; }
        float w = fog.pointShadow.x * weight * facing * (1.0 - smoothstep(.25 * range, range, d));
        if (w < .01) continue;
        vec3 t1 = normalize(cross(l0, abs(l0.y) < .9 ? vec3(0, 1, 0) : vec3(1, 0, 0))), t2 = cross(l0, t1);
        vec3 target = center + (t1 * area.x + t2 * area.y) * .3, toTarget = target - v;
        float dt = length(toTarget);
        vec3 l = toTarget / dt, start = v + n * (.01 + .002 * v.z);
        float len = dt - ignore;
        int steps = clamp(int(ceil(len / .4)), 8, 40);   // a step at most .4 yd: a character is not stepped over
        for (int s = 0; s < steps; ++s) {
            vec3 q = start + l * ((float(s) + jitter) / float(steps) * len);
            if (q.z < .3) break;
            vec2 pq = pixelOf(q);
            vec3 hit;
            if (!viewAt(pq, hit)) continue;
            float front = q.z - hit.z;
            if (front > .03 + .003 * q.z && front < 1.0 + .01 * q.z && distance(hit, center) > ignore && !floating(hit)) {   // (thickness: a character)
                // Fading out behind the caster: whole at its foot, gone pointShadowLength yards behind it.
                float reach = fog.pointShadowLength.x;
                lit *= 1.0 - w * (1.0 - smoothstep(reach / 3.0, reach, distance(v, hit)));
                break;
            }
        }
    }
    return lit;
}

// Toward the sun from v: the share of the way (0..1) at which the depth blocks it, 1 = open. landOnly: only land blocks
// (not what the game's maps hold, not floating things).
float march(vec3 v, vec3 n, vec3 l, float len, int steps, float jitter, float thickness, bool landOnly) {
    vec3 start = v + n * (.01 + .002 * v.z);
    for (int i = 0; i < steps; ++i) {
        float s = (float(i) + jitter) / float(steps);
        float t = s * s * len + .05;   // denser near the surface
        vec3 q = start + l * t;
        if (q.z < .3) return 1.0;
        vec3 w;
        vec2 pq = pixelOf(q);
        if (!viewAt(pq, w)) continue;
        float front = q.z - w.z;
        if (front > .02 + .003 * q.z && front < thickness + .02 * t && !(landOnly && (gameCaster(w) || floating(w)))) return s;
    }
    return 1.0;
}

void main() {
    vec2 p = floor(gl_FragCoord.xy) * fogShadeFactor + .5 * fogShadeFactor;   // this texel's centre in full-resolution pixels
    vec3 v;
    if (!viewAt(p, v)) { outShade = vec4(1.0, 1.0, 0.0, 1.0); return; }   // sky / cleared
    if (fog.mode.y != 0u && texelFetch(waterMask, ivec2(p), 0).a > 0.0) { outShade = vec4(1.0, 1.0, v.z, 1.0); return; }
    if (floating(v)) { outShade = vec4(1.0, 1.0, v.z, 1.0); return; }   // a name or an icon: not shaded either
    // Normal from the depth: the smaller difference on each axis (no normal across an edge).
    vec3 a, b, c, d;
    bool ha = viewAt(p + vec2(fogShadeFactor, 0), a), hb = viewAt(p - vec2(fogShadeFactor, 0), b);
    bool hc = viewAt(p + vec2(0, fogShadeFactor), c), hd = viewAt(p - vec2(0, fogShadeFactor), d);
    vec3 dx = ha && (!hb || abs(a.z - v.z) < abs(v.z - b.z)) ? a - v : hb ? v - b : vec3(1, 0, 0);
    vec3 dy = hc && (!hd || abs(c.z - v.z) < abs(v.z - d.z)) ? c - v : hd ? v - d : vec3(0, 1, 0);
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, v) > 0.0) n = -n;   // toward the camera
    float jitter = fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(.06711056, .00583715))));
    // Ambient occlusion.
    float ao = 1.0;
    if (fog.shade.y > 0.0) {
        const float radius = 1.5;
        float rpx = radius * fog.proj.x * abs(fog.viewport.z) * .5 / v.z;
        if (rpx > 2.0) {
            float occ = 0.0;
            for (int i = 0; i < 6; ++i) {
                float angle = (float(i) + jitter) * 1.0471976;
                vec2 dir = vec2(cos(angle), sin(angle));
                for (int j = 0; j < 4; ++j) {
                    vec3 w;
                    if (!viewAt(p + dir * rpx * (float(j) + .5 + .5 * jitter) / 4.0, w)) continue;
                    vec3 h = w - v;
                    float len = length(h);
                    if (len < radius && len > .02) occ += max(dot(n, h / len) - .15, 0.0) * (1.0 - len * len / (radius * radius));
                }
            }
            ao = 1.0 - fog.shade.y * clamp(occ / 24.0 * 2.0, 0.0, 1.0);
        }
    }
    // Sun: contact and terrain, where the game's cascades see it and the surface faces it.
    float sun = 1.0;
    vec3 l = normalize(fog.shadeLight.xyz);
    if ((fog.shade.x > 0.0 || fog.shade.z > 0.0) && dot(n, l) > .05 && gameLit(v)) {
        if (fog.shade.x > 0.0) sun *= 1.0 - fog.shade.x * (1.0 - march(v, n, l, clamp(.04 * v.z, .4, 3.0), 16, jitter, .6 + .01 * v.z, false));
        // Terrain: faded in from shadeLight.w yards on (near the character the game's own shadows are there).
        float from = fog.shadeLight.w, far = from > 0.0 ? smoothstep(from, from + max(10.0, .5 * from), v.z) : 1.0;
        if (fog.shade.z > 0.0 && far > 0.0) sun *= 1.0 - fog.shade.z * far * (1.0 - march(v, n, l, min(100.0, .6 * v.z + 10.0), 24, jitter, 4.0 + .05 * v.z, true));
    }
    outShade = vec4(ao, sun, v.z, fog.pointShadow.x > 0.0 ? pointShadows(p, v, n, jitter) : 1.0);
}
