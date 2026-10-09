#pragma once
#include "camera_analyzer.hpp"
#include "camera_math.hpp"
#include "discovery_internal.hpp"
#include "inspector_uniform_gpu.hpp"
#include "memory_camera.hpp"
#include "memory_camera_scan.hpp"
#include "depth_reconstruction.hpp"
#include <fstream>

// Human-oriented "Scene Debugger": automatic correlation and ranking on top
// of the existing Discovery/Inspector capture (draw/pipeline/resource
// tracking) and MatrixAnalyzer/CameraAnalyzer. The raw per-buffer Inspector
// pages remain available under Advanced/Raw; this layer exists so the user
// does not have to manually read descriptor sets/offsets/matrices to find a
// camera.
//
// Scope note: this slice covers automatic camera-candidate discovery with
// real cross-frame AND within-frame (multi-draw) evidence, plus a
// first-class "verified water anchor" derived from the repository's
// independently-verified WoW water P/V layout (see camera_math.hpp),
// captured via BOTH the host-mapped path and a dedicated GPU readback
// (see schedule_gpu_uniform_read in inspector_uniform_gpu.hpp) since real
// WoW's water UBO is not expected to be host-mappable. It deliberately does
// NOT yet include depth/lighting/shadow/fog ranking, process-memory
// cross-validation, guided tests, snapshot A/B, general cross-shader camera
// grouping, visualization or persistent knowledge - those are later stages
// of the same roadmap and are reported as not-yet-implemented rather than
// faked.
namespace depth_probe {
struct OverlayUniform;
// Called once per frame (from update_overlay_uniform, alongside
// populate_inspector_menu) regardless of whether F10 is open - drives the
// Camera Telemetry HUD (F9) and its recording session (Ctrl+F9) purely from
// camera_hud_input()'s state, independent of the Inspector menu.
// `sceneDepthImage`: the depth image the overlay samples (depth_selection.cpp), compared with the world draws' target.
void update_camera_telemetry(VkDevice, OverlayUniform&, uint64_t sceneDepthImage);
}
namespace depth_probe::discovery {

// The known WoW water VS UBO (set 2 / binding 0, Projection at byte 256,
// View at byte 336 - see docs/water.md and shaders/water_camera.frag)
// derived directly via the verified convention, independent of the generic
// per-shader CameraCandidate ranking. Every field below is a diagnostic
// stage, in pipeline order, so a failure can be pinpointed exactly (F10 ->
// Scene Debugger -> Overview shows all of them) rather than the anchor just
// silently staying empty.
struct WaterCameraAnchor {
    bool vsSeen = false, fsSeen = false, bindingFound = false;
    uint64_t bufferRange = 0;
    std::string hostCaptureStatus = "not attempted";
    std::string gpuReadbackStatus = "not attempted";
    size_t bytesCaptured = 0;
    bool projAvailable = false, viewAvailable = false;
    std::string cameraMathStatus = "not run";
    uint32_t set = 0, binding = 0, element = 0;

    bool valid = false;
    uint64_t updatedFrame = 0;                        // frameSerial of the last successful readback
    DerivedCamera camera;
};

// A recording session started by Ctrl+F9 (see camera_hud_input.hpp) -
// independent of whether the HUD is visible or F10 is open. Files are
// opened once at session start and appended every sampled frame; nothing
// here is re-opened/re-created mid-session. summary.txt is written once, at
// session end.
struct CameraTelemetrySession {
    bool active = false;
    std::string directory;
    std::ofstream csv, jsonl;
    uint64_t samples = 0, startedFrame = 0;
    bool hasPrevious = false;
    Vec3 previousPosition{}, previousForward{};
    std::string lastMarker;
};

// A CGCamera-shaped struct found by the Ctrl+F8 scan, re-read every frame
// while the HUD/gizmo is on so the live one can be told apart from copies.
struct MemoryScanTrack {
    uint64_t address = 0;
    RawMemoryCamera last;
    bool valid = false;
    uint32_t changes = 0;               // frames whose facing/position differed from the previous read
    float waterPosition = -1, waterForwardDeg = -1; // vs a fresh water camera, identity conversion
};

// Second, independent camera source: WoW's own camera object read from
// process memory (see memory_camera.hpp). Layout, facing-row meaning and FOV
// axis start as hypotheses; the time-paired comparison with the water camera
// (fed from scene_debugger_poll) is what can confirm or correct them.
struct MemoryCameraSource {
    bool enabled = true;                  // DEPTH_PROBE_MEM_CAMERA=0 disables every read
    bool configured = false;
    WowCameraLayout layout;
    SignedPermutation rows{{1, 3, -2}};   // (forward, up, right) <- facing rows
    bool rowsFromEnv = false, rowsFromComparison = false;
    FovAxis fovAxis = FovAxis::Vertical;
    bool fovAxisFromEnv = false;
    // Synthesized fovY = raw fov * fovScale. 0.6 is what the first real WoW run measured (raw 1.5708,
    // water fovY 54.00); replaced by the live ratio whenever a water pair arrives.
    float fovScale = .6f;
    bool fovScaleFromEnv = false;
    std::string fovMatch = "UNKNOWN";     // "X" (raw == water fovX) or "Y X<ratio>" (water fovY / raw)

    RawMemoryCamera raw;                  // latest read that produced `camera` (or failed)
    std::string origin;                   // CHAIN, ENV or SCAN
    std::string chainStatus = "not read"; // the configured chain/address on its own, even when SCAN is used
    std::string status = "not read";      // empty when `camera` is valid
    bool valid = false;
    DerivedCamera camera;
    std::string projectionSource;         // WATER (last verified water projection) or SYNTH
    uint64_t readFrame = 0;
    CameraEvidenceLevel level = CameraEvidenceLevel::MemoryCandidate;

    // Time pairing: the memory camera read when the water draw was recorded,
    // compared once that draw's UBO readback completes.
    bool pendingPairValid = false;
    RawMemoryCamera pendingPair;
    CameraSourceComparator comparator;
    uint64_t evidenceAddress = 0;         // counters below apply to this struct address only
    uint32_t pairedSamples = 0, matchedSamples = 0;
    bool lastPairMatched = false;
    float recentMatchRate = 0;            // decayed share of recent pairs that matched
    uint64_t lastPairFrame = 0;
    bool haveFirstMatch = false;
    Vec3 firstMatchForward{};
    float forwardSpanDeg = 0;

    bool haveWaterProjection = false;
    Mat4Cols waterProjection;

    std::unique_ptr<MemoryCameraScanner> scanner;
    uint64_t scanGenerationSeen = 0;
    std::vector<MemoryScanTrack> tracks;
    int scanSelected = -1;
    std::vector<MemoryCameraChain> scanChains;
};

// F7: the gizmo frozen at one world position/orientation; only the view
// projection keeps following the live camera, so a correct camera keeps it
// glued to the world like a real object.
struct GizmoLock {
    bool active = false;
    std::string source;
    Vec3 anchor, right, up, forward;
};

// What the game's world draws looked like this frame (record time): the viewport (half-pixel offset,
// negative height, 0..0.94 depth range) and depth attachment that per-pixel reconstruction must invert,
// plus the memory camera read while they were recorded.
struct WorldFrame {
    bool seen = false;
    uint64_t frame = 0;
    WorldViewport viewport;
    uint64_t depthImage = 0;
    uint32_t draws = 0;
    bool snapshotPending = true;         // re-armed every present
    bool haveSnapshot = false;
    RawMemoryCamera snapshot;
};

struct SceneDebugger {
    uint64_t lastFrameSerial = UINT64_MAX;
    std::map<uint64_t, uint32_t> sampledShadersThisFrame; // vsHash -> draws sampled this frame so far, capped
    std::map<uint64_t, uint64_t> shaderDrawsCumulative;   // vsHash -> draws observed while the Camera page has been open (session-wide)
    size_t uniformCaptureBytes = 0;                       // reset every frame; own budget, independent of the Inspector's
    std::vector<TrackedMatrixBinding> tracked;            // bounded, see maxTrackedBindings in camera_analyzer.cpp
    std::vector<CameraCandidate> cameras;                 // ranked, recomputed once per frame while active
    uint32_t cameraIndex = 0;
    uint64_t totalDrawsThisFrame = 0, totalDrawsLastFrame = 0;

    WaterCameraAnchor waterAnchor;
    std::shared_ptr<GpuUniformRead> pendingWaterRead; // in-flight GPU readback, polled once per frame
    uint64_t nextWaterReadbackFrame = 0;              // once valid, re-check periodically rather than every frame

    CameraTelemetrySession telemetry;
    MemoryCameraSource memory;
    GizmoLock gizmoLock;
    WorldFrame world;
    bool lastReconValid = false, lastReconDepthMatches = false; // log only on change
    uint32_t lastReconMode = 0;
    uint64_t sceneDepthImage = 0;   // the overlay's selected scene depth, refreshed every present
};

SceneDebugger& scene_debugger_state(Device&);
}
