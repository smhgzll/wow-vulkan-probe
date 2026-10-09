#include "game_lights_capture.hpp"

#include "discovery_internal.hpp"
#include "layer_state.hpp"
#include "scene_debugger.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace depth_probe::discovery {
namespace {
struct Shared {
    std::mutex lock;
    std::atomic<bool> enabled{false};
    std::vector<lights::ViewLight> frame;   // this frame's sightings, view space, deduplicated
    lights::Registry registry;
    uint64_t read = 0, lit = 0, otherLayout = 0, frames = 0, nextLog = 0, turningFrames = 0;
    bool haveLast = false;
    float lastForward[3]{}, lastRight[3]{};
};
Shared& shared() { static Shared s; return s; }
}  // namespace

void game_lights_observe_draw(Device& s, const Command& c) {
    auto& g = shared();
    if (!g.enabled.load(std::memory_order_relaxed)) return;
    if (!c.pipeline || !c.pipeline->vs || !c.target.depth || !c.target.depth->image) return;
    const uint64_t scene = s.sceneDebugger ? s.sceneDebugger->sceneDepthImage : 0;
    if (!scene || c.target.depth->image->handle != scene) return;
    float regs[lights::registerCount][4];
    if (!read_bound_uniform_bytes(s, c, 2, 0, lights::firstRegister * 16, sizeof regs, regs)) return;
    lights::ViewLight found[lights::slots];
    const int n = lights::parse_m2_lights(regs, found);
    std::lock_guard lock(g.lock);
    ++g.read;
    if (n < 0) { ++g.otherLayout; return; }
    if (n > 0) ++g.lit;
    for (int i = 0; i < n && g.frame.size() < 64; ++i) lights::add_unique(g.frame, found[i]);
}

std::vector<lights::Light> game_lights_present(bool enabled, bool haveCamera, const lights::Basis& camera, uint64_t frame) {
    auto& g = shared();
    std::lock_guard lock(g.lock);
    if (enabled != g.enabled.load()) log_line(std::string("[GAME-LIGHTS] ") + (enabled ? "on: reading the lit models' point lights" : "off"));
    g.enabled = enabled;
    if (!enabled) { g.frame.clear(); g.registry.clear(); g.read = g.lit = g.otherLayout = g.frames = 0; return {}; }
    // While the camera turns, the present's camera is not the draws' (a frame apart): their sightings would land yards off.
    bool turning = false;
    if (haveCamera) {
        if (g.haveLast) {
            const float cf = camera.forward[0] * g.lastForward[0] + camera.forward[1] * g.lastForward[1] + camera.forward[2] * g.lastForward[2];
            const float cr = camera.right[0] * g.lastRight[0] + camera.right[1] * g.lastRight[1] + camera.right[2] * g.lastRight[2];
            turning = std::min(cf, cr) < std::cos(.3f * 3.14159265f / 180.f);
        }
        std::copy_n(camera.forward, 3, g.lastForward); std::copy_n(camera.right, 3, g.lastRight); g.haveLast = true;
    }
    if (turning) ++g.turningFrames;
    else if (haveCamera)
        for (const auto& v : g.frame) {
            float w[3];
            lights::view_to_world(camera, v.pos, w);
            const float d = std::sqrt(v.pos[0] * v.pos[0] + v.pos[1] * v.pos[1] + v.pos[2] * v.pos[2]);
            g.registry.add(w, v.color, frame, lights::merge_radius(d));
        }
    g.frame.clear();
    g.registry.expire(frame);
    ++g.frames;
    if (frame >= g.nextLog) {
        g.nextLog = frame + 900;
        // [GAME-LIGHTS] every 900 presents: how many, what was read, the nearest few (rank, don't dump).
        auto list = g.registry.lights();
        auto dist = [&](const lights::Light& l) { float d = 0; for (int i = 0; i < 3; ++i) d += (l.pos[i] - camera.position[i]) * (l.pos[i] - camera.position[i]); return std::sqrt(d); };
        std::sort(list.begin(), list.end(), [&](const auto& a, const auto& b) { return dist(a) < dist(b); });
        char b[256];
        std::snprintf(b, sizeof b, "[GAME-LIGHTS] %zu point lights | per frame: %.1f draws read, %.1f lit, %.1f other layouts; %llu of %llu frames turning (not placed)%s", list.size(),
                      double(g.read) / std::max<uint64_t>(g.frames, 1), double(g.lit) / std::max<uint64_t>(g.frames, 1), double(g.otherLayout) / std::max<uint64_t>(g.frames, 1),
                      (unsigned long long)g.turningFrames, (unsigned long long)g.frames, haveCamera ? "" : " | no camera: sightings not placed");
        std::string line = b;
        for (size_t i = 0; i < list.size() && i < 4; ++i) {
            std::snprintf(b, sizeof b, " | %.1f yd at %.1f,%.1f,%.1f colour %.2f %.2f %.2f seen %u", double(dist(list[i])), double(list[i].pos[0]), double(list[i].pos[1]),
                          double(list[i].pos[2]), double(list[i].color[0]), double(list[i].color[1]), double(list[i].color[2]), list[i].sightings);
            line += b;
        }
        log_line(line);
        g.read = g.lit = g.otherLayout = g.frames = g.turningFrames = 0;
    }
    return g.registry.lights();
}

std::vector<lights::Light> game_lights_snapshot() { auto& g = shared(); std::lock_guard lock(g.lock); return g.registry.lights(); }

size_t game_lights_count() { auto& g = shared(); std::lock_guard lock(g.lock); return g.registry.lights().size(); }
}  // namespace depth_probe::discovery
