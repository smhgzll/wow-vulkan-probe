#pragma once
#include <array>
#include <cstdint>

// The water surface: CPU reference of shaders/water_surface.glsl (keep identical). One definition of the moving surface, used by
// every water effect (reflection, refraction, lighting, foam, glints): a base plane (the water level the game draws, flat) plus a sum
// of Gerstner (trochoidal) waves in WORLD space (yards, z up). A water particle at rest position x0 on the base plane moves to
//   P(x0) = (x0 + sum Q_i A_i D_i cos(theta_i), base + sum A_i sin(theta_i)),   theta_i = k_i D_i.x0 + phase_i(t)
// (GPU Gems 1 ch. 1; Tessendorf 2001). The normal and the horizontal Jacobian come from the same partial derivatives.
//
// Why per pixel, not per vertex (measured 2026-09-29, 115 water captures, tools: scratchpad water_mesh_density.py): WoW's water tiles
// are triangle strips on a regular 4.167-yard grid (33.33-yd map chunk / 8), up to 17x17 vertices per 66.7-yd tile, triangles 4.17 x 4.17
// x 5.89 yd. A 6.5-yd wave gets 1.6 vertices per wavelength there (aliased), an 11-yd wave 2.8 (faceted); only swells of 30+ yd would
// deform smoothly. So the composite intersects each pixel's view ray with the displaced surface instead (intersect_surface); the game's
// flat mesh keeps defining the coverage (the water mask) and the base plane.
namespace depth_probe::water {

struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };

constexpr float gravity = 10.7283f;   // yd/s^2: 9.80665 m/s^2 / 0.9144 m/yd (unit conversion, not tuning)
constexpr unsigned maxWaves = 5;

// One wave as configured (water.ini: waveN=direction,wavelength,amplitude,steepness,speed). A slot with wavelength or amplitude 0 is unused.
struct WaveSetting {
    float directionDeg = 0;   // travel direction, degrees from world +X toward +Y, added to WaveSettings::windDeg
    float wavelength = 0;     // yards
    float amplitude = 0;      // yards: crest height over the base plane
    float steepness = 1;      // orbit shape: 0 = particles only bob up and down, 1 = circular orbit (a true trochoid, sharper crests)
    float speed = 1;          // x the deep-water phase speed sqrt(g / k)
    bool operator==(const WaveSetting&) const = default;
};
// Defaults are TUNING values (calm water, slopes k*A ~ .02-.025 per wave): not measured from WoW, which has no waves of its own.
struct WaveSettings {
    std::array<WaveSetting, maxWaves> waves{{{0.f, 31.f, .10f, 1.f, 1.f}, {28.f, 19.f, .07f, 1.f, 1.f}, {-22.f, 11.5f, .045f, 1.f, 1.f},
                                             {47.f, 6.5f, .025f, 1.f, 1.f}, {0.f, 0.f, 0.f, 1.f, 1.f}}};
    float windDeg = 25.f;     // rotates every wave (tuning)
    float speed = .75f;       // global multiplier of the physical speeds (tuning: full deep-water speed read as sliding in a calm harbour)
    bool operator==(const WaveSettings&) const = default;
};

// What the shaders get (WaterParams::waveA / waveB / surface).
struct PackedWaves {
    float a[maxWaves][4]{};   // direction x, direction y, k = 2 pi / wavelength, amplitude (yards)
    float b[maxWaves][4]{};   // omega (rad/s), Q (orbit shape after the no-fold limit), phase now (rad, in [0, 2 pi)), 0
    float surface[4]{};       // active waves, sum of amplitudes, sum of Q k A (< 1: the surface never folds), sum of k A (slope bound)
};
// heightScale and steepnessScale are the menu's WAVE HEIGHT / WAVE STEEPNESS. seconds is unwrapped time (double: the phase stays exact
// after hours; the shader never multiplies omega by a large float time). The per-wave phase offsets are fixed constants.
PackedWaves pack_waves(const WaveSettings&, float heightScale, float steepnessScale, double seconds);

// A wave's weight at a pixel footprint (yards covered by one pixel on the water): waves shorter than ~8 footprints fade out, they
// would only alias and shimmer (and they could keep the ray intersection from converging at grazing angles).
float wave_lod(float k, float footprint);

struct SurfacePoint {
    Vec3 position;        // world position on the displaced surface
    Vec3 normal;          // world, unit, pointing up
    Vec2 parameter;       // x0: the rest position of the water there - textures and foam ride the waves with it
    Vec3 displacement;    // P - (x0, base): horizontal orbit and height
    float jacobian = 1;   // horizontal compression of the surface: 1 flat, < 1 on crests, 0 about to fold
    // Partial derivatives: dP/dx0 = (1 - sxx, -sxy, slope.x), dP/dy0 = (-sxy, 1 - syy, slope.y).
    float sxx = 0, syy = 0, sxy = 0;
    Vec2 slope;
};
// delay (seconds): the surface as it was that long ago (phase(t - delay) = phase(t) + omega * delay).
SurfacePoint evaluate_surface(const PackedWaves&, Vec2 x0, float baseZ, float footprint, float delay = 0.f);
// The displaced surface along a view ray (world; dir need not be unit but must point down): Newton's method on the rest position x0,
// from the base-plane hit, for F(x0) = P.xy(x0) - (the ray's x/y at height P.z(x0)) = 0 - its 2x2 derivative comes from the same partial
// derivatives as the normal. (A plain fixed-point iteration contracts only by ~cot(elevation) * sum k A: at 6 degrees it was still 3 cm
// off after 4 steps.) `hit` is the ray's point at the surface height (what the pixel sees), `residual` how far the surface point is from
// the ray (yards; 0 when converged).
bool intersect_surface(const PackedWaves&, Vec3 origin, Vec3 dir, float baseZ, float footprint, SurfacePoint& out, Vec3& hit,
                       float* residual = nullptr, int iterations = 3);

}  // namespace depth_probe::water
