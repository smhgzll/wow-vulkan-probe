#pragma once
// The game's point lights (docs/lights.md). WoW 3.3.5a lights its models (M2) per vertex from DXVK's D3D9 vertex
// constants (set 2, binding 0); a "lights" frame scan next to a campfire (frame-scan-105226-2) showed the layout:
//   c28      minus the models' light direction (view space, unit length, w .996)
//   c33..c36 up to four point light colours (rgb, w 1)
//   c37..c40 their positions, same order (view space: x = camera right, y = up, z = forward; w 1)
// An unused slot is all zero. Every lit draw of a frame carries the same lights (in its own order); three torches came out
// at the same world height (55.39) through the camera's basis. Pure code, no Vulkan (tests/game_lights_test.cpp).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace depth_probe::lights {

constexpr uint32_t firstRegister = 28, registerCount = 13;   // c28..c40, read as one block
constexpr int slots = 4;

struct ViewLight { float pos[3]{}; float color[3]{}; };

// regs = c28..c40 of one draw. -1: not this layout (c28 not a unit direction, a slot with a colour but no position w of 1,
// a colour out of a light's range); else the lights written to out (0..4). A position within half a yard of the camera
// is not a light (the campfire's own draw showed its light at (0.03, 0, 0.04)).
int parse_m2_lights(const float (*regs)[4], ViewLight out[slots]);

struct Basis { float position[3]{}, right[3]{}, up[3]{}, forward[3]{}; };
void view_to_world(const Basis&, const float view[3], float world[3]);

struct Light {
    float pos[3]{}, color[3]{};
    uint64_t firstSeen = 0, lastSeen = 0;
    uint32_t sightings = 0;
};

// Distinct lights over time: a sighting within `merge` yards of a known light refreshes the nearest such light (its position
// a running mean, its colour the latest), else starts a new one; lights not seen for keepFrames frames are dropped (torches
// stay: a light is only seen while a lit model is near it). At most maxLights (the least recently seen goes first).
class Registry {
public:
    static constexpr float mergeYards = .75f;
    static constexpr uint64_t keepFrames = 1800;
    static constexpr size_t maxLights = 64;
    void add(const float world[3], const float color[3], uint64_t frame, float merge = mergeYards);
    void expire(uint64_t frame);
    const std::vector<Light>& lights() const { return list; }
    void clear() { list.clear(); }
private:
    std::vector<Light> list;
};

// How far apart two sightings of one light may be at this distance from the camera: the view -> world placement uses the
// camera of the present, a frame off the draw's while turning (in game: 1.7..2.5 yd at 130 yd made 27..64 copies of 2 lights).
inline float merge_radius(float distance) { return distance * .02f > Registry::mergeYards ? distance * .02f : Registry::mergeYards; }

// Deduplicates one frame's sightings (the same light in many draws): true when it was new.
bool add_unique(std::vector<ViewLight>& frame, const ViewLight& l);

}  // namespace depth_probe::lights
