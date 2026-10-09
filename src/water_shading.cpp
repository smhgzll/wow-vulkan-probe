#include "water_shading.hpp"
#include <algorithm>
#include <cmath>

namespace depth_probe::water {

float fresnel_rough(float NV, float alpha) {
    const float nv = std::clamp(NV, 0.f, 1.f);
    return fresnelR0 + (std::max(1.f - alpha, fresnelR0) - fresnelR0) * std::pow(1.f - nv, 5.f);
}

void transmittance(const float sigma[3], float clarity, float path, float out[3]) {
    const float scale = 8.f + (1.f - 8.f) * std::clamp(clarity, 0.f, 1.f);
    for (int c = 0; c < 3; ++c) out[c] = std::exp(-sigma[c] * scale * path);
}

float pixel_roughness(float setting, float microScale, float lod) {
    const float rms = .235f * .34f * microScale, variance = rms * rms;
    return std::clamp(std::sqrt(setting * setting + variance * (1.f - std::exp2(-lod))), .02f, 1.f);
}

}  // namespace depth_probe::water
