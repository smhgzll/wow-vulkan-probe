#pragma once
// Softer edges for WoW's own sun shadows (fog.ini shadow_softness, docs/fog.md). Its pixel shaders filter each cascade
// sample with 9 depth-compare taps whose offsets are pixel constants (DXVK set 2 / binding 3): terrain FS 9e7279ff c19..c27 in
// Mulgore, (u, v, 0, 0) each, within one texel of the 2048 maps (multiples of 0.2 / 2048) - found by running the shader
// (tools/spirv-eval.py) and moving each constant. Spreading the offsets by s widens the filter s times: softer shadow edges
// without touching the game's shaders. Pure code (tests/game_pcf_test.cpp).
#include <cstddef>

namespace depth_probe::gamepcf {

constexpr float texel = 1.f / 2048.f;
constexpr size_t minRun = 7, maxRun = 12;

// A tap offset as the game sets it: (u, v, 0, 0), both within one texel (plus a margin), not both zero.
bool looks_like_tap(const float r[4]);
// The first run of at least minRun consecutive tap-shaped registers in regs[0..n): true with its first register and length.
bool find_taps(const float (*regs)[4], size_t n, size_t& first, size_t& count);
// Spreads the run by s (> 1). false (untouched) when it is already spread (an offset beyond one texel) or s <= 1.
bool spread(float (*regs)[4], size_t first, size_t count, float s);

}  // namespace depth_probe::gamepcf
