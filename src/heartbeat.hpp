#pragma once
#include <cstdint>

namespace depth_probe {

struct HeartbeatConfig {
    float bpm = 72.0f;
    float strength = 1.0f;
    bool enabled = true;
};

// Matches the heartbeat vec4 in OverlayUniform on both x86 ABIs.
struct HeartbeatConstants {
    float phase = 0.0f;
    float strength = 0.0f;
    float aspectRatio = 1.0f;
    float padding = 0.0f;
};
static_assert(sizeof(HeartbeatConstants) == 16);

HeartbeatConfig read_heartbeat_config();
HeartbeatConstants heartbeat_at(const HeartbeatConfig& config, double seconds,
                                uint32_t width, uint32_t height);
HeartbeatConstants current_heartbeat(uint32_t width, uint32_t height);

} // namespace depth_probe
