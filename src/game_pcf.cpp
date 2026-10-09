#include "game_pcf.hpp"

#include <cmath>

namespace depth_probe::gamepcf {

bool looks_like_tap(const float r[4]) {
    const float limit = 1.05f * texel;
    return std::isfinite(r[0]) && std::isfinite(r[1]) && std::fabs(r[0]) <= limit && std::fabs(r[1]) <= limit &&
           (r[0] != 0.f || r[1] != 0.f) && r[2] == 0.f && r[3] == 0.f;
}

bool find_taps(const float (*regs)[4], size_t n, size_t& first, size_t& count) {
    for (size_t i = 0; i < n;) {
        if (!looks_like_tap(regs[i])) { ++i; continue; }
        size_t j = i;
        while (j < n && j - i < maxRun && looks_like_tap(regs[j])) ++j;
        if (j - i >= minRun) { first = i; count = j - i; return true; }
        i = j;
    }
    return false;
}

bool spread(float (*regs)[4], size_t first, size_t count, float s) {
    if (!(s > 1.f)) return false;
    for (size_t i = first; i < first + count; ++i) if (!looks_like_tap(regs[i])) return false;   // spread already (or changed)
    for (size_t i = first; i < first + count; ++i) { regs[i][0] *= s; regs[i][1] *= s; }
    return true;
}

}  // namespace depth_probe::gamepcf
