#pragma once
// WoW's own constants, rewritten in the world draws while they are recorded (docs/fog.md): its distance fog pushed out
// (fog.ini game_fog_distance, vertex constants) and its sun shadow filter spread (shadow_softness, pixel constants). A no-op
// while both are 1 (as the game).
#include <cstdint>
#include <string>

namespace depth_probe::discovery {
struct Device;
struct Command;
void game_fog_observe_draw(Device&, const Command&);
// Present: the distance factor for the next frame's draws (1 = untouched, 0 = no game fog) and the end of this frame; the
// spread of the game's shadow filter taps (1 = untouched, fog.ini shadow_softness).
void game_fog_present(float distance, uint64_t frame, float softness = 1.f);
std::string game_fog_state();   // "FOG 303-606 YD" once seen, else empty
}  // namespace depth_probe::discovery
