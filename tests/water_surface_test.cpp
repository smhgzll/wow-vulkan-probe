#include "water_surface.hpp"
#include "water_shading.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace depth_probe::water;
#define CHECK(x) do{if(!(x)){std::fflush(stdout);std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
#define NEAR(a,b,e) CHECK(std::fabs((a)-(b))<(e))

static Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 unit(Vec3 v) { const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return {v.x / l, v.y / l, v.z / l}; }
static Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

int main() {
    const WaveSettings defaults;
    const auto waves = pack_waves(defaults, 1.f, 1.f, 12.5);
    // The defaults stay calm (tuning values, not measured): four waves, slopes k*A summing to well under .15, crests far from folding.
    CHECK(waves.surface[0] == 4.f && waves.surface[3] < .12f && waves.surface[2] < .12f);
    // Flat: no waves (WAVE HEIGHT 0) is exactly the base plane.
    {
        const auto flat = pack_waves(defaults, 0.f, 1.f, 12.5);
        CHECK(flat.surface[0] == 0.f);
        const auto s = evaluate_surface(flat, {37.f, -12.f}, 4.f, 0.f);
        CHECK(s.position.x == 37.f && s.position.y == -12.f && s.position.z == 4.f && s.normal.z == 1.f && s.jacobian == 1.f);
        SurfacePoint out; Vec3 hit;
        CHECK(intersect_surface(flat, {0, 0, 20}, {1, .5f, -2}, 4.f, 0.f, out, hit));
        NEAR(hit.x, 8.f, 1e-5f); NEAR(hit.y, 4.f, 1e-5f); NEAR(hit.z, 4.f, 1e-6f);
    }
    // The analytic normal and Jacobian are those of the displaced surface: finite differences of P(x0) (double-sided, h = .01 yd).
    {
        double worst = 0, worstJ = 0;
        for (float x = -30; x <= 30; x += 7.3f)
            for (float y = -20; y <= 20; y += 5.9f) {
                const float h = .01f;
                const auto s = evaluate_surface(waves, {x, y}, 0.f, 0.f);
                const auto px = sub(evaluate_surface(waves, {x + h, y}, 0.f, 0.f).position, evaluate_surface(waves, {x - h, y}, 0.f, 0.f).position);
                const auto py = sub(evaluate_surface(waves, {x, y + h}, 0.f, 0.f).position, evaluate_surface(waves, {x, y - h}, 0.f, 0.f).position);
                const auto n = unit(cross(px, py));
                worst = std::max(worst, double(1.f - dot(n, s.normal)));
                const float j = (px.x * py.y - px.y * py.x) / (4 * h * h);
                worstJ = std::max(worstJ, double(std::fabs(j - s.jacobian)));
                CHECK(s.normal.z > .9f);
            }
        std::printf("normal vs finite differences: worst 1 - cos %.2e, Jacobian |error| %.2e\n", worst, worstJ);
        CHECK(worst < 2e-6 && worstJ < 2e-3);
    }
    // A single wave with vertical motion only (steepness 0) travels along its direction at the deep-water speed (x the speed settings).
    {
        WaveSettings one; for (auto& w : one.waves) w = {};
        one.waves[0] = {10.f, 12.f, .2f, 0.f, 1.f}; one.windDeg = 20.f; one.speed = .5f;
        const auto a = pack_waves(one, 1.f, 1.f, 100.0), b = pack_waves(one, 1.f, 1.f, 101.5);
        const float k = 6.2831853f / 12.f, c = std::sqrt(gravity / k) * .5f, angle = 30.f * 3.14159265f / 180.f;
        NEAR(a.b[0][0] / a.a[0][2], c, 1e-4f);
        const Vec2 x0{3.f, -4.f}, moved{x0.x + std::cos(angle) * c * 1.5f, x0.y + std::sin(angle) * c * 1.5f};
        NEAR(evaluate_surface(a, x0, 0.f, 0.f).position.z, evaluate_surface(b, moved, 0.f, 0.f).position.z, 1e-4f);
        CHECK(std::fabs(evaluate_surface(a, x0, 0.f, 0.f).position.z - evaluate_surface(b, x0, 0.f, 0.f).position.z) > .02f);   // it did move
    }
    // No jump when the old float clock wrapped (3600 s): the phase is computed in double from unwrapped time.
    {
        const auto a = pack_waves(defaults, 1.f, 1.f, 3599.99), b = pack_waves(defaults, 1.f, 1.f, 3600.01);
        for (int i = 0; i < 4; ++i) {
            float d = std::fabs(a.b[i][2] - b.b[i][2]); d = std::min(d, 6.2831853f - d);
            NEAR(d, a.b[i][0] * .02f, 1e-4f);
        }
    }
    // The surface never folds: huge waves (x10) with full steepness are limited to sum Q k A <= .95, and the Jacobian stays positive.
    {
        const auto big = pack_waves(defaults, 10.f, 1.f, 7.0);
        CHECK(big.surface[2] <= .95f + 1e-5f);
        float lowest = 2;
        for (float x = -40; x <= 40; x += .37f)
            for (float y = -40; y <= 40; y += 2.3f) lowest = std::min(lowest, evaluate_surface(big, {x, y}, 0.f, 0.f).jacobian);
        std::printf("x10 waves: sum QkA %.3f, lowest Jacobian %.3f\n", big.surface[2], lowest);
        CHECK(lowest > 0.f && lowest < .8f);   // compressed crests, never folded
    }
    // Level of detail: a 6.5-yd wave is gone at 2.5 yd per pixel and whole at .5; a huge footprint leaves the flat plane.
    {
        const float k = 6.2831853f / 6.5f;
        CHECK(wave_lod(k, 2.5f) == 0.f && wave_lod(k, .5f) == 1.f && wave_lod(k, 0.f) == 1.f);
        const auto far = evaluate_surface(waves, {5.f, 5.f}, 1.f, 100.f);
        CHECK(far.position.z == 1.f && far.normal.z == 1.f);
    }
    // Ray intersection: the pixel's hit lies on the surface (residual) from overhead down to grazing views, with the footprint the
    // composite uses (1080p, 54-degree fov: ~.000945 yd per pixel per yard of depth, stretched by 1/sin(elevation)).
    {
        const Vec3 camera{100.f, 50.f, 12.f};
        const float elevations[]{90.f, 45.f, 20.f, 10.f, 6.f, 3.f, 1.5f};
        for (float elevation : elevations) {
            float worst = 0;
            for (float azimuth = 0; azimuth < 360; azimuth += 17) {
                const float e = elevation * 3.14159265f / 180.f, a = azimuth * 3.14159265f / 180.f;
                const Vec3 dir{std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), -std::sin(e)};
                const float distance = 12.f / std::sin(e), footprint = distance * std::cos(std::min(e, 1.5f)) * .000945f / std::max(std::sin(e), .05f);
                SurfacePoint s; Vec3 hit; float residual = 0;
                CHECK(intersect_surface(waves, camera, dir, 0.f, footprint, s, hit, &residual));
                worst = std::max(worst, residual);
                CHECK(std::fabs(hit.z) <= waves.surface[1] + 1e-4f);
                // the hit is on the ray
                const Vec3 toHit = sub(hit, camera);
                const float along = dot(toHit, dir);
                NEAR(toHit.x, dir.x * along, 2e-3f); NEAR(toHit.y, dir.y * along, 2e-3f);
            }
            std::printf("elevation %4.1f deg: worst residual %.2e yd\n", elevation, worst);
            CHECK(worst < 1e-3f);
        }
        SurfacePoint s; Vec3 hit;
        CHECK(!intersect_surface(waves, camera, {1.f, 0.f, .2f}, 0.f, 0.f, s, hit));    // looking up: no water
        CHECK(!intersect_surface(waves, {0, 0, -3}, {0, 0, -1}, 0.f, 0.f, s, hit));     // camera under the base plane
    }
    // World-space anchoring: two cameras looking at the same water particle see the same surface point and normal (the surface is a
    // function of world position and time only; nothing depends on the camera).
    {
        const Vec2 x0{-14871.3f + 14871.f, 395.6f - 395.f};   // near the origin: float precision of the test itself
        const auto target = evaluate_surface(waves, x0, 0.f, 0.f);
        const Vec3 cameras[]{{-20.f, 5.f, 15.f}, {30.f, 40.f, 6.f}, {.2f, .7f, 40.f}};
        for (const auto& c : cameras) {
            SurfacePoint s; Vec3 hit;
            CHECK(intersect_surface(waves, c, sub(target.position, c), 0.f, 0.f, s, hit, nullptr, 8));
            NEAR(hit.x, target.position.x, 2e-3f); NEAR(hit.y, target.position.y, 2e-3f); NEAR(hit.z, target.position.z, 2e-3f);
            CHECK(dot(s.normal, target.normal) > .99999f);
        }
    }
    // MODERN shading terms (water_shading.cpp == the composite's modern path).
    {
        // Fresnel: R0 looking straight down, rising to the roughness ceiling at grazing angles, never falling as the view flattens.
        NEAR(fresnel_rough(1.f, .06f), fresnelR0, 1e-6f);
        NEAR(fresnel_rough(0.f, .02f), .98f, 1e-5f);
        NEAR(fresnel_rough(0.f, .3f), .7f, 1e-5f);                    // a rough surface is no perfect mirror even at grazing angles
        float previous = 0;
        for (float nv = 1; nv >= 0; nv -= .01f) { const float f = fresnel_rough(nv, .06f); CHECK(f >= previous - 1e-6f); previous = f; }
        CHECK(fresnel_rough(.2f, .06f) > fresnel_rough(.2f, .25f));   // rougher: less reflection at the same angle
        // The old classic floor: at 60 degrees from the vertical the modern weight is under a sixth of it (the "flat mirror" look).
        CHECK(fresnel_rough(.5f, .06f) < .30f / 6.f);
        // Beer-Lambert: nothing through no water, red goes first with pure-water absorption, more path = less light, clarity 0 = 8x murkier.
        const float sigma[3]{.31f, .055f, .008f};
        float t[3], t2[3], murky[3];
        transmittance(sigma, 1.f, 0.f, t); CHECK(t[0] == 1.f && t[1] == 1.f && t[2] == 1.f);
        transmittance(sigma, 1.f, 3.f, t); CHECK(t[0] < t[1] && t[1] < t[2] && t[2] < 1.f);
        transmittance(sigma, 1.f, 6.f, t2); for (int c = 0; c < 3; ++c) NEAR(t2[c], t[c] * t[c], 1e-5f);
        transmittance(sigma, 0.f, 3.f, murky); NEAR(murky[0], std::exp(-.31f * 8.f * 3.f), 1e-6f);
        std::printf("pure water, 3 yd: transmittance %.3f %.3f %.3f (clarity 1)\n", t[0], t[1], t[2]);
        // Roughness: nothing added where the micro ripples are resolved (mip 0) or switched off, their variance half-added when averaged away.
        NEAR(pixel_roughness(.06f, 1.f, 0.f), .06f, 1e-6f);
        NEAR(pixel_roughness(.06f, 0.f, 8.f), .06f, 1e-6f);
        const float var = (.235f * .34f) * (.235f * .34f);
        NEAR(pixel_roughness(.06f, 1.f, 30.f), std::sqrt(.06f * .06f + var), 1e-5f);
        CHECK(pixel_roughness(.0f, 1.f, 0.f) == .02f);                // floor
    }
    std::puts("PASS: water surface - flat without waves, analytic normal/Jacobian = finite differences, waves travel at the set speed, no phase jump at the clock wrap, never folds, LOD, ray hits converge down to grazing, world-anchored; Fresnel, Beer-Lambert and pixel roughness terms");
    return 0;
}
