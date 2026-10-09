#include "game_fog_capture.hpp"

#include "discovery_internal.hpp"
#include "game_fog.hpp"
#include "game_pcf.hpp"
#include "layer_state.hpp"
#include "scene_debugger.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>

namespace depth_probe::discovery {
namespace {
struct Shared {
    std::mutex lock;
    std::atomic<float> distance{1.f}, softness{1.f};
    struct Taps { size_t first = 0, count = 0; uint32_t scans = 0; uint64_t draws = 0; };
    std::unordered_map<uint64_t, Taps> taps;   // by pixel shader hash
    uint64_t spreadDraws = 0;
    gamefog::Pusher pusher;
    std::unordered_map<uint64_t, gamefog::ShaderFogRegisters> shaders;   // by vertex shader hash
    std::map<uint16_t, uint64_t> pushedByRegister;
    uint64_t draws = 0, pushed = 0, frames = 0, nextLog = 0;
};
Shared& shared() { static Shared s; return s; }
}  // namespace

namespace {
// The pixel constants' shadow filter taps (set 2 / binding 3) spread by s: found per pixel shader (first draws, then every
// 5000), then only that run is checked and spread (never twice: spread offsets no longer look like the game's).
void spread_taps(Device& s, const Command& c, Shared& g, float soft) {
    if (!c.pipeline->fs) return;
    uint64_t bytes = 0;
    uint8_t* p = bound_uniform_span(s, c, 2, 3, bytes);
    if (!p || bytes < 16 * gamepcf::minRun) return;
    auto* regs = reinterpret_cast<float(*)[4]>(p);
    const size_t n = std::min<uint64_t>(bytes / 16, 256);
    std::lock_guard lock(g.lock);
    auto& t = g.taps[c.pipeline->fs->hash];
    if (t.scans < 3 || t.draws % 5000 == 0) {
        size_t first = 0, count = 0;
        if (gamepcf::find_taps(regs, n, first, count)) { t.first = first; t.count = count; }
        ++t.scans;
    }
    ++t.draws;
    if (t.count && t.first + t.count <= n && gamepcf::spread(regs, t.first, t.count, soft)) ++g.spreadDraws;
}
}  // namespace

void game_fog_observe_draw(Device& s, const Command& c) {
    auto& g = shared();
    const float k = g.distance.load(std::memory_order_relaxed), soft = g.softness.load(std::memory_order_relaxed);
    if (k == 1.f && soft == 1.f) return;
    if (!c.pipeline || !c.pipeline->vs || !c.target.depth || !c.target.depth->image) return;
    const uint64_t scene = s.sceneDebugger ? s.sceneDebugger->sceneDepthImage : 0;
    if (!scene || c.target.depth->image->handle != scene) return;
    if (soft != 1.f) spread_taps(s, c, g, soft);
    if (k == 1.f) return;
    uint64_t bytes = 0;
    uint8_t* p = bound_uniform_span(s, c, 2, 0, bytes);   // DXVK's D3D9 vertex constants: c[i] at 16 i
    if (!p || bytes < 16) return;
    auto* regs = reinterpret_cast<float(*)[4]>(p);
    const size_t n = std::min<uint64_t>(bytes / 16, 256);
    std::lock_guard lock(g.lock);
    auto& sh = g.shaders[c.pipeline->vs->hash];
    if (sh.wants_scan()) sh.learn(regs, n);
    ++sh.draws;
    ++g.draws;
    for (auto i : sh.registers)
        if (i < n && g.pusher.observe(regs[i], k)) { ++g.pushed; ++g.pushedByRegister[i]; }
}

void game_fog_present(float distance, uint64_t frame, float softness) {
    auto& g = shared();
    std::lock_guard lock(g.lock);
    if (softness != g.softness.load()) {
        char b[96];
        std::snprintf(b, sizeof b, softness == 1.f ? "[GAME-SHADOWS] filter as the game" : "[GAME-SHADOWS] the game's shadow filter spread %.1fx (softer edges)", double(softness));
        log_line(b);
    }
    g.softness = softness;
    if (softness != 1.f && frame >= g.nextLog && distance == 1.f) {   // with the fog pushed too its log below carries the count
        g.nextLog = frame + 900;
        log_line("[GAME-SHADOWS] shadow filter spread in " + std::to_string(g.spreadDraws / 900) + " draws per frame (" + std::to_string(g.taps.size()) + " pixel shaders seen)");
        g.spreadDraws = 0;
    }
    if (distance != g.distance.load()) {
        char b[96];
        if (distance == 1.f) std::snprintf(b, sizeof b, "[GAME-FOG] as the game");
        else if (distance == 0.f) std::snprintf(b, sizeof b, "[GAME-FOG] off: the game's own fog removed");
        else std::snprintf(b, sizeof b, "[GAME-FOG] the game's own fog pushed %.1fx farther", double(distance));
        log_line(b);
    }
    g.distance = distance;
    if (distance == 1.f) return;
    g.pusher.end_frame();
    ++g.frames;
    if (frame >= g.nextLog && g.pusher.known()) {
        g.nextLog = frame + 900;
        const float* f = g.pusher.fog();
        char b[256];
        std::snprintf(b, sizeof b, "[GAME-FOG] the game's fog (%.5f, %.3f): none up to %.0f yd, all from %.0f yd -> now %s | per frame %.1f draws read, %.1f registers pushed",
                      double(f[0]), double(f[1]), double(gamefog::fog_start(f)), double(gamefog::fog_end(f)),
                      distance == 0.f ? "off" : (std::to_string(int(gamefog::fog_start(f) * distance)) + "-" + std::to_string(int(gamefog::fog_end(f) * distance)) + " yd").c_str(),
                      double(g.draws) / std::max<uint64_t>(g.frames, 1), double(g.pushed) / std::max<uint64_t>(g.frames, 1));
        std::string line = b;
        for (const auto& [reg, count] : g.pushedByRegister) line += " c" + std::to_string(reg) + " x" + std::to_string(count);
        if (softness != 1.f) line += " | shadow filter spread in " + std::to_string(g.spreadDraws / std::max<uint64_t>(g.frames, 1)) + " draws per frame";
        g.spreadDraws = 0;
        log_line(line);
        g.draws = g.pushed = g.frames = 0;
        g.pushedByRegister.clear();
    }
}

std::string game_fog_state() {
    auto& g = shared();
    std::lock_guard lock(g.lock);
    if (!g.pusher.known()) return {};
    const float* f = g.pusher.fog();
    return "FOG " + std::to_string(int(gamefog::fog_start(f))) + "-" + std::to_string(int(gamefog::fog_end(f))) + " YD";
}
}  // namespace depth_probe::discovery
