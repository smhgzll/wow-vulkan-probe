#include "game_lights_capture.hpp"
#include "game_fog_capture.hpp"
#include "game_soft_shadows.hpp"
#include "scene_debugger.hpp"
#include "surface_inspector.hpp" // Inspector::page - scene_debugger_active() needs the full type
#include "water_reflections.hpp" // wowWaterVS/wowWaterFS - the one independently-verified camera layout to anchor on
#include "camera_hud_input.hpp"
#include "overlay_uniform.hpp"
#include "layer_state.hpp" // log_line
#include "volumetric_fog.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace depth_probe::discovery {
bool scene_debugger_frame_reconstruction(Device&, ReconstructionSetup&, std::string&);   // below: the fog's frame camera
namespace {
constexpr uint32_t maxSamplesPerShaderPerFrame = 4; // within-frame cross-draw evidence needs more than one sample
constexpr size_t maxSceneDebuggerBytesPerFrame = 2 * 1024 * 1024; // reset every frame, not cumulative
constexpr size_t maxWaterGpuReadbackBudget = 4096; // one 896-byte read at a time, generous headroom
constexpr uint64_t waterAnchorRecheckInterval = 120; // frames between re-validation once already verified

// Shared by the host-mapped path (capture_scene_debugger_uniforms) and the
// GPU-readback path (scene_debugger_poll): once >= 400 bytes starting at the
// water UBO's effective offset are in hand, slice out P (+256) and V (+336)
// and derive via the verified convention - identical regardless of which
// capture path produced the bytes.
void process_water_bytes(WaterCameraAnchor& anchor, const std::vector<uint8_t>& bytes) {
    anchor.projAvailable = bytes.size() >= 320;
    anchor.viewAvailable = bytes.size() >= 400;
    anchor.bytesCaptured = bytes.size();
    if (!anchor.viewAvailable) {
        anchor.cameraMathStatus = "failed: buffer too small (" + std::to_string(bytes.size()) + " bytes, need >= 400)";
        anchor.valid = false;
        return;
    }
    float projRaw[16], viewRaw[16];
    std::memcpy(projRaw, bytes.data() + 256, 64);
    std::memcpy(viewRaw, bytes.data() + 336, 64);
    anchor.camera = derive_known_wow_camera(projRaw, viewRaw);
    anchor.valid = anchor.camera.valid;
    anchor.cameraMathStatus = anchor.valid ? "ok" : ("failed: " + anchor.camera.invalidReason);
}

// Publishes the repository's verified water P/V layout directly into the
// ranked list via publish_verified_camera - NOT by looking for an existing
// tracked-bindings-derived entry to tag. Round-2 did the latter
// (apply_water_anchor only modified a candidate if one already existed) and
// that was the bug found in real WoW: the water UBO is only reachable via
// the GPU-readback path (process_water_bytes, called from
// scene_debugger_poll), which never touches update_tracked_binding/
// recompute_camera_candidates at all, so no candidate ever existed to tag.
// A successful derivation IS the evidence here; it does not need frame
// history or draw counts on top of it.
void publish_water_anchor(SceneDebugger& sd) {
    if (!sd.waterAnchor.valid) return;
    publish_verified_camera(sd.cameras, wowWaterVS, wowWaterFS, sd.waterAnchor.set, sd.waterAnchor.binding, sd.waterAnchor.element, 256, 336, sd.waterAnchor.camera);
}

// ---- Camera Telemetry HUD text + recording (round 4) ----------------------
// Deliberately independent of the F10 menu/Inspector text buffer
// (inspector_ui.cpp's `line()`/`inspectorText`) - see camera_hud_input.hpp
// for why. Writes into OverlayUniform::hudText, a separate packed buffer.
void hud_line(OverlayUniform& u, unsigned row, const std::string& text) {
    if (row >= 24) return;
    std::fill_n(u.hudText + row * 12, 12, 0u);
    for (size_t i = 0; i < std::min(size_t(48), text.size()); ++i) {
        const auto c = uint8_t(std::toupper(static_cast<unsigned char>(text[i])));
        const auto index = row * 48 + i;
        u.hudText[index / 4] |= uint32_t(c) << ((index % 4) * 8);
    }
}

// 3D camera gizmo (F8) offsets/sizes - configurable via env vars (read once,
// cached), per the user's explicit "make the exact offsets configurable"
// request. World units (WoW yards). Defaults place the anchor near a
// third-person character's upper body, not at the camera origin.
struct GizmoConfig { float forwardOffset, upOffset, rightOffset, armLength, headSize, crossSize; };
const GizmoConfig& gizmo_config() {
    static const GizmoConfig config = [] {
        auto env = [](const char* name, float fallback) {
            const char* v = std::getenv(name);
            if (!v || !*v) return fallback;
            char* end = nullptr;
            const float parsed = std::strtof(v, &end);
            return (end && end != v) ? parsed : fallback;
        };
        return GizmoConfig{
            env("DEPTH_PROBE_GIZMO_FORWARD_OFFSET", 6.f), env("DEPTH_PROBE_GIZMO_UP_OFFSET", -2.f), env("DEPTH_PROBE_GIZMO_RIGHT_OFFSET", 0.f),
            env("DEPTH_PROBE_GIZMO_ARM_LENGTH", 2.5f), env("DEPTH_PROBE_GIZMO_HEAD_SIZE", .4f), env("DEPTH_PROBE_GIZMO_CROSS_SIZE", .3f),
        };
    }();
    return config;
}
Vec3 gizmo_anchor(const DerivedCamera& c, const GizmoConfig& cfg) {
    return {
        c.position.x + c.forward.x * cfg.forwardOffset + c.up.x * cfg.upOffset + c.right.x * cfg.rightOffset,
        c.position.y + c.forward.y * cfg.forwardOffset + c.up.y * cfg.upOffset + c.right.y * cfg.rightOffset,
        c.position.z + c.forward.z * cfg.forwardOffset + c.up.z * cfg.upOffset + c.right.z * cfg.rightOffset,
    };
}

// The camera the HUD shows and the gizmo is drawn with, per the Shift+F8 source mode.
struct DisplayCamera { bool have = false; DerivedCamera camera; std::string name; };
DisplayCamera select_display_camera(const SceneDebugger& sd, CameraHudInput::GizmoSource mode, bool haveActive, const DerivedCamera& active) {
    DisplayCamera d;
    const bool water = sd.waterAnchor.valid, memory = sd.memory.valid;
    if (mode == CameraHudInput::GizmoSource::Water) { if (water) d = {true, sd.waterAnchor.camera, "WATER_GPU"}; }
    else if (mode == CameraHudInput::GizmoSource::Memory) { if (memory) d = {true, sd.memory.camera, "MEMORY"}; }
    else if (memory) d = {true, sd.memory.camera, "MEMORY"};
    else if (haveActive) d = {true, active, active.source == "known-wow-water-anchor" ? "WATER_GPU" : "ACTIVE"};
    return d;
}

// Fills the gizmo's uniform fields for shaders/gizmo.vert/.frag. gizmoAnchor[3]
// stays 0 (no draw, see overlay_uniform.cpp) when hidden or without a camera.
// Locked (F7): anchor and axes are the frozen snapshot; the view projection
// still follows the live camera.
void populate_gizmo_uniform(OverlayUniform& u, bool visible, const DisplayCamera& d, const GizmoLock& lock) {
    if (!visible || !d.have) { u.gizmoAnchor[3] = 0.f; return; }
    const auto& cfg = gizmo_config();
    const auto& c = d.camera;
    const Vec3 anchor = lock.active ? lock.anchor : gizmo_anchor(c, cfg);
    const Vec3& right = lock.active ? lock.right : c.right;
    const Vec3& up = lock.active ? lock.up : c.up;
    const Vec3& forward = lock.active ? lock.forward : c.forward;
    u.gizmoAnchor[0] = anchor.x; u.gizmoAnchor[1] = anchor.y; u.gizmoAnchor[2] = anchor.z; u.gizmoAnchor[3] = 1.f;
    u.gizmoRight[0] = right.x; u.gizmoRight[1] = right.y; u.gizmoRight[2] = right.z; u.gizmoRight[3] = cfg.armLength;
    u.gizmoUp[0] = up.x; u.gizmoUp[1] = up.y; u.gizmoUp[2] = up.z; u.gizmoUp[3] = cfg.headSize;
    u.gizmoForward[0] = forward.x; u.gizmoForward[1] = forward.y; u.gizmoForward[2] = forward.z; u.gizmoForward[3] = cfg.crossSize;
    const auto vp = mat4_mul(c.projection, c.view);
    const float cols[16]{vp.c0.x, vp.c0.y, vp.c0.z, vp.c0.w, vp.c1.x, vp.c1.y, vp.c1.z, vp.c1.w,
                          vp.c2.x, vp.c2.y, vp.c2.z, vp.c2.w, vp.c3.x, vp.c3.y, vp.c3.z, vp.c3.w};
    std::copy(std::begin(cols), std::end(cols), u.gizmoVP);
}

// Light gizmo: where the fog's sun, the game's lighting direction and the sky address point (world directions; the vertex
// shader projects them). Needs the display camera's view projection even when the camera gizmo itself is hidden.
void populate_light_gizmo(OverlayUniform& u, const DisplayCamera& d, const discovery::FogState* fog) {
    for (auto& m : u.lightMarkers) std::fill(std::begin(m), std::end(m), 0.f);
    u.lightGizmo[0] = 0.f;
    if (!fog || !fog->settings.lightGizmo || !camera_hud_input().fogMode || !d.have) return;
    const auto vp = mat4_mul(d.camera.projection, d.camera.view);
    const float cols[16]{vp.c0.x, vp.c0.y, vp.c0.z, vp.c0.w, vp.c1.x, vp.c1.y, vp.c1.z, vp.c1.w,
                          vp.c2.x, vp.c2.y, vp.c2.z, vp.c2.w, vp.c3.x, vp.c3.y, vp.c3.z, vp.c3.w};
    std::copy(std::begin(cols), std::end(cols), u.gizmoVP);
    u.lightGizmo[0] = 1.f;
    int n = 0;
    auto add = [&](const float* dir, float kind) { if (n < 4) { std::copy_n(dir, 3, u.lightMarkers[n]); u.lightMarkers[n][3] = kind; ++n; } };
    // The fog's sun last: drawn over the others (it shares the direction of one of them - it hid under it).
    if (fog->gameSun.ok) add(fog->gameSun.toward, 2);
    if (fog->gameSun.skyOk) add(fog->gameSun.skyToward, 3);
    if (fog->seenDiscValid) add(fog->seenDisc, 4);
    if (fog->lastSunOn) add(fog->lastSunToward, 1);
}

// Point lights (fog menu LIGHT SOURCES, docs/lights.md): the sightings of this frame's lit draws go into world space with the
// frame's own camera (the one the fog reconstructs with); the nearest 16 are drawn as circles in their colour.
void populate_point_lights(Device& s, OverlayUniform& u, const DisplayCamera& d, const discovery::FogState* fog) {
    for (auto& p : u.pointLights) std::fill(std::begin(p), std::end(p), 0.f);
    for (auto& p : u.pointLightColors) std::fill(std::begin(p), std::end(p), 0.f);
    u.lightGizmo[1] = 0.f;
    const bool on = fog && fog->settings.lightSources, read = fog && (fog->settings.lightSources || fog->settings.pointShadows > 0);
    lights::Basis basis;
    ReconstructionSetup frameCamera;
    std::string why;
    const bool haveCamera = read && scene_debugger_frame_reconstruction(s, frameCamera, why);
    if (haveCamera) {
        const Vec3 axes[3]{frameCamera.right, frameCamera.up, frameCamera.forward};
        float* into[3]{basis.right, basis.up, basis.forward};
        for (int a = 0; a < 3; ++a) { into[a][0] = axes[a].x; into[a][1] = axes[a].y; into[a][2] = axes[a].z; }
        for (int i = 0; i < 3; ++i) basis.position[i] = float(frameCamera.camera[i]);
    }
    auto list = game_lights_present(read, haveCamera, basis, s.frameSerial);   // (also for the fog's point-light shadows)
    if (!on || !d.have || list.empty()) return;
    const auto& c = d.camera;
    auto dist2 = [&](const lights::Light& l) { const float x = l.pos[0] - c.position.x, y = l.pos[1] - c.position.y, z = l.pos[2] - c.position.z; return x * x + y * y + z * z; };
    std::sort(list.begin(), list.end(), [&](const auto& a, const auto& b) { return dist2(a) < dist2(b); });
    const auto vp = mat4_mul(c.projection, c.view);
    const float cols[16]{vp.c0.x, vp.c0.y, vp.c0.z, vp.c0.w, vp.c1.x, vp.c1.y, vp.c1.z, vp.c1.w,
                          vp.c2.x, vp.c2.y, vp.c2.z, vp.c2.w, vp.c3.x, vp.c3.y, vp.c3.z, vp.c3.w};
    std::copy(std::begin(cols), std::end(cols), u.gizmoVP);
    const size_t n = std::min<size_t>(list.size(), 16);
    for (size_t i = 0; i < n; ++i) {
        std::copy_n(list[i].pos, 3, u.pointLights[i]);
        u.pointLights[i][3] = 1.f;
        const float peak = std::max({list[i].color[0], list[i].color[1], list[i].color[2], 1e-6f});
        for (int k = 0; k < 3; ++k) u.pointLightColors[i][k] = list[i].color[k] / peak;
        u.pointLightColors[i][3] = .6f;
    }
    u.lightCameraRight[0] = c.right.x; u.lightCameraRight[1] = c.right.y; u.lightCameraRight[2] = c.right.z;
    u.lightCameraUp[0] = c.up.x; u.lightCameraUp[1] = c.up.y; u.lightCameraUp[2] = c.up.z;
    u.lightGizmo[1] = float(n);
}

void update_gizmo_lock(GizmoLock& lock, bool wantLocked, const DisplayCamera& d) {
    if (!wantLocked) {
        if (lock.active) log_line("[CAMERA-GIZMO] unlocked");
        lock = {};
        return;
    }
    if (!d.have || (lock.active && lock.source == d.name)) return;
    const auto& c = d.camera;
    lock = {true, d.name, gizmo_anchor(c, gizmo_config()), c.right, c.up, c.forward};
    char buf[160];
    std::snprintf(buf, sizeof buf, "[CAMERA-GIZMO] locked source=%s anchor=%.2f,%.2f,%.2f", d.name.c_str(), double(lock.anchor.x), double(lock.anchor.y), double(lock.anchor.z));
    log_line(buf);
}

// ---- Process-memory camera ------------------------------------------------
constexpr size_t maxScanTracks = 16;
constexpr float matchPosition = 1.f, matchDegrees = 2.f;   // per time-paired sample, under the detected conversion
constexpr uint32_t verifiedSamples = 120;
constexpr float minRecentMatchRate = .6f;   // decayed share of recent pairs that matched (~last 20 pairs)
constexpr float verifiedSpanDegrees = 30.f;
constexpr float radToDeg = 57.2957795f;

float angle_between(const Vec3& a, const Vec3& b) {
    const float la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z), lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    if (la < 1e-8f || lb < 1e-8f) return 180.f;
    return std::acos(std::clamp((a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb), -1.f, 1.f)) * radToDeg;
}
std::string hex32(uint64_t v) { char b[24]; std::snprintf(b, sizeof b, "0x%08llX", (unsigned long long)v); return b; }
const char* level_short(CameraEvidenceLevel l) {
    return l == CameraEvidenceLevel::Verified ? "VERIFIED" : l == CameraEvidenceLevel::MemoryGpuMatch ? "GPU MATCH" : "CANDIDATE";
}

void configure_memory_source(MemoryCameraSource& m) {
    if (m.configured) return;
    m.configured = true;
    if (const char* v = std::getenv("DEPTH_PROBE_MEM_CAMERA"); v && !std::strcmp(v, "0")) m.enabled = false;
    m.layout = wow_camera_layout_from_env();
    if (const char* v = std::getenv("DEPTH_PROBE_MEM_CAMERA_ROWS"); v && *v) {
        if (parse_signed_permutation(v, m.rows)) m.rowsFromEnv = true;
        else log_line(std::string("[MEM-CAMERA] ignoring malformed DEPTH_PROBE_MEM_CAMERA_ROWS=") + v + " (expected e.g. +1,+3,-2)");
    }
    if (const char* v = std::getenv("DEPTH_PROBE_MEM_FOV_SCALE"); v && *v) {
        char* end = nullptr;
        const float scale = std::strtof(v, &end);
        if (end != v && scale > .1f && scale < 3.f) { m.fovScale = scale; m.fovScaleFromEnv = true; }
    }
    if (const char* v = std::getenv("DEPTH_PROBE_MEM_FOV_AXIS"); v && *v) {
        m.fovAxis = !std::strcmp(v, "horizontal") ? FovAxis::Horizontal : FovAxis::Vertical;
        m.fovAxisFromEnv = true;
    }
    const auto& l = m.layout;
    char buf[320];
    std::snprintf(buf, sizeof buf, "[MEM-CAMERA] config enabled=%d static=0x%llX offset=0x%X direct=0x%llX fields=pos+0x%X facing+0x%X near+0x%X far+0x%X fov+0x%X aspect+0x%X rows=%s fovAxis=%s fovScale=%.3f",
                  int(m.enabled), (unsigned long long)l.staticPointer, l.cameraOffset, (unsigned long long)l.directAddress, l.position, l.facing, l.nearClip, l.farClip, l.fov, l.aspect,
                  describe_rows(m.rows).c_str(), m.fovAxis == FovAxis::Vertical ? "vertical" : "horizontal", double(m.fovScale));
    log_line(buf);
}

// The configured chain (or DEPTH_PROBE_MEM_CAMERA_ADDR); when that fails a sanity
// check, the scan hit currently selected as live.
RawMemoryCamera read_memory_camera_raw(const MemoryCameraSource& m, std::string& origin, std::string& chainStatus) {
    const auto& reader = self_memory_reader();
    auto raw = read_wow_camera(reader, m.layout);
    const auto reason = memory_camera_sanity(raw);
    chainStatus = reason.empty() ? "OK" : reason;
    origin = m.layout.directAddress ? "ENV" : "CHAIN";
    if (!reason.empty() && m.scanSelected >= 0 && size_t(m.scanSelected) < m.tracks.size()) {
        WowCameraLayout direct = m.layout;
        direct.directAddress = m.tracks[size_t(m.scanSelected)].address;
        auto scanned = read_wow_camera(reader, direct);
        if (memory_camera_sanity(scanned).empty()) { origin = "SCAN"; return scanned; }
    }
    return raw;
}

bool camera_struct_changed(const RawMemoryCamera& a, const RawMemoryCamera& b) {
    auto diff = [](const Vec3& x, const Vec3& y) { return std::fabs(x.x - y.x) > 1e-4f || std::fabs(x.y - y.y) > 1e-4f || std::fabs(x.z - y.z) > 1e-4f; };
    return diff(a.position, b.position) || diff(a.rows[0], b.rows[0]) || diff(a.rows[1], b.rows[1]) || diff(a.rows[2], b.rows[2]);
}

void log_scan_result(const MemoryCameraScanStatus& st, const WowCameraLayout& l) {
    char buf[320];
    std::snprintf(buf, sizeof buf, "[MEM-CAMERA-SCAN] %s regions=%zu scannedMB=%.1f hits=%zu chains=%zu", st.note.c_str(), st.regions,
                  double(st.bytesScanned) / 1048576.0, st.hits.size(), st.chains.size());
    log_line(buf);
    for (size_t i = 0; i < st.hits.size() && i < 16; ++i) {
        const auto& c = st.hits[i].raw;
        std::snprintf(buf, sizeof buf, "[MEM-CAMERA-SCAN] hit=%zu address=0x%llX pos=%.2f,%.2f,%.2f row0=%.3f,%.3f,%.3f near=%.3f far=%.1f fov=%.4f aspect=%.4f",
                      i, (unsigned long long)st.hits[i].address, double(c.position.x), double(c.position.y), double(c.position.z),
                      double(c.rows[0].x), double(c.rows[0].y), double(c.rows[0].z), double(c.nearClip), double(c.farClip), double(c.fov), double(c.aspect));
        log_line(buf);
    }
    for (auto& ch : st.chains) {
        std::snprintf(buf, sizeof buf, "[MEM-CAMERA-SCAN] chain static=0x%llX offset=0x%X pointer=0x%llX camera=0x%llX (configured static=0x%llX offset=0x%X)",
                      (unsigned long long)ch.staticAddress, ch.offset, (unsigned long long)ch.pointerAddress, (unsigned long long)ch.camera,
                      (unsigned long long)l.staticPointer, l.cameraOffset);
        log_line(buf);
    }
}

// Adopts a finished scan's hits, re-reads each one, and selects the one that
// behaves like the live camera: agrees with a fresh water camera, else is
// the configured chain's address, else changes the most.
void update_memory_scan(SceneDebugger& sd, uint64_t frame) {
    auto& m = sd.memory;
    if (!m.scanner) return;
    const auto st = m.scanner->status();
    if (st.finished && st.generation != m.scanGenerationSeen) {
        m.scanGenerationSeen = st.generation;
        m.tracks.clear();
        m.scanSelected = -1;
        for (auto& h : st.hits) if (m.tracks.size() < maxScanTracks) m.tracks.push_back({h.address, h.raw, true});
        m.scanChains = st.chains;
        log_scan_result(st, m.layout);
    }
    if (m.tracks.empty()) return;
    const auto& reader = self_memory_reader();
    const bool waterFresh = sd.waterAnchor.valid && frame - sd.waterAnchor.updatedFrame <= 2;
    for (auto& t : m.tracks) {
        WowCameraLayout direct = m.layout;
        direct.directAddress = t.address;
        const auto now = read_wow_camera(reader, direct);
        t.valid = memory_camera_sanity(now).empty();
        if (t.valid && t.last.readOk && camera_struct_changed(now, t.last)) ++t.changes;
        if (t.valid && waterFresh) {
            const auto r = pair_residuals(now, sd.waterAnchor.camera, SignedPermutation{}, m.rows);
            t.waterPosition = r.position;
            t.waterForwardDeg = r.forwardDeg;
        }
        t.last = now;
    }
    // Water comparisons here are not time-paired, hence looser than matchPosition/matchDegrees.
    int best = -1;
    auto rank = [&](const MemoryScanTrack& t) {
        if (!t.valid) return -1;
        if (t.waterPosition >= 0 && t.waterPosition < 3.f && t.waterForwardDeg < 10.f) return 3;
        if (t.address == m.raw.address && m.raw.readOk) return 2;
        return t.changes > 0 ? 1 : 0;
    };
    for (size_t i = 0; i < m.tracks.size(); ++i) {
        if (best < 0) { if (rank(m.tracks[i]) >= 0) best = int(i); continue; }
        const int ri = rank(m.tracks[i]), rb = rank(m.tracks[size_t(best)]);
        if (ri > rb || (ri == rb && m.tracks[i].changes > m.tracks[size_t(best)].changes)) best = int(i);
    }
    m.scanSelected = best;
}

void update_memory_camera(SceneDebugger& sd, uint64_t frame) {
    auto& m = sd.memory;
    configure_memory_source(m);
    if (!m.enabled) { m.valid = false; m.status = "disabled by DEPTH_PROBE_MEM_CAMERA=0"; return; }
    update_memory_scan(sd, frame);
    m.raw = read_memory_camera_raw(m, m.origin, m.chainStatus);
    m.readFrame = frame;
    if (m.raw.readOk && m.raw.address != m.evidenceAddress) {
        // Agreement counters belong to one struct instance; the detected conversion is layout-level and kept.
        m.evidenceAddress = m.raw.address;
        m.pairedSamples = m.matchedSamples = 0;
        m.lastPairMatched = m.haveFirstMatch = false;
        m.recentMatchRate = 0;
        m.forwardSpanDeg = 0;
    }
    m.status = memory_camera_sanity(m.raw);
    m.valid = false;
    if (m.status.empty()) {
        const auto& r = m.raw;
        const Mat4Cols projection = m.haveWaterProjection ? m.waterProjection : synthesize_projection(r.fov * m.fovScale, m.fovAxis, r.aspect, r.nearClip, r.farClip);
        m.projectionSource = m.haveWaterProjection ? "WATER" : "SYNTH";
        m.camera = memory_camera_to_derived(r, m.rows, projection);
        m.valid = m.camera.valid;
        if (!m.valid) m.status = m.camera.invalidReason;
    }
    // A single pair can miss while the camera moves fast (the memory read and the water draw are up to a frame
    // apart; first real session: p95 .49 yd / 2 deg, max 9 yd while zooming), so agreement is a recent match
    // rate, not the last pair - otherwise the level flickers and the candidate order flips with it.
    const bool agreed = m.valid && m.comparator.result().stable && m.recentMatchRate >= minRecentMatchRate;
    m.level = agreed && m.matchedSamples >= verifiedSamples && m.forwardSpanDeg >= verifiedSpanDegrees ? CameraEvidenceLevel::Verified
            : agreed ? CameraEvidenceLevel::MemoryGpuMatch : CameraEvidenceLevel::MemoryCandidate;
}

// A water readback completed: that draw's time-paired memory read goes to the comparator.
void pair_memory_with_water(SceneDebugger& sd, uint64_t frame) {
    auto& m = sd.memory;
    const auto& water = sd.waterAnchor.camera;
    m.haveWaterProjection = true;
    m.waterProjection = water.projection;
    if (!m.pendingPairValid) return;
    m.pendingPairValid = false;
    const auto& pair = m.pendingPair;
    if (!memory_camera_sanity(pair).empty() || pair.address != m.evidenceAddress) return;
    m.comparator.add(pair, water);
    const auto& r = m.comparator.result();
    ++m.pairedSamples;
    m.lastPairFrame = frame;
    m.lastPairMatched = r.stable && r.latest.position < matchPosition && r.latest.forwardDeg < matchDegrees &&
                        r.latest.upDeg < matchDegrees && r.latest.rightDeg < matchDegrees;
    m.recentMatchRate = m.recentMatchRate * .95f + (m.lastPairMatched ? .05f : 0.f);
    if (m.lastPairMatched) {
        ++m.matchedSamples;
        if (!m.haveFirstMatch) { m.haveFirstMatch = true; m.firstMatchForward = water.forward; }
        else m.forwardSpanDeg = std::max(m.forwardSpanDeg, angle_between(m.firstMatchForward, water.forward));
    }
    if (r.stable && !m.rowsFromEnv && r.rows != m.rows) {
        log_line("[MEM-CAMERA] facing rows " + describe_rows(m.rows) + " -> " + describe_rows(r.rows) + " (stable comparison with the water camera, world " + describe_world(r.world) + ")");
        m.rows = r.rows;
        m.rowsFromComparison = true;
    }
    // Real WoW (2026-09-24): raw 1.5708 (90 deg) while the water projection's fovY is 54.00 - a 0.6 scale,
    // not a different axis. Record the measured ratio rather than assuming it stays 0.6.
    const float memoryDeg = pair.fov * radToDeg;
    const float fovX = 2.f * std::atan(std::tan(water.fovY * .5f / radToDeg) * water.aspect) * radToDeg;
    const float ratioY = memoryDeg > 0 ? water.fovY / memoryDeg : 0;
    char match[32];
    if (std::fabs(memoryDeg - fovX) < .5f && std::fabs(memoryDeg - water.fovY) >= .5f) std::snprintf(match, sizeof match, "X");
    else std::snprintf(match, sizeof match, "Y X%.3f", double(ratioY));
    if (m.fovMatch != match) {
        char buf[220];
        std::snprintf(buf, sizeof buf, "[MEM-CAMERA] raw fov %.4f rad = %.2f deg; water fovY %.2f fovX %.2f -> %s", double(pair.fov), double(memoryDeg), double(water.fovY), double(fovX), match);
        log_line(buf);
    }
    m.fovMatch = match;
    if (!m.fovAxisFromEnv && !m.fovScaleFromEnv) {
        if (m.fovMatch == "X") { m.fovAxis = FovAxis::Horizontal; m.fovScale = 1.f; }
        else if (ratioY > .1f && ratioY < 3.f) { m.fovAxis = FovAxis::Vertical; m.fovScale = ratioY; }
    }
}

void publish_memory_candidate(SceneDebugger& sd) {
    auto& m = sd.memory;
    if (!m.valid) {
        sd.cameras.erase(std::remove_if(sd.cameras.begin(), sd.cameras.end(), [](const CameraCandidate& c) { return c.derivedCamera.source == "wow-memory-cgcamera"; }), sd.cameras.end());
        return;
    }
    std::vector<std::string> evidence;
    char buf[200];
    const auto& l = m.layout;
    if (m.origin == "CHAIN") std::snprintf(buf, sizeof buf, "CGCAMERA %s VIA [0x%llX]+0x%X", hex32(m.raw.address).c_str(), (unsigned long long)l.staticPointer, l.cameraOffset);
    else std::snprintf(buf, sizeof buf, "CGCAMERA %s VIA %s", hex32(m.raw.address).c_str(), m.origin == "ENV" ? "DEPTH_PROBE_MEM_CAMERA_ADDR" : "SHIFT+F7 SCAN");
    evidence.push_back(buf);
    const auto& r = m.comparator.result();
    if (m.level == CameraEvidenceLevel::MemoryCandidate)
        evidence.push_back(r.have ? "NOT YET MATCHED WITH WATER: " + std::to_string(r.samples) + " PAIRS, " + (r.stable ? "LAST PAIR OFF" : "CONVERSION NOT STABLE")
                                  : "SANITY CHECKS ONLY - NO WATER PAIR YET");
    else {
        std::snprintf(buf, sizeof buf, "MATCHES WATER: %u/%u PAIRS, ROTATION SPAN %.0f DEG, WORLD %s", m.matchedSamples, m.pairedSamples, double(m.forwardSpanDeg), describe_world(r.world).c_str());
        evidence.push_back(buf);
    }
    std::snprintf(buf, sizeof buf, "ROWS %s%s", describe_rows(m.rows).c_str(), m.rowsFromEnv ? " (ENV)" : m.rowsFromComparison ? " (FROM COMPARISON)" : " (HYPOTHESIS)");
    evidence.push_back(buf);
    const auto& c = m.camera;
    std::snprintf(buf, sizeof buf, "POSITION %.2f, %.2f, %.2f", double(c.position.x), double(c.position.y), double(c.position.z));
    evidence.push_back(buf);
    std::snprintf(buf, sizeof buf, "RAW FOV %.4f ASPECT %.3f NEAR %.3f FAR %.1f - FOV MATCHES WATER %s", double(m.raw.fov), double(m.raw.aspect), double(m.raw.nearClip), double(m.raw.farClip), m.fovMatch.c_str());
    evidence.push_back(buf);
    evidence.push_back("PROJECTION: " + m.projectionSource);
    publish_memory_camera(sd.cameras, m.level, c, std::move(evidence));
}

const char* marker_label(uint32_t digit) {
    switch (digit) {
    case 1: return "BASELINE"; case 2: return "TURN_LEFT"; case 3: return "TURN_RIGHT";
    case 4: return "LOOK_UP"; case 5: return "LOOK_DOWN"; case 6: return "ZOOM_IN";
    case 7: return "ZOOM_OUT"; case 8: return "MOVE_FORWARD"; case 9: return "END_TEST";
    default: return "UNKNOWN";
    }
}

// ---- Depth -> world reconstruction (Shift+F6) --------------------------------
const char* recon_mode_name(uint32_t mode) {
    static const char* names[]{"OFF", "DEPTH CLASSES", "DISTANCE", "WORLD POSITION", "WORLD GRID", "NORMALS", "HEIGHT", "PROBE"};
    return mode < std::size(names) ? names[mode] : "?";
}
float env_float(const char* name, float fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const float parsed = std::strtof(v, &end);
    return end != v && parsed > 0 ? parsed : fallback;
}

struct Reconstruction {
    bool valid = false;
    std::string reason;
    ReconstructionSetup setup;
    DerivedCamera camera;
    bool fromSnapshot = false;
    float lagDeg = 0;            // record-time snapshot vs present-time memory camera
    bool depthMatches = false;   // overlay's sampled depth image == the world draws' depth attachment
};

// Canonical pose: the VERIFIED memory camera, frozen when this frame's world draws were recorded.
// Projection from the same memory struct (fov * measured scale), not from the water UBO.
Reconstruction build_reconstruction(const SceneDebugger& sd, uint64_t frame, uint64_t sceneDepthImage) {
    Reconstruction r;
    const auto& m = sd.memory;
    const auto& w = sd.world;
    r.depthMatches = w.seen && w.depthImage == sceneDepthImage;
    if (!m.valid) { r.reason = "MEMORY CAMERA: " + m.status; return r; }
    if (!w.seen || frame > w.frame + 2) { r.reason = "NO WORLD DRAW SEEN THIS FRAME"; return r; }
    const RawMemoryCamera& source = w.haveSnapshot ? w.snapshot : m.raw;
    r.fromSnapshot = w.haveSnapshot;
    const auto projection = synthesize_projection(source.fov * m.fovScale, m.fovAxis, source.aspect, source.nearClip, source.farClip);
    r.camera = memory_camera_to_derived(source, m.rows, projection);
    if (!r.camera.valid) { r.reason = "CAMERA: " + r.camera.invalidReason; return r; }
    r.lagDeg = angle_between(r.camera.forward, m.camera.forward);
    r.setup = make_reconstruction(r.camera, w.viewport);
    if (!r.setup.valid) { r.reason = r.setup.reason; return r; }
    r.valid = true;
    return r;
}

void populate_recon_uniform(OverlayUniform& u, uint32_t mode, const Reconstruction& r, const GizmoLock& lock, const DisplayCamera& d) {
    std::fill(std::begin(u.recon), std::end(u.recon), 0u);
    u.recon[0] = mode;
    u.recon[1] = mode && r.valid ? 1u : 0u;
    if (!u.recon[1]) return;
    const auto& s = r.setup;
    const float viewport[4]{s.viewport.x, s.viewport.y, s.viewport.width, s.viewport.height};
    std::copy(std::begin(viewport), std::end(viewport), u.reconViewport);
    u.reconDepth[0] = s.viewport.minDepth; u.reconDepth[1] = s.viewport.maxDepth; u.reconDepth[2] = s.p22; u.reconDepth[3] = s.p23;
    static const float gridSpacing = env_float("DEPTH_PROBE_RECON_GRID", 1.f), probeRadius = env_float("DEPTH_PROBE_RECON_PROBE_RADIUS", 1.f);
    u.reconProj[0] = s.p00; u.reconProj[1] = s.p11; u.reconProj[2] = gridSpacing; u.reconProj[3] = probeRadius;
    u.reconRight[0] = s.right.x; u.reconRight[1] = s.right.y; u.reconRight[2] = s.right.z; u.reconRight[3] = float(s.camera[2]);
    u.reconUp[0] = s.up.x; u.reconUp[1] = s.up.y; u.reconUp[2] = s.up.z; u.reconUp[3] = 0;
    u.reconForward[0] = s.forward.x; u.reconForward[1] = s.forward.y; u.reconForward[2] = s.forward.z; u.reconForward[3] = 0;
    for (int i = 0; i < 3; ++i) u.reconCameraMod[i] = float(std::fmod(s.camera[i], 1000.0));
    const bool probe = lock.active || d.have;
    const Vec3 anchor = lock.active ? lock.anchor : d.have ? gizmo_anchor(d.camera, gizmo_config()) : Vec3{};
    u.reconCameraMod[3] = probe ? 1.f : 0.f;
    u.reconProbe[0] = float(double(anchor.x) - s.camera[0]);
    u.reconProbe[1] = float(double(anchor.y) - s.camera[1]);
    u.reconProbe[2] = float(double(anchor.z) - s.camera[2]);
    u.reconProbe[3] = 0;
}

// 24 x 48, glyphs limited to A-Z 0-9 . - : [ ] , + _ / and space (menu_font.glsl).
void populate_hud_text(OverlayUniform& u, const SceneDebugger& sd, const DisplayCamera& d, uint64_t frameSerial, const CameraHudInput& hud, const Reconstruction& recon, uint64_t sceneDepthImage, const FogState* fogState) {
    std::fill(std::begin(u.hudText), std::end(u.hudText), 0u);
    char buf[96];
    auto line = [&](unsigned row, const char* fmt, auto... args) { std::snprintf(buf, sizeof buf, fmt, args...); hud_line(u, row, buf); };
    const auto& m = sd.memory;
    const auto& w = sd.waterAnchor;

    line(0, "%s  SRC %s  MODE %s", sd.telemetry.active ? "[REC]" : "CAMERA HUD", d.have ? d.name.c_str() : "NONE", gizmo_source_name(hud.gizmoSource));
    const bool generic = std::any_of(sd.cameras.begin(), sd.cameras.end(), [](const CameraCandidate& c) { return is_generic_gpu_level(c.level); });
    if (w.valid) line(1, "WATER OK AGE %llu  MEM %s  GEN %s", (unsigned long long)(frameSerial - std::min(frameSerial, w.updatedFrame)), m.valid ? m.origin.c_str() : "NO", generic ? "1" : "NONE");
    else line(1, "WATER NONE  MEM %s  GEN %s", m.valid ? m.origin.c_str() : "NO", generic ? "1" : "NONE");
    if (d.have) {
        const auto& c = d.camera;
        line(2, "POS %.2f %.2f %.2f", double(c.position.x), double(c.position.y), double(c.position.z));
        line(3, "FWD %.3f %.3f %.3f", double(c.forward.x), double(c.forward.y), double(c.forward.z));
        line(4, "UP  %.3f %.3f %.3f", double(c.up.x), double(c.up.y), double(c.up.z));
        line(5, "RGT %.3f %.3f %.3f", double(c.right.x), double(c.right.y), double(c.right.z));
        line(6, "FOVY %.2f ASP %.3f N %.3f F %.1f", double(c.fovY), double(c.aspect), double(c.nearPlane), double(c.farPlane));
        const auto health = analyze_view_basis(c.view);
        line(7, "DET %.4f %s", double(health.determinant), health.orthonormal ? "ORTHONORMAL" : "NOT ORTHONORMAL");
    } else {
        line(2, "NO CAMERA FOR MODE %s", gizmo_source_name(hud.gizmoSource));
        hud_line(u, 3, "WATER: LOOK AT WATER FOR A MOMENT");
        hud_line(u, 4, "MEMORY: SEE MEM LINES BELOW");
    }

    if (!m.enabled) hud_line(u, 9, "MEM DISABLED - DEPTH_PROBE_MEM_CAMERA=0");
    else if (m.valid) line(9, "MEM %s %s %s", m.origin.c_str(), hex32(m.raw.address).c_str(), level_short(m.level));
    else hud_line(u, 9, "MEM NO: " + m.status);
    if (hud.fogMode && fogState) {
        const auto& f = *fogState;
        const auto& fs = f.settings;
        const auto g = fog::grid_for(fs.preset, sd.world.seen ? uint32_t(std::fabs(sd.world.viewport.width)) : 1920u, sd.world.seen ? uint32_t(std::fabs(sd.world.viewport.height)) : 1080u, fs.maxDistance, fs.distribution);
        static const char* modes[]{"OFF", "FINAL", "FROXEL DENSITY", "FROXEL EXTINCTION", "FROXEL LIGHTING", "INTEGRATED SCATTERING", "TRANSMITTANCE", "SUN VISIBILITY"};
        line(9, "FOG %s  SHIFT+F5", modes[hud.fogMode % 8]);
        line(10, "FROXELS %uX%uX%u %s MAXD %.0f", g.x, g.y, g.z, fs.preset == fog::Preset::Low ? "LOW" : fs.preset == fog::Preset::High ? "HIGH" : "MEDIUM", double(fs.maxDistance));
        line(11, "APPLIED %u WORLD PASSES %u DROPPED %u", f.lastComposites, f.lastWorldPasses, f.lastDropped);
        if (!f.lastProblem.empty()) hud_line(u, 12, "NOT APPLIED: " + f.lastProblem);
        else hud_line(u, 12, "BEFORE UI AT: " + f.lastTrigger);
        line(13, "GPU MEM %.2f INJ %.2f LIT %.2f INT %.2f CMP %.2f", fog_gpu_memories(f), f.gpuInject, f.gpuLight, f.gpuIntegrate, f.gpuComposite);
        line(14, "GPU TOTAL %.2f MS CPU %.2f MS TEMPORAL %s %.2f", fog_gpu_total(f), f.cpuComposite + f.cpuPrelude,
             fs.temporalWeight <= 0 ? "OFF" : f.lastHistoryUsed ? "ON" : "RESET", double(f.lastHistoryWeight));
        line(15, "DENS %.4f FALL %.3f BASE Z %.1f %s G %.2f", double(fs.medium.baseDensity), double(fs.medium.heightFalloff), double(f.lastBaseHeight), fs.heightReference == fog::HeightReference::Ground ? "GROUND" : fs.heightReference == fog::HeightReference::Camera ? "CAM" : "WORLD", double(fs.anisotropy));
        {
            char exposure[48] = "";
            if (fs.autoExposure) std::snprintf(exposure, sizeof exposure, "  EXP %.2f KEY %.2f", double(f.exposure), double(f.exposureKey));
            hud_line(u, 16, std::string("WATER SURFACE ") + (f.lastWaterSurface ? "YES" : "NO") + exposure + "  FOG.INI " + f.settingsNote);
        }
        if (!hud.gizmoVisible) {
            if (!fs.sunFromGame) hud_line(u, 17, "SUN MANUAL (FOG.INI)");
            else if (f.lastSunSource == "seen disc") line(17, "SUN = SEEN DISC EL %.1f (%llu SIGHTINGS)", std::asin(double(f.seenDisc[2])) * 180 / M_PI, (unsigned long long)f.seenDiscCount);
            else if (f.gameSun.ok && f.lastSunFromDisc) line(17, "SUN SKY DISC EL %.1f AZ %.0f (LIGHT EL %.1f)", double(f.gameSun.skyElevationDeg), double(f.gameSun.skyAzimuthDeg), double(f.gameSun.elevationDeg));
            else if (f.gameSun.ok) line(17, "SUN GAME LIGHT EL %.1f AZ %.0f %s", double(f.gameSun.elevationDeg), double(f.gameSun.azimuthDeg), f.lastSunFromGame ? "USED" : "");
            else hud_line(u, 17, "SUN GAME NOT READ: " + f.gameSun.failure);
            if (f.weather.ok && f.weather.kind)
                line(18, "AMBIENT %s %.2f %.2f %.2f %s %.2f SUN %d%%", f.lastAmbientFromGame ? "GAME" : "FOG.INI", double(f.lastAmbient[0]), double(f.lastAmbient[1]),
                     double(f.lastAmbient[2]), weather_name(f.weather.kind), double(f.weather.intensity), int(f.lastSunWeatherScale * 100 + .5f));
            else line(18, "AMBIENT %s %.2f %.2f %.2f", f.lastAmbientFromGame ? "GAME" : "FOG.INI", double(f.lastAmbient[0]), double(f.lastAmbient[1]), double(f.lastAmbient[2]));
            hud_line(u, 19, (f.lastShadowProblem.empty() ? "SHADOWS SUN " + std::to_string(int(fs.shadowLength)) + " YD" : "SHADOWS: " + f.lastShadowProblem) +
                                " SKY " + std::to_string(int(fs.skyLight * 100 + .5f)) + "% CLEARS MEM " + std::to_string(f.memoryClears) + " SUN " + std::to_string(f.sunMapClears));
        }
    } else if (hud.reconMode) {
        const auto& w = sd.world;
        line(10, "RECON %u %s  SHIFT+F6", hud.reconMode, recon_mode_name(hud.reconMode));
        if (w.seen) line(11, "DEPTH %s %s %s", hex32(sceneDepthImage).c_str(), recon.depthMatches ? "= WORLD TARGET" : "NOT WORLD TARGET", recon.depthMatches ? "" : hex32(w.depthImage).c_str());
        else line(11, "DEPTH %s, WORLD TARGET NOT SEEN", hex32(sceneDepthImage).c_str());
        if (w.seen) line(12, "VP %.1f %.1f %.0f %.0f Z %.2f-%.3f N%u", double(w.viewport.x), double(w.viewport.y), double(w.viewport.width), double(w.viewport.height),
                         double(w.viewport.minDepth), double(w.viewport.maxDepth), w.draws);
        if (!recon.valid) hud_line(u, 13, "NOT RECONSTRUCTING: " + recon.reason);
        else {
            line(13, "CAM MEMORY %s LAG %.2f DEG", recon.fromSnapshot ? "RECORD SNAP" : "PRESENT READ", double(recon.lagDeg));
            line(14, "P22 %.9f P23 %.6f FOVY %.2f", double(recon.setup.p22), double(recon.setup.p23), double(recon.camera.fovY));
            if (w.depthImage && sd.waterAnchor.valid) {
                const auto& wp = sd.waterAnchor.camera.projection;
                line(15, "VS WATER DP22 %.1e DFOVY %.3f", double(recon.setup.p22 - wp.c2.z), double(recon.camera.fovY - sd.waterAnchor.camera.fovY));
            }
        }
    } else if (m.enabled) {
        if (m.layout.directAddress) line(10, "ADDR %s: %s", hex32(m.layout.directAddress).c_str(), m.chainStatus.c_str());
        else line(10, "CHAIN [%s]+0X%X: %s", hex32(m.layout.staticPointer).c_str(), m.layout.cameraOffset, m.chainStatus.c_str());
        if (m.raw.readOk) line(11, "RAW FOV %.4f ASP %.3f N %.3f F %.1f", double(m.raw.fov), double(m.raw.aspect), double(m.raw.nearClip), double(m.raw.farClip));
        const auto& r = m.comparator.result();
        if (r.have) {
            line(12, "CMP %u %s %s %s", r.samples, r.stable ? "STABLE" : "SEARCHING", describe_world(r.world).c_str(), describe_rows(r.rows).c_str());
            line(13, "DPOS %.2f DF %.1f DU %.1f DR %.1f", double(r.latest.position), double(r.latest.forwardDeg), double(r.latest.upDeg), double(r.latest.rightDeg));
            line(14, "MATCH %u/%u SPAN %.0f FOV %s PROJ %s", m.matchedSamples, m.pairedSamples, double(m.forwardSpanDeg), m.fovMatch.c_str(), m.valid ? m.projectionSource.c_str() : "-");
        } else hud_line(u, 12, "CMP: NEEDS WATER ON SCREEN WITH MEM OK");
        if (m.scanner) {
            const auto st = m.scanner->status();
            if (st.running) line(15, "SCAN %llu/%llu MB", (unsigned long long)(st.bytesScanned >> 20), (unsigned long long)(st.bytesPlanned >> 20));
            else if (m.scanSelected >= 0) line(15, "SCAN %zu HITS SEL %s CHAINS %zu", m.tracks.size(), hex32(m.tracks[size_t(m.scanSelected)].address).c_str(), m.scanChains.size());
            else line(15, "SCAN %zu HITS, NONE LIVE", m.tracks.size());
        } else hud_line(u, 15, "SCAN: SHIFT+F7");
    }

    if (hud.gizmoVisible) {
        line(17, "GIZMO ON  LOCK %s  SRC %s", sd.gizmoLock.active ? "ON" : "OFF", d.have ? d.name.c_str() : "NONE");
        const auto& cfg = gizmo_config();
        if (d.have) {
            const auto anchor = sd.gizmoLock.active ? sd.gizmoLock.anchor : gizmo_anchor(d.camera, cfg);
            line(18, "ANCHOR %.2f %.2f %.2f", double(anchor.x), double(anchor.y), double(anchor.z));
        } else hud_line(u, 18, "GIZMO WAITING FOR CAMERA");
        line(19, "OFFSET F%.1f U%.1f R%.1f LEN%.1f", double(cfg.forwardOffset), double(cfg.upOffset), double(cfg.rightOffset), double(cfg.armLength));
    }
    if (sd.telemetry.active) line(20, "F6 MARK NEXT %s LAST %s", marker_label(hud.nextMarker), sd.telemetry.lastMarker.empty() ? "-" : sd.telemetry.lastMarker.c_str());
    else line(20, "FRAME %llu", (unsigned long long)frameSerial);
    hud_line(u, 21, "SHIFT+ F9 REC  F8 SOURCE  F7 SCAN");
    if (sd.telemetry.active) line(22, "REC %llu SAMPLES - SHIFT+F9 STOPS", (unsigned long long)sd.telemetry.samples);
    else hud_line(u, 22, "NOT RECORDING - SHIFT+F9 STARTS");
    hud_line(u, 23, "F9 HUD  F8 GIZMO  F7 LOCK  F6 MARK");
}

std::string telemetry_root() {
    namespace fs = std::filesystem;
    if (const char* env = std::getenv("DEPTH_PROBE_EXPORT_DIR"); env && *env) return env;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return (fs::path(xdg) / "vulkan-depth-probe/exports").string();
    if (const char* home = std::getenv("HOME"); home && *home) return (fs::path(home) / ".local/share/vulkan-depth-probe/exports").string();
    return "/tmp/depth-probe-inspector-exports";
}

void start_camera_telemetry_session(CameraTelemetrySession& t, uint64_t frameSerial) {
    namespace fs = std::filesystem;
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    const fs::path dir = fs::path(telemetry_root()) / (std::string("camera-session-") + stamp);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { log_line("[CAMERA-TELEMETRY] failed to create session directory: " + ec.message()); return; }
    t.directory = dir.string();
    t.csv.open(dir / "camera.csv", std::ios::out | std::ios::trunc);
    t.jsonl.open(dir / "camera.jsonl", std::ios::out | std::ios::trunc);
    if (t.csv) t.csv << "timestamp,frame,source,pos_x,pos_y,pos_z,forward_x,forward_y,forward_z,up_x,up_y,up_z,right_x,right_y,right_z,"
                          "fov_y,aspect,near,far,view_det,right_len,up_len,forward_len,right_dot_up,right_dot_forward,up_dot_forward,delta_position,delta_angle,camera_age_frames,"
                          "mem_valid,mem_origin,mem_level,mem_address,mem_pos_x,mem_pos_y,mem_pos_z,mem_forward_x,mem_forward_y,mem_forward_z,"
                          "mem_up_x,mem_up_y,mem_up_z,mem_right_x,mem_right_y,mem_right_z,mem_fov_raw,mem_aspect,mem_near,mem_far,mem_rows,mem_status,"
                          "cmp_samples,cmp_stable,cmp_world,cmp_rows,cmp_pos_residual,cmp_forward_deg,cmp_up_deg,cmp_right_deg,cmp_last_pair_frame,"
                          "cmp_matched,cmp_paired,cmp_span_deg,mem_fov_match\n";
    t.active = bool(t.csv) && bool(t.jsonl);
    t.samples = 0; t.hasPrevious = false; t.startedFrame = frameSerial; t.lastMarker.clear();
    log_line(t.active ? "[CAMERA-TELEMETRY] recording started directory=" + t.directory
                       : "[CAMERA-TELEMETRY] recording failed to open files in " + t.directory);
}

std::string json_floats16(const Mat4Cols& m) {
    const float f[16]{m.c0.x, m.c0.y, m.c0.z, m.c0.w, m.c1.x, m.c1.y, m.c1.z, m.c1.w, m.c2.x, m.c2.y, m.c2.z, m.c2.w, m.c3.x, m.c3.y, m.c3.z, m.c3.w};
    std::ostringstream o; o << '[';
    for (int i = 0; i < 16; ++i) { if (i) o << ','; o << f[i]; }
    o << ']'; return o.str();
}
std::string json_vec3(const Vec3& v) { std::ostringstream o; o << '[' << v.x << ',' << v.y << ',' << v.z << ']'; return o.str(); }

void write_marker(CameraTelemetrySession& t, const std::string& label, uint64_t frame) {
    t.lastMarker = label;
    if (!t.jsonl) return;
    t.jsonl << "{\"event\":\"MARKER\",\"label\":\"" << label << "\",\"frame\":" << frame << "}\n";
    t.jsonl.flush();
    log_line("[CAMERA-TELEMETRY] marker=" + label + " frame=" + std::to_string(frame));
}

std::string memory_camera_json(const MemoryCameraSource& m) {
    std::ostringstream o;
    o << std::setprecision(9);
    const auto& raw = m.raw;
    o << "{\"valid\":" << (m.valid ? "true" : "false") << ",\"enabled\":" << (m.enabled ? "true" : "false") << ",\"origin\":\"" << m.origin
      << "\",\"status\":\"" << m.status << "\",\"chainStatus\":\"" << m.chainStatus << "\",\"address\":\"" << hex32(raw.address)
      << "\",\"chainValue\":\"" << hex32(raw.chainValue) << "\",\"level\":\"" << level_short(m.level) << "\",\"rows\":\"" << describe_rows(m.rows)
      << "\",\"rowsSource\":\"" << (m.rowsFromEnv ? "env" : m.rowsFromComparison ? "comparison" : "hypothesis") << "\"";
    if (raw.readOk)
        o << ",\"raw\":{\"position\":" << json_vec3(raw.position) << ",\"facingRows\":[" << json_vec3(raw.rows[0]) << ',' << json_vec3(raw.rows[1]) << ',' << json_vec3(raw.rows[2])
          << "],\"near\":" << raw.nearClip << ",\"far\":" << raw.farClip << ",\"fov\":" << raw.fov << ",\"aspect\":" << raw.aspect << "}";
    if (m.valid) {
        const auto& c = m.camera;
        o << ",\"derived\":{\"position\":" << json_vec3(c.position) << ",\"forward\":" << json_vec3(c.forward) << ",\"up\":" << json_vec3(c.up)
          << ",\"right\":" << json_vec3(c.right) << ",\"view\":" << json_floats16(c.view) << ",\"projection\":" << json_floats16(c.projection)
          << ",\"projectionSource\":\"" << m.projectionSource << "\"}";
    }
    const auto& r = m.comparator.result();
    o << ",\"comparison\":{\"samples\":" << r.samples << ",\"stable\":" << (r.stable ? "true" : "false");
    if (r.have)
        o << ",\"world\":\"" << describe_world(r.world) << "\",\"rows\":\"" << describe_rows(r.rows) << "\",\"meanScore\":" << r.meanScore
          << ",\"runnerUpScore\":" << r.runnerUpScore << ",\"latest\":{\"position\":" << r.latest.position << ",\"forwardDeg\":" << r.latest.forwardDeg
          << ",\"upDeg\":" << r.latest.upDeg << ",\"rightDeg\":" << r.latest.rightDeg << ",\"frame\":" << m.lastPairFrame << "}";
    o << ",\"matched\":" << m.matchedSamples << ",\"paired\":" << m.pairedSamples << ",\"forwardSpanDeg\":" << m.forwardSpanDeg
      << ",\"fovMatch\":\"" << m.fovMatch << "\"}}";
    return o.str();
}

void write_camera_telemetry_sample(SceneDebugger& sd, uint64_t frame) {
    auto& t = sd.telemetry;
    const auto& anchor = sd.waterAnchor;
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();

    float deltaPos = -1.f, deltaAngle = -1.f;
    BasisHealth health{};
    if (anchor.valid) {
        health = analyze_view_basis(anchor.camera.view);
        if (t.hasPrevious) {
            const auto& p = anchor.camera.position; const auto& f = anchor.camera.forward;
            const float dx = p.x - t.previousPosition.x, dy = p.y - t.previousPosition.y, dz = p.z - t.previousPosition.z;
            deltaPos = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float dot = std::clamp(f.x * t.previousForward.x + f.y * t.previousForward.y + f.z * t.previousForward.z, -1.f, 1.f);
            deltaAngle = std::acos(dot) * (180.f / 3.14159265358979323846f);
        }
        t.previousPosition = anchor.camera.position; t.previousForward = anchor.camera.forward; t.hasPrevious = true;
    }

    if (t.csv) {
        t.csv << std::fixed << std::setprecision(6) << now << ',' << frame << ',' << (anchor.valid ? "WATER_GPU_TRANSFORM" : "") << ',';
        if (anchor.valid) {
            const auto& c = anchor.camera;
            t.csv << c.position.x << ',' << c.position.y << ',' << c.position.z << ','
                  << c.forward.x << ',' << c.forward.y << ',' << c.forward.z << ','
                  << c.up.x << ',' << c.up.y << ',' << c.up.z << ','
                  << c.right.x << ',' << c.right.y << ',' << c.right.z << ','
                  << c.fovY << ',' << c.aspect << ',' << c.nearPlane << ',' << c.farPlane << ','
                  << health.determinant << ',' << health.rightLen << ',' << health.upLen << ',' << health.forwardLen << ','
                  << health.rightDotUp << ',' << health.rightDotForward << ',' << health.upDotForward << ','
                  << deltaPos << ',' << deltaAngle << ',' << (frame >= anchor.updatedFrame ? frame - anchor.updatedFrame : 0);
        } else {
            t.csv << ",,,,,,,,,,,,,,,,,,,,,,,,,";
        }
        const auto& m = sd.memory;
        const auto& r = m.comparator.result();
        t.csv << ',' << (m.valid ? 1 : 0) << ',' << m.origin << ',' << (m.valid ? level_short(m.level) : "") << ',' << hex32(m.raw.address) << ',';
        if (m.valid) {
            const auto& c = m.camera;
            t.csv << c.position.x << ',' << c.position.y << ',' << c.position.z << ',' << c.forward.x << ',' << c.forward.y << ',' << c.forward.z << ','
                  << c.up.x << ',' << c.up.y << ',' << c.up.z << ',' << c.right.x << ',' << c.right.y << ',' << c.right.z << ','
                  << m.raw.fov << ',' << m.raw.aspect << ',' << m.raw.nearClip << ',' << m.raw.farClip << ',';
        } else t.csv << ",,,,,,,,,,,,,,,,";
        // Status text never contains commas (memory_camera_sanity / read failures), so it needs no quoting.
        t.csv << describe_rows(m.rows) << ',' << m.status << ',' << r.samples << ',' << (r.stable ? 1 : 0) << ','
              << (r.have ? describe_world(r.world) : "") << ',' << (r.have ? describe_rows(r.rows) : "") << ',';
        if (r.have) t.csv << r.latest.position << ',' << r.latest.forwardDeg << ',' << r.latest.upDeg << ',' << r.latest.rightDeg << ',' << m.lastPairFrame;
        else t.csv << ",,,,";
        t.csv << ',' << m.matchedSamples << ',' << m.pairedSamples << ',' << m.forwardSpanDeg << ',' << m.fovMatch;
        t.csv << '\n';
        t.csv.flush();
    }

    if (t.jsonl) {
        std::ostringstream o;
        o << "{\"frame\":" << frame << ",\"time\":" << std::fixed << std::setprecision(6) << now << ",\"waterGpu\":";
        if (anchor.valid) {
            const auto& c = anchor.camera;
            o << "{\"valid\":true,\"view\":" << json_floats16(c.view) << ",\"projection\":" << json_floats16(c.projection) << ",\"inverseView\":" << json_floats16(c.inverseView)
              << ",\"position\":" << json_vec3(c.position) << ",\"forward\":" << json_vec3(c.forward) << ",\"up\":" << json_vec3(c.up) << ",\"right\":" << json_vec3(c.right)
              << ",\"ageFrames\":" << (frame >= anchor.updatedFrame ? frame - anchor.updatedFrame : 0) << ",\"fovY\":" << c.fovY << ",\"aspect\":" << c.aspect << ",\"near\":" << c.nearPlane << ",\"far\":" << c.farPlane
              << ",\"viewDet\":" << health.determinant << ",\"orthonormal\":" << (health.orthonormal ? "true" : "false")
              << ",\"source\":{\"vs\":\"" << hex(wowWaterVS) << "\",\"fs\":\"" << hex(wowWaterFS) << "\",\"set\":" << anchor.set
              << ",\"binding\":" << anchor.binding << ",\"projectionOffset\":256,\"viewOffset\":336}}";
        } else o << "{\"valid\":false}";
        o << ",\"genericGpuCandidates\":[";
        bool comma = false; size_t shown = 0;
        for (auto& cam : sd.cameras) {
            if (cam.level == CameraEvidenceLevel::GpuVerifiedWater) continue;
            if (shown++ >= 5) break;
            if (comma) o << ',';
            comma = true;
            o << "{\"vsHash\":\"" << hex(cam.vsHash) << "\",\"confidence\":" << cam.confidence
              << ",\"hasProj\":" << (cam.hasProj ? "true" : "false") << ",\"projOffset\":" << cam.projOffset
              << ",\"hasView\":" << (cam.hasView ? "true" : "false") << ",\"viewOffset\":" << cam.viewOffset
              << ",\"drawsObserved\":" << cam.drawsObserved << ",\"framesObserved\":" << cam.framesObserved
              << ",\"hasDerivedCamera\":" << (cam.hasDerivedCamera ? "true" : "false") << "}";
        }
        o << "],\"memoryCamera\":" << memory_camera_json(sd.memory) << "}";
        t.jsonl << o.str() << '\n';
        t.jsonl.flush();
    }
    ++t.samples;
}

void write_telemetry_summary(SceneDebugger& sd) {
    auto& t = sd.telemetry;
    std::ofstream summary(std::filesystem::path(t.directory) / "summary.txt", std::ios::out | std::ios::trunc);
    if (!summary) return;
    summary << "CAMERA TELEMETRY SESSION\n\nDirectory: " << t.directory << "\nSamples: " << t.samples << "\nStarted at frame: " << t.startedFrame << "\n\n";
    summary << "WATER_GPU_TRANSFORM (not independently verified as the canonical camera - see docs/inspector.md)\n";
    summary << "Status: " << (sd.waterAnchor.valid ? "VALID" : "NOT AVAILABLE") << "\n";
    if (sd.waterAnchor.valid) {
        const auto& c = sd.waterAnchor.camera;
        const auto health = analyze_view_basis(c.view);
        summary << "Last position: " << c.position.x << ", " << c.position.y << ", " << c.position.z << "\n";
        summary << "FOV " << c.fovY << " deg, aspect " << c.aspect << ", near " << c.nearPlane << ", far " << c.farPlane << "\n";
        summary << "Basis: " << (health.orthonormal ? "orthonormal" : "NOT orthonormal") << ", det=" << health.determinant
                << (health.orthonormal && health.determinant < 0 ? " (handedness/axis reflection present)" : "") << "\n";
    }
    summary << "\nGENERIC GPU CAMERA CANDIDATES (top, at session end)\n";
    size_t shown = 0;
    for (auto& cam : sd.cameras) {
        if (cam.level == CameraEvidenceLevel::GpuVerifiedWater) continue;
        if (shown >= 5) break;
        summary << "#" << ++shown << " VS " << hex(cam.vsHash) << " confidence " << int(cam.confidence * 100 + .5f) << "%\n";
    }
    if (!shown) summary << "(none)\n";
    const auto& m = sd.memory;
    const auto& r = m.comparator.result();
    summary << "\nMEMORY CAMERA (WoW process memory, CGCamera)\n";
    summary << "Status: " << (m.valid ? "VALID" : "NOT AVAILABLE - " + m.status) << "\nOrigin: " << m.origin << "  address " << hex32(m.raw.address)
            << "  chain [" << hex32(m.layout.staticPointer) << "]+" << hex32(m.layout.cameraOffset) << ": " << m.chainStatus << "\n";
    summary << "Facing rows: " << describe_rows(m.rows) << (m.rowsFromEnv ? " (env)" : m.rowsFromComparison ? " (from comparison)" : " (hypothesis)") << "\n";
    if (m.raw.readOk) summary << "Raw fov " << m.raw.fov << " rad, aspect " << m.raw.aspect << ", near " << m.raw.nearClip << ", far " << m.raw.farClip << " - fov matches water: " << m.fovMatch << "\n";
    summary << "Comparison with WATER_GPU_TRANSFORM: " << r.samples << " time-paired samples, " << (r.stable ? "STABLE" : "not stable");
    if (r.have) summary << ", world " << describe_world(r.world) << ", rows " << describe_rows(r.rows) << ", last residual pos " << r.latest.position
                        << " fwd " << r.latest.forwardDeg << " up " << r.latest.upDeg << " right " << r.latest.rightDeg << " deg";
    summary << "\nMatched " << m.matchedSamples << "/" << m.pairedSamples << " pairs, forward rotation span " << m.forwardSpanDeg << " deg\n";
    summary << "\nCANONICAL / VERIFIED SCENE CAMERA\n";
    if (m.valid && m.level == CameraEvidenceLevel::Verified)
        summary << "VERIFIED: memory and water cameras agreed over " << m.matchedSamples << " time-paired samples spanning " << m.forwardSpanDeg << " deg of rotation.\n";
    else if (m.valid && m.level == CameraEvidenceLevel::MemoryGpuMatch)
        summary << "MEMORY <-> GPU MATCH, not yet VERIFIED (needs " << verifiedSamples << " matched pairs and " << verifiedSpanDegrees << " deg of rotation).\n";
    else summary << "NOT VERIFIED - the two independent sources have not been shown to agree.\n";
    log_line("[CAMERA-TELEMETRY] summary written " + t.directory + "/summary.txt");
}

void stop_camera_telemetry_session(SceneDebugger& sd) {
    auto& t = sd.telemetry;
    if (!t.active) return;
    write_telemetry_summary(sd);
    t.csv.close(); t.jsonl.close();
    t.active = false;
    log_line("[CAMERA-TELEMETRY] recording stopped samples=" + std::to_string(t.samples));
}
}

SceneDebugger& scene_debugger_state(Device& s) {
    if (!s.sceneDebugger) s.sceneDebugger = std::make_shared<SceneDebugger>();
    return *s.sceneDebugger;
}

// Also true whenever the Camera Telemetry HUD (F9) or its recording
// (Ctrl+F9) is active, independent of the F10 menu - required so the
// capture/derivation pipeline below keeps running after F10 closes, per the
// user's explicit "close F10 and still see live values" requirement.
static void observe_world_draw(SceneDebugger& sd, const Dynamic& dynamic, const Target& target, uint64_t frame);
// World geometry is drawn with depth range 0..<1 (WoW: 0..0.94; the sky uses 0.998..1). The first such
// draw after a present also freezes the memory camera for that frame's reconstruction.
static void observe_world_draw(SceneDebugger& sd, const Dynamic& dynamic, const Target& target, uint64_t frame) {
    if (!(dynamic.valid & Viewport) || !target.depth || !target.depth->image) return;
    const auto& v = dynamic.viewport;
    if (v.minDepth != 0.f || !(v.maxDepth > .5f && v.maxDepth < .999f) || v.width <= 0 || v.height == 0) return;
    auto& w = sd.world;
    if (w.frame != frame) w.draws = 0;
    w.seen = true;
    w.frame = frame;
    w.viewport = {v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth};
    w.depthImage = target.depth->image->handle;
    ++w.draws;
    if (w.snapshotPending && sd.memory.configured && sd.memory.enabled) {
        std::string origin, chainStatus;
        w.snapshot = read_memory_camera_raw(sd.memory, origin, chainStatus);
        w.haveSnapshot = memory_camera_sanity(w.snapshot).empty();
        w.snapshotPending = false;
    }
}

// The fog's camera for the frame being recorded: the memory camera frozen at this frame's first world draw
// (read now if none was frozen), the same projection/viewport path as the reconstruction views.
bool scene_debugger_frame_reconstruction(Device& s, ReconstructionSetup& out, std::string& reason) {
    auto& sd = scene_debugger_state(s);
    auto& m = sd.memory;
    configure_memory_source(m);
    if (!m.enabled) { reason = "memory camera disabled (DEPTH_PROBE_MEM_CAMERA=0)"; return false; }
    const auto& w = sd.world;
    if (!w.seen || s.frameSerial > w.frame + 2) { reason = "no world draw seen this frame"; return false; }
    RawMemoryCamera source;
    if (w.haveSnapshot) source = w.snapshot;
    else { std::string origin, chain; source = read_memory_camera_raw(m, origin, chain); }
    if (const auto why = memory_camera_sanity(source); !why.empty()) { reason = "memory camera: " + why; return false; }
    const auto projection = synthesize_projection(source.fov * m.fovScale, m.fovAxis, source.aspect, source.nearClip, source.farClip);
    const auto camera = memory_camera_to_derived(source, m.rows, projection);
    if (!camera.valid) { reason = "camera: " + camera.invalidReason; return false; }
    out = make_reconstruction(camera, w.viewport);
    if (!out.valid) { reason = out.reason; return false; }
    return true;
}

bool scene_debugger_full_capture(const Device& s) { return s.inspector && s.inspector->page == 10; }
bool scene_debugger_active(const Device& s) { return scene_debugger_full_capture(s) || camera_hud_input().wants_capture(); }

bool scene_debugger_peek_draw(Device& s, const Command& c) {
    if (!scene_debugger_active(s)) return false;
    if (camera_hud_input().reconMode || camera_hud_input().fogMode) observe_world_draw(scene_debugger_state(s), c.dynamic, c.target, s.frameSerial);
    return c.pipeline && c.pipeline->vs && c.pipeline->vs->hash == wowWaterVS; // snapshot it: scene_debugger_observe_draw reads its UBO
}

void scene_debugger_observe_draw(const DeviceData& data, Device& s, Command& c, const Draw& draw, VkCommandBuffer) {
    if (!scene_debugger_active(s)) return;
    auto p = draw.pipeline;
    if (camera_hud_input().reconMode || camera_hud_input().fogMode) observe_world_draw(scene_debugger_state(s), draw.dynamic, draw.target, s.frameSerial);
    if (!p || !p->vs || p->vs->hash != wowWaterVS) return;
    auto& sd = scene_debugger_state(s);
    sd.waterAnchor.vsSeen = true;
    if (p->fs && p->fs->hash == wowWaterFS) sd.waterAnchor.fsSeen = true;

    if (sd.pendingWaterRead) return; // one readback already in flight
    // The HUD/gizmo/recorder need a live camera, not a 120-frame-old one; only one readback is ever in flight.
    if (sd.waterAnchor.valid && !camera_hud_input().wants_capture() && s.frameSerial < sd.nextWaterReadbackFrame) return;

    if (c.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || !(c.usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) || c.conditional || c.transformFeedback || c.activeQueries) {
        sd.waterAnchor.gpuReadbackStatus = "not attempted: unsupported command buffer shape";
        return;
    }

    // Record-time-only resolve, mirroring the Inspector's own pick-query
    // path (inspector_replay.cpp) - not the submit-time SubmittedDraw the
    // rest of this file otherwise works with.
    SubmittedDraw temp; temp.draw = draw; resolve_resources(s, temp);
    for (auto& r : temp.resources) {
        if (r.set != 2 || r.binding != 0) continue;
        sd.waterAnchor.bindingFound = true;
        sd.waterAnchor.bufferRange = r.descriptor.range;
        sd.waterAnchor.set = r.set; sd.waterAnchor.binding = r.binding; sd.waterAnchor.element = r.element;
        size_t budgetUsed = 0;
        auto read = schedule_gpu_uniform_read(data, s, c, draw, r, budgetUsed, maxWaterGpuReadbackBudget);
        if (read->snapshot->reason == "gpu-readback-pending") {
            sd.pendingWaterRead = read;
            sd.waterAnchor.gpuReadbackStatus = "pending";
            // The memory camera as of this water draw, compared once the readback completes.
            if (sd.memory.configured && sd.memory.enabled) {
                std::string origin, chainStatus;
                sd.memory.pendingPair = read_memory_camera_raw(sd.memory, origin, chainStatus);
                sd.memory.pendingPairValid = true;
            }
        } else {
            sd.waterAnchor.gpuReadbackStatus = "failed: " + read->snapshot->reason;
        }
        break;
    }
}

void scene_debugger_poll(Device& s) {
    if (!s.sceneDebugger) return;
    auto& sd = *s.sceneDebugger;
    if (!sd.pendingWaterRead) return;
    if (!sd.pendingWaterRead->batch || !sd.pendingWaterRead->batch->poll()) return; // not submitted/ready yet
    auto& snap = *sd.pendingWaterRead->snapshot;
    auto& anchor = sd.waterAnchor;
    if (!snap.reason.empty()) {
        anchor.gpuReadbackStatus = "failed: " + snap.reason;
    } else {
        anchor.gpuReadbackStatus = "ok";
        process_water_bytes(anchor, snap.bytes);
        if (anchor.valid) {
            anchor.updatedFrame = s.frameSerial;
            sd.nextWaterReadbackFrame = s.frameSerial + waterAnchorRecheckInterval;
            pair_memory_with_water(sd, s.frameSerial);
        }
    }
    sd.memory.pendingPairValid = false;
    sd.pendingWaterRead.reset();
}

void capture_scene_debugger_uniforms(Device& s, SubmittedDraw& draw) {
    if (!scene_debugger_full_capture(s)) return;
    auto& sd = scene_debugger_state(s);
    if (sd.lastFrameSerial != s.frameSerial) {
        sd.lastFrameSerial = s.frameSerial;
        sd.sampledShadersThisFrame.clear();
        sd.uniformCaptureBytes = 0;
        sd.totalDrawsLastFrame = sd.totalDrawsThisFrame;
        sd.totalDrawsThisFrame = 0;
    }
    ++sd.totalDrawsThisFrame;
    auto p = draw.draw.pipeline;
    if (!p || !p->vs) return;
    ++sd.shaderDrawsCumulative[p->vs->hash];
    auto& sampledThisFrame = sd.sampledShadersThisFrame[p->vs->hash];
    if (sampledThisFrame >= maxSamplesPerShaderPerFrame) return; // within-frame cap: enough draws to compare, bounded cost
    ++sampledThisFrame;

    for (const auto& r : draw.resources) {
        if (r.descriptor.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && r.descriptor.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
        bool declared = false;
        for (auto shader : {p->vs, p->fs}) if (shader) for (const auto& b : shader->bindings) declared |= b.storage == 2 && b.set == r.set && b.binding == r.binding;
        if (!declared) continue;
        auto snap = try_capture_uniform(s, draw, r, sd.uniformCaptureBytes, maxSceneDebuggerBytesPerFrame);

        // Verified water anchor, host-mapped attempt: independent of whether
        // this succeeds, record the reason - the GPU-readback path
        // (scene_debugger_observe_draw/scene_debugger_poll) is scheduled
        // separately at record time and may resolve this on its own.
        if (p->vs->hash == wowWaterVS && r.set == 2 && r.binding == 0) {
            auto& anchor = sd.waterAnchor;
            anchor.bindingFound = true; anchor.set = r.set; anchor.binding = r.binding; anchor.element = r.element;
            anchor.bufferRange = r.descriptor.range;
            if (!snap->reason.empty()) anchor.hostCaptureStatus = "failed: " + snap->reason;
            else if (snap->bytes.size() < 400) anchor.hostCaptureStatus = "failed: only " + std::to_string(snap->bytes.size()) + " bytes";
            else {
                anchor.hostCaptureStatus = "ok";
                process_water_bytes(anchor, snap->bytes);
                if (anchor.valid) anchor.updatedFrame = s.frameSerial;
            }
        }

        if (!snap->reason.empty() || snap->bytes.empty()) continue;
        update_tracked_binding(sd.tracked, p->vs->hash, r.set, r.binding, r.element, snap->bytes, s.frameSerial);
        // snap and its bytes are not retained past this call - only the
        // derived MatrixCandidate + small rolling counters survive in `tracked`.
    }
}
} // namespace depth_probe::discovery

namespace depth_probe {
void update_camera_telemetry(VkDevice device, OverlayUniform& u, uint64_t sceneDepthImage) {
    using namespace discovery;
    std::lock_guard lock(mutex); // sd is also written at record/submit time under this lock
    auto& s = device_state(device);
    auto& sd = scene_debugger_state(s);
    auto& hud = camera_hud_input();

    if (hud.recording && !sd.telemetry.active) start_camera_telemetry_session(sd.telemetry, s.frameSerial);
    else if (!hud.recording && sd.telemetry.active) stop_camera_telemetry_session(sd);

    if (hud.take_scan_request()) {
        configure_memory_source(sd.memory);
        if (!sd.memory.enabled) log_line("[MEM-CAMERA-SCAN] not started: DEPTH_PROBE_MEM_CAMERA=0");
        else {
            if (!sd.memory.scanner) sd.memory.scanner = std::make_unique<MemoryCameraScanner>();
            log_line(sd.memory.scanner->start(sd.memory.layout) ? "[MEM-CAMERA-SCAN] started" : "[MEM-CAMERA-SCAN] already running");
        }
    }
    if (scene_debugger_active(s)) {
        // Once per present, independent of whether any draw was snapshotted this frame.
        scene_debugger_poll(s);
        update_memory_camera(sd, s.frameSerial);
        sd.cameras = recompute_camera_candidates(sd.tracked, sd.shaderDrawsCumulative, sd.cameras);
        publish_water_anchor(sd);
        publish_memory_candidate(sd);
        if (sd.cameraIndex >= sd.cameras.size()) sd.cameraIndex = sd.cameras.empty() ? 0 : uint32_t(sd.cameras.size() - 1);
    }

    DerivedCamera active;
    const bool haveActive = active_scene_camera(sd.cameras, active);
    const auto display = select_display_camera(sd, hud.gizmoSource, haveActive, active);
    update_gizmo_lock(sd.gizmoLock, hud.gizmoLocked, display);

    if (const auto marker = hud.take_marker(); marker && sd.telemetry.active) write_marker(sd.telemetry, marker_label(marker), s.frameSerial);

    sd.sceneDepthImage = sceneDepthImage;
    if (hud.fogMode || s.fog) fog_state(s).sceneDepthImage = sceneDepthImage;
    const auto recon = build_reconstruction(sd, s.frameSerial, sceneDepthImage);
    populate_recon_uniform(u, hud.reconMode, recon, sd.gizmoLock, display);
    if (hud.reconMode && (hud.reconMode != sd.lastReconMode || recon.valid != sd.lastReconValid || recon.depthMatches != sd.lastReconDepthMatches)) {
        log_line(std::string("[DEPTH-RECON] mode=") + recon_mode_name(hud.reconMode) + " valid=" + std::to_string(recon.valid) +
                 " reason=\"" + recon.reason + "\" sceneDepth=" + hex32(sceneDepthImage) + " worldTarget=" + hex32(sd.world.depthImage) +
                 " viewport=" + std::to_string(sd.world.viewport.x) + "," + std::to_string(sd.world.viewport.y) + "," + std::to_string(sd.world.viewport.width) + "," +
                 std::to_string(sd.world.viewport.height) + " depth=" + std::to_string(sd.world.viewport.minDepth) + ".." + std::to_string(sd.world.viewport.maxDepth));
        sd.lastReconValid = recon.valid;
        sd.lastReconDepthMatches = recon.depthMatches;
    }
    sd.lastReconMode = hud.reconMode;
    sd.world.snapshotPending = true;

    u.hud[0] = hud.panel_visible() ? 1u : 0u;
    if (u.hud[0]) populate_hud_text(u, sd, display, s.frameSerial, hud, recon, sceneDepthImage, s.fog.get());
    else std::fill(std::begin(u.hudText), std::end(u.hudText), 0u);

    populate_gizmo_uniform(u, hud.gizmoVisible, display, sd.gizmoLock);
    populate_light_gizmo(u, display, s.fog.get());
    populate_point_lights(s, u, display, s.fog.get());
    {   // the game's own fog / shadow filter: with the fog off too (they need no fog, only the settings)
        const auto& settings = fog_settings_now(s);
        game_fog_present(settings.gameFogDistance, s.frameSerial, settings.shadowSoftness);
        soft_shadows_present(settings.softShadows, settings.shadowFade, settings.shadowFadeDistance, settings.indoorShadows, s.frameSerial);
    }

    if (sd.telemetry.active) write_camera_telemetry_sample(sd, s.frameSerial);
}
}
