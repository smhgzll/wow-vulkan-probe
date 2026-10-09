#include "fog_math.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
using namespace depth_probe::fog;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
#define NEAR(a,b,e) CHECK(std::fabs((a)-(b))<(e))

int main() {
    // Presets from the render size.
    {
        const auto m = grid_for(Preset::Medium, 1920, 1080, 400, 8);
        CHECK(m.x == 160 && m.y == 90 && m.z == 64);
        const auto l = grid_for(Preset::Low, 1920, 1080, 400, 8), h = grid_for(Preset::High, 1920, 1080, 400, 8);
        CHECK(l.x == 120 && l.y == 68 && l.z == 48);
        CHECK(h.x == 240 && h.y == 135 && h.z == 96);
        CHECK(grid_for(Preset::Medium, 2560, 1440, 400, 8).x == 214);
    }
    // Slice distribution: endpoints, round trip, monotonic, fine near / coarse far.
    {
        const Grid g{160, 90, 64, 400.f, 8.f};
        NEAR(depth_of_slice(g, 0), 0.f, 1e-5f);
        NEAR(depth_of_slice(g, 1), 400.f, 1e-2f);
        float previous = -1;
        for (int i = 0; i <= 64; ++i) {
            const float t = i / 64.f, d = depth_of_slice(g, t);
            CHECK(d > previous);
            previous = d;
            NEAR(slice_of_depth(g, d), t, 1e-5f);
        }
        const float nearSlab = depth_of_slice(g, 1 / 64.f), farSlab = 400.f - depth_of_slice(g, 63 / 64.f);
        CHECK(nearSlab > .4f && nearSlab < .6f);
        CHECK(farSlab > 20.f && farSlab < 30.f);
        CHECK(slice_of_depth(g, 800.f) > 1.f); // beyond the volume: caller clamps
    }
    // Exponential height medium.
    {
        Medium m;
        m.baseDensity = .05f; m.baseHeight = 10.f; m.heightFalloff = std::log(2.f) / 20.f;
        NEAR(extinction_at(m, 10.f), .05f, 1e-6f);
        NEAR(extinction_at(m, 30.f), .025f, 1e-5f);
        NEAR(extinction_at(m, -10.f), .1f, 1e-5f);
        m.maxExtinction = .2f;
        NEAR(extinction_at(m, -1000.f), .2f, 1e-6f);   // clamped, never inf
        m.heightFalloff = 0;
        NEAR(extinction_at(m, 5000.f), .05f, 1e-6f);   // homogeneous
    }
    // Henyey-Greenstein: normalised for every g, isotropic at 0, forward peak for g > 0.
    for (float g : {-.5f, 0.f, .3f, .7f}) {
        double integral = 0;
        const int n = 20000;
        for (int i = 0; i < n; ++i) {
            const double c = -1.0 + (i + .5) * 2.0 / n;
            integral += henyey_greenstein(float(c), g) * 2.0 * 3.14159265358979 * (2.0 / n);
        }
        NEAR(integral, 1.0, 2e-3);
    }
    NEAR(henyey_greenstein(.3f, 0), 1.f / (4.f * 3.14159265f), 1e-6f);
    CHECK(henyey_greenstein(1.f, .5f) > henyey_greenstein(-1.f, .5f) * 20.f);
    // Analytic slab: exact for homogeneous media, so one thick slab == many thin ones.
    {
        const float S[3]{.02f, .03f, .05f};
        Accumulated thin, thick;
        for (int i = 0; i < 1000; ++i) accumulate(thin, integrate_slab(S, .04f, .05f));
        accumulate(thick, integrate_slab(S, .04f, 50.f));
        for (int c = 0; c < 3; ++c) NEAR(thin.scatter[c], thick.scatter[c], 1e-4f);
        NEAR(thin.transmittance, thick.transmittance, 1e-4f);
        NEAR(thick.transmittance, std::exp(-2.f), 1e-5f);
        // Clear medium: no extinction, scattering source integrates linearly.
        const auto clear = integrate_slab(S, 0.f, 10.f);
        NEAR(clear.transmittance, 1.f, 1e-7f);
        NEAR(clear.scattered[0], .2f, 1e-6f);
        NEAR(integrate_slab(S, 1e-7f, 10.f).scattered[0], .2f, 1e-4f); // continuous at sigmaT -> 0
    }
    // Energy: an optically thick medium with albedo a lit by L converges to a*L (Frostbite's check).
    {
        const float sigmaT = .1f, albedo = .8f, light = 2.f;
        const float S[3]{sigmaT * albedo * light, sigmaT * albedo * light, sigmaT * albedo * light};
        Accumulated a;
        for (int i = 0; i < 64; ++i) accumulate(a, integrate_slab(S, sigmaT, 20.f));
        NEAR(a.scatter[0], albedo * light, 1e-4f);
        CHECK(a.transmittance < 1e-20f);
        const float scene[3]{1, 1, 1};
        float out[3];
        composite(scene, a, out);
        NEAR(out[0], 1.6f, 1e-4f);
        Accumulated none;
        composite(scene, none, out);
        NEAR(out[1], 1.f, 1e-7f);
    }
    // Temporal reprojection: a froxel centre of the current camera, located in the previous camera's grid, is the same
    // world point (the previous camera moved and turned); points behind or beside the previous frustum are rejected.
    {
        const Grid g{160, 90, 64, 400.f, 8.f};
        auto basis = [](float yaw, float pitch, FrustumCamera& c) {
            const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
            const float f[3]{cp * cy, cp * sy, sp}, r[3]{sy, -cy, 0};
            const float u[3]{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
            for (int i = 0; i < 3; ++i) { c.forward[i] = f[i]; c.right[i] = r[i]; c.up[i] = u[i]; }
            c.p11 = 1.f / std::tan(27.f * 3.14159265f / 180.f); c.p00 = c.p11 / 1.7777778f;
        };
        FrustumCamera now{}, before{};
        basis(.30f, -.20f, now);
        basis(.27f, -.18f, before);
        const double camNow[3]{-14400.0, 420.0, 31.0}, camBefore[3]{-14400.8, 419.7, 30.2};
        int checked = 0;
        for (float u : {.05f, .5f, .93f}) for (float v : {.1f, .5f, .9f}) for (float w : {.1f, .5f, .95f}) {
            const float uvw[3]{u, v, w};
            float o[3];
            froxel_offset(g, now, uvw, o);
            const double world[3]{camNow[0] + o[0], camNow[1] + o[1], camNow[2] + o[2]};
            const float rel[3]{float(o[0] + (camNow[0] - camBefore[0])), float(o[1] + (camNow[1] - camBefore[1])), float(o[2] + (camNow[2] - camBefore[2]))};
            float prev[3];
            if (!froxel_uvw(g, before, rel, prev)) continue;
            float back[3];
            froxel_offset(g, before, prev, back);
            for (int i = 0; i < 3; ++i) NEAR(camBefore[i] + back[i], world[i], 2e-3 * (1 + std::fabs(o[2]) + depth_of_slice(g, w)));
            ++checked;
        }
        CHECK(checked >= 20);
        const float behind[3]{-now.forward[0] * 5, -now.forward[1] * 5, -now.forward[2] * 5};
        float uvw[3];
        CHECK(!froxel_uvw(g, before, behind, uvw));
        const float wide[3]{now.right[0] * 50 + now.forward[0] * 5, now.right[1] * 50 + now.forward[1] * 5, now.right[2] * 50 + now.forward[2] * 5};
        CHECK(!froxel_uvw(g, now, wide, uvw));
        const float same[3]{now.forward[0] * 100, now.forward[1] * 100, now.forward[2] * 100};
        CHECK(froxel_uvw(g, now, same, uvw) && std::fabs(uvw[0] - .5f) < 1e-5f && std::fabs(uvw[1] - .5f) < 1e-5f && std::fabs(uvw[2] - slice_of_depth(g, 100)) < 1e-5f);
    }
    // Smoke coverage: E[smoothstep(t-.7, t+.7, z)] for z ~ N(0,1). Symmetric at t = 0; values match a numpy integration
    // (amount .5 -> .351, .8 -> .234) and the fBm's measured coverage within .01 (.358, .243: z is close to normal).
    CHECK(std::fabs(smoke_mean(0) - .5f) < 1e-4f);
    CHECK(std::fabs(smoke_mean(smoke_threshold(.5f)) - .351f) < 2e-3f);
    CHECK(std::fabs(smoke_mean(smoke_threshold(.8f)) - .234f) < 2e-3f);
    for (float a = 0; a < 1; a += .1f) CHECK(smoke_mean(smoke_threshold(a + .1f)) < smoke_mean(smoke_threshold(a)));
    // Game cascades: the Shadowglen constants (c53..c64 of VS f4256ccf, frame-scan-34266-1) pass; reordered / broken rows are
    // caught; the light direction is minus the depth row; a view-space point is looked up in the finest cascade holding it.
    {
        const float real[12][4]{{-.03105f, -.02527f, .02995f, -.49468f}, {-.03918f, .02065f, -.02320f, .38591f}, {-.0000026f, -.00019f, -.00016f, .50187f},
                                {-.01553f, -.01264f, .01497f, -.22103f}, {-.01959f, .01032f, -.01160f, .19232f}, {-.0000026f, -.00019f, -.00016f, .50161f},
                                {-.00388f, -.00316f, .00374f, -.06056f}, {-.00490f, .00258f, -.00290f, .05277f}, {-.0000026f, -.00019f, -.00016f, .50109f},
                                {-.00097f, -.00079f, .00094f, -.02318f}, {-.00122f, .00065f, -.00073f, .02014f}, {-.0000026f, -.00019f, -.00016f, .49843f}};
        CHECK(!cascade_rows_problem(real));
        int order[4];
        cascade_order(real, order);
        CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 3);
        float toward[3];
        CHECK(cascade_light_view(real, toward));
        NEAR(toward[1] * toward[1] + toward[2] * toward[2], 1.f, 1e-3f);
        CHECK(toward[1] > .7f && toward[2] > .6f);                          // the sky direction in view space was (0.013, 0.758, 0.653)
        float broken[12][4];
        std::copy(&real[0][0], &real[0][0] + 48, &broken[0][0]);
        broken[5][1] = -.00030f;                                            // cascade 1's depth row differs
        CHECK(cascade_rows_problem(broken) != nullptr);
        std::copy(&real[0][0], &real[0][0] + 48, &broken[0][0]);
        for (int i = 0; i < 3; ++i) broken[0][i] = real[2][i] * 100;        // u along the light
        CHECK(cascade_rows_problem(broken) != nullptr);
        // Light clip space around the camera: the camera itself is at the middle of the coarsest cascade; 5 yd ahead is in the
        // finest, 250 yd ahead in the 320-yd one, 5 km away in none.
        const float camera[3]{0, 0, 0}, near[3]{0, 0, 5}, far[3]{0, 0, 250}, outside[3]{5000, 0, 5000};
        float xyd[3];
        CHECK(cascade_lookup(real, camera, 0, xyd) == 0);
        cascade_lookup(real, near, .01f, xyd);
        CHECK(cascade_lookup(real, near, .01f, xyd) == 0);
        CHECK(cascade_lookup(real, far, .01f, xyd) == 2);
        CHECK(cascade_lookup(real, outside, .01f, xyd) == -1);
        const float c3x = real[9][3], c3y = real[10][3];                    // the camera in the coarsest: near its middle
        CHECK(std::fabs(c3x) < .05f && std::fabs(c3y) < .05f);
        NEAR(2.f / std::sqrt(real[9][0] * real[9][0] + real[9][1] * real[9][1] + real[9][2] * real[9][2]), 1280.f, 5.f);   // 1280 yd wide
    }
    // Sun phase with an aureole lobe: still normalised over the sphere, far brighter right at the sun, a little dimmer sideways.
    {
        double integral = 0;
        const int n = 20000;
        for (int i = 0; i < n; ++i) { const double c = -1 + 2 * (i + .5) / n; integral += sun_phase(float(c), .35f, .04f, .9f) * 2 * M_PI * (2.0 / n); }
        NEAR(integral, 1.0, 2e-3);
        CHECK(sun_phase(1, .35f, .04f, .9f) > 3 * henyey_greenstein(1, .35f));   // .848 vs .254 right at the sun
        CHECK(sun_phase(0, .35f, .04f, .9f) < henyey_greenstein(0, .35f));
        NEAR(sun_phase(.3f, .35f, 0, .9f), henyey_greenstein(.3f, .35f), 1e-6f);   // 0 = the single lobe (x87: not bit-exact)
    }
    // Sun glare: 1 at the disc, falling off (core 2.5 deg, skirt 8), nothing when no part of the disc is visible, a sliver strong.
    NEAR(glare_psf(0), 1.f, 1e-6f);
    for (float d = 0; d < 19.5f; d += .5f) CHECK(glare_psf(d + .5f) < glare_psf(d));
    CHECK(glare_psf(20) == 0 && glare_psf(25) == 0);                      // faded out by the composite's 20-degree cut: no ring
    NEAR(glare_psf(19.9f), 0, 1e-3f);
    CHECK(glare_amount(.7f, 0, 1) == 0);
    NEAR(glare_amount(1, .064f, 0), .4f, 1e-4f);                         // 6.4% of the disc: cube root .4
    CHECK(glare_amount(2, 1, 0) == 1);                                    // clamped
    // light_limit: unchanged up to half the ceiling, continuous with the same slope there, always below the ceiling, rising.
    CHECK(soft_limit(.3f, .8f) == .3f && soft_limit(.4f, .8f) == .4f);
    NEAR(soft_limit(.401f, .8f) - soft_limit(.4f, .8f), .001f, 2e-5f);
    CHECK(soft_limit(2.7f, .8f) < .8f && soft_limit(2.7f, .8f) > .78f && soft_limit(100.f, .8f) <= .8f);
    for (float x = .4f; x < 5; x += .1f) CHECK(soft_limit(x + .1f, .8f) > soft_limit(x, .8f));
    CHECK(soft_limit(3.f, 0.f) == 3.f);                                  // 0: no limit
    // Exposure key: the mean log2 luminance between the 40th and 98th percentile of the bins (log2 from -8, 1/8 wide).
    {
        uint32_t bins[64]{};
        float key = 0;
        CHECK(!exposure_key(bins, 64, -8, .125f, .4f, .98f, key));
        bins[60] = 1000;                                                 // everything at log2 -0.4375 (~0.74)
        CHECK(exposure_key(bins, 64, -8, .125f, .4f, .98f, key));
        NEAR(key, std::exp2(-8 + 60.5f * .125f), 1e-5f);
        bins[16] = 1500;                                                 // 60% dark (log2 -5.9): below the 40th percentile...
        CHECK(exposure_key(bins, 64, -8, .125f, .4f, .98f, key));
        CHECK(key > .1f && key < .74f);                                  // ...partly counted (the band starts inside them)
        bins[16] = 9000;                                                 // mostly dark: the key follows the dark part
        CHECK(exposure_key(bins, 64, -8, .125f, .4f, .98f, key) && key < .05f);
    }
    // Exposure: only darkens, brings the key to the target, never below the floor.
    NEAR(exposure_for(.3f, .55f, .6f), 1.f, 1e-6f);                      // a normal frame: untouched
    NEAR(exposure_for(.69f, .55f, .6f), .55f / .69f, 1e-6f);             // fogged Booty Bay: ~0.8
    NEAR(exposure_for(.89f, .55f, .6f), .618f, 1e-3f);                   // the white screenshot
    NEAR(exposure_for(2.f, .55f, .6f), .6f, 1e-6f);                      // floor
    // History weight: still at rest, `moving` from a 1-degree turn or a .5-yd step a frame, never above `still`.
    NEAR(motion_history_weight(.95f, .8f, 0, 0), .95f, 1e-6f);
    NEAR(motion_history_weight(.95f, .8f, 1, 0), .8f, 1e-6f);
    NEAR(motion_history_weight(.95f, .8f, 0, .5f), .8f, 1e-6f);
    NEAR(motion_history_weight(.95f, .8f, 20, 3), .8f, 1e-6f);
    NEAR(motion_history_weight(.95f, .8f, .5f, .1f), .875f, 1e-6f);   // half a degree
    NEAR(motion_history_weight(.7f, .8f, 5, 0), .7f, 1e-6f);          // moving above still: still
    // Ground level: the 20th percentile of the floor cells' heights (1-yard bins from -128 relative to the camera).
    {
        uint32_t bins[256]{};
        float level = 0;
        CHECK(!ground_level(bins, 256, -128, 1, .2f, 200, level));            // nothing seen
        bins[128 - 5] = 800;                                                 // flat land 5 yd under the camera
        bins[128 + 30] = 150; bins[128 + 60] = 50;                           // canopy tops / roofs seen from above
        CHECK(ground_level(bins, 256, -128, 1, .2f, 200, level));
        CHECK(level >= -5 && level < -4);                                    // inside the land's bin
        bins[128 - 25] = 100;                                                // a valley 25 yd down, 1/11 of the cells
        CHECK(ground_level(bins, 256, -128, 1, .2f, 200, level) && level >= -5 && level < -4);
        bins[128 - 25] = 600;                                                // a large valley: the ground is its floor
        CHECK(ground_level(bins, 256, -128, 1, .2f, 200, level) && level >= -25 && level < -24);
        uint32_t few[256]{};
        few[100] = 150;
        CHECK(!ground_level(few, 256, -128, 1, .2f, 200, level));            // too few cells: keep the last level
    }
    // Sun disc finder: which clusters are a disc (values from the Booty Bay [FOG-SUN] log, 480x270 picture).
    {
        auto cluster = [](uint32_t n, double x, double y, double sx, double sy) { DiscCluster c; c.count = n; c.sky = 60000; c.meanX = x; c.meanY = y; c.spreadX = sx; c.spreadY = sy; c.width = 480; c.height = 270; return c; };
        CHECK(!disc_cluster_problem(cluster(119, 238, 100, 3.1, 3.0)));                     // the white disc at 954,400
        CHECK(!disc_cluster_problem(cluster(245, 237, 99, 4.4, 4.4)));                      // the warm disc at 950,398
        CHECK(std::string(disc_cluster_problem(cluster(259, 251, 24, 15.4, 22.8))) == "spread");   // haze around the disc
        CHECK(std::string(disc_cluster_problem(cluster(158, 89, 167, 5.9, 1.2))) == "streak");     // a band along the horizon
        CHECK(std::string(disc_cluster_problem(cluster(5, 320, 253, 3.7, .5))) == "small");
        CHECK(std::string(disc_cluster_problem(cluster(78, 316, 2, 4.1, 1.6))) == "edge");        // cut by the top edge
        CHECK(std::string(disc_cluster_problem(cluster(78, 477, 100, 2, 2))) == "edge");
        DiscCluster wholeSky = cluster(40000, 240, 135, 5, 5);
        CHECK(std::string(disc_cluster_problem(wholeSky)) == "whole sky");
    }
    // The disc is put on the game light's azimuth arc; only the elevation is kept.
    {
        const float arc[2]{.6230f, .6230f};
        double d[3]{.4911, .4680, .7347};   // a sighting at azimuth 43.6
        disc_onto_arc(arc, d);
        NEAR(d[0], d[1], 1e-12);
        NEAR(d[2], .7347, 1e-12);
        NEAR(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0, 1e-12);
        CHECK(d[0] > 0);
        const float none[2]{0, 0};
        double e[3]{.6, 0, .8};
        disc_onto_arc(none, e);             // no azimuth known: unchanged
        CHECK(e[0] == .6 && e[1] == 0 && e[2] == .8);
    }
    // Temporal filter: small changes refine, one outlier never moves the sun, a real change moves it after confirmHits.
    {
        auto dir = [](double elevationDeg, double azimuthDeg, double* d) {
            const double e = elevationDeg * M_PI / 180, a = azimuthDeg * M_PI / 180;
            d[0] = std::cos(e) * std::cos(a); d[1] = std::cos(e) * std::sin(a); d[2] = std::sin(e);
        };
        auto elevation = [](const DiscTrack& t) { return std::asin(t.dir[2]) * 180 / M_PI; };
        DiscTrack t;
        double d[3];
        dir(45, 45, d);
        CHECK(t.add(d) == DiscTrack::Waiting && !t.valid);                  // the first sighting is not trusted alone
        CHECK(t.add(d) == DiscTrack::Waiting);
        CHECK(t.add(d) == DiscTrack::Moved && t.valid);
        NEAR(elevation(t), 45, 1e-6);
        dir(45.5, 45, d);
        CHECK(t.add(d) == DiscTrack::Smoothed);
        CHECK(elevation(t) > 45 && elevation(t) < 45.5);
        // Outliers (a glint on the horizon, a bright cloud higher up) alternate with the real disc: never moved.
        for (int i = 0; i < 10; ++i) {
            dir(i % 2 ? 1.8 : 51.2, 45, d);
            CHECK(t.add(d) == DiscTrack::Waiting);
            dir(45.2, 45, d);
            CHECK(t.add(d) == DiscTrack::Smoothed);
        }
        CHECK(std::fabs(elevation(t) - 45.2) < .3);
        // Two outliers in a row are still not enough; a third different one restarts the count.
        dir(30, 45, d); CHECK(t.add(d) == DiscTrack::Waiting);
        dir(30, 45, d); CHECK(t.add(d) == DiscTrack::Waiting);
        dir(60, 45, d); CHECK(t.add(d) == DiscTrack::Waiting);
        CHECK(std::fabs(elevation(t) - 45.2) < .3);
        // A real change (the disc seen elsewhere three times): the sun moves there.
        dir(30, 45, d); CHECK(t.add(d) == DiscTrack::Waiting);
        dir(30.3, 45, d); CHECK(t.add(d) == DiscTrack::Waiting);
        dir(30.1, 45, d); CHECK(t.add(d) == DiscTrack::Moved);
        CHECK(std::fabs(elevation(t) - 30.13) < .05);
        t.reset();
        CHECK(!t.valid && t.hits == 0);
        // Parts of a disc (Booty Bay: a torch flame against the orange horizon, 13 texels at elevation 3.5 on the arc, while the
        // real disc showed 13-25 texels past a hull): never move the fog sun, only confirm it when near; the whole disc does.
        CHECK(!disc_whole(13, 1.4, 1.104, 480, .9) && !disc_whole(25, 1.4, 1.104, 480, 1));
        CHECK(disc_whole(100, 1.4, 1.104, 480, 1) && disc_whole(196, 1.4, 1.104, 480, .9));
        CHECK(disc_whole(9, 1.4, 1.732, 32, .97));                               // the smoke's 128x128 frame: a 9-texel disc is whole
        CHECK(!disc_whole(60, 1.4, 1.104, 480, .7));                              // off-centre the disc projects larger
        dir(46, 45, d);
        CHECK(t.add(d, false) == DiscTrack::Waiting && !t.valid);                  // with no fog sun, parts can start it (3 in a row)
        CHECK(t.add(d, false) == DiscTrack::Waiting && t.add(d, false) == DiscTrack::Moved && t.valid);
        t.reset();
        for (int i = 0; i < 3; ++i) t.add(d);
        CHECK(t.valid && std::fabs(elevation(t) - 46) < 1e-6);
        for (int i = 0; i < 6; ++i) { dir(3.5, 45, d); CHECK(t.add(d, false) == DiscTrack::Partial); }
        dir(46.8, 45, d);
        CHECK(t.add(d, false) == DiscTrack::Confirmed && std::fabs(elevation(t) - 46) < 1e-6);   // near: confirms, does not pull
        CHECK(t.hits == 0);
    }
    std::puts("PASS: fog grid presets, slice distribution, height medium, HG normalisation, analytic slab integration, energy limit, temporal reprojection, smoke coverage mean, game cascade rows, sun aureole phase, sun glare, light limit, exposure key and goal, ground level percentile, sun disc clusters, disc on the light's arc, disc temporal filter, parts of a disc never move a fog sun (but can start one)");
    return 0;
}
