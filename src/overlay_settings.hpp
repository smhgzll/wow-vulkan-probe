#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace depth_probe {
// std140 scalar groups, identical on 32- and 64-bit hosts. The first 32 bytes preserve legacy settings.
struct alignas(16) OverlaySettings {
    float aoStrength = 1.0f;
    float aoRadius = 16.0f; // outer radius; inner radii retain 3/16 and 8/16 ratios
    float aoBias = 0.002f;  // relative-depth smoothstep lower bound; width stays .038
    float rainbowStrength = 1.0f;
    float hueFrequency = 1.0f;
    float opacity = 0.95f;
    uint32_t effectEnabled = 1;
    uint32_t debugMode = 3;
    uint32_t depthConvention = 0; // 0 Auto/unknown, 1 Normal Z, 2 Reversed Z
    uint32_t projectionMode = 0; // 0 unavailable, 1 manual symmetric perspective, 2 manual orthographic
    uint32_t legacyAOEnabled = 0, reservedDepth = 0;
    float nearPlane = 0, farPlane = 0, verticalFov = 0, orthoHeight = 0;
    uint32_t ssaoEnabled = 0, ssaoSamples = 12, aoBlur = 0, rainbowEnabled = 0;
    float ssaoStrength = 1, ssaoRadius = .5f, ssaoBias = .025f, reservedAOFloat = 0;
    uint32_t sharpenEnabled=0, chromaticEnabled=0, fogEnabled=0, contoursEnabled=0;
    uint32_t outlineEnabled=0, gpuTimingEnabled=0, drawProbeCapture=0, reservedEffects=0;
    float sharpenStrength=.3f, chromaticAmount=1, contourFrequency=64, contourIntensity=.35f;
    float fogStart=10, fogEnd=100, fogDensity=.5f, fogR=.3f;
    float fogG=.4f, fogB=.6f, edgeStrength=.5f, edgeWidth=1;
    uint32_t ssrEnabled=0, ssrSteps=24, reservedSSR[2]{};
    float ssrStepSize=.25f, ssrThickness=.15f, ssrMaxDistance=20, ssrFade=.1f;
    float reflectionStrength=.35f, reservedSSRFloat[3]{};
    bool operator==(const OverlaySettings&) const = default;
};
static_assert(sizeof(OverlaySettings) == 224 && alignof(OverlaySettings) == 16);
static_assert(offsetof(OverlaySettings, hueFrequency) == 16 && offsetof(OverlaySettings, debugMode) == 28);
struct SettingControl {
    const char* key;
    float low, high;
    float OverlaySettings::* floating = nullptr;
    uint32_t OverlaySettings::* integer = nullptr;
};
const std::vector<SettingControl>& settings_controls();
uint32_t setting_label(const char* key);
float setting_value(const OverlaySettings&, const SettingControl&);
float setting_fraction(const OverlaySettings&, const SettingControl&);
void set_setting_fraction(OverlaySettings&, const SettingControl&, float);
void step_setting(OverlaySettings&, const SettingControl&, int direction);
OverlaySettings sanitize_settings(OverlaySettings settings);
std::string settings_config_path();
OverlaySettings load_settings_file(const std::string& path, bool* malformed = nullptr);
bool save_settings_file(const std::string& path, const OverlaySettings& settings);
OverlaySettings overlay_settings();
void set_overlay_settings(OverlaySettings settings);
void reset_overlay_settings();
bool save_overlay_settings();
void reload_overlay_settings();
// Metadata is checked at most once a second; file contents only on change.
void poll_settings_reload();
} // namespace depth_probe
