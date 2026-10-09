#include "water_surface.hpp"
#include <algorithm>
#include <cmath>

namespace depth_probe::water {
namespace {
constexpr double twoPi = 6.283185307179586;
constexpr float phaseOffset[maxWaves]{0.f, 1.7f, 3.9f, 5.1f, 2.3f};   // fixed: the crests of different waves do not line up at the origin
float smoothstep(float e0, float e1, float x) { const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
}

PackedWaves pack_waves(const WaveSettings& s, float heightScale, float steepnessScale, double seconds) {
    PackedWaves p;
    unsigned n = 0;
    float sumA = 0, sumQkA = 0, sumkA = 0;
    for (unsigned i = 0; i < maxWaves; ++i) {
        const auto& w = s.waves[i];
        const float amplitude = w.amplitude * std::max(heightScale, 0.f);
        if (!(w.wavelength > .05f) || !(amplitude > 0.f)) continue;
        const double angle = double(s.windDeg + w.directionDeg) * twoPi / 360.0, k = twoPi / double(w.wavelength);
        const double omega = std::sqrt(double(gravity) * k) * double(std::max(w.speed, 0.f)) * double(std::max(s.speed, 0.f));
        p.a[n][0] = float(std::cos(angle)); p.a[n][1] = float(std::sin(angle)); p.a[n][2] = float(k); p.a[n][3] = amplitude;
        p.b[n][0] = float(omega);
        p.b[n][1] = std::clamp(w.steepness * steepnessScale, 0.f, 1.f);
        p.b[n][2] = float(std::fmod(std::fmod(double(phaseOffset[i]) - omega * seconds, twoPi) + twoPi, twoPi));
        sumA += amplitude; sumkA += float(k) * amplitude; sumQkA += p.b[n][1] * float(k) * amplitude;
        ++n;
    }
    // No folding: the horizontal Jacobian stays positive while sum Q k A < 1 (the displacement's derivative has norm <= that sum).
    if (sumQkA > .95f) {
        const float scale = .95f / sumQkA;
        for (unsigned i = 0; i < n; ++i) p.b[i][1] *= scale;
        sumQkA = .95f;
    }
    p.surface[0] = float(n); p.surface[1] = sumA; p.surface[2] = sumQkA; p.surface[3] = sumkA;
    return p;
}

float wave_lod(float k, float footprint) {
    if (!(footprint > 0.f)) return 1.f;
    return smoothstep(3.f, 8.f, 6.2831853f / (k * footprint));
}

SurfacePoint evaluate_surface(const PackedWaves& p, Vec2 x0, float baseZ, float footprint, float delay) {
    float dx = 0, dy = 0, dz = 0, sxx = 0, syy = 0, sxy = 0, cx = 0, cy = 0;
    const int n = int(p.surface[0]);
    for (int i = 0; i < n; ++i) {
        const float Dx = p.a[i][0], Dy = p.a[i][1], k = p.a[i][2], A = p.a[i][3] * wave_lod(k, footprint), Q = p.b[i][1];
        const float theta = k * (Dx * x0.x + Dy * x0.y) + p.b[i][2] + p.b[i][0] * delay, s = std::sin(theta), c = std::cos(theta);
        dx += Q * A * Dx * c; dy += Q * A * Dy * c; dz += A * s;
        const float kA = k * A;
        sxx += Q * kA * Dx * Dx * s; syy += Q * kA * Dy * Dy * s; sxy += Q * kA * Dx * Dy * s;
        cx += kA * Dx * c; cy += kA * Dy * c;
    }
    SurfacePoint out;
    out.sxx = sxx; out.syy = syy; out.sxy = sxy; out.slope = {cx, cy};
    out.parameter = x0;
    out.displacement = {dx, dy, dz};
    out.position = {x0.x + dx, x0.y + dy, baseZ + dz};
    // dP/dx0 x dP/dy0 with dP/dx = (1 - sxx, -sxy, cx), dP/dy = (-sxy, 1 - syy, cy).
    out.jacobian = (1.f - sxx) * (1.f - syy) - sxy * sxy;
    Vec3 nrm{-(cx * (1.f - syy) + sxy * cy), -(cy * (1.f - sxx) + sxy * cx), out.jacobian};
    const float len = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
    out.normal = len > 1e-12f ? Vec3{nrm.x / len, nrm.y / len, nrm.z / len} : Vec3{0, 0, 1};
    return out;
}

bool intersect_surface(const PackedWaves& p, Vec3 o, Vec3 d, float baseZ, float footprint, SurfacePoint& out, Vec3& hit, float* residual,
                       int iterations) {
    if (!(d.z < -1e-6f)) return false;   // the ray must go down toward the water
    float t = (baseZ - o.z) / d.z;
    if (!(t > 0.f)) return false;        // the camera is below the base plane
    Vec2 x0{o.x + t * d.x, o.y + t * d.y};
    const Vec2 r{d.x / d.z, d.y / d.z};   // horizontal move of the ray per yard of height
    for (int i = 0; i < iterations; ++i) {
        const auto s = evaluate_surface(p, x0, baseZ, footprint);
        t = (s.position.z - o.z) / d.z;
        const Vec2 f{s.position.x - (o.x + t * d.x), s.position.y - (o.y + t * d.y)};
        const float m00 = 1.f - s.sxx - r.x * s.slope.x, m01 = -s.sxy - r.x * s.slope.y,
                    m10 = -s.sxy - r.y * s.slope.x, m11 = 1.f - s.syy - r.y * s.slope.y, det = m00 * m11 - m01 * m10;
        if (std::fabs(det) > .05f) x0 = {x0.x - (m11 * f.x - m01 * f.y) / det, x0.y - (m00 * f.y - m10 * f.x) / det};
        else x0 = {x0.x - f.x, x0.y - f.y};   // nearly tangent (a silhouette): the plain step
    }
    out = evaluate_surface(p, x0, baseZ, footprint);
    t = (out.position.z - o.z) / d.z;
    hit = {o.x + t * d.x, o.y + t * d.y, out.position.z};
    if (residual) residual[0] = std::hypot(hit.x - out.position.x, hit.y - out.position.y);
    return true;
}

}  // namespace depth_probe::water
