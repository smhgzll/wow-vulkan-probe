#pragma once
// MODERN water shading terms: CPU reference of the formulas in shaders/water_composite.frag's modern path (keep identical). Tested in
// tests/water_surface_test.cpp and, against the GPU, in tests/water_vulkan_smoke.cpp (the FRESNEL and ABSORPTION debug views).
namespace depth_probe::water {

// Air-water reflectance at normal incidence: ((1 - n) / (1 + n))^2 with n ~ 1.33 (MEASURED physics; the shader uses .02).
constexpr float fresnelR0 = .02f;
// Schlick's Fresnel with the roughness-aware ceiling (Lagarde): R0 + (max(1 - alpha, R0) - R0) (1 - N.V)^5. A rough surface does not reach a
// perfect mirror at grazing angles.
float fresnel_rough(float NV, float alpha);
// Beer-Lambert transmittance through `path` yards of water, per channel: exp(-sigma * scale * path), scale = mix(8, 1, clarity) (clarity 1 =
// the sigma as given, pure water by default; 0 = 8x murkier: TUNING).
void transmittance(const float sigma[3], float clarity, float path, float out[3]);
// GGX roughness of a pixel: the setting plus the micro-ripple slope variance the mip filter averaged away, half of it (HEURISTIC; the full
// Toksvig width flattened the sun path). microScale = the MICRO RIPPLES setting, lod = the mip level the micro ripples were sampled at.
float pixel_roughness(float setting, float microScale, float lod);

}  // namespace depth_probe::water
