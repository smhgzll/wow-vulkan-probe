#include "memory_camera.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

namespace depth_probe {
namespace {
constexpr float degrees = 180.f / 3.14159265358979323846f;
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3& a) { const float l = length(a); return l > 1e-8f ? scale(a, 1.f / l) : Vec3{}; }
bool finite(const Vec3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
float component(const Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }
float angle_deg(float cosine) { return std::acos(std::clamp(cosine, -1.f, 1.f)) * degrees; }
Vec3 pick(const Vec3 in[3], int8_t signedIndex) {
    const Vec3& v = in[std::abs(signedIndex) - 1];
    return signedIndex < 0 ? scale(v, -1.f) : v;
}
float read_f32(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }

uint64_t env_u64(const char* name, uint64_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const auto parsed = std::strtoull(v, &end, 0);
    return end != v ? parsed : fallback;
}
}

Vec3 apply(const SignedPermutation& p, const Vec3& v) {
    auto out = [&](int i) { const float c = component(v, std::abs(p.m[i]) - 1); return p.m[i] < 0 ? -c : c; };
    return {out(0), out(1), out(2)};
}

const std::vector<SignedPermutation>& signed_permutations() {
    static const std::vector<SignedPermutation> all = [] {
        std::vector<SignedPermutation> list;
        const int perms[6][3]{{1, 2, 3}, {1, 3, 2}, {2, 1, 3}, {2, 3, 1}, {3, 1, 2}, {3, 2, 1}};
        for (auto& p : perms)
            for (int signs = 0; signs < 8; ++signs) {
                SignedPermutation s;
                for (int i = 0; i < 3; ++i) s.m[i] = int8_t((signs >> i) & 1 ? -p[i] : p[i]);
                list.push_back(s);
            }
        return list;
    }();
    return all;
}

std::string describe_world(const SignedPermutation& p) {
    std::string out;
    for (int i = 0; i < 3; ++i) { out += p.m[i] < 0 ? '-' : '+'; out += char('X' + std::abs(p.m[i]) - 1); }
    return out;
}

std::string describe_rows(const SignedPermutation& p) {
    const char* names[3]{"F", "U", "R"};
    std::string out;
    for (int i = 0; i < 3; ++i) {
        if (i) out += ' ';
        out += names[i];
        out += p.m[i] < 0 ? "-R" : "+R";
        out += char('0' + std::abs(p.m[i]) - 1);
    }
    return out;
}

bool parse_signed_permutation(const std::string& text, SignedPermutation& out) {
    std::istringstream in(text);
    SignedPermutation p;
    bool used[3]{};
    for (int i = 0; i < 3; ++i) {
        int v = 0;
        if (!(in >> v) || v == 0 || std::abs(v) > 3 || used[std::abs(v) - 1]) return false;
        used[std::abs(v) - 1] = true;
        p.m[i] = int8_t(v);
        if (i < 2 && !(in >> std::ws && in.get() == ',')) return false;
    }
    out = p;
    return true;
}

uint32_t WowCameraLayout::span() const {
    return std::max({position + 12, facing + 36, nearClip + 4, farClip + 4, fov + 4, aspect + 4});
}

WowCameraLayout wow_camera_layout_from_env() {
    WowCameraLayout l;
    l.staticPointer = env_u64("DEPTH_PROBE_MEM_CAMERA_STATIC", l.staticPointer);
    l.cameraOffset = uint32_t(env_u64("DEPTH_PROBE_MEM_CAMERA_OFFSET", l.cameraOffset));
    l.directAddress = env_u64("DEPTH_PROBE_MEM_CAMERA_ADDR", 0);
    if (const char* v = std::getenv("DEPTH_PROBE_MEM_CAMERA_FIELDS"); v && *v) {
        uint32_t f[6];
        const char* p = v;
        int n = 0;
        for (; n < 6 && *p; ++n) {
            char* end = nullptr;
            f[n] = uint32_t(std::strtoul(p, &end, 0));
            if (end == p || f[n] > 0x1000) break;
            p = *end == ',' ? end + 1 : end;
        }
        if (n == 6) { l.position = f[0]; l.facing = f[1]; l.nearClip = f[2]; l.farClip = f[3]; l.fov = f[4]; l.aspect = f[5]; }
    }
    return l;
}

RawMemoryCamera decode_wow_camera(const uint8_t* s, const WowCameraLayout& l, uint64_t address) {
    RawMemoryCamera c;
    c.readOk = true;
    c.address = address;
    c.position = {read_f32(s + l.position), read_f32(s + l.position + 4), read_f32(s + l.position + 8)};
    for (int r = 0; r < 3; ++r)
        c.rows[r] = {read_f32(s + l.facing + r * 12), read_f32(s + l.facing + r * 12 + 4), read_f32(s + l.facing + r * 12 + 8)};
    c.nearClip = read_f32(s + l.nearClip);
    c.farClip = read_f32(s + l.farClip);
    c.fov = read_f32(s + l.fov);
    c.aspect = read_f32(s + l.aspect);
    return c;
}

RawMemoryCamera read_wow_camera(const MemoryReader& memory, const WowCameraLayout& l) {
    RawMemoryCamera failed;
    uint64_t base = l.directAddress;
    if (!base) {
        uint32_t worldFrame = 0, camera = 0;
        if (!memory.read_u32(l.staticPointer, worldFrame)) { failed.failure = "static pointer unreadable"; return failed; }
        failed.chainValue = worldFrame;
        if (!worldFrame) { failed.failure = "world frame null - loading or login"; return failed; }
        if (!memory.read_u32(uint64_t(worldFrame) + l.cameraOffset, camera)) { failed.failure = "camera pointer unreadable"; return failed; }
        if (!camera) { failed.failure = "camera pointer null"; return failed; }
        base = camera;
    }
    std::vector<uint8_t> bytes(l.span());
    if (!memory.read(base, bytes.data(), bytes.size())) { failed.address = base; failed.failure = "camera struct unreadable"; return failed; }
    auto c = decode_wow_camera(bytes.data(), l, base);
    c.chainValue = failed.chainValue;
    return c;
}

std::string memory_camera_sanity(const RawMemoryCamera& c) {
    if (!c.readOk) return c.failure.empty() ? "not read" : c.failure;
    if (!finite(c.position) || !finite(c.rows[0]) || !finite(c.rows[1]) || !finite(c.rows[2]) ||
        !std::isfinite(c.nearClip) || !std::isfinite(c.farClip) || !std::isfinite(c.fov) || !std::isfinite(c.aspect))
        return "non-finite field";
    if (std::fabs(c.position.x) > 50000 || std::fabs(c.position.y) > 50000 || std::fabs(c.position.z) > 50000) return "position out of world range";
    for (int i = 0; i < 3; ++i)
        if (std::fabs(length(c.rows[i]) - 1.f) > .02f) return "facing row " + std::to_string(i) + " not unit length";
    if (std::fabs(dot(c.rows[0], c.rows[1])) > .02f || std::fabs(dot(c.rows[0], c.rows[2])) > .02f || std::fabs(dot(c.rows[1], c.rows[2])) > .02f)
        return "facing rows not orthogonal";
    if (!(c.nearClip > .001f && c.nearClip < 50.f)) return "near clip implausible";
    if (!(c.farClip > c.nearClip * 2.f && c.farClip < 100000.f)) return "far clip implausible";
    if (!(c.fov > .1f && c.fov < 3.1f)) return "fov implausible - radians expected";
    if (!(c.aspect > .25f && c.aspect < 8.f)) return "aspect implausible";
    return {};
}

Mat4Cols synthesize_projection(float fov, FovAxis axis, float aspect, float n, float f) {
    const float tanHalfY = axis == FovAxis::Vertical ? std::tan(fov * .5f) : std::tan(fov * .5f) / aspect;
    const float sy = 1.f / tanHalfY, sx = sy / aspect;
    const float zz = f / (f - n);
    return {{sx, 0, 0, 0}, {0, sy, 0, 0}, {0, 0, zz, 1}, {0, 0, -n * zz, 0}};
}

DerivedCamera memory_camera_to_derived(const RawMemoryCamera& c, const SignedPermutation& rows, const Mat4Cols& projection) {
    DerivedCamera bad;
    bad.source = "wow-memory-cgcamera";
    if (const auto reason = memory_camera_sanity(c); !reason.empty()) { bad.invalidReason = reason; return bad; }
    const Vec3 forward = normalized(pick(c.rows, rows.m[0]));
    const Vec3 up = normalized(sub(pick(c.rows, rows.m[1]), scale(forward, dot(pick(c.rows, rows.m[1]), forward))));
    Vec3 right = pick(c.rows, rows.m[2]);
    right = normalized(sub(sub(right, scale(forward, dot(right, forward))), scale(up, dot(right, up))));
    const Vec3& p = c.position;
    const float viewRaw[16]{
        right.x, up.x, forward.x, 0,
        right.y, up.y, forward.y, 0,
        right.z, up.z, forward.z, 0,
        -dot(right, p), -dot(up, p), -dot(forward, p), 1,
    };
    const Mat4Cols& q = projection;
    const float projRaw[16]{q.c0.x, q.c0.y, q.c0.z, q.c0.w, q.c1.x, q.c1.y, q.c1.z, q.c1.w,
                            q.c2.x, q.c2.y, q.c2.z, q.c2.w, q.c3.x, q.c3.y, q.c3.z, q.c3.w};
    auto out = derive_known_wow_camera(projRaw, viewRaw);
    out.source = "wow-memory-cgcamera";
    return out;
}

CameraPairResiduals pair_residuals(const RawMemoryCamera& m, const DerivedCamera& gpu, const SignedPermutation& world, const SignedPermutation& rows) {
    CameraPairResiduals r;
    if (!m.readOk || !gpu.valid) return r;
    r.position = length(sub(apply(world, m.position), gpu.position));
    r.forwardDeg = angle_deg(dot(normalized(apply(world, pick(m.rows, rows.m[0]))), gpu.forward));
    r.upDeg = angle_deg(dot(normalized(apply(world, pick(m.rows, rows.m[1]))), gpu.up));
    r.rightDeg = angle_deg(dot(normalized(apply(world, pick(m.rows, rows.m[2]))), gpu.right));
    return r;
}

void CameraSourceComparator::reset() { scores.clear(); weight = 0; current = {}; }

void CameraSourceComparator::add(const RawMemoryCamera& m, const DerivedCamera& gpu) {
    if (!m.readOk || !gpu.valid || !memory_camera_sanity(m).empty()) return;
    const auto& perms = signed_permutations();
    const size_t n = perms.size();
    constexpr float decay = .97f;
    if (scores.size() != n * n) scores.assign(n * n, 0.f);
    for (auto& s : scores) s *= decay;
    weight = weight * decay + 1.f;

    const Vec3 gpuAxes[3]{gpu.forward, gpu.up, gpu.right};
    for (size_t w = 0; w < n; ++w) {
        Vec3 tr[3];
        for (int j = 0; j < 3; ++j) tr[j] = normalized(apply(perms[w], m.rows[j]));
        // angle[axis][row][sign]: every row assignment below only indexes into this.
        float angle[3][3][2];
        for (int a = 0; a < 3; ++a)
            for (int j = 0; j < 3; ++j) {
                const float d = dot(tr[j], gpuAxes[a]);
                angle[a][j][0] = angle_deg(d);
                angle[a][j][1] = angle_deg(-d);
            }
        const float position = std::min(length(sub(apply(perms[w], m.position), gpu.position)), 100.f);
        for (size_t q = 0; q < n; ++q) {
            float s = position;
            for (int a = 0; a < 3; ++a) s += angle[a][std::abs(perms[q].m[a]) - 1][perms[q].m[a] < 0];
            scores[w * n + q] += s;
        }
    }
    size_t best = 0, runnerUp = SIZE_MAX;
    for (size_t i = 1; i < scores.size(); ++i) if (scores[i] < scores[best]) best = i;
    for (size_t i = 0; i < scores.size(); ++i) if (i != best && (runnerUp == SIZE_MAX || scores[i] < scores[runnerUp])) runnerUp = i;
    current.have = true;
    current.world = perms[best / n];
    current.rows = perms[best % n];
    current.meanScore = scores[best] / weight;
    current.runnerUpScore = scores[runnerUp] / weight;
    ++current.samples;
    current.latest = pair_residuals(m, gpu, current.world, current.rows);
    current.stable = current.samples >= 20 && current.meanScore < 6.f && current.runnerUpScore > current.meanScore + 30.f;
}

void scan_camera_structs(const uint8_t* bytes, size_t size, uint64_t base, const WowCameraLayout& l, std::vector<CameraScanHit>& out, size_t maxHits) {
    const size_t span = l.span();
    if (size < span) return;
    for (size_t o = 0; o + span <= size && out.size() < maxHits; o += 4) {
        const float fov = read_f32(bytes + o + l.fov);
        if (!(fov > .1f && fov < 3.1f)) continue;
        const float nearClip = read_f32(bytes + o + l.nearClip);
        if (!(nearClip > .001f && nearClip < 50.f)) continue;
        auto c = decode_wow_camera(bytes + o, l, base + o);
        if (!memory_camera_sanity(c).empty()) continue;
        // Identity facing at the origin is a default-initialized struct, not a live camera.
        if (c.position.x == 0 && c.position.y == 0 && c.position.z == 0 && c.rows[0].x == 1 && c.rows[1].y == 1 && c.rows[2].z == 1) continue;
        out.push_back({base + o, c});
    }
}

void find_pointers_to(const uint8_t* bytes, size_t size, uint64_t base, uint32_t target, uint32_t maxBelow,
                      std::vector<uint64_t>& addresses, std::vector<uint32_t>& offsets, size_t maxHits) {
    for (size_t o = 0; o + 4 <= size && addresses.size() < maxHits; o += 4) {
        uint32_t v;
        std::memcpy(&v, bytes + o, 4);
        if (v <= target && target - v <= maxBelow) { addresses.push_back(base + o); offsets.push_back(target - v); }
    }
}
}
