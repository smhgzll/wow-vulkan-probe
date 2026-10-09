// Depth -> world reconstruction. Mirror of src/depth_reconstruction.cpp (keep identical); validated on real
// captures by tools/depth-reconstruction-check.py. Positions are camera-relative ("offset" = world - camera).
// Needs depthTexture (the selected scene depth, same extent as this framebuffer) and fx.
bool reconIsWorld(float raw) { return raw >= fx.reconDepth.x && raw < fx.reconDepth.y; }
float reconRaw(ivec2 p) { return texelFetch(depthTexture, clamp(p, ivec2(0), textureSize(depthTexture, 0) - 1), 0).r; }
// View space of the game camera: x right, y up, z forward (distance along the view axis).
bool reconView(ivec2 p, out vec3 v) {
    v = vec3(0);
    float raw = reconRaw(p);
    if (!reconIsWorld(raw)) return false;
    vec2 ndc = ((vec2(p) + .5) - fx.reconViewport.xy) / fx.reconViewport.zw * 2.0 - 1.0;
    float nz = (raw - fx.reconDepth.x) / (fx.reconDepth.y - fx.reconDepth.x);
    float z = fx.reconDepth.w / (nz - fx.reconDepth.z);
    v = vec3(ndc * z / fx.reconProj.xy, z);
    return z > 0.0;
}
vec3 reconOffset(vec3 v) { return fx.reconRight.xyz * v.x + fx.reconUp.xyz * v.y + fx.reconForward.xyz * v.z; }

vec3 reconHeat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}
// World-offset step to the neighbouring pixel in x and y, from the side with the smaller depth jump (so a
// silhouette does not smear). Stands in for dFdx/dFdy, which are undefined after the per-pixel early returns.
// Without a usable neighbour, a pixel's footprint facing the camera.
void reconSteps(ivec2 p, vec3 center, out vec3 dx, out vec3 dy) {
    vec3 r, l, u, d;
    bool hr = reconView(p + ivec2(1, 0), r), hl = reconView(p - ivec2(1, 0), l);
    bool hu = reconView(p - ivec2(0, 1), u), hd = reconView(p + ivec2(0, 1), d);
    float pixel = center.z * 2.0 / (fx.reconProj.y * abs(fx.reconViewport.w));
    dx = (hr && (!hl || abs(r.z - center.z) < abs(center.z - l.z))) ? reconOffset(r - center) : hl ? reconOffset(center - l) : fx.reconRight.xyz * pixel;
    dy = (hu && (!hd || abs(u.z - center.z) < abs(center.z - d.z))) ? reconOffset(u - center) : hd ? reconOffset(center - d) : fx.reconUp.xyz * pixel;
}
// Line strength for iso-lines of a quantity every `spacing`; `step` = how much it changes per pixel.
float reconLines(float value, float step, float spacing, float widthPx) {
    float d = abs(fract(value / spacing - .5) - .5) * spacing / max(step, 1e-6);
    return 1.0 - clamp(d / widthPx, 0.0, 1.0);
}
vec3 reconNormal(ivec2 p, vec3 center) {
    vec3 dx, dy;
    reconSteps(p, center, dx, dy);
    vec3 n = cross(dx, dy);
    n = dot(n, n) > 1e-20 ? normalize(n) : vec3(0, 0, 1);
    return dot(n, -reconOffset(center)) < 0.0 ? -n : n; // face the camera
}

// Modes: 1 depth classes, 2 distance, 3 world position, 4 world grid, 5 normals, 6 height, 7 probe.
vec4 reconDebug(ivec2 p) {
    uint mode = fx.recon.x;
    float raw = reconRaw(p);
    if (mode == 1u) {
        if (raw >= 1.0) return vec4(1, 0, 1, .85);                     // cleared, nothing drawn
        if (!reconIsWorld(raw)) return vec4(.15, .35, 1, .85);        // sky depth ranges (0.998..1)
        vec3 v; reconView(p, v);
        return vec4(vec3(1.0 - clamp(length(v) / 400.0, 0.0, .9)), .85);
    }
    vec3 v;
    if (!reconView(p, v)) return vec4(0);
    vec3 off = reconOffset(v);
    vec3 world = fx.reconCameraMod.xyz + off;                         // world modulo 1000 yd
    float worldZ = fx.reconRight.w + off.z;                           // true world height
    float dist = length(off);
    vec3 dx, dy;
    reconSteps(p, v, dx, dy);
    vec3 step3 = max(abs(dx), abs(dy));                               // per-pixel change of world x, y, z
    float stepDist = max(abs(length(off + dx) - dist), abs(length(off + dy) - dist));
    if (mode == 2u) {
        float ring = reconLines(dist, stepDist, 10.0, 1.2);
        return vec4(mix(reconHeat(dist / 300.0), vec3(1), ring), .8);
    }
    if (mode == 3u) return vec4(fract(world / 16.0), .85);
    if (mode == 4u) {
        float s = fx.reconProj.z;
        float fine = max(reconLines(world.x, step3.x, s, 1.0), reconLines(world.y, step3.y, s, 1.0));
        float coarse = max(reconLines(world.x, step3.x, s * 10.0, 1.8), reconLines(world.y, step3.y, s * 10.0, 1.8));
        float contour = reconLines(worldZ, step3.z, s, 1.0);
        float sea = reconLines(worldZ + 500.0, step3.z, 1000.0, 2.0); // z = 0
        vec4 c = vec4(1, 1, 1, fine * .7);
        c = mix(c, vec4(.25, .9, 1, .8), contour);
        c = mix(c, vec4(1, .8, .15, .9), coarse);
        c = mix(c, vec4(1, .15, .1, 1), sea);
        return c;
    }
    if (mode == 5u) return vec4(reconNormal(p, v) * .5 + .5, .9);
    if (mode == 6u) {
        float rel = worldZ - fx.reconRight.w;
        vec3 c = reconHeat((rel + 30.0) / 60.0) * (1.0 - .6 * reconLines(worldZ, step3.z, 1.0, 1.0));
        return vec4(mix(c, vec3(1, .15, .1), reconLines(worldZ + 500.0, step3.z, 1000.0, 2.0)), .85);
    }
    if (mode == 7u) {
        if (fx.reconCameraMod.w == 0.0) return vec4(0);
        vec3 d = off - fx.reconProbe.xyz;
        float r = length(d), radius = fx.reconProj.w;
        if (r < radius) return vec4(1, .15, .1, .75);
        float stepR = max(abs(length(d + dx) - r), abs(length(d + dy) - r));
        float shell = reconLines(r, stepR, radius, 1.2) * step(r, radius * 5.0);
        float slice = reconLines(off.z - fx.reconProbe.z + 500.0, step3.z, 1000.0, 1.5); // horizontal plane through the probe
        return vec4(mix(vec3(1, .6, .2), vec3(1, 1, .2), slice), max(shell * .8, slice));
    }
    return vec4(0);
}
