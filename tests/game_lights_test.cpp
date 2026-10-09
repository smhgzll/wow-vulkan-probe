// The game's point lights (src/game_lights.*) on constants from a real "lights" frame scan (frame-scan-105226-2, three
// torches by a campfire; M2 vertex shader d4fa457d, draw 37).
#include "game_lights.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) < (eps))

using namespace depth_probe::lights;

int main() {
    // c28..c40 of the draw
    const float lit[13][4]{
        {-.4333f, -.8222f, .3691f, .9961f},  // c28: minus the light direction
        {.9961f, .6667f, .3608f, 14.f},     // c29
        {.5540f, -.0990f, .8266f, -7.379f}, {.5838f, -.6617f, -.4705f, 3.0454f}, {-.5935f, -.7432f, .3088f, 1996.1075f},   // c30..c32
        {.4197f, .2610f, .1199f, 1.f}, {.3096f, .1925f, .0884f, 1.f}, {.3352f, .2085f, .0958f, 1.f}, {0, 0, 0, 0},       // c33..c36 colours
        {-7.9444f, -5.2424f, -.9853f, 1.f}, {-1.2539f, -5.9566f, -2.3756f, 1.f}, {.4177f, 2.8022f, 14.6752f, 1.f}, {0, 0, 0, 0}};   // c37..c40
    ViewLight out[slots];
    CHECK(parse_m2_lights(lit, out) == 3);
    NEAR(out[0].pos[0], -7.9444, 1e-4); NEAR(out[0].color[0], .4197, 1e-4);
    NEAR(out[2].pos[2], 14.6752, 1e-4); NEAR(out[2].color[1], .2085, 1e-4);
    // Through the scan's camera the three sit at one world height (torches on flat ground).
    const Basis b{{-2918.62f, -224.01f, 59.6f}, {.78f, .626f, 0.f}, {-.286f, .356f, .89f}, {-.557f, .694f, -.457f}};
    float w[3][3];
    for (int i = 0; i < 3; ++i) view_to_world(b, out[i].pos, w[i]);
    NEAR(w[0][2], 55.39, .03); NEAR(w[1][2], 55.39, .03); NEAR(w[2][2], 55.39, .03);
    // An unlit model: the same layout, every slot empty.
    float unlit[13][4];
    std::memcpy(unlit, lit, sizeof unlit);
    for (int k = 5; k < 13; ++k) for (auto& v : unlit[k]) v = 0;
    CHECK(parse_m2_lights(unlit, out) == 0);
    // The campfire's own draw: its light at the camera is not one.
    float own[13][4];
    std::memcpy(own, lit, sizeof own);
    own[9][0] = .03f; own[9][1] = 0; own[9][2] = .04f;
    CHECK(parse_m2_lights(own, out) == 2);
    // Terrain shader 44c15e1a in Shadowglen (scan 34266-1): c28 = (-0.0029, 1.1111, 1, 0) - not this layout.
    float terrain[13][4]{};
    terrain[0][0] = -.0029f; terrain[0][1] = 1.1111f; terrain[0][2] = 1.f;
    CHECK(parse_m2_lights(terrain, out) == -1);
    // A colour without a position of w 1: not this layout.
    float broken[13][4];
    std::memcpy(broken, lit, sizeof broken);
    broken[9][3] = 0;
    CHECK(parse_m2_lights(broken, out) == -1);
    // Deduplication within a frame: the same light from two draws counts once.
    std::vector<ViewLight> frame;
    CHECK(add_unique(frame, out[0]) && !add_unique(frame, out[0]) && add_unique(frame, out[1]) && frame.size() == 2);
    // The registry: sightings within .75 yd merge, others add; unseen for keepFrames frames: dropped.
    Registry r;
    const float c[3]{1, .6f, .3f};
    const float p0[3]{10, 20, 30}, p1[3]{10.3f, 20, 30}, p2[3]{15, 20, 30};
    r.add(p0, c, 1); r.add(p1, c, 2); r.add(p2, c, 2);
    CHECK(r.lights().size() == 2 && r.lights()[0].sightings == 2);
    NEAR(r.lights()[0].pos[0], 10.15, 1e-4);
    r.add(p2, c, 100);
    r.expire(2 + Registry::keepFrames + 1);
    CHECK(r.lights().size() == 1 && r.lights()[0].pos[0] == 15.f);
    r.expire(100 + Registry::keepFrames + 1);
    CHECK(r.lights().empty());
    // Far away a sighting may be off by more (turning): merge_radius grows with the distance, and the nearest light takes it.
    NEAR(merge_radius(10), .75, 1e-6);
    NEAR(merge_radius(130), 2.6, 1e-5);
    const float far0[3]{100, 0, 0}, far1[3]{102.5f, 0, 0}, far2[3]{101.8f, 0, 0}, near3[3]{104.5f, 0, 0};
    r.add(far0, c, 1, merge_radius(130)); r.add(near3, c, 1, merge_radius(130)); r.add(far1, c, 2, merge_radius(130));
    CHECK(r.lights().size() == 2 && r.lights()[1].sightings == 2);   // 102.5 is within 2.6 of both, nearer to 104.5: that one took it
    r.clear();
    r.add(far0, c, 1); r.add(far2, c, 2);
    CHECK(r.lights().size() == 2);   // .75 yd alone: two copies
    for (int i = 0; i < 70; ++i) { const float p[3]{float(i) * 2, 0, 0}; r.add(p, c, uint64_t(i)); }
    CHECK(r.lights().size() == Registry::maxLights && r.lights().front().pos[0] > 0);   // the least recently seen went first
    if (failures) return 1;
    std::puts("PASS: point lights from M2 vertex constants (3 torches, empty slots, the campfire's own, other layouts rejected), view -> world, per-frame dedup, registry merge (nearest, distance-scaled) / expiry / cap");
    return 0;
}
