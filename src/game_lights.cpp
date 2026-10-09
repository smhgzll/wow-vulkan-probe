#include "game_lights.hpp"

#include <algorithm>
#include <cmath>

namespace depth_probe::lights {

int parse_m2_lights(const float (*regs)[4], ViewLight out[slots]) {
    const float* dir = regs[0];
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(std::fabs(len - 1.f) < .01f)) return -1;
    int n = 0;
    for (int k = 0; k < slots; ++k) {
        const float* c = regs[33 - firstRegister + k];
        const float* p = regs[37 - firstRegister + k];
        const bool colour = c[0] != 0 || c[1] != 0 || c[2] != 0;
        const bool position = p[0] != 0 || p[1] != 0 || p[2] != 0 || p[3] != 0;
        if (!colour && !position) continue;                                  // an unused slot
        if (!colour || p[3] != 1.f || c[3] != 1.f) return -1;
        for (int i = 0; i < 3; ++i) if (!(c[i] >= 0.f && c[i] <= 8.f) || !std::isfinite(p[i]) || std::fabs(p[i]) > 2000.f) return -1;
        if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] < .25f) continue;        // at the camera: not a light
        std::copy_n(p, 3, out[n].pos);
        std::copy_n(c, 3, out[n].color);
        ++n;
    }
    return n;
}

void view_to_world(const Basis& b, const float v[3], float w[3]) {
    for (int i = 0; i < 3; ++i) w[i] = b.position[i] + v[0] * b.right[i] + v[1] * b.up[i] + v[2] * b.forward[i];
}

void Registry::add(const float world[3], const float color[3], uint64_t frame, float merge) {
    Light* nearest = nullptr;
    float best = merge * merge;
    for (auto& l : list) {
        const float dx = l.pos[0] - world[0], dy = l.pos[1] - world[1], dz = l.pos[2] - world[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= best) { best = d2; nearest = &l; }
    }
    if (nearest) {
        auto& l = *nearest;
        const float keep = float(std::min<uint32_t>(l.sightings, 30)) / float(std::min<uint32_t>(l.sightings, 30) + 1);
        for (int i = 0; i < 3; ++i) { l.pos[i] = keep * l.pos[i] + (1 - keep) * world[i]; l.color[i] = color[i]; }
        l.lastSeen = frame;
        ++l.sightings;
        return;
    }
    if (list.size() >= maxLights) list.erase(std::min_element(list.begin(), list.end(), [](const Light& a, const Light& b) { return a.lastSeen < b.lastSeen; }));
    Light l;
    std::copy_n(world, 3, l.pos);
    std::copy_n(color, 3, l.color);
    l.firstSeen = l.lastSeen = frame;
    l.sightings = 1;
    list.push_back(l);
}

void Registry::expire(uint64_t frame) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Light& l) { return frame > l.lastSeen + keepFrames; }), list.end());
}

bool add_unique(std::vector<ViewLight>& frame, const ViewLight& l) {
    for (const auto& f : frame) {
        const float dx = f.pos[0] - l.pos[0], dy = f.pos[1] - l.pos[1], dz = f.pos[2] - l.pos[2];
        if (dx * dx + dy * dy + dz * dz < 1e-4f) return false;
    }
    frame.push_back(l);
    return true;
}

}  // namespace depth_probe::lights
