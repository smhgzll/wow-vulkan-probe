#pragma once
#include "camera_math.hpp"
#include "matrix_analyzer.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Pure temporal/cross-draw analysis on top of MatrixAnalyzer's single-snapshot
// candidates - no Vulkan or Device dependency, so it is unit-testable on its
// own (see tests/camera_analyzer_test.cpp). scene_debugger.cpp is the only
// caller; it owns the Vulkan-side capture (which buffers to read, when).
namespace depth_probe::discovery {

// How a CameraCandidate's identity was established. Ordered roughly by
// strength of evidence. The Memory* / Verified levels are produced by the
// process-memory camera (memory_camera.hpp, scene_debugger.cpp) only when
// the evidence named in each comment actually holds.
enum class CameraEvidenceLevel {
    Heuristic,          // single-snapshot MatrixAnalyzer structure only
    GpuTemporal,        // stable/changing behavior observed across frames and/or draws
    GpuVerifiedWater,   // matches the repository's independently-verified WoW water P/V layout exactly
    MemoryCandidate,    // a CGCamera-shaped memory struct passes every sanity check
    MemoryGpuMatch,     // memory camera agrees with the GPU water camera under a stable detected conversion
    Verified,           // that agreement held over many time-paired samples with real camera rotation
};
constexpr bool is_generic_gpu_level(CameraEvidenceLevel l) { return l == CameraEvidenceLevel::Heuristic || l == CameraEvidenceLevel::GpuTemporal; }

// One 4x4 candidate window at a fixed (shader, set, binding, element, byte
// offset) identity, tracked across frames while the Camera Candidates page is
// open. Keyed by vsHash rather than a buffer handle because DXVK rotates
// buffer/pool handles every frame while a shader's own UBO layout is stable.
struct TrackedMatrixBinding {
    uint64_t vsHash = 0;
    uint32_t set = 0, binding = 0, element = 0;
    size_t offset = 0;

    // Cross-frame: does this binding's representative sample change from one
    // observed frame to the next? A camera View should; a Projection mostly
    // shouldn't; a static prop's World matrix might never.
    uint32_t framesObserved = 0;
    uint32_t sameFrames = 0;
    uint32_t rotationChangedFrames = 0;
    uint32_t translationChangedFrames = 0;
    bool hasCrossFrameReference = false;
    float crossFrameReference[16]{};

    // Within-frame: when several draws sharing this shader are sampled in
    // the SAME frame (see scene_debugger.cpp's per-shader sample cap), does
    // this offset read identically across them? A camera/global transform
    // should be shared by every draw in a frame; an object/world transform
    // usually differs draw to draw. This is accumulated across the whole
    // session (not reset per frame) because a handful of same-frame samples
    // is not statistically meaningful on its own.
    uint64_t withinFrameSerial = UINT64_MAX; // which frame `withinFrameReference` was taken in
    float withinFrameReference[16]{};
    uint32_t withinFrameSamplesTotal = 0;
    uint32_t withinFrameIdenticalTotal = 0;
    uint32_t withinFrameDifferentTotal = 0;

    MatrixCandidate lastCandidate;
};

// A ranked hypothesis that a (projection-like, view-like) pair of bindings
// from one shader is "the" scene camera. Either half may be missing (a
// shader might expose only one, or bundle both into a combined
// WorldViewProjection matrix MatrixAnalyzer cannot yet separate).
struct CameraCandidate {
    uint64_t key = 0;     // stable across recomputes so user verdicts persist
    uint64_t vsHash = 0;
    bool hasProj = false; uint32_t projSet = 0, projBinding = 0, projElement = 0; uint32_t projOffset = 0;
    bool hasView = false; uint32_t viewSet = 0, viewBinding = 0, viewElement = 0; uint32_t viewOffset = 0;
    float confidence = 0.f; // 0..1, see evidence for the additive breakdown
    CameraEvidenceLevel level = CameraEvidenceLevel::Heuristic;
    std::vector<std::string> evidence;
    int verdict = 0;        // 0 unset, 1 accepted, -1 rejected (set from the Camera Candidates page)
    float lastProj[16]{}, lastView[16]{};
    uint64_t drawsObserved = 0;
    uint32_t framesObserved = 0;

    // Populated only for candidates whose camera was directly derived (right
    // now: the verified water anchor via publish_verified_camera; the
    // generic per-shader path does not derive a concrete camera yet since
    // its row/column-major convention isn't independently known).
    bool hasDerivedCamera = false;
    DerivedCamera derivedCamera;
};

// Runs MatrixAnalyzer on `bytes` (up to `maxCandidatesPerBuffer` windows, not
// just the single best one - a UBO can and does contain more than one
// structurally interesting matrix, e.g. the real water UBO's Projection at
// +256 AND View at +336) and folds each result above the confidence floor
// into the matching entry of `tracked` (creating one if new, up to a bounded
// count), updating both its cross-frame and within-frame statistics.
// `frameSerial` distinguishes "another sample from the frame already in
// progress" (within-frame comparison) from "a new frame started" (which
// finalizes the outgoing frame's representative sample into the cross-frame
// comparison).
void update_tracked_binding(std::vector<TrackedMatrixBinding>& tracked, uint64_t vsHash, uint32_t set, uint32_t binding, uint32_t element, const std::vector<uint8_t>& bytes, uint64_t frameSerial, size_t maxCandidatesPerBuffer = 8);

// Re-derives a ranked camera-candidate list from the current tracked
// bindings and per-shader draw counts. `previous` supplies user verdicts
// (accept/reject) to carry forward, matched by each candidate's stable key.
std::vector<CameraCandidate> recompute_camera_candidates(const std::vector<TrackedMatrixBinding>& tracked, const std::map<uint64_t, uint64_t>& drawsPerShader, const std::vector<CameraCandidate>& previous);

// Directly creates (or replaces) the ranked-list entry for a camera that was
// derived WITHOUT going through update_tracked_binding/recompute_camera_
// candidates at all - i.e. a source whose byte layout and math convention
// are independently verified (currently: the water anchor, see
// camera_math.hpp's derive_known_wow_camera). This does NOT require any
// tracked bindings, frame history or draw count: a successful verified
// derivation is already stronger evidence than the heuristic path produces
// even after many observed frames, so it is published immediately at
// CameraEvidenceLevel::GpuVerifiedWater and confidence 1.0, inserted at the
// FRONT of `cameras` (heuristic candidates remain below it, untouched). Any
// existing entry for `vsHash` is removed first (this is authoritative, not
// merged with whatever the generic per-shader grouping independently
// found) - but its user verdict (accept/reject), if any, is preserved.
void publish_verified_camera(std::vector<CameraCandidate>& cameras, uint64_t vsHash, uint64_t fsHash, uint32_t set, uint32_t binding, uint32_t element, uint32_t projOffset, uint32_t viewOffset, const DerivedCamera& camera);

// The single camera future consumers (and the UI's "Active Scene Camera")
// should read, regardless of which source produced it: prefers a candidate
// the user explicitly accepted, else the highest-ranked candidate that has
// a concrete derived camera. Returns false (out untouched) if nothing
// qualifies yet - most candidates don't have a derived camera at all today,
// only ones published via publish_verified_camera do.
bool active_scene_camera(const std::vector<CameraCandidate>& cameras, DerivedCamera& out);

// Creates or replaces the single process-memory camera entry (vsHash 0).
// Placed first when its level is MemoryGpuMatch or better, otherwise right
// after the verified water entry: a sanity-only memory struct is weaker
// evidence than the verified water layout. User verdicts are preserved.
void publish_memory_camera(std::vector<CameraCandidate>& cameras, CameraEvidenceLevel level, const DerivedCamera& camera, std::vector<std::string> evidence);
}
