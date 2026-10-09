#include "camera_analyzer.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace depth_probe;
using namespace depth_probe::discovery;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)

namespace {
std::vector<uint8_t> bytes_of(const std::vector<float>& floats) {
    std::vector<uint8_t> out(floats.size() * 4);
    std::memcpy(out.data(), floats.data(), out.size());
    return out;
}
// Same shapes used in matrix_analyzer_test.cpp: a realistic (non-axis-aligned)
// View-like matrix and a realistic Projection-like matrix, so this test
// exercises real MatrixAnalyzer classifications, not hand-picked candidates.
std::vector<float> rotation_matrix(float angleRad, float tx, float ty, float tz) {
    const float c = std::cos(angleRad), s = std::sin(angleRad);
    return {c,0,s,tx, 0,1,0,ty, -s,0,c,tz, 0,0,0,1};
}
std::vector<float> projection_matrix() {
    return {1.732f,0,0,0, 0,2.309f,0,0, 0,0,1.00025f,-0.20005f, 0,0,1,0};
}
bool has_evidence_prefix(const CameraCandidate& c, const std::string& prefix) {
    for (auto& e : c.evidence) if (e.rfind(prefix, 0) == 0) return true;
    return false;
}
}

int main() {
    // REGRESSION: the real WoW water UBO (docs/water.md, verified against
    // shaders/water_camera.frag) has a Projection matrix at byte 256 AND a
    // View matrix at byte 336 in the SAME captured buffer. A single
    // update_tracked_binding() call on that buffer must produce tracked
    // entries at BOTH offsets - not just the single highest-confidence
    // window. (This was broken: update_tracked_binding called
    // find_matrix_candidates(bytes, 1) and looked only at .front(), so with
    // Projection's fixed 0.5 confidence always beating View's ~0.25-0.4,
    // the View window at +336 was silently never tracked.)
    {
        std::vector<uint8_t> ubo(400, 0);
        auto proj = bytes_of(projection_matrix()), view = bytes_of(rotation_matrix(.4f, 10, 20, 30));
        std::copy(proj.begin(), proj.end(), ubo.begin() + 256);
        std::copy(view.begin(), view.end(), ubo.begin() + 336);
        std::vector<TrackedMatrixBinding> tracked;
        update_tracked_binding(tracked, /*vsHash*/ 0x2fce10e0177c7507ull, 2, 0, 0, ubo, /*frameSerial*/ 0);
        bool sawProj = false, sawView = false;
        for (auto& t : tracked) {
            if (t.offset == 256) sawProj = true;
            if (t.offset == 336) sawView = true;
        }
        CHECK(sawProj && sawView);
    }

    // No tracked bindings at all -> no candidates.
    { std::vector<TrackedMatrixBinding> tracked; CHECK(recompute_camera_candidates(tracked, {}, {}).empty()); }

    // A single call below MatrixAnalyzer's confidence floor must not create
    // a tracked entry (garbage/degenerate buffers should not pollute the list).
    {
        std::vector<TrackedMatrixBinding> tracked;
        std::vector<float> zero(16, 0.f); // degenerate, confidence ~0.05
        update_tracked_binding(tracked, 1, 2, 0, 0, bytes_of(zero), 0);
        CHECK(tracked.empty());
    }

    // update_tracked_binding across several simulated frames (one sample per
    // frame, distinct frameSerial each time): a stationary rotation (same
    // bytes every call) must accumulate sameFrames and zero rotation/
    // translation change. framesObserved/sameFrames lag one frame behind the
    // call count by design - a frame's representative sample is only folded
    // into the cross-frame comparison once the NEXT frame's first sample
    // proves that frame is over (see camera_analyzer.cpp).
    {
        std::vector<TrackedMatrixBinding> tracked;
        auto rot = bytes_of(rotation_matrix(0.3f, 10, 20, 30));
        for (uint64_t i = 0; i < 12; ++i) update_tracked_binding(tracked, /*vsHash*/42, 2, 0, 0, rot, i);
        CHECK(tracked.size() == 1);
        CHECK(tracked[0].framesObserved == 11);
        CHECK(tracked[0].sameFrames == 10);
        CHECK(tracked[0].rotationChangedFrames == 0 && tracked[0].translationChangedFrames == 0);
    }

    // A rotation that visibly changes every frame (simulating the camera
    // being rotated) must register rotationChangedFrames.
    {
        std::vector<TrackedMatrixBinding> tracked;
        for (uint64_t i = 0; i < 12; ++i) update_tracked_binding(tracked, 42, 2, 0, 0, bytes_of(rotation_matrix(0.1f * float(i), 10, 20, 30)), i);
        CHECK(tracked.size() == 1);
        CHECK(tracked[0].rotationChangedFrames >= 9);
    }

    // Within-frame: several draws sampled in the SAME frame (same shader,
    // same binding) with an IDENTICAL matrix must accumulate
    // withinFrameIdentical - this is the "shared by many draws in one frame"
    // signal a camera/global transform should show, distinct from a
    // per-object World matrix.
    {
        std::vector<TrackedMatrixBinding> tracked;
        for (uint64_t frame = 0; frame < 10; ++frame)
            for (int draw = 0; draw < 4; ++draw)
                update_tracked_binding(tracked, 250, 0, 1, 0, bytes_of(rotation_matrix(0.05f * float(frame), 1, 2, 3)), frame);
        CHECK(tracked.size() == 1);
        CHECK(tracked[0].withinFrameSamplesTotal >= 4);
        CHECK(tracked[0].withinFrameDifferentTotal == 0);
        CHECK(tracked[0].withinFrameIdenticalTotal == tracked[0].withinFrameSamplesTotal);
    }

    // Within-frame: several draws sampled in the SAME frame with a DIFFERENT
    // matrix each time (simulating distinct per-object World matrices) must
    // accumulate withinFrameDifferent instead.
    {
        std::vector<TrackedMatrixBinding> tracked;
        for (uint64_t frame = 0; frame < 10; ++frame)
            for (int draw = 0; draw < 4; ++draw)
                update_tracked_binding(tracked, 251, 0, 1, 0, bytes_of(rotation_matrix(0.05f * float(frame) + float(draw), 1, 2, 3)), frame);
        CHECK(tracked.size() == 1);
        CHECK(tracked[0].withinFrameDifferentTotal > 0);
        CHECK(tracked[0].withinFrameIdenticalTotal == 0);
    }

    // Scoring must actually use within-frame globality: a view-like binding
    // shared identically across draws in each frame must outscore an
    // otherwise-equivalent one that differs draw to draw, and say why.
    {
        std::vector<TrackedMatrixBinding> shared, perObject;
        for (uint64_t frame = 0; frame < 10; ++frame)
            for (int draw = 0; draw < 4; ++draw) {
                update_tracked_binding(shared, 300, 0, 1, 0, bytes_of(rotation_matrix(0.05f * float(frame), 1, 2, 3)), frame);
                update_tracked_binding(perObject, 301, 0, 1, 0, bytes_of(rotation_matrix(0.05f * float(frame) + float(draw), 1, 2, 3)), frame);
            }
        auto sharedCam = recompute_camera_candidates(shared, {}, {});
        auto perObjectCam = recompute_camera_candidates(perObject, {}, {});
        CHECK(sharedCam.size() == 1 && perObjectCam.size() == 1);
        CHECK(has_evidence_prefix(sharedCam.front(), "+20 identical across"));
        bool sawPenalty = false; for (auto& e : perObjectCam.front().evidence) if (e.rfind("-", 0) == 0 && e.find("differs across") != std::string::npos) sawPenalty = true;
        CHECK(sawPenalty);
        CHECK(sharedCam.front().confidence > perObjectCam.front().confidence);
    }

    // Candidate key must encode set/binding/element, not just vsHash+offset -
    // two candidates sharing a vsHash and offset but from different
    // descriptor bindings must not collide (the previous key only mixed
    // vsHash + offsets).
    {
        std::vector<TrackedMatrixBinding> a, b;
        for (uint64_t i = 0; i < 10; ++i) {
            update_tracked_binding(a, 400, /*set*/0, /*binding*/0, 0, bytes_of(projection_matrix()), i);
            update_tracked_binding(b, 400, /*set*/3, /*binding*/7, 0, bytes_of(projection_matrix()), i);
        }
        auto camsA = recompute_camera_candidates(a, {}, {});
        auto camsB = recompute_camera_candidates(b, {}, {});
        CHECK(camsA.size() == 1 && camsB.size() == 1);
        CHECK(camsA.front().projSet == 0 && camsA.front().projBinding == 0);
        CHECK(camsB.front().projSet == 3 && camsB.front().projBinding == 7);
        CHECK(camsA.front().key != camsB.front().key);
    }

    // Camera candidate formation: a shader exposing both a Projection-like
    // and a View-like binding, with the view changing over time and the
    // shader used by many draws, should score clearly higher than either
    // half alone and should carry both offsets.
    {
        std::vector<TrackedMatrixBinding> tracked;
        for (uint64_t i = 0; i < 20; ++i) {
            update_tracked_binding(tracked, 7, 0, 0, 0, bytes_of(projection_matrix()), i);              // stable projection
            update_tracked_binding(tracked, 7, 0, 1, 0, bytes_of(rotation_matrix(0.05f * float(i), 1, 2, 3)), i); // changing view
        }
        std::map<uint64_t,uint64_t> draws{{7, 800}};
        auto cams = recompute_camera_candidates(tracked, draws, {});
        CHECK(cams.size() == 1);
        const auto& c = cams.front();
        CHECK(c.hasProj && c.hasView);
        CHECK(c.projBinding == 0 && c.viewBinding == 1);
        CHECK(c.drawsObserved == 800);
        CHECK(c.confidence > .5f); // proj + changing view + paired + heavily shared should stack up
        CHECK(has_evidence_prefix(c, "+10 both a projection-like"));
        CHECK(has_evidence_prefix(c, "+15 shader used by 800 observed draws") || has_evidence_prefix(c, "+14 shader used by 800 observed draws"));

        // A projection-only candidate (no paired view) from a different
        // shader must score lower and lack the pairing bonus.
        std::vector<TrackedMatrixBinding> projOnly;
        for (uint64_t i = 0; i < 20; ++i) update_tracked_binding(projOnly, 9, 0, 0, 0, bytes_of(projection_matrix()), i);
        auto lone = recompute_camera_candidates(projOnly, {{9, 5}}, {});
        CHECK(lone.size() == 1 && lone.front().hasProj && !lone.front().hasView);
        CHECK(lone.front().confidence < c.confidence);
    }

    // A view-like binding that never changes should score lower than one
    // that does, and should say so explicitly (a static object transform is
    // a real alternative hypothesis, not swept under the rug).
    {
        std::vector<TrackedMatrixBinding> still, moving;
        for (uint64_t i = 0; i < 20; ++i) {
            update_tracked_binding(still, 11, 0, 1, 0, bytes_of(rotation_matrix(0.3f, 5, 5, 5)), i); // identical every frame
            update_tracked_binding(moving, 11, 0, 1, 0, bytes_of(rotation_matrix(0.05f * float(i), 5, 5, 5)), i);
        }
        auto stillCam = recompute_camera_candidates(still, {}, {});
        auto movingCam = recompute_camera_candidates(moving, {}, {});
        CHECK(stillCam.size() == 1 && movingCam.size() == 1);
        CHECK(has_evidence_prefix(stillCam.front(), "+0 never changed"));
        CHECK(movingCam.front().confidence > stillCam.front().confidence);
    }

    // Verdict persistence: accepting a candidate must survive a recompute
    // that reproduces the same (vsHash, offsets) key, and accepted
    // candidates must sort first regardless of confidence.
    {
        std::vector<TrackedMatrixBinding> weak, strong;
        for (uint64_t i = 0; i < 20; ++i) {
            update_tracked_binding(weak, 100, 0, 0, 0, bytes_of(projection_matrix()), i);
            update_tracked_binding(strong, 200, 0, 0, 0, bytes_of(projection_matrix()), i);
            update_tracked_binding(strong, 200, 0, 1, 0, bytes_of(rotation_matrix(0.05f * float(i), 1, 1, 1)), i);
        }
        std::vector<TrackedMatrixBinding> all = weak; all.insert(all.end(), strong.begin(), strong.end());
        auto first = recompute_camera_candidates(all, {{200, 500}}, {});
        CHECK(first.size() == 2);
        CHECK(first.front().vsHash == 200); // stronger candidate (proj+view+draws) ranks first unaccepted
        // Accept the weaker one, then recompute from the same tracked state.
        for (auto& c : first) if (c.vsHash == 100) c.verdict = 1;
        auto second = recompute_camera_candidates(all, {{200, 500}}, first);
        CHECK(second.front().vsHash == 100); // accepted candidate now pinned first despite lower confidence
        CHECK(second.front().verdict == 1);
    }

    // REGRESSION (round 3): a successfully-derived verified-anchor camera
    // (water, captured via GPU readback since real WoW's water UBO is not
    // host-mappable - see scene_debugger.cpp) was reaching only the
    // diagnostic WaterCameraAnchor state and never Camera Candidates, because
    // the old apply_water_anchor() only TAGGED an existing tracked-bindings-
    // derived candidate rather than creating one. publish_verified_camera
    // must create the candidate directly, with NO tracked bindings, frame
    // history or draw count involved at all - a verified derivation is
    // already stronger evidence than the heuristic path even after 8 frames.
    auto plausible_camera = []() {
        DerivedCamera cam; cam.valid = true;
        cam.position = {-8921.42f, 632.11f, 97.23f};
        cam.forward = {0.f, 0.f, 1.f}; cam.up = {0.f, 1.f, 0.f}; cam.right = {1.f, 0.f, 0.f};
        cam.fovY = 67.38f; cam.aspect = .75f; cam.nearPlane = 1.f; cam.farPlane = 101.f;
        return cam;
    };
    {
        std::vector<CameraCandidate> cameras; // empty - no generic tracking involved at all
        const auto cam = plausible_camera();
        publish_verified_camera(cameras, 0x2fce10e0177c7507ull, 0x6e0413111ae30ee9ull, 2, 0, 0, 256, 336, cam);
        CHECK(cameras.size() >= 1);
        CHECK(cameras.front().vsHash == 0x2fce10e0177c7507ull);
        CHECK(cameras.front().level == CameraEvidenceLevel::GpuVerifiedWater);
        CHECK(cameras.front().confidence >= .99f);
        CHECK(cameras.front().hasDerivedCamera && cameras.front().derivedCamera.valid);
        CHECK(std::fabs(cameras.front().derivedCamera.position.x - (-8921.42f)) < 1e-2f);
        CHECK(cameras.front().projOffset == 256 && cameras.front().viewOffset == 336);

        DerivedCamera out;
        CHECK(active_scene_camera(cameras, out)); // Active Scene Camera must become valid too
        CHECK(std::fabs(out.position.x - (-8921.42f)) < 1e-2f);
    }

    // Re-publishing (e.g. the periodic re-check once already verified) must
    // replace the entry in place, not duplicate it, and must preserve a
    // user's prior accept/reject verdict.
    {
        std::vector<CameraCandidate> cameras;
        const auto cam = plausible_camera();
        publish_verified_camera(cameras, 500, 501, 2, 0, 0, 256, 336, cam);
        CHECK(cameras.size() == 1);
        cameras.front().verdict = 1;
        publish_verified_camera(cameras, 500, 501, 2, 0, 0, 256, 336, cam);
        CHECK(cameras.size() == 1);
        CHECK(cameras.front().verdict == 1);
    }

    // Publishing into a list that already has generic heuristic candidates
    // must place the verified anchor at the FRONT and leave the others
    // below it, untouched - not replace the whole list.
    {
        std::vector<CameraCandidate> cameras;
        CameraCandidate heuristic; heuristic.vsHash = 999; heuristic.confidence = .3f; heuristic.level = CameraEvidenceLevel::Heuristic;
        cameras.push_back(heuristic);
        publish_verified_camera(cameras, 42, 43, 2, 0, 0, 256, 336, plausible_camera());
        CHECK(cameras.size() == 2);
        CHECK(cameras.front().vsHash == 42);
        CHECK(cameras.back().vsHash == 999);
    }

    // active_scene_camera: empty list / no derived camera anywhere -> false,
    // not a garbage/default camera silently treated as real.
    {
        std::vector<CameraCandidate> cameras;
        DerivedCamera out;
        CHECK(!active_scene_camera(cameras, out));
        CameraCandidate heuristicOnly; heuristicOnly.vsHash = 7; heuristicOnly.confidence = .9f; // no hasDerivedCamera
        cameras.push_back(heuristicOnly);
        CHECK(!active_scene_camera(cameras, out));
    }

    std::puts("PASS: multi-matrix-per-UBO tracking, within-frame globality (identical/different across draws), key distinguishes bindings, cross-frame temporal deltas, candidate pairing/scoring, static-vs-moving evidence, verdict persistence, verified-anchor publish (no temporal history required) and active scene camera");
}
