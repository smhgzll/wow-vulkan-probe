#include "fog_math.hpp"
#include <algorithm>
#include <cmath>

namespace depth_probe::fog {
namespace {
constexpr float pi = 3.14159265358979323846f;
uint32_t cells(uint32_t pixels, uint32_t size) { return std::max(1u, (pixels + size - 1) / size); }
}

Grid grid_for(Preset preset, uint32_t width, uint32_t height, float maxDistance, float distribution) {
    const uint32_t size = preset == Preset::Low ? 16 : preset == Preset::Medium ? 12 : 8;
    const uint32_t slices = preset == Preset::Low ? 48 : preset == Preset::Medium ? 64 : 96;
    return {cells(width, size), cells(height, size), slices, maxDistance, distribution};
}

float depth_of_slice(const Grid& g, float t) {
    return g.distribution * (std::exp(t * std::log1p(g.maxDistance / g.distribution)) - 1.f);
}

float slice_of_depth(const Grid& g, float d) {
    return std::log1p(std::max(d, 0.f) / g.distribution) / std::log1p(g.maxDistance / g.distribution);
}

float extinction_at(const Medium& m, float z) {
    const float e = m.baseDensity * std::exp(std::min(-(z - m.baseHeight) * m.heightFalloff, 80.f));
    return std::clamp(e, 0.f, m.maxExtinction);
}

float henyey_greenstein(float c, float g) {
    const float gg = g * g;
    const float denom = std::max(1.f + gg - 2.f * g * c, 1e-6f);
    return (1.f - gg) / (4.f * pi * denom * std::sqrt(denom));
}

Slab integrate_slab(const float source[3], float sigmaT, float ds) {
    Slab s;
    s.transmittance = std::exp(-sigmaT * ds);
    // (1 - T) / sigmaT without cancellation for thin/clear slabs: -expm1(-x) / sigmaT.
    const float weight = sigmaT * ds > 1e-4f ? -std::expm1(-sigmaT * ds) / sigmaT : ds;
    for (int i = 0; i < 3; ++i) s.scattered[i] = source[i] * weight;
    return s;
}

void accumulate(Accumulated& a, const Slab& s) {
    for (int i = 0; i < 3; ++i) a.scatter[i] += a.transmittance * s.scattered[i];
    a.transmittance *= s.transmittance;
}

void composite(const float scene[3], const Accumulated& a, float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = scene[i] * a.transmittance + a.scatter[i];
}

float smoke_threshold(float amount) { return 1.2f * amount - .2f; }
float smoke_mean(float t) {
    double sum = 0;
    const int n = 2400;
    const double lo = -6, step = 12.0 / n;
    for (int i = 0; i <= n; ++i) {
        const double z = lo + i * step, x = std::clamp((z - (t - .7)) / 1.4, 0.0, 1.0);
        sum += (i == 0 || i == n ? .5 : 1.0) * x * x * (3 - 2 * x) * std::exp(-.5 * z * z);
    }
    return float(sum * step / std::sqrt(2 * M_PI));
}

bool froxel_uvw(const Grid& g, const FrustumCamera& c, const float o[3], float uvw[3]) {
    auto dot = [&](const float a[3]) { return a[0] * o[0] + a[1] * o[1] + a[2] * o[2]; };
    const float x = dot(c.right), y = dot(c.up), z = dot(c.forward);
    uvw[0] = uvw[1] = uvw[2] = 0;
    if (z <= 1e-4f) return false;
    uvw[0] = x * c.p00 / z * .5f + .5f;
    uvw[1] = y * c.p11 / z * .5f + .5f;
    uvw[2] = slice_of_depth(g, z);
    for (int i = 0; i < 3; ++i) if (!(uvw[i] >= 0.f && uvw[i] <= 1.f)) return false;
    return true;
}

void froxel_offset(const Grid& g, const FrustumCamera& c, const float uvw[3], float o[3]) {
    const float z = depth_of_slice(g, uvw[2]);
    const float x = (uvw[0] * 2.f - 1.f) / c.p00 * z, y = (uvw[1] * 2.f - 1.f) / c.p11 * z;
    for (int i = 0; i < 3; ++i) o[i] = c.right[i] * x + c.up[i] * y + c.forward[i] * z;
}

namespace {
float row_dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float row_length(const float* a) { return std::sqrt(row_dot(a, a)); }
}
const char* cascade_rows_problem(const float rows[12][4]) {
    const float* depth = rows[2];
    const float dl = row_length(depth);
    if (!(dl > 1e-9f)) return "no depth row";
    for (int k = 0; k < 4; ++k) {
        const float *u = rows[3 * k], *v = rows[3 * k + 1], *d = rows[3 * k + 2];
        for (int i = 0; i < 3; ++i) if (std::fabs(d[i] - depth[i]) > 1e-3f * dl) return "the cascades' depth rows differ";
        const float ul = row_length(u), vl = row_length(v);
        if (!(ul > 1e-9f) || !(vl > 1e-9f)) return "an empty u / v row";
        if (std::fabs(row_dot(u, d)) > .05f * ul * dl || std::fabs(row_dot(v, d)) > .05f * vl * dl || std::fabs(row_dot(u, v)) > .05f * ul * vl)
            return "u / v rows not across the light";
    }
    return nullptr;
}
void cascade_order(const float rows[12][4], int order[4]) {
    for (int k = 0; k < 4; ++k) order[k] = k;
    std::sort(order, order + 4, [&](int a, int b) { return row_length(rows[3 * a]) > row_length(rows[3 * b]); });
}
bool cascade_light_view(const float rows[12][4], float toward[3]) {
    const float l = row_length(rows[2]);
    if (!(l > 0)) return false;
    for (int i = 0; i < 3; ++i) toward[i] = -rows[2][i] / l;
    return true;
}
int cascade_lookup(const float rows[12][4], const float view[3], float margin, float xyd[3]) {
    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i < 3; ++i) xyd[i] = row_dot(rows[3 * k + i], view) + rows[3 * k + i][3];
        if (std::fabs(xyd[0]) <= 1 - margin && std::fabs(xyd[1]) <= 1 - margin) return k;
    }
    return -1;
}

float sun_phase(float cosTheta, float g, float aureole, float aureoleG) {
    const float base = henyey_greenstein(cosTheta, g);
    return aureole > 0 ? base + (henyey_greenstein(cosTheta, aureoleG) - base) * aureole : base;
}

float glare_psf(float d) {
    const float t = std::clamp((d - 12.f) / 8.f, 0.f, 1.f), fade = 1 - t * t * (3 - 2 * t);   // smoothstep(12, 20)
    return (.6f / (1 + (d / 2.5f) * (d / 2.5f)) + .4f / (1 + (d / 8) * (d / 8))) * fade;
}
float glare_amount(float strength, float visible, float degrees) {
    return std::clamp(strength * std::cbrt(std::clamp(visible, 0.f, 1.f)) * glare_psf(degrees), 0.f, 1.f);
}

float soft_limit(float peak, float ceiling) {
    if (ceiling <= 0 || peak <= .5f * ceiling) return peak;
    const float knee = .5f * ceiling, room = ceiling - knee;
    return knee + room * (1.f - std::exp(-(peak - knee) / room));
}

bool exposure_key(const uint32_t* bins, uint32_t count, float lowestLog2, float binLog2, float lowShare, float highShare, float& key) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += bins[i];
    if (!total) return false;
    const double lo = double(lowShare) * total, hi = double(highShare) * total;
    double below = 0, weight = 0, sum = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const double a = std::max(below, lo), b = std::min(below + bins[i], hi);
        if (b > a) { weight += b - a; sum += (b - a) * (lowestLog2 + (i + .5) * binLog2); }
        below += bins[i];
    }
    if (weight <= 0) return false;
    key = float(std::exp2(sum / weight));
    return true;
}

float exposure_for(float key, float target, float darkest) {
    if (key <= 0) return 1.f;
    return std::clamp(target / key, std::min(darkest, 1.f), 1.f);
}

float motion_history_weight(float still, float moving, float turnDegrees, float stepYards) {
    const float motion = std::clamp(std::max(turnDegrees / 1.f, stepYards / .5f), 0.f, 1.f);
    return still - (still - std::min(moving, still)) * motion;
}

bool ground_level(const uint32_t* bins, uint32_t count, float lowest, float binSize, float percentile, uint32_t minCells, float& level) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += bins[i];
    if (total < minCells || total == 0) return false;
    const double target = std::clamp(double(percentile), 0.0, 1.0) * double(total);
    double below = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!bins[i]) continue;
        if (below + bins[i] >= target) {
            level = lowest + (float(i) + float((target - below) / bins[i])) * binSize;
            return true;
        }
        below += bins[i];
    }
    level = lowest + float(count) * binSize;
    return true;
}

const char* disc_cluster_problem(const DiscCluster& c) {
    // The disc covers ~100-250 texels of a 480x270 picture (1080p); specks of a few texels are glints. Relative to the picture.
    if (c.count < std::max<uint32_t>(6, c.width * c.height / 10000)) return "small";
    if (uint64_t(c.count) * 2 > c.sky) return "whole sky";                 // a bright sky, not a disc in it
    if (c.spreadX > 6 || c.spreadY > 6) return "spread";                   // haze / clouds: a disc is ~10 texels across
    if (c.spreadX > 2.5 * c.spreadY + 1 || c.spreadY > 2.5 * c.spreadX + 1) return "streak";
    // Two standard deviations reach the disc's rim (uniform disc: sigma = radius / 2): the rim must be inside the picture. A
    // disc cut by the edge has its centroid pulled inward and a rim estimate that crosses the edge.
    const double rx = 2 * c.spreadX, ry = 2 * c.spreadY;
    if (c.meanX - rx < 0 || c.meanY - ry < 0 || c.meanX + rx > double(c.width) - 1 || c.meanY + ry > double(c.height) - 1) return "edge";
    return nullptr;
}

void disc_onto_arc(const float a[2], double d[3]) {
    const double l = std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1]);
    if (l < 1e-6) return;
    const double z = std::clamp(d[2], -1.0, 1.0), h = std::sqrt(std::max(0.0, 1 - z * z));
    d[0] = h * a[0] / l; d[1] = h * a[1] / l; d[2] = z;
}

bool disc_whole(uint32_t count, double radiusDegrees, double p00, uint32_t pictureWidth, double cosFromForward) {
    const double r = std::tan(radiusDegrees * M_PI / 180) * p00 * pictureWidth * .5;
    const double c = std::clamp(cosFromForward, .2, 1.0);
    return count >= wholeDiscShare * M_PI * r * r / (c * c * c);
}

DiscTrack::Step DiscTrack::add(const double d[3], bool whole) {
    auto cosOf = [](double degrees) { return std::cos(degrees * M_PI / 180); };
    auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto blend = [](double* into, const double* with, double keepShare) {
        double l = 0;
        for (int i = 0; i < 3; ++i) { into[i] = keepShare * into[i] + (1 - keepShare) * with[i]; l += into[i] * into[i]; }
        l = std::sqrt(l);
        for (int i = 0; i < 3; ++i) into[i] /= l;
    };
    if (!whole && valid) return dot(d, dir) >= cosOf(smoothDegrees) ? Confirmed : Partial;   // with no fog sun yet: a candidate as any
    if (valid && dot(d, dir) >= cosOf(smoothDegrees)) {
        blend(dir, d, keep);
        hits = 0;
        return Smoothed;
    }
    if (hits > 0 && dot(d, candidate) >= cosOf(confirmDegrees)) {
        blend(candidate, d, double(hits) / (hits + 1));   // running mean of the sightings
        ++hits;
    } else {
        std::copy_n(d, 3, candidate);
        hits = 1;
    }
    if (hits < confirmHits) return Waiting;
    std::copy_n(candidate, 3, dir);
    valid = true;
    hits = 0;
    return Moved;
}
}
