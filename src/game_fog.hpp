#pragma once
// WoW's own distance fog, pushed out (docs/fog.md "The game's own fog"). WoW 3.3.5a does not use D3D9's fog states: its
// vertex shaders compute a linear fog factor f = clamp(x * viewDepth + y, 0, 1) from one constant register (x, y, 1, 0) and
// its pixel shaders blend toward a flat fog colour - no light, no shadow. Found by running the shaders on a frame scan's
// constants (tools/spirv-eval.py + a register sensitivity sweep, 2026-09-28): the register is c28 in the terrain / building
// shaders (44c15e1a, a9262451, 5a11f860), c46 in seven model shaders, c24 in cc9ffdd2; w is never read; other registers can
// hold exact copies (c6, c12, c14). Durotar at dusk: (-0.00327, 2.0, 1, 0) = no fog up to 303 yd, all fog from 606 yd.
// Scaling x by 1/k moves both ends k times farther (y stays); x = 0 removes it. Pure code (tests/game_fog_test.cpp).
#include <cstdint>
#include <map>
#include <vector>

namespace depth_probe::gamefog {

// The shape of a fog register: a small negative slope, an offset of at least 1 (no fog up close), z 1, w 0.
bool looks_like_fog(const float r[4]);
// Distances (yards) where the fog starts (f < 1) and where it covers everything (f = 0).
float fog_start(const float r[4]);
float fog_end(const float r[4]);

// This frame's fog, learned from the world's draws (the most frequent fog-shaped value), and the rewrite for distance k:
// registers equal to the learned fog (within 1% of its slope) get x / k (k = 0: x = 0, no fog); a register already pushed
// is no longer within 1% of the slope, so it is never pushed twice.
class Pusher {
public:
    // A fog-shaped value seen in a world draw's constants (before any rewrite): votes for this frame's fog.
    void vote(const float r[4]);
    // End of frame: the most voted value becomes the fog the next frame's draws are matched against.
    void end_frame();
    bool known() const { return haveFog; }
    const float* fog() const { return current; }
    // true: r is this frame's fog and was rewritten for distance k (k == 1: never).
    bool push(float r[4], float k) const;
    // A world draw's register: votes for the frame's fog unless it is one this layer already pushed (the same memory seen by
    // a later draw - its vote would make the pushed value "the fog"), then pushes it. true: rewritten.
    bool observe(float r[4], float k);
private:
    bool haveFog = false;
    float current[4]{};
    std::map<std::pair<int32_t, int32_t>, std::pair<uint32_t, std::vector<float>>> votes;   // quantised (x, y) -> count, value
};

// Per vertex shader, the registers that held the fog (found by full scans of its constants, then only those are read).
// One shader can hold the fog in another register for other objects (Booty Bay, 2026-09-29: a tree's branches and its trunk
// took turns being fogged by the game - each rescan had replaced the list with the one object it looked at), so every scan
// adds to the list (at most maxRegisters) and nothing is forgotten; a register that holds something else in a draw is not
// pushed there (push() checks the fog's shape and value).
struct ShaderFogRegisters {
    std::vector<uint16_t> registers;
    uint32_t scans = 0;         // full scans done (the first draws of a shader, then again every rescanEvery draws)
    uint64_t draws = 0;
    static constexpr uint64_t rescanEvery = 500;
    static constexpr size_t maxRegisters = 16;
    bool wants_scan() const { return scans < 8 || draws % rescanEvery == 0; }
    // A full scan of one draw's constants: the fog-shaped registers join the list.
    void learn(const float (*regs)[4], size_t n);
};

}  // namespace depth_probe::gamefog
