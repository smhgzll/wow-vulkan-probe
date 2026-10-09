#include "game_fog.hpp"

#include <algorithm>
#include <cmath>

namespace depth_probe::gamefog {

bool looks_like_fog(const float r[4]) {
    return std::isfinite(r[0]) && std::isfinite(r[1]) && r[0] < 0.f && r[0] > -.1f && r[1] >= 1.f && r[1] <= 100.f && r[2] == 1.f && r[3] == 0.f;
}
float fog_start(const float r[4]) { return r[0] < 0 ? (r[1] - 1.f) / -r[0] : INFINITY; }
float fog_end(const float r[4]) { return r[0] < 0 ? r[1] / -r[0] : INFINITY; }

void Pusher::vote(const float r[4]) {
    const auto key = std::make_pair(int32_t(std::lround(r[0] * 1e6)), int32_t(std::lround(r[1] * 1e4)));
    auto& v = votes[key];
    if (!v.first) v.second.assign(r, r + 4);
    ++v.first;
}

void Pusher::end_frame() {
    if (votes.empty()) return;   // no world drawn this frame: keep the last fog
    auto best = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second.first < b.second.first; });
    std::copy_n(best->second.second.data(), 4, current);
    haveFog = true;
    votes.clear();
}

bool Pusher::push(float r[4], float k) const {
    if (!haveFog || k == 1.f || !looks_like_fog(r)) return false;
    if (std::fabs(r[0] - current[0]) > .01f * std::fabs(current[0]) || std::fabs(r[1] - current[1]) > .01f * std::fabs(current[1])) return false;
    r[0] = k > 0.f ? r[0] / k : 0.f;
    return true;
}

bool Pusher::observe(float r[4], float k) {
    if (!looks_like_fog(r)) return false;
    const bool pushedAlready = haveFog && k > 0.f && k != 1.f && std::fabs(r[0] - current[0] / k) <= .001f * std::fabs(current[0] / k) &&
                               std::fabs(r[1] - current[1]) <= .01f * std::fabs(current[1]);
    if (pushedAlready) return false;
    vote(r);
    return push(r, k);
}

void ShaderFogRegisters::learn(const float (*regs)[4], size_t n) {
    ++scans;
    for (size_t i = 0; i < n && registers.size() < maxRegisters; ++i)
        if (looks_like_fog(regs[i]) && std::find(registers.begin(), registers.end(), uint16_t(i)) == registers.end()) registers.push_back(uint16_t(i));
}

}  // namespace depth_probe::gamefog
