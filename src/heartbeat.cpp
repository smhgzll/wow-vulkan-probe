#include "heartbeat.hpp"
#include "layer_state.hpp"

#include <cerrno>
#include <chrono>
#include <cmath>

namespace depth_probe {
namespace {
float read_setting(const char* name, float fallback, float low, float high) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value, &end);
    if (end == value || *end || errno == ERANGE || !std::isfinite(parsed)) {
        log_line(std::string("[OVERLAY-HEARTBEAT] invalid ") + name + "; using default");
        return fallback;
    }
    if (parsed < low || parsed > high)
        log_line(std::string("[OVERLAY-HEARTBEAT] clamping ") + name);
    return std::clamp(parsed, low, high);
}
} // namespace

HeartbeatConfig read_heartbeat_config() {
    HeartbeatConfig config;
    if (const char* enabled = std::getenv("DEPTH_PROBE_HEARTBEAT"); enabled && *enabled)
        config.enabled = std::strcmp(enabled, "0") != 0;
    config.bpm = read_setting("DEPTH_PROBE_HEARTBEAT_BPM", 72.0f, 20.0f, 240.0f);
    config.strength = read_setting("DEPTH_PROBE_HEARTBEAT_STRENGTH", 1.0f, 0.0f, 1.0f);
    return config;
}

HeartbeatConstants heartbeat_at(const HeartbeatConfig& config, double seconds,
                                uint32_t width, uint32_t height) {
    HeartbeatConstants constants;
    // Wrap in double precision before uploading a float: long sessions keep
    // smooth timing and the wrap is visually continuous in the periodic shader.
    constants.phase = static_cast<float>(std::fmod(std::max(0.0, seconds) * config.bpm / 60.0, 1.0));
    constants.strength = config.enabled ? config.strength : 0.0f;
    constants.aspectRatio = height ? static_cast<float>(width) / height : 1.0f;
    return constants;
}

HeartbeatConstants current_heartbeat(uint32_t width, uint32_t height) {
    static const HeartbeatConfig config = [] {
        const auto value = read_heartbeat_config();
        std::ostringstream log;
        log << "[OVERLAY-HEARTBEAT] enabled=" << value.enabled
            << " bpm=" << value.bpm << " strength=" << value.strength
            << " origin=screen-center";
        log_line(log.str());
        return value;
    }();
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return heartbeat_at(config, seconds, width, height);
}

} // namespace depth_probe
