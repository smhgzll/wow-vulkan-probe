// Froxel volumetric fog, shared by fog_inject/fog_light/fog_integrate (compute) and fog_composite (fragment).
// Mirror of src/fog_math.cpp (keep identical); layout mirrors FogParams in src/volumetric_fog.hpp (std140).
// Grid: X x Y froxels over the camera's NDC square (u = ndc.x * .5 + .5, v = ndc.y * .5 + .5, v up), Z slices over
// view depth [0, maxDistance] with d(t) = C (exp(t ln(1 + D/C)) - 1). Positions are camera-relative.
layout(std140, set = 0, binding = 0) uniform FogParams {
    vec4 grid;        // X, Y, Z, maxDistance
    vec4 dist;        // C, ln(1 + D/C), light_limit (ceiling of the light the fog adds, 0 = none), unused
    vec4 proj;        // P00, P11, P22, P23 (WoW +Z forward, 0..1 depth)
    vec4 viewport;    // game world-draw viewport x, y, width, height (height < 0 in WoW)
    vec4 depthRange;  // viewport minDepth, maxDepth, max fog opacity (max_opacity: 1 - T capped, 1 = none), exposure (0 = 1)
    vec4 right;       // camera right (world), w = camera world z
    vec4 up;          // camera up
    vec4 forward;     // camera forward
    vec4 medium;      // base density (1/yd), height falloff (1/yd), base height (world z), max extinction
    vec4 albedo;      // rgb, w = phase anisotropy g
    vec4 emissive;    // rgb radiance per yard; w = share of the fog applied over sky pixels (sky_fog)
    vec4 ambient;     // rgb isotropic in-scattering radiance
    vec4 sunDir;      // xyz direction TOWARD the light (world), w = enabled
    vec4 sunColor;    // rgb
    uvec4 mode;       // debug view (0 final, 1 density, 2 extinction, 3 lighting, 4 scattering, 5 transmittance, 6 sun/sky visibility), water, capture frame (1: write the visibility volume), 0
    vec4 jitter;      // xyz sub-froxel sample position in [0,1), w = history weight (0 = no history)
    vec4 prevRight;   // previous camera right, w = previous P00
    vec4 prevUp;      // previous camera up, w = previous P11
    vec4 prevForward;
    vec4 prevOffset;  // current camera - previous camera
    vec4 noise;       // amount, lattice size (yards), coverage threshold, 1 / mean coverage (fog::smoke_threshold/_mean)
    vec4 noiseOrigin; // (camera + wind drift) modulo 1536
    vec4 noiseDetail; // extra drift of the finer smoke octaves (rising, sideways), yards modulo 1536
    vec4 shadowRight;    // shadow camera (the previous frame's: its depth is in the shadow image) right, w = P00
    vec4 shadowUp;       // w = P11
    vec4 shadowForward;  // w = march length (yards), 0 = no shadows
    vec4 shadowOffset;   // current camera - shadow camera
    vec4 shadowViewport; // that frame's world viewport in full-resolution pixels
    vec4 shadowImage;    // shadow image width, height, downsample factor
    // Height memory (fog_memory_*.comp): top-down world grid of the highest surface seen per cell, toroidal around the camera.
    vec4 memoryA;        // camera position in cells relative to its cell (fraction x, y), camera world z, cell size (yards)
    ivec4 memoryB;       // camera cell x, y, grid size N, flags (1 = read it, 2 = clear everything, 4 = write it this frame)
    vec4 memoryC;        // decay this frame (yards), sky light share of the ambient, this submission's compute slot, sun strength under an open sky (x)
    // Sun map (fog_memory_*.comp): the same toroidal grid, but in the plane perpendicular to the sun (axes sunU, sunV); per
    // cell the observed surface CLOSEST to the sun (largest distance along sunS) - a shadow map filled from what was seen.
    vec4 sunMapA;        // camera position in cells relative to its cell (u, v fraction), camera distance along sunS, cell size
    ivec4 sunMapB;       // camera cell u, v, grid size, flags (1 read, 2 clear everything)
    vec4 sunU;           // xyz: first axis across the sun
    vec4 sunV;           // xyz: second axis across the sun
    vec4 sunS;           // xyz: toward the sun (the basis is kept until the sun moved over half a degree)
    vec4 sunArc;         // sun disc finder: xy the game light's horizontal direction, z sin(band half width) (0 = whole sky), w sin(lowest elevation)
    // The game's sun cascades (roadmap M10, fog::cascade_rows_problem): cascade k = rows 3k..3k+2 (light clip x, y in [-1, 1],
    // depth in [0, 1]) of the view-space position in the SHADOW camera's frame (the one whose composite reduced the maps).
    vec4 cascade[12];
    vec4 cascadeInfo;    // x 1 = the reduced maps (fog_light binding 16) hold them, y depth bias, z map size, w v sign (1: v = .5 - .5y)
    vec4 phase;          // sun_aureole: x weight of the narrow forward lobe (0 = none), y its g; z the drawn disc's radius (radians)
    vec4 glare;          // sun_glare: xyz toward the drawn disc, w strength (0 = none)
    vec4 rays;           // sun_rays (toward glare.xyz): x strength (0 = none), y share of the way to the disc marched, z weight kept per step, w sky weight falloff (degrees)
    vec4 shade;          // scene shading: x contact shadows, y ambient occlusion, z terrain shadows (0 = off), w 1 = frameCascade valid
    vec4 shadeLight;     // xyz toward the game's sun in view space, w terrain shadows from (yards from the camera)
    vec4 frameCascade[12];   // this frame's cascade rows (view space, finest first)
    vec4 pointShadow;    // point-light shadows: x strength (0 = off), y range (yards), z lights in pointLights, w yards around a light that block nothing
    vec4 pointLights[8]; // the nearest point lights: view-space position, w weight (day / night)
    vec4 pointShadowLength;  // x yards behind the caster where its point-light shadow is gone
} fog;
const float fogShadeFactor = 2.0;  // FogResources::shadeFactor: the shading image is half the resolution
const float fogRaysFactor = 4.0;  // FogResources::raysFactor: the sun rays' images are a quarter of the resolution
const int fogCascadeSize = 512;   // FogResources::cascadeSize: the reduced maps' side

const float fogPi = 3.14159265358979323846;

// ---- height memory ----------------------------------------------------------------------------------------------------
// World cell of a point given relative to the camera (cells are memoryA.w yards; the camera's own cell is memoryB.xy).
ivec2 fogMemoryCell(vec3 offset) { return fog.memoryB.xy + ivec2(floor(fog.memoryA.xy + offset.xy / fog.memoryA.w)); }
int fogWrap(int a, int n) { return ((a % n) + n) % n; }
ivec2 fogMemoryTexel(ivec2 cell) { return ivec2(fogWrap(cell.x, fog.memoryB.z), fogWrap(cell.y, fog.memoryB.z)); }
uint fogMemoryTag(ivec2 cell) { return (uint(cell.x) & 0xffffu) | ((uint(cell.y) & 0xffffu) << 16); }
bool fogMemoryInside(ivec2 cell) { return all(lessThan(abs(cell - fog.memoryB.xy), ivec2(fog.memoryB.z / 2 - 1))); }
// Heights as order-preserving uints (atomicMax works on them); 0 = nothing seen.
ivec2 fogSunCell(vec3 offset) { return fog.sunMapB.xy + ivec2(floor(fog.sunMapA.xy + vec2(dot(offset, fog.sunU.xyz), dot(offset, fog.sunV.xyz)) / fog.sunMapA.w)); }
bool fogSunInside(ivec2 cell) { return all(lessThan(abs(cell - fog.sunMapB.xy), ivec2(fog.sunMapB.z / 2 - 1))); }
// Host-read counters per compute slot (binding 12): the sun finder's 16 words, then the ground histogram (fog_ground.comp),
// the exposure histogram, the water histogram.
const uint fogReadbackWords = 16u + 256u + 64u + 256u;
const uint fogGroundBins = 256u;          // 1-yard bins of floor height relative to the camera, from -128 yd
const uint fogExposureBins = 64u;         // fog_exposure.comp: the fogged picture's luminance, log2 from -8 in 1/8 steps
const uint fogWaterBins = 256u;           // fog_memory_scatter.comp: water surface pixels by height, bins as the ground's
const uint fogFloorEmpty = 0xffffffffu;   // floor memory: nothing seen (every real encoded height is smaller)
uint fogHeightEncode(float z) { uint u = floatBitsToUint(z); return (u & 0x80000000u) != 0u ? ~u : u | 0x80000000u; }
float fogHeightDecode(uint u) { return uintBitsToFloat((u & 0x80000000u) != 0u ? u & 0x7fffffffu : ~u); }

float fogDepthOfSlice(float t) { return fog.dist.x * (exp(t * fog.dist.y) - 1.0); }
float fogSliceOfDepth(float d) { return log(1.0 + max(d, 0.0) / fog.dist.x) / fog.dist.y; }
float fogExtinction(float worldZ) {
    float e = fog.medium.x * exp(min(-(worldZ - fog.medium.z) * fog.medium.y, 80.0));
    return clamp(e, 0.0, fog.medium.w);
}
float fogPhaseHG(float cosTheta, float g) {
    float gg = g * g;
    float denom = max(1.0 + gg - 2.0 * g * cosTheta, 1e-6);
    return (1.0 - gg) / (4.0 * fogPi * denom * sqrt(denom));
}
// The sun's phase (fog::sun_phase, keep identical): the anisotropy lobe, plus sun_aureole of a narrow forward lobe - the bright
// ring of air around the sun that also outlines an occluder the sun is half behind.
float fogSunPhase(float cosTheta) {
    float base = fogPhaseHG(cosTheta, fog.albedo.w);
    return fog.phase.x > 0.0 ? mix(base, fogPhaseHG(cosTheta, fog.phase.y), fog.phase.x) : base;
}
// View-space direction (not normalised, z = 1) through NDC xy.
vec3 fogViewRay(vec2 ndc) { return vec3(ndc.x / fog.proj.x, ndc.y / fog.proj.y, 1.0); }
vec3 fogWorldOffset(vec3 v) { return fog.right.xyz * v.x + fog.up.xyz * v.y + fog.forward.xyz * v.z; }
// Beer-Lambert slab: T = exp(-sigma ds), scattered = S (1 - T) / sigma (-> S ds as sigma -> 0).
vec4 fogSlab(vec3 source, float sigmaT, float ds) {
    float x = sigmaT * ds;
    float weight = x > 1e-4 ? (1.0 - exp(-x)) / sigmaT : ds;
    return vec4(source * weight, exp(-x));
}
vec4 fogAccumulate(vec4 acc, vec4 slab) { return vec4(acc.rgb + acc.a * slab.rgb, acc.a * slab.a); }

// Froxel footprint (yards) at view depth z: the lateral froxel size, or a third of the slice thickness when larger.
float fogFootprint(float z) {
    float lateral = z * 2.0 / (fog.proj.y * fog.grid.y);
    float thickness = (z + fog.dist.x) * fog.dist.y / fog.grid.z;
    return max(lateral, thickness * .35);
}

// ---- the fog's look over a pixel (fog_composite.frag, fog_exposure.comp) ---------------------------------------------
// light_limit (fog::soft_limit, keep identical): the brightest channel of the added light is unchanged up to half the ceiling,
// then bent toward the ceiling with the same slope at the knee.
float fogSoftLimit(float peak, float ceiling) {
    if (ceiling <= 0.0 || peak <= .5 * ceiling) return peak;
    float knee = .5 * ceiling, room = ceiling - knee;
    return knee + room * (1.0 - exp(-(peak - knee) / room));
}
// Integrated fog (rgb in-scattered light, a transmittance) -> what the composite applies: max_opacity caps what it hides
// (its light scaled with it), light_limit bends the light it adds, and sky pixels take sky_fog of it.
vec4 fogShape(vec4 f, bool world) {
    float opacity = 1.0 - f.a, cap = fog.depthRange.z > 0.0 ? fog.depthRange.z : 1.0;   // 0: a layer build without max_opacity
    if (opacity > cap) f = vec4(f.rgb * (cap / opacity), 1.0 - cap);
    float peak = max(f.r, max(f.g, f.b));
    if (fog.dist.z > 0.0 && peak > .5 * fog.dist.z) f.rgb *= fogSoftLimit(peak, fog.dist.z) / peak;
    if (!world) { float k = fog.emissive.w; f = vec4(f.rgb * k, 1.0 - k * (1.0 - f.a)); }
    return f;
}
float fogExposure() { return fog.depthRange.w > 0.0 ? fog.depthRange.w : 1.0; }
// Sun glare (fog::glare_psf / fog::glare_amount, keep identical): veiling glare around a source, as a function of the angle from
// it in degrees - a core of 2.5 degrees and a skirt of 8, faded out between 12 and 20 (the composite looks no further: no ring
// at the cut) - and the pull toward the sun's colour it gives with the visible share of the disc (cube root: a sliver of the
// sun is still blinding).
float fogGlarePsf(float degrees) {
    return (.6 / (1.0 + (degrees / 2.5) * (degrees / 2.5)) + .4 / (1.0 + (degrees / 8.0) * (degrees / 8.0))) * (1.0 - smoothstep(12.0, 20.0, degrees));
}
float fogGlareAmount(float strength, float visible, float degrees) { return clamp(strength * pow(clamp(visible, 0.0, 1.0), 1.0 / 3.0) * fogGlarePsf(degrees), 0.0, 1.0); }
