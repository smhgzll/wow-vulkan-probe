#include "draw_classifier.hpp"
#include "discovery_internal.hpp"
#include "layer_state.hpp"
#include "scene_debugger.hpp"
#include "water_reflections.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace depth_probe::discovery {
bool draw_classes_enabled() {
    static const bool on = [] { const char* e = std::getenv("DEPTH_PROBE_DRAW_CLASSES"); return !e || std::strcmp(e, "0") != 0; }();
    return on;
}

DrawFacts draw_facts(const Device& s, const Pipeline* p, const Dynamic& dynamic, const Target& t, uint32_t* topology, const PushState* fragmentPush) {
    const auto e = effective_state(p, dynamic);
    DrawFacts f;
    f.viewportKnown = e.valid & Viewport; f.minDepth = e.viewport.minDepth; f.maxDepth = e.viewport.maxDepth;
    f.depthTestKnown = e.valid & DepthTest; f.depthTest = e.depthTest;
    f.depthWriteKnown = e.valid & DepthWrite; f.depthWrite = e.depthWrite;
    f.compareKnown = e.valid & Compare; f.depthCompare = e.compare;
    f.blendKnown = e.valid & Blend; f.blend = e.blend;
    f.blendFactorsKnown = e.valid & BlendEquation; f.blendSrc = e.blendSrc; f.blendDst = e.blendDst;
    f.colorMaskKnown = e.valid & ColorMask; f.colorMask = e.colorMask;
    f.rasterDiscard = (e.valid & Discard) && e.rasterDiscard;
    f.hasDepthTarget = t.depth && t.depth->image;
    const uint64_t scene = s.sceneDebugger ? s.sceneDebugger->sceneDepthImage : 0;
    f.sceneTarget = scene && f.hasDepthTarget && t.depth->image->handle == scene;
    f.waterShader = p && p->fs && p->fs->hash == wowWaterFS;
    f.alphaTest = p ? p->alphaTest.state : -1;
    f.alphaCompare = p ? p->alphaTest.compare : 0xff;
    // DXVK's D3D9 render_state push block (fragment stage): fog colour, scale, end, density, then alpha_ref at byte 24.
    if (fragmentPush && (fragmentPush->known >> 6 & 1)) { f.alphaRefKnown = true; f.alphaRef = fragmentPush->words[6]; }
    if (topology) *topology = (e.valid & Topology) ? e.topology : UINT32_MAX;
    return f;
}

DrawSemantic classify_snapshot(const Device& s, const Draw& d) {
    if (d.semantic.cls != DrawClass::Unknown || d.semantic.evidence) return d.semantic;   // classified when it was recorded
    return classify_draw(draw_facts(s, d.pipeline.get(), d.dynamic, d.target, nullptr, &d.push[1]));
}

DrawSemantic classify_recorded(Device& s, const Command& c, DrawFacts* facts) {
    uint32_t topology = UINT32_MAX;
    auto f = draw_facts(s, c.pipeline.get(), c.dynamic, c.target, &topology, &c.push[1]);
    // Fast-linked pipelines (roadmap M4) take the alpha test from DXVK's spec_state UBO per draw, not from the specialization:
    // read the word the fragment shader reads (its location found in the SPIR-V) from the host-coherent ring.
    const char* alphaSource = c.pipeline ? c.pipeline->alphaTest.source : "no pipeline";
    if (f.alphaTest < 0 && c.pipeline && c.pipeline->fs && c.pipeline->fs->dxvkAlphaTest.uboKnown && std::strcmp(alphaSource, "fast-link (DXVK spec UBO)") == 0) {
        const auto& t = c.pipeline->fs->dxvkAlphaTest;
        uint32_t word = 0;
        if (read_bound_uniform_word(s, c, t.uboSet, t.uboBinding, t.uboOffset, word)) {
            f.alphaCompare = (word >> t.offset) & ((1u << t.bits) - 1);
            f.alphaTest = f.alphaCompare == VK_COMPARE_OP_ALWAYS ? 0 : 1;
            alphaSource = "fast-link, read from the spec UBO";
        } else alphaSource = "fast-link, spec UBO not readable";
    }
    if (facts) *facts = f;
    const auto sem = classify_draw(f);
    if (!s.drawClasses) s.drawClasses = std::make_shared<DrawClassStats>();
    auto& st = *s.drawClasses;
    const auto k = size_t(sem.cls);
    ++(f.sceneTarget ? st.scene : st.other)[k];
    ++(f.sceneTarget ? st.currentScene : st.currentOther)[k];
    if (sem.cls == DrawClass::Unknown) ++st.unknownReasons[sem.reason];
    if (f.sceneTarget) st.segments.add(sem.cls, c.passOrdinal, c.passDraws == 1, f.depthWriteKnown && f.depthWrite, f.blendKnown && f.blend);   // passDraws counts this draw already
    if (sem.evidence & EvCutout) ++st.cutoutRefs[f.alphaRefKnown ? std::min(f.alphaRef, 255u) : 256u];
    if ((sem.evidence & EvWorldRange) && f.depthWrite && f.blendKnown && !f.blend)
        ++st.alphaSources[alphaSource];
    if (f.sceneTarget && (f.blend || sem.cls == DrawClass::Unknown) && st.signatures.size() < 4096) {
        const uint64_t fs = c.pipeline && c.pipeline->fs ? c.pipeline->fs->hash : 0;
        uint64_t key = fs;
        for (uint64_t v : {uint64_t(sem.cls), uint64_t(f.blendSrc), uint64_t(f.blendDst), uint64_t(f.depthWrite), uint64_t(topology)}) key = (key ^ v) * 1099511628211ull;
        auto& sig = st.signatures[key];
        if (!sig.count) sig = {0, sem.cls, f.blendSrc, f.blendDst, f.depthWrite, topology, fs};
        ++sig.count;
    }
    return sem;
}

namespace {
std::string per_frame(uint64_t total, uint64_t frames) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", double(total) / double(std::max<uint64_t>(frames, 1)));
    return b;
}
std::string class_counts(const std::array<uint64_t, DrawClassStats::classes>& counts, uint64_t frames) {
    std::string out;
    for (size_t i = 0; i < counts.size(); ++i) if (counts[i]) out += std::string(out.empty() ? "" : " ") + draw_class_name(DrawClass(i)) + " " + per_frame(counts[i], frames);
    return out.empty() ? "none" : out;
}
}

// [DRAW-CLASS] every 900 presents, averaged per frame: the classes on the scene target and elsewhere, why draws stayed
// UNKNOWN (ranked), where the solid world draws' alpha test came from, and the most frequent blended / unknown scene
// signatures - the data the PARTICLE rule and the pass segmentation (milestone 2) will be built from.
void draw_classes_present(Device& s) {
    if (!s.drawClasses) return;
    auto& st = *s.drawClasses;
    st.lastScene = st.currentScene; st.lastOther = st.currentOther;
    st.currentScene = {}; st.currentOther = {};
    if (st.segments.draws) {
        ++st.segmentFrames;
        auto fold = [](DrawClassStats::SplitTotals& t, const SegmentSplit& s) {
            if (!s.found) return;
            ++t.found;
            t.clean += s.solidsAfter == 0 && s.blendedBefore == 0;
            t.solidsAfter += s.solidsAfter; t.solidsAfterMax = std::max<uint64_t>(t.solidsAfterMax, s.solidsAfter);
            t.blendedBefore += s.blendedBefore; t.blendedBeforeMax = std::max<uint64_t>(t.blendedBeforeMax, s.blendedBefore);
            t.atPassStart += s.atPassStart; t.index += s.index;
        };
        fold(st.anyBlended, st.segments.anyBlended);
        fold(st.alphaBlended, st.segments.alphaBlended);
        fold(st.phaseStart, st.segments.phaseStart);
    }
    st.segments = {};
    if (++st.frames < 900) return;
    const uint64_t n = st.frames;
    log_line("[DRAW-CLASS] per frame over " + std::to_string(n) + " frames | scene target: " + class_counts(st.scene, n) + " | other targets: " + class_counts(st.other, n));
    auto ranked = [&](const std::map<const char*, uint64_t>& m, size_t limit) {
        std::vector<std::pair<uint64_t, const char*>> v;
        for (auto [k, count] : m) v.emplace_back(count, k);
        std::sort(v.rbegin(), v.rend());
        std::string out;
        for (size_t i = 0; i < v.size() && i < limit; ++i) out += std::string(i ? "; " : "") + per_frame(v[i].first, n) + " " + v[i].second;
        return out.empty() ? std::string("none") : out;
    };
    log_line("[DRAW-CLASS] unknown because: " + ranked(st.unknownReasons, 6));
    log_line("[DRAW-CLASS] solid world draws, alpha test from: " + ranked(st.alphaSources, 4));
    {   // blended cutouts (roadmap M4): how many, and their alpha reference (D3DRS_ALPHAREF, of 255)
        uint64_t total = 0;
        std::vector<std::pair<uint64_t, uint32_t>> refs;
        for (auto [ref, count] : st.cutoutRefs) { total += count; refs.emplace_back(count, ref); }
        std::sort(refs.rbegin(), refs.rend());
        std::string out;
        for (size_t i = 0; i < refs.size() && i < 4; ++i)
            out += std::string(i ? ", " : "") + per_frame(refs[i].first, n) + (refs[i].second > 255 ? " ref unknown" : " ref " + std::to_string(refs[i].second));
        log_line("[DRAW-CLASS] blended cutouts (alpha test + depth write): " + per_frame(total, n) + " per frame" + (out.empty() ? "" : " - " + out));
    }
    std::vector<const DrawClassStats::Signature*> sigs;
    for (auto& [key, sig] : st.signatures) sigs.push_back(&sig);
    std::sort(sigs.begin(), sigs.end(), [](auto* a, auto* b) { return a->count > b->count; });
    std::string top;
    for (size_t i = 0; i < sigs.size() && i < 8; ++i) {
        const auto& g = *sigs[i];
        char b[160];
        std::snprintf(b, sizeof b, "%s%s %s src%u dst%u dw%u topo%d fs %016llx", i ? "; " : "", per_frame(g.count, n).c_str(), draw_class_name(g.cls), g.src, g.dst, g.depthWrite,
                      g.topology == UINT32_MAX ? -1 : int(g.topology), (unsigned long long)g.fs);
        top += b;
    }
    log_line("[DRAW-CLASS] top blended/unknown scene signatures: " + (top.empty() ? std::string("none") : top));
    // [DRAW-SEGMENTS]: how well one "fog goes here" split between the solid and the blended world would work (milestone 2).
    if (st.segmentFrames) {
        auto pct = [&](uint64_t v, uint64_t of) { return std::to_string(int(100.0 * double(v) / double(std::max<uint64_t>(of, 1)) + .5)) + "%"; };
        auto rule = [&](const char* name, const DrawClassStats::SplitTotals& t) {
            return std::string(name) + "found in " + pct(t.found, st.segmentFrames) + " of frames, clean " + pct(t.clean, t.found) +
                   ", solid draws after it avg " + per_frame(t.solidsAfter, t.found) + " max " + std::to_string(t.solidsAfterMax) +
                   ", blended before it avg " + per_frame(t.blendedBefore, t.found) + " max " + std::to_string(t.blendedBeforeMax) +
                   ", at a render pass start " + pct(t.atPassStart, t.found) + ", index avg " + per_frame(t.index, t.found);
        };
        log_line("[DRAW-SEGMENTS] " + std::to_string(st.segmentFrames) + " frames with scene draws | C (first depth-writing TRANSPARENT or WATER): " +
                 rule("", st.phaseStart) + " | B (first TRANSPARENT/WATER): " + rule("", st.alphaBlended) +
                 " | A (first blended): " + rule("", st.anyBlended));
    }
    const auto keepLast = std::make_pair(st.lastScene, st.lastOther);
    st = {};
    st.lastScene = keepLast.first; st.lastOther = keepLast.second;
}
}
