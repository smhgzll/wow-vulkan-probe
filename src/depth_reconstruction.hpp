#pragma once
#include "camera_math.hpp"
#include <string>

// Per-pixel world position from the game's depth buffer. Reference
// implementation of shaders/depth_world.glsl (keep the two identical), checked
// against real captures by tools/depth-reconstruction-check.py:
//   ndc.xy = (pixelCenter - viewport.xy) / viewport.wh * 2 - 1   (WoW: viewport height negative)
//   ndc.z  = (raw - minDepth) / (maxDepth - minDepth)          (WoW world draws: 0 .. 0.94)
//   view.z = P23 / (ndc.z - P22), view.xy = ndc.xy * view.z / (P00, P11)
//   world  = camera + right * view.x + up * view.y + forward * view.z
// Camera-relative on purpose: WoW coordinates reach ~17000, where float
// inverse(P*V) loses the centimetres a world-space grid needs.
namespace depth_probe {
struct WorldViewport {
    float x = 0, y = 0, width = 0, height = 0, minDepth = 0, maxDepth = 1;
};

struct ReconstructionSetup {
    bool valid = false;
    std::string reason;
    float p00 = 0, p11 = 0, p22 = 0, p23 = 0;
    Vec3 right, up, forward;
    double camera[3]{};
    WorldViewport viewport;
};

// Requires the verified WoW projection structure (symmetric, +Z forward, 0..1) and a world viewport
// whose depth range is not empty.
ReconstructionSetup make_reconstruction(const DerivedCamera&, const WorldViewport&);

// False when `raw` is not world geometry: at or beyond maxDepth (sky draws use 0.998..1, clears 1.0).
bool reconstruct_view(const ReconstructionSetup&, float pixelCenterX, float pixelCenterY, float raw, Vec3& view);
bool reconstruct_world(const ReconstructionSetup&, float pixelCenterX, float pixelCenterY, float raw, double world[3]);

// Forward direction, for tests and tools: world point -> window pixel + raw depth, exactly as the GPU would store it.
bool project_world(const DerivedCamera&, const WorldViewport&, const double world[3], float& pixelX, float& pixelY, float& raw);
}
