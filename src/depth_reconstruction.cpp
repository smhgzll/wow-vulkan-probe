#include "depth_reconstruction.hpp"
#include <cmath>

namespace depth_probe {
ReconstructionSetup make_reconstruction(const DerivedCamera& c, const WorldViewport& vp) {
    ReconstructionSetup s;
    const auto& p = c.projection;
    if (!c.valid) { s.reason = "camera invalid"; return s; }
    if (!(p.c0.x > 0 && p.c1.y > 0 && p.c2.z > 1 && p.c3.z < 0 && std::fabs(p.c2.w - 1) < 1e-4f &&
          std::fabs(p.c2.x) < 1e-6f && std::fabs(p.c2.y) < 1e-6f)) { s.reason = "projection not the WoW +Z 0..1 symmetric form"; return s; }
    if (!(vp.width != 0 && vp.height != 0 && vp.maxDepth > vp.minDepth)) { s.reason = "world viewport empty"; return s; }
    s.p00 = p.c0.x; s.p11 = p.c1.y; s.p22 = p.c2.z; s.p23 = p.c3.z;
    s.right = c.right; s.up = c.up; s.forward = c.forward;
    s.camera[0] = c.position.x; s.camera[1] = c.position.y; s.camera[2] = c.position.z;
    s.viewport = vp;
    s.valid = true;
    return s;
}

bool reconstruct_view(const ReconstructionSetup& s, float px, float py, float raw, Vec3& view) {
    if (!s.valid || !(raw >= s.viewport.minDepth) || raw >= s.viewport.maxDepth) return false;
    const float ndcX = (px - s.viewport.x) / s.viewport.width * 2.f - 1.f;
    const float ndcY = (py - s.viewport.y) / s.viewport.height * 2.f - 1.f;
    const float ndcZ = (raw - s.viewport.minDepth) / (s.viewport.maxDepth - s.viewport.minDepth);
    const float z = s.p23 / (ndcZ - s.p22);
    if (!(z > 0) || !std::isfinite(z)) return false;
    view = {ndcX * z / s.p00, ndcY * z / s.p11, z};
    return true;
}

bool reconstruct_world(const ReconstructionSetup& s, float px, float py, float raw, double world[3]) {
    Vec3 v;
    if (!reconstruct_view(s, px, py, raw, v)) return false;
    world[0] = s.camera[0] + double(s.right.x) * v.x + double(s.up.x) * v.y + double(s.forward.x) * v.z;
    world[1] = s.camera[1] + double(s.right.y) * v.x + double(s.up.y) * v.y + double(s.forward.y) * v.z;
    world[2] = s.camera[2] + double(s.right.z) * v.x + double(s.up.z) * v.y + double(s.forward.z) * v.z;
    return true;
}

bool project_world(const DerivedCamera& c, const WorldViewport& vp, const double w[3], float& px, float& py, float& raw) {
    const double rel[3]{w[0] - c.position.x, w[1] - c.position.y, w[2] - c.position.z};
    auto along = [&](const Vec3& a) { return float(rel[0] * a.x + rel[1] * a.y + rel[2] * a.z); };
    const Vec4 view{along(c.right), along(c.up), along(c.forward), 1.f};
    const auto clip = mat4_mul_vec4(c.projection, view);
    if (!(clip.w > 0)) return false;
    const float nx = clip.x / clip.w, ny = clip.y / clip.w, nz = clip.z / clip.w;
    px = vp.x + (nx * .5f + .5f) * vp.width;
    py = vp.y + (ny * .5f + .5f) * vp.height;
    raw = vp.minDepth + nz * (vp.maxDepth - vp.minDepth);
    return true;
}
}
