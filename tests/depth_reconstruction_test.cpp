#include "depth_reconstruction.hpp"
#include "memory_camera.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <vector>
using namespace depth_probe;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)

namespace {
// Real WoW water UBO, Booty Bay (exports/water-capture-101750-1-mode2-view0): P at +256, V at +336.
const float realP[16]{1.1039682626724243f,0,0,0, 0,1.9626103639602661f,0,0, 0,0,1.0002527236938477f,1.0f, 0,0,-0.20005054771900177f,0};
const float realV[16]{0.6243106126785278f,0.42488765716552734f,0.6555202007293701f,0, -0.7811762094497681f,0.33956727385520935f,0.5238872170448303f,0,
                      -4.750454252189229e-09f,0.8391451239585876f,-0.5439075827598572f,0, 9326.787109375f,5958.53125f,9244.5615234375f,1.0f};
// The game's world viewport in that capture (and in 4694 captured world draws).
const WorldViewport wowViewport{.5f, 1080.5f, 1920.f, -1080.f, 0.f, .94f};

// Same capture: pixel, mask_depth (our replay of the water draw, same viewport) and the view-space
// position our mask pass rasterized from the water mesh itself - an independent reference.
struct RealPixel { int x, y; float raw; float view[3]; };
const RealPixel realWater[]{
    {100, 1000, 0.9341633319854736f, {-25.121692657470703f, -13.43718433380127f, 30.958385467529297f}},
    {960, 700, 0.9357522130012512f, {0.0f, -6.329504013061523f, 41.92543029785156f}},
    {400, 400, 0.937341034412384f, {-34.304195404052734f, 8.576047897338867f, 64.92127990722656f}},
    {1500, 900, 0.934692919254303f, {17.28070831298828f, -11.520469665527344f, 33.91529083251953f}},
    {700, 150, 0.938664972782135f, {-29.33562469482422f, 44.00343704223633f, 119.57760620117188f}},
};

float d24(float raw) { return std::round(raw * 16777215.f) / 16777215.f; }
double dist3(const double a[3], const double b[3]) { return std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2])); }
}

int main() {
    const auto gpu = derive_known_wow_camera(realP, realV);
    CHECK(gpu.valid);
    const auto setup = make_reconstruction(gpu, wowViewport);
    CHECK(setup.valid);

    // 1. Real pixels: depth -> view matches the independently rasterized water surface.
    for (const auto& p : realWater) {
        Vec3 v;
        CHECK(reconstruct_view(setup, p.x + .5f, p.y + .5f, p.raw, v));
        const float err = std::sqrt((v.x - p.view[0]) * (v.x - p.view[0]) + (v.y - p.view[1]) * (v.y - p.view[1]) + (v.z - p.view[2]) * (v.z - p.view[2]));
        CHECK(err < 2e-4f * p.view[2] + 1e-3f);
        // Its world height is the sea level the water mesh sits at (z = 0).
        double w[3];
        CHECK(reconstruct_world(setup, p.x + .5f, p.y + .5f, p.raw, w));
        CHECK(std::fabs(w[2]) < .01);
    }
    // Without the half-pixel viewport offset or the 0..0.94 range the same pixels are measurably wrong.
    {
        auto noOffset = wowViewport; noOffset.x = 0; noOffset.y = 1080;
        auto fullRange = wowViewport; fullRange.maxDepth = 1;
        const auto a = make_reconstruction(gpu, noOffset), b = make_reconstruction(gpu, fullRange);
        Vec3 va, vb;
        const auto& p = realWater[1];
        CHECK(reconstruct_view(a, p.x + .5f, p.y + .5f, p.raw, va) && std::fabs(va.x - p.view[0]) > .01f);
        CHECK(reconstruct_view(b, p.x + .5f, p.y + .5f, p.raw, vb) && std::fabs(vb.z - p.view[2]) > 10.f);
    }

    // 2. Round trip through the GPU's own forward mapping (full P matrix + viewport transform) and D24 storage.
    // Points spread over the view frustum, 1 to 600 yards out.
    std::vector<std::array<double, 3>> points;
    for (double t : {1.0, 7.5, 40.0, 150.0, 600.0})
        for (double a : {-.7, 0.0, .55})
            for (double b : {-.4, .3}) {
                const auto& c = gpu;
                points.push_back({c.position.x + t * (c.forward.x + a * c.right.x + b * c.up.x),
                                  c.position.y + t * (c.forward.y + a * c.right.y + b * c.up.y),
                                  c.position.z + t * (c.forward.z + a * c.right.z + b * c.up.z)});
            }
    const double eye[3]{gpu.position.x, gpu.position.y, gpu.position.z};
    int onScreen = 0;
    for (auto& w : points) {
        float px, py, raw;
        CHECK(project_world(gpu, wowViewport, w.data(), px, py, raw));
        double back[3];
        if (px < 0 || px > 1920 || py < 0 || py > 1080) continue;
        CHECK(reconstruct_world(setup, px, py, d24(raw), back));
        ++onScreen;
        const double d = dist3(w.data(), eye);
        // One D24 step at distance d spans about d*d*P22/|P23|/(0.94 * 2^24) yards.
        CHECK(dist3(w.data(), back) < d * d * 1.00025 / .20005 / (.94 * 16777215.0) * 2 + 2e-5 * d + 1e-3);
    }

    CHECK(onScreen == int(points.size()));

    // 3. Non-world depth: the sky's depth ranges and the clear value are not reconstructed.
    double w[3];
    CHECK(!reconstruct_world(setup, 960.5f, 540.5f, .94f, w));
    CHECK(!reconstruct_world(setup, 960.5f, 540.5f, .9985f, w));
    CHECK(!reconstruct_world(setup, 960.5f, 540.5f, 1.f, w));
    CHECK(reconstruct_world(setup, 960.5f, 540.5f, .9399f, w));

    // 4. The canonical memory camera (pose from memory, projection from its raw fov * 0.6) reconstructs the same world.
    {
        RawMemoryCamera m;
        m.readOk = true;
        m.position = gpu.position;
        m.rows[0] = gpu.forward; m.rows[1] = {-gpu.right.x, -gpu.right.y, -gpu.right.z}; m.rows[2] = gpu.up;
        m.nearClip = .2f; m.farClip = 791.666687f; m.fov = 1.5707964f; m.aspect = 1.7777778f;
        const auto proj = synthesize_projection(m.fov * .6f, FovAxis::Vertical, m.aspect, m.nearClip, m.farClip);
        const auto memCam = memory_camera_to_derived(m, SignedPermutation{{1, 3, -2}}, proj);
        CHECK(memCam.valid);
        const auto memSetup = make_reconstruction(memCam, wowViewport);
        CHECK(memSetup.valid);
        for (const auto& p : realWater) {
            double a[3], b[3];
            CHECK(reconstruct_world(setup, p.x + .5f, p.y + .5f, p.raw, a));
            CHECK(reconstruct_world(memSetup, p.x + .5f, p.y + .5f, p.raw, b));
            CHECK(dist3(a, b) < .002 * p.view[2] + .01);
        }
    }

    // 5. Setup rejects what it cannot invert.
    {
        auto bad = gpu; bad.projection.c2.w = 0;
        CHECK(!make_reconstruction(bad, wowViewport).valid);
        auto emptyRange = wowViewport; emptyRange.maxDepth = 0;
        CHECK(!make_reconstruction(gpu, emptyRange).valid);
    }
    std::puts("PASS: depth reconstruction on real WoW pixels, GPU round trip with D24, sky/clear rejection, memory camera equivalence");
    return 0;
}
