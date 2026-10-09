#pragma once
// The game's point lights in the layer (docs/lights.md): read from the lit models' vertex constants while the world is
// recorded (while the fog menu's LIGHT SOURCES row or point-light shadows are on), kept in world space at the present for the
// light gizmo and the fog's point-light shadows (fog_shade.frag).
#include "game_lights.hpp"

#include <cstdint>
#include <vector>

namespace depth_probe::discovery {
struct Device;
struct Command;
// Record time, every draw: a draw on the scene's depth target whose set 2 / binding 0 constants have the M2 light layout
// adds its point lights (view space) to this frame's sightings. A no-op while nothing wants them.
void game_lights_observe_draw(Device&, const Command&);
// Present: on / off for the next frame's draws; with the frame's camera the frame's sightings join the registry (world
// space, merged, expired). Returns the registry's lights.
std::vector<lights::Light> game_lights_present(bool enabled, bool haveCamera, const lights::Basis& camera, uint64_t frame);
size_t game_lights_count();
// The registry's lights now (world space), for the fog's point-light shadows.
std::vector<lights::Light> game_lights_snapshot();
}  // namespace depth_probe::discovery
