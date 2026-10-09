#include "camera_math.hpp"
#include "matrix_analyzer.hpp"
#include <cmath>
#include <initializer_list>

namespace depth_probe {
namespace {
constexpr float pi = 3.14159265358979323846f;
float sumsq(std::initializer_list<float> vs) { float s = 0; for (float v : vs) s += v * v; return s; }
bool finite4(const Vec4& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }
float dot3(const Vec4& a, const Vec4& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(const Vec4& v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-8f) return {0, 0, 0};
    return {v.x / len, v.y / len, v.z / len};
}

// Shared math for both the verified-water and generic derivation paths,
// once `p`/`m` are already known (or assumed) to be in the water-shader's
// column convention. `structuralCheck` runs the same validity gate
// shaders/water_camera.frag applies before it trusts p/m; skipping it for
// the generic path is intentional - an unverified layout can't be held to
// "matches the exact known WoW structure", only to "produces a sane camera".
DerivedCamera compute(const Mat4Cols& p, const Mat4Cols& m, const std::string& source, bool structuralCheck) {
    DerivedCamera out;
    out.source = source;
    out.projection = p;
    out.view = m;
    if (!finite4(p.c0) || !finite4(p.c1) || !finite4(p.c2) || !finite4(p.c3) || !finite4(m.c0) || !finite4(m.c1) || !finite4(m.c2) || !finite4(m.c3)) {
        out.invalidReason = "non-finite matrix component";
        return out;
    }
    if (structuralCheck) {
        // Mirrors shaders/water_camera.frag's own validity gate (slightly
        // relaxed epsilons: this runs on the same GPU-produced floats but
        // through a separate CPU path, so exact GLSL-epsilon parity isn't
        // required, only "clearly the same structure").
        bool ok = p.c0.x > 0 && p.c1.y > 0 && p.c2.z > 1 && p.c3.z < 0 && std::fabs(p.c2.w - 1) < 1e-3f;
        ok = ok && std::sqrt(sumsq({p.c0.y, p.c0.z, p.c0.w})) < 1e-4f && std::sqrt(sumsq({p.c1.x, p.c1.z, p.c1.w})) < 1e-4f
                && std::sqrt(sumsq({p.c2.x, p.c2.y})) < 1e-4f && std::sqrt(sumsq({p.c3.x, p.c3.y, p.c3.w})) < 1e-4f;
        if (!ok) { out.invalidReason = "projection does not match the verified WoW structure"; return out; }
        ok = std::sqrt(sumsq({m.c0.w, m.c1.w, m.c2.w})) < 1e-4f && std::fabs(m.c3.w - 1) < 1e-3f;
        const float g00 = dot3(m.c0, m.c0), g11 = dot3(m.c1, m.c1), g22 = dot3(m.c2, m.c2);
        const float g01 = dot3(m.c0, m.c1), g02 = dot3(m.c0, m.c2), g12 = dot3(m.c1, m.c2);
        ok = ok && std::fabs(g00 - 1) < 1e-3f && std::fabs(g11 - 1) < 1e-3f && std::fabs(g22 - 1) < 1e-3f
                && std::fabs(g01) < 1e-3f && std::fabs(g02) < 1e-3f && std::fabs(g12) < 1e-3f;
        if (!ok) { out.invalidReason = "view does not match the verified WoW orthonormal/affine structure"; return out; }
    }
    if (!mat4_inverse(p, out.inverseProjection)) { out.invalidReason = "projection not invertible"; return out; }
    if (!mat4_inverse(m, out.inverseView)) { out.invalidReason = "view not invertible"; return out; }
    const auto& invM = out.inverseView;
    if (std::fabs(invM.c3.w) < 1e-8f) { out.invalidReason = "inverse view w ~ 0"; return out; }
    out.position = {invM.c3.x / invM.c3.w, invM.c3.y / invM.c3.w, invM.c3.z / invM.c3.w};
    // Basis vectors: this is NOT taken from the shader (it only ever needs
    // position) - it is the natural extension of the same inverse-view
    // technique, treating the inverse view's first three columns as the
    // view-space X/Y/Z axes expressed in world space.
    out.right = normalize(invM.c0);
    out.up = normalize(invM.c1);
    out.forward = normalize(invM.c2);
    out.nearPlane = -p.c3.z / p.c2.z;
    out.farPlane = -p.c3.z / (p.c2.z - 1.f);
    out.fovY = 2.f * std::atan(1.f / p.c1.y) * (180.f / pi);
    out.aspect = p.c0.x != 0.f ? p.c1.y / p.c0.x : 0.f;
    if (!std::isfinite(out.nearPlane) || !std::isfinite(out.farPlane) || out.nearPlane <= 0 || out.farPlane <= out.nearPlane ||
        !std::isfinite(out.fovY) || out.fovY <= 0 || out.fovY >= 180 || !std::isfinite(out.aspect) || out.aspect <= 0) {
        out.invalidReason = "derived near/far/FOV/aspect not sane";
        return out;
    }
    out.valid = true;
    return out;
}
}

Mat4Cols read_columns(const float raw[16]) {
    return Mat4Cols{{raw[0], raw[1], raw[2], raw[3]}, {raw[4], raw[5], raw[6], raw[7]}, {raw[8], raw[9], raw[10], raw[11]}, {raw[12], raw[13], raw[14], raw[15]}};
}

Vec4 mat4_mul_vec4(const Mat4Cols& m, const Vec4& v) {
    return {
        m.c0.x * v.x + m.c1.x * v.y + m.c2.x * v.z + m.c3.x * v.w,
        m.c0.y * v.x + m.c1.y * v.y + m.c2.y * v.z + m.c3.y * v.w,
        m.c0.z * v.x + m.c1.z * v.y + m.c2.z * v.z + m.c3.z * v.w,
        m.c0.w * v.x + m.c1.w * v.y + m.c2.w * v.z + m.c3.w * v.w,
    };
}

Mat4Cols mat4_mul(const Mat4Cols& a, const Mat4Cols& b) {
    return {mat4_mul_vec4(a, b.c0), mat4_mul_vec4(a, b.c1), mat4_mul_vec4(a, b.c2), mat4_mul_vec4(a, b.c3)};
}

bool mat4_inverse(const Mat4Cols& m, Mat4Cols& out) {
    const Vec4* cols[4] = {&m.c0, &m.c1, &m.c2, &m.c3};
    auto component = [](const Vec4& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; };
    float rowMajor[16];
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) rowMajor[row * 4 + col] = component(*cols[col], row);
    float inv[16];
    if (!invert4x4(rowMajor, inv)) return false;
    auto column = [&](int j) { return Vec4{inv[0 * 4 + j], inv[1 * 4 + j], inv[2 * 4 + j], inv[3 * 4 + j]}; };
    out.c0 = column(0); out.c1 = column(1); out.c2 = column(2); out.c3 = column(3);
    return true;
}

DerivedCamera derive_known_wow_camera(const float projRaw[16], const float viewRaw[16]) {
    return compute(read_columns(projRaw), read_columns(viewRaw), "known-wow-water-anchor", true);
}

DerivedCamera derive_generic_camera(const float projRaw[16], const float viewRaw[16], bool transposed, const std::string& source) {
    if (!transposed) return compute(read_columns(projRaw), read_columns(viewRaw), source, false);
    float pt[16], vt[16];
    transpose4x4(projRaw, pt);
    transpose4x4(viewRaw, vt);
    return compute(read_columns(pt), read_columns(vt), source, false);
}

CameraComparison compare_camera(const DerivedCamera& a, const DerivedCamera& b) {
    CameraComparison out;
    if (!a.valid || !b.valid) return out;
    const float dx = a.position.x - b.position.x, dy = a.position.y - b.position.y, dz = a.position.z - b.position.z;
    out.positionError = std::sqrt(dx * dx + dy * dy + dz * dz);
    out.orientationDot = a.forward.x * b.forward.x + a.forward.y * b.forward.y + a.forward.z * b.forward.z;
    out.fovDeltaDegrees = std::fabs(a.fovY - b.fovY);
    return out;
}

float mat4_determinant(const Mat4Cols& m) {
    const Vec4* cols[4] = {&m.c0, &m.c1, &m.c2, &m.c3};
    auto component = [](const Vec4& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; };
    float a[4][4];
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) a[row][col] = component(*cols[col], row);
    auto det3 = [](float a1, float a2, float a3, float b1, float b2, float b3, float c1, float c2, float c3) {
        return a1 * (b2 * c3 - b3 * c2) - a2 * (b1 * c3 - b3 * c1) + a3 * (b1 * c2 - b2 * c1);
    };
    return a[0][0] * det3(a[1][1], a[1][2], a[1][3], a[2][1], a[2][2], a[2][3], a[3][1], a[3][2], a[3][3])
         - a[0][1] * det3(a[1][0], a[1][2], a[1][3], a[2][0], a[2][2], a[2][3], a[3][0], a[3][2], a[3][3])
         + a[0][2] * det3(a[1][0], a[1][1], a[1][3], a[2][0], a[2][1], a[2][3], a[3][0], a[3][1], a[3][3])
         - a[0][3] * det3(a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2], a[3][0], a[3][1], a[3][2]);
}

BasisHealth analyze_view_basis(const Mat4Cols& view) {
    BasisHealth h;
    h.determinant = mat4_determinant(view);
    h.rightLen = std::sqrt(dot3(view.c0, view.c0));
    h.upLen = std::sqrt(dot3(view.c1, view.c1));
    h.forwardLen = std::sqrt(dot3(view.c2, view.c2));
    h.rightDotUp = dot3(view.c0, view.c1);
    h.rightDotForward = dot3(view.c0, view.c2);
    h.upDotForward = dot3(view.c1, view.c2);
    h.orthonormal = std::fabs(h.rightLen - 1.f) < .01f && std::fabs(h.upLen - 1.f) < .01f && std::fabs(h.forwardLen - 1.f) < .01f &&
                     std::fabs(h.rightDotUp) < .01f && std::fabs(h.rightDotForward) < .01f && std::fabs(h.upDotForward) < .01f;
    return h;
}
}
