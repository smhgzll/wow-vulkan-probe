#include "camera_analyzer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace depth_probe::discovery {
namespace {
constexpr size_t maxTrackedBindings = 256;
constexpr uint32_t minFramesForTemporalEvidence = 8;   // avoid single-sample noise in "changed"/"stable" claims
constexpr uint32_t minWithinFrameSamplesForEvidence = 4;

std::string signed_pct(float score) { char b[16]; std::snprintf(b, sizeof b, "%+d", int(std::lround(score))); return b; }

// splitmix64-style mixing - a plain XOR/shift combine (the previous version)
// can collide when different (set,binding,element) pairs happen to produce
// the same offset; this folds the full binding identity in properly.
uint64_t hash_combine(uint64_t seed, uint64_t value) {
    value += 0x9e3779b97f4a7c15ull + seed;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}
uint64_t binding_key(bool has, uint32_t set, uint32_t binding, uint32_t element, uint32_t offset) {
    if (!has) return ~0ull; // distinct sentinel for "this half is absent"
    return (uint64_t(set) << 48) ^ (uint64_t(binding) << 32) ^ (uint64_t(element) << 16) ^ uint64_t(offset);
}
uint64_t combine_key(uint64_t vsHash,
                      bool hasProj, uint32_t projSet, uint32_t projBinding, uint32_t projElement, uint32_t projOffset,
                      bool hasView, uint32_t viewSet, uint32_t viewBinding, uint32_t viewElement, uint32_t viewOffset) {
    uint64_t k = hash_combine(0, vsHash);
    k = hash_combine(k, binding_key(hasProj, projSet, projBinding, projElement, projOffset));
    k = hash_combine(k, binding_key(hasView, viewSet, viewBinding, viewElement, viewOffset));
    return k;
}

// Rotation part = upper-left 3x3 (indices 0,1,2, 4,5,6, 8,9,10 of a
// memory-ordered 16-float read); translation part = the remaining row/column
// slots (3,7,11,12,13,14). Convention-agnostic on purpose - see
// matrix_analyzer.hpp for why row vs column major is not assumed here.
float rotation_delta(const float a[16], const float b[16]) {
    float d = 0.f;
    for (int i : {0,1,2,4,5,6,8,9,10}) d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
}
float translation_delta(const float a[16], const float b[16]) {
    float d = 0.f;
    for (int i : {3,7,11,12,13,14}) d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
}
bool identical16(const float a[16], const float b[16]) {
    for (int i = 0; i < 16; ++i) if (std::fabs(a[i] - b[i]) > 1e-5f) return false;
    return true;
}

bool is_projection(const TrackedMatrixBinding& t) { return t.lastCandidate.classification == "Projection (candidate)"; }
bool is_view_like(const TrackedMatrixBinding& t) { return t.lastCandidate.classification.rfind("View or World", 0) == 0 || t.lastCandidate.classification.rfind("Ambiguous", 0) == 0; }

// Prefers a candidate that is both structurally view-like AND, when enough
// within-frame samples exist, shared identically across the draws it was
// sampled from in the same frame - the strongest available signal that this
// is a global camera transform rather than a per-object World matrix.
float view_priority(const TrackedMatrixBinding& t) {
    float p = t.lastCandidate.confidence;
    if (t.withinFrameSamplesTotal >= minWithinFrameSamplesForEvidence)
        p += 0.5f * float(t.withinFrameIdenticalTotal) / float(t.withinFrameSamplesTotal);
    return p;
}
}

void update_tracked_binding(std::vector<TrackedMatrixBinding>& tracked, uint64_t vsHash, uint32_t set, uint32_t binding, uint32_t element, const std::vector<uint8_t>& bytes, uint64_t frameSerial, size_t maxCandidatesPerBuffer) {
    for (const auto& cand : find_matrix_candidates(bytes, maxCandidatesPerBuffer)) {
        if (cand.confidence < .1f) continue; // not worth tracking - would never contribute useful camera evidence

        TrackedMatrixBinding* found = nullptr;
        for (auto& t : tracked)
            if (t.vsHash == vsHash && t.set == set && t.binding == binding && t.element == element && t.offset == cand.offset) { found = &t; break; }
        if (!found) {
            if (tracked.size() >= maxTrackedBindings) continue;
            tracked.push_back({});
            found = &tracked.back();
            found->vsHash = vsHash; found->set = set; found->binding = binding; found->element = element; found->offset = cand.offset;
        }
        found->lastCandidate = cand;

        if (found->withinFrameSerial != frameSerial) {
            // First sample of this offset in a new frame: finalize the
            // outgoing frame's representative sample (its within-frame
            // reference) into the cross-frame comparison, then start a
            // fresh within-frame reference for the new frame.
            if (found->withinFrameSerial != UINT64_MAX) {
                ++found->framesObserved;
                if (found->hasCrossFrameReference) {
                    if (identical16(found->withinFrameReference, found->crossFrameReference)) ++found->sameFrames;
                    else {
                        if (rotation_delta(found->withinFrameReference, found->crossFrameReference) > .01f) ++found->rotationChangedFrames;
                        if (translation_delta(found->withinFrameReference, found->crossFrameReference) > .05f) ++found->translationChangedFrames;
                    }
                }
                std::copy(std::begin(found->withinFrameReference), std::end(found->withinFrameReference), found->crossFrameReference);
                found->hasCrossFrameReference = true;
            }
            found->withinFrameSerial = frameSerial;
            std::copy(std::begin(cand.raw), std::end(cand.raw), found->withinFrameReference);
        } else {
            ++found->withinFrameSamplesTotal;
            if (identical16(cand.raw, found->withinFrameReference)) ++found->withinFrameIdenticalTotal;
            else ++found->withinFrameDifferentTotal;
        }
    }
}

std::vector<CameraCandidate> recompute_camera_candidates(const std::vector<TrackedMatrixBinding>& tracked, const std::map<uint64_t, uint64_t>& drawsPerShader, const std::vector<CameraCandidate>& previous) {
    std::map<uint64_t, std::vector<const TrackedMatrixBinding*>> byShader;
    for (auto& t : tracked) byShader[t.vsHash].push_back(&t);

    std::vector<CameraCandidate> next;
    for (auto& [vsHash, bindings] : byShader) {
        const TrackedMatrixBinding* bestProj = nullptr; const TrackedMatrixBinding* bestView = nullptr;
        for (auto* t : bindings) {
            if (is_projection(*t) && (!bestProj || t->lastCandidate.confidence > bestProj->lastCandidate.confidence)) bestProj = t;
            if (is_view_like(*t) && (!bestView || view_priority(*t) > view_priority(*bestView))) bestView = t;
        }
        if (!bestProj && !bestView) continue;

        CameraCandidate c;
        c.vsHash = vsHash;
        c.hasProj = bestProj; c.hasView = bestView;
        if (bestProj) {
            c.projSet = bestProj->set; c.projBinding = bestProj->binding; c.projElement = bestProj->element; c.projOffset = uint32_t(bestProj->offset);
            std::copy(std::begin(bestProj->lastCandidate.raw), std::end(bestProj->lastCandidate.raw), c.lastProj);
        }
        if (bestView) {
            c.viewSet = bestView->set; c.viewBinding = bestView->binding; c.viewElement = bestView->element; c.viewOffset = uint32_t(bestView->offset);
            std::copy(std::begin(bestView->lastCandidate.raw), std::end(bestView->lastCandidate.raw), c.lastView);
        }
        c.key = combine_key(vsHash, c.hasProj, c.projSet, c.projBinding, c.projElement, c.projOffset, c.hasView, c.viewSet, c.viewBinding, c.viewElement, c.viewOffset);

        float score = 0.f;
        bool anyTemporalEvidence = false;
        if (bestProj) {
            const float contribution = std::min(30.f, 30.f * bestProj->lastCandidate.confidence / .6f);
            score += contribution;
            c.evidence.push_back(signed_pct(contribution) + " perspective (projection) structure found");
            if (bestProj->framesObserved >= minFramesForTemporalEvidence) {
                anyTemporalEvidence = true;
                const float stability = 1.f - float(bestProj->rotationChangedFrames + bestProj->translationChangedFrames) / float(bestProj->framesObserved);
                const float stabContribution = 15.f * std::max(0.f, stability);
                score += stabContribution;
                c.evidence.push_back(signed_pct(stabContribution) + " projection stayed stable across " + std::to_string(bestProj->framesObserved) + " observed frames");
            }
        }
        if (bestView) {
            const float contribution = std::min(25.f, 25.f * bestView->lastCandidate.confidence / .45f);
            score += contribution;
            c.evidence.push_back(signed_pct(contribution) + " rotation/translation (view) structure found");
            if (bestView->framesObserved >= minFramesForTemporalEvidence) {
                anyTemporalEvidence = true;
                const float changedFraction = float(bestView->rotationChangedFrames + bestView->translationChangedFrames) / float(bestView->framesObserved);
                const float changeContribution = 20.f * std::min(1.f, changedFraction * 3.f);
                score += changeContribution;
                if (changedFraction > .01f) c.evidence.push_back(signed_pct(changeContribution) + " changed across frames like a moving camera view, not a static object transform");
                else c.evidence.push_back("+0 never changed in " + std::to_string(bestView->framesObserved) + " observed frames - could be a static world matrix; run the Camera Test to be sure");
            }
            if (bestView->withinFrameSamplesTotal >= minWithinFrameSamplesForEvidence) {
                anyTemporalEvidence = true;
                const float globality = float(bestView->withinFrameIdenticalTotal) / float(bestView->withinFrameSamplesTotal);
                const int pct = int(globality * 100 + .5f);
                if (globality >= .7f) {
                    const float contrib2 = 20.f * globality;
                    score += contrib2;
                    c.evidence.push_back(signed_pct(contrib2) + " identical across " + std::to_string(bestView->withinFrameSamplesTotal) + " sampled draws within the same frame (" + std::to_string(pct) + "% - camera/global-transform-like)");
                } else if (globality < .3f) {
                    const float penalty = -20.f * (1.f - globality);
                    score += penalty;
                    c.evidence.push_back(signed_pct(penalty) + " differs across " + std::to_string(bestView->withinFrameSamplesTotal) + " sampled draws within the same frame (only " + std::to_string(pct) + "% identical - looks like a per-object World matrix, not a shared camera)");
                } else {
                    c.evidence.push_back(std::to_string(pct) + "% identical across " + std::to_string(bestView->withinFrameSamplesTotal) + " sampled draws within the same frame - inconclusive");
                }
            }
            if (bestView->lastCandidate.classification.rfind("Ambiguous", 0) == 0) {
                score -= 15.f;
                c.evidence.push_back("-15 structure is ambiguous (could be an axis-aligned rotation, not necessarily the camera view)");
            }
        }
        if (bestProj && bestView) { score += 10.f; c.evidence.push_back("+10 both a projection-like and a view-like binding found in the same shader"); }
        if (auto it = drawsPerShader.find(vsHash); it != drawsPerShader.end()) {
            c.drawsObserved = it->second;
            const float sharedContribution = std::min(15.f, 3.f * std::log2(float(c.drawsObserved + 1)));
            score += sharedContribution;
            c.evidence.push_back(signed_pct(sharedContribution) + " shader used by " + std::to_string(c.drawsObserved) + " observed draws (higher = more likely shared scene-wide, not a one-off object)");
        }
        c.confidence = std::clamp(score / 100.f, 0.f, 1.f);
        c.framesObserved = std::max(bestProj ? bestProj->framesObserved : 0u, bestView ? bestView->framesObserved : 0u);
        c.level = anyTemporalEvidence ? CameraEvidenceLevel::GpuTemporal : CameraEvidenceLevel::Heuristic;
        next.push_back(std::move(c));
    }

    for (auto& c : next) for (auto& old : previous) if (old.key == c.key && old.verdict) c.verdict = old.verdict;
    std::stable_sort(next.begin(), next.end(), [](const CameraCandidate& a, const CameraCandidate& b) {
        if ((a.verdict == 1) != (b.verdict == 1)) return a.verdict == 1;
        if ((a.verdict == -1) != (b.verdict == -1)) return a.verdict != -1;
        if (a.level != b.level) return a.level > b.level;
        return a.confidence > b.confidence;
    });
    return next;
}

void publish_verified_camera(std::vector<CameraCandidate>& cameras, uint64_t vsHash, uint64_t fsHash, uint32_t set, uint32_t binding, uint32_t element, uint32_t projOffset, uint32_t viewOffset, const DerivedCamera& camera) {
    int preservedVerdict = 0;
    for (auto it = cameras.begin(); it != cameras.end(); )
        if (it->vsHash == vsHash) { preservedVerdict = it->verdict; it = cameras.erase(it); }
        else ++it;

    CameraCandidate c;
    c.vsHash = vsHash;
    c.hasProj = true; c.projSet = set; c.projBinding = binding; c.projElement = element; c.projOffset = projOffset;
    c.hasView = true; c.viewSet = set; c.viewBinding = binding; c.viewElement = element; c.viewOffset = viewOffset;
    c.level = CameraEvidenceLevel::GpuVerifiedWater;
    c.confidence = 1.f;
    c.verdict = preservedVerdict;
    c.hasDerivedCamera = true;
    c.derivedCamera = camera;
    // A fixed, distinct salt so this never collides with combine_key's
    // output space above - there is only ever one verified-anchor entry per
    // vsHash, so it does not need offset/binding folded in the same way.
    c.key = hash_combine(vsHash, 0x57A7EA1C0417ull);

    c.evidence.push_back("VERIFIED WATER ANCHOR (not from the heuristic path)");
    char buf[192];
    std::snprintf(buf, sizeof buf, "position %.2f, %.2f, %.2f", double(camera.position.x), double(camera.position.y), double(camera.position.z));
    c.evidence.push_back(buf);
    std::snprintf(buf, sizeof buf, "forward %.3f, %.3f, %.3f  up %.3f, %.3f, %.3f  right %.3f, %.3f, %.3f",
                  double(camera.forward.x), double(camera.forward.y), double(camera.forward.z),
                  double(camera.up.x), double(camera.up.y), double(camera.up.z),
                  double(camera.right.x), double(camera.right.y), double(camera.right.z));
    c.evidence.push_back(buf);
    std::snprintf(buf, sizeof buf, "FOV %.2f deg, aspect %.3f, near %.3f, far %.1f", double(camera.fovY), double(camera.aspect), double(camera.nearPlane), double(camera.farPlane));
    c.evidence.push_back(buf);
    std::snprintf(buf, sizeof buf, "source: VS %016llx FS %016llx S%u/B%u P+%u V+%u", (unsigned long long)vsHash, (unsigned long long)fsHash, set, binding, projOffset, viewOffset);
    c.evidence.push_back(buf);

    cameras.insert(cameras.begin(), std::move(c));
}

void publish_memory_camera(std::vector<CameraCandidate>& cameras, CameraEvidenceLevel level, const DerivedCamera& camera, std::vector<std::string> evidence) {
    constexpr uint64_t memoryKey = 0x3E3C4A3E7A11ull;
    int preservedVerdict = 0;
    for (auto it = cameras.begin(); it != cameras.end(); )
        if (it->key == memoryKey) { preservedVerdict = it->verdict; it = cameras.erase(it); }
        else ++it;
    CameraCandidate c;
    c.key = memoryKey;
    c.level = level;
    c.confidence = level >= CameraEvidenceLevel::MemoryGpuMatch ? 1.f : .5f;
    c.verdict = preservedVerdict;
    c.hasDerivedCamera = true;
    c.derivedCamera = camera;
    c.evidence = std::move(evidence);
    auto at = cameras.begin();
    if (level < CameraEvidenceLevel::MemoryGpuMatch)
        while (at != cameras.end() && at->level == CameraEvidenceLevel::GpuVerifiedWater) ++at;
    cameras.insert(at, std::move(c));
}

bool active_scene_camera(const std::vector<CameraCandidate>& cameras, DerivedCamera& out) {
    for (auto& c : cameras) if (c.verdict == 1 && c.hasDerivedCamera && c.derivedCamera.valid) { out = c.derivedCamera; return true; }
    for (auto& c : cameras) if (c.hasDerivedCamera && c.derivedCamera.valid) { out = c.derivedCamera; return true; }
    return false;
}
}
