// Semantic draw classification rules (src/draw_semantics.hpp). The first cases are the states of real WoW 3.3.5a draws
// exported by the Inspector (exports/selection-*/draw-state.json, 2026-09): terrain, the water tile, the sky dome.
#include "draw_semantics.hpp"
#include "draw_segments.hpp"
#include <initializer_list>
#include <utility>
#include <cstdio>
#include <cstdlib>
using namespace depth_probe;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)

namespace {
constexpr uint32_t Zero = 0, One = 1, SrcColor = 2, DstColor = 4, SrcAlpha = 6, OneMinusSrcAlpha = 7;
// WoW's world pass: viewport depth 0..0.94, the scene depth target, everything known.
DrawFacts world(bool depthWrite, bool blend, uint32_t src = SrcAlpha, uint32_t dst = OneMinusSrcAlpha) {
    DrawFacts f;
    f.viewportKnown = true; f.minDepth = 0; f.maxDepth = .94f;
    f.depthTestKnown = f.depthTest = true;
    f.depthWriteKnown = true; f.depthWrite = depthWrite;
    f.blendKnown = true; f.blend = blend;
    f.blendFactorsKnown = blend; f.blendSrc = src; f.blendDst = dst;
    f.colorMaskKnown = true; f.colorMask = 15;
    f.hasDepthTarget = f.sceneTarget = true;
    return f;
}
void expect(const DrawFacts& f, DrawClass cls, DrawConfidence confidence, int line) {
    const auto s = classify_draw(f);
    if (s.cls != cls || s.confidence != confidence) {
        std::fprintf(stderr, "line %d: got %s %s (%s; %s), want %s %s\n", line, draw_class_name(s.cls), draw_confidence_name(s.confidence), s.reason,
                     draw_evidence_text(s.evidence).c_str(), draw_class_name(cls), draw_confidence_name(confidence));
        std::abort();
    }
}
#define EXPECT(f, cls, conf) expect(f, DrawClass::cls, DrawConfidence::conf, __LINE__)
}

int main() {
    // Terrain (selection 52015-1a3WUe): depth test + write, LESS_OR_EQUAL, no blend, colour mask 15, world range.
    auto terrain = world(true, false);
    EXPECT(terrain, Opaque, Medium);                     // alpha test not known (fast-link or no pattern): solid, maybe cutout
    terrain.alphaTest = 0; EXPECT(terrain, Opaque, High);
    terrain.alphaTest = 1; EXPECT(terrain, AlphaTest, High);
    CHECK(classify_draw(world(true, false)).evidence & EvAlphaTestUnknown);

    // The water tile (selection 46981-inffke): blended triangle strip without depth write, the known water FS.
    auto water = world(false, true); water.waterShader = true;
    EXPECT(water, Water, High);
    water.waterShader = false; EXPECT(water, Transparent, High);   // same state, another shader: plain alpha blend
    // The water FS somewhere that is neither the world range nor the scene target is not taken for water.
    auto odd = world(false, true); odd.waterShader = true; odd.sceneTarget = false; odd.minDepth = .2f; odd.maxDepth = .3f;
    EXPECT(odd, Unknown, None);

    // Sky dome (selection 76402-8eenjz): viewport 0.999..1, depth test, no depth write, blended.
    auto sky = world(false, true); sky.minDepth = .999f; sky.maxDepth = 1;
    EXPECT(sky, Sky, High);
    sky.depthTest = false; EXPECT(sky, Sky, Medium);
    sky.sceneTarget = false; EXPECT(sky, Unknown, None);          // sky range on some other depth target: not the world's sky

    // UI: full 0..1 range, no depth test (WoW draws it onto the scene targets or the swapchain).
    auto ui = world(false, true); ui.minDepth = 0; ui.maxDepth = 1; ui.depthTest = false;
    EXPECT(ui, Ui, Medium);
    ui.sceneTarget = false; ui.hasDepthTarget = false; EXPECT(ui, Ui, Medium);
    ui.depthTest = true; EXPECT(ui, Unknown, None);               // a depth-tested model in a UI frame: not claimed
    ui.depthTestKnown = false; EXPECT(ui, Unknown, None);

    // Blends in the world range.
    EXPECT(world(false, true, SrcAlpha, One), Additive, High);
    EXPECT(world(false, true, One, One), Additive, High);
    EXPECT(world(true, true, SrcAlpha, One), Additive, Medium);  // additive that writes depth: unusual
    EXPECT(world(false, true), Transparent, High);
    EXPECT(world(true, true), Transparent, Medium);
    EXPECT(world(false, true, One, OneMinusSrcAlpha), Transparent, High);   // premultiplied alpha
    EXPECT(world(false, true, DstColor, Zero), Transparent, High);          // modulate
    EXPECT(world(false, true, Zero, SrcColor), Transparent, High);
    CHECK(classify_draw(world(false, true, DstColor, Zero)).evidence & EvModulateBlend);
    EXPECT(world(false, true, One, Zero), Unknown, None);                   // replace blend without depth write: not claimed
    // WoW's WMO two-layer materials (frame-scan-42775-2): layer 1 SRC_ALPHA/ZERO with depth write overwrites like no blend;
    // layer 2 ONE_MINUS_SRC_ALPHA/ONE with depth write is an additive layer on the same geometry.
    EXPECT(world(true, true, SrcAlpha, Zero), Opaque, Medium);
    CHECK(classify_draw(world(true, true, SrcAlpha, Zero)).evidence & EvReplaceBlend);
    EXPECT(world(true, true, OneMinusSrcAlpha, One), Additive, Medium);
    EXPECT(world(true, true, DstColor, Zero), Transparent, Medium);         // modulate still reads the destination
    auto factors = world(false, true); factors.blendFactorsKnown = false; EXPECT(factors, Unknown, None);

    // Conservative corners.
    // Depth-only (WoW's sun shadow cascades: 2048^2 D24S8, full-range viewport, depth test + write, colour mask 0).
    auto shadow = terrain; shadow.colorMask = 0; shadow.minDepth = 0; shadow.maxDepth = 1; shadow.sceneTarget = false;
    EXPECT(shadow, DepthOnly, High);
    auto prepass = terrain; prepass.colorMask = 0; EXPECT(prepass, DepthOnly, Medium);        // on the scene target: a depth prepass
    auto mask = terrain; mask.colorMask = 0; mask.depthWrite = false; EXPECT(mask, Unknown, None);   // stencil / occlusion query
    auto discard = terrain; discard.rasterDiscard = true; EXPECT(discard, Unknown, None);
    auto noViewport = terrain; noViewport.viewportKnown = false; EXPECT(noViewport, Unknown, None);
    EXPECT(world(false, false), Unknown, None);                             // unblended, no depth write
    auto noTest = terrain; noTest.depthTest = false; EXPECT(noTest, Unknown, None);
    auto rangeOdd = terrain; rangeOdd.minDepth = .2f; rangeOdd.maxDepth = .9f; EXPECT(rangeOdd, Unknown, None);
    auto unknownWrite = terrain; unknownWrite.depthWriteKnown = false; EXPECT(unknownWrite, Unknown, None);
    auto unknownBlend = terrain; unknownBlend.blendKnown = false; EXPECT(unknownBlend, Unknown, None);
    CHECK(classify_draw(unknownBlend).evidence & EvStateUnknown);

    // No rule produces PARTICLE yet (nothing verified tells WoW's particles from other blended world draws).
    for (uint32_t src : {Zero, One, SrcColor, DstColor, SrcAlpha}) for (uint32_t dst : {Zero, One, SrcColor, OneMinusSrcAlpha})
        for (bool write : {false, true}) CHECK(classify_draw(world(write, true, src, dst)).cls != DrawClass::Particle);

    // Evidence text for logs / exports.
    CHECK(draw_evidence_text(classify_draw(terrain).evidence).find("WORLD-RANGE") != std::string::npos);
    CHECK(draw_evidence_text(classify_draw(terrain).evidence).find("DEPTH-WRITE") != std::string::npos);
    CHECK(std::string(draw_class_name(DrawClass::AlphaTest)) == "ALPHA_TEST");
    CHECK(std::string(draw_class_name(DrawClass::DepthOnly)) == "DEPTH_ONLY");
    // Segmentation (draw_segments.hpp) on the scene-target class sequences of real frame scans. One render pass holds the
    // whole world (pass 1); the first draw starts it.
    auto run = [](std::initializer_list<std::pair<DrawClass, uint32_t>> runs, uint32_t newPassAt = UINT32_MAX) {
        SegmentFrame f;
        uint32_t i = 0;
        for (auto [c, n] : runs) for (uint32_t k = 0; k < n; ++k, ++i)
            f.add(c, i >= newPassAt ? 2 : 1, i == 0 || i == newPassAt, !draw_class_blended(c) || c == DrawClass::Transparent);   // foliage writes depth
        return f;
    };
    using C = DrawClass;
    // Booty Bay, frame-scan-36309-*: clean - both rules split at 497, nothing on the wrong side (the late depth-only draw writes no colour).
    const auto clean = run({{C::Opaque, 152}, {C::Sky, 31}, {C::Opaque, 314}, {C::Transparent, 32}, {C::Water, 93}, {C::Transparent, 3},
                            {C::Additive, 2}, {C::Transparent, 3}, {C::Additive, 1}, {C::DepthOnly, 1}});
    CHECK(clean.anyBlended.found && clean.anyBlended.index == 497 && clean.anyBlended.solidsAfter == 0);
    CHECK(clean.alphaBlended.index == 497 && clean.alphaBlended.solidsAfter == 0 && clean.alphaBlended.blendedBefore == 0);
    CHECK(!clean.anyBlended.atPassStart && clean.anyBlended.passOrdinal == 1);   // inside the world pass
    CHECK(clean.count[size_t(C::Water)] == 93 && clean.first[size_t(C::Water)] == 529 && clean.last[size_t(C::Sky)] == 182);
    // Booty Bay, frame-scan-42775-1: a model's additive glow among the opaque draws. Rule A splits there and leaves 6 opaque
    // draws after the fog; rule B splits at the foliage (308) with that one glow before it.
    const auto glow = run({{C::Opaque, 118}, {C::Sky, 26}, {C::Opaque, 157}, {C::Additive, 1}, {C::Opaque, 6}, {C::Transparent, 27}, {C::Water, 115}});
    CHECK(glow.anyBlended.index == 301 && glow.anyBlended.solidsAfter == 6);
    CHECK(glow.alphaBlended.index == 308 && glow.alphaBlended.solidsAfter == 0 && glow.alphaBlended.blendedBefore == 1);
    // A split that starts a render pass of its own needs no pass split.
    const auto ownPass = run({{C::Opaque, 10}, {C::Transparent, 5}}, 10);
    CHECK(ownPass.alphaBlended.atPassStart && ownPass.alphaBlended.passOrdinal == 2);
    CHECK(!run({{C::Opaque, 10}, {C::Sky, 3}}).anyBlended.found);
    // frame-scan-42775-2: WMO second layers (additive WITH depth write) among the opaque draws are fogged right with their own
    // depth - not counted before split B; rule A splits at the first of them and leaves the rest of the world after the fog.
    SegmentFrame layers;
    for (int k = 0; k < 125; ++k) layers.add(C::Opaque, 1, k == 0, true);
    for (int k = 0; k < 7; ++k) { layers.add(C::Opaque, 1, false, true); layers.add(C::Additive, 1, false, true); }
    for (int k = 0; k < 400; ++k) layers.add(C::Opaque, 1, false, true);
    layers.add(C::Additive, 1, false, false);                                  // a glow without depth write
    for (int k = 0; k < 50; ++k) layers.add(C::Transparent, 1, false, true);
    CHECK(layers.alphaBlended.index == 540 && layers.alphaBlended.blendedBefore == 1 && layers.alphaBlended.solidsAfter == 0);
    CHECK(layers.anyBlended.index == 126 && layers.anyBlended.solidsAfter == 406);
    // frame-scan-42775-11: an M2's alpha batch (TRANSPARENT without depth write) and lamp glows inside the opaque phase. Rule B
    // splits at that batch and leaves the rest of the models after the fog; rule C waits for the foliage / water.
    SegmentFrame models;
    for (int k = 0; k < 476; ++k) models.add(C::Opaque, 1, k == 0, true);
    models.add(C::Transparent, 1, false, false);
    models.add(C::Additive, 1, false, false); models.add(C::Additive, 1, false, false);
    for (int k = 0; k < 29; ++k) models.add(C::Opaque, 1, false, true);
    for (int k = 0; k < 41; ++k) models.add(C::Transparent, 1, false, true);
    for (int k = 0; k < 99; ++k) models.add(C::Water, 1, false, false);
    CHECK(models.alphaBlended.index == 476 && models.alphaBlended.solidsAfter == 29);
    CHECK(models.phaseStart.index == 508 && models.phaseStart.solidsAfter == 0 && models.phaseStart.blendedBefore == 3);
    CHECK(clean.phaseStart.index == 497 && clean.phaseStart.solidsAfter == 0 && glow.phaseStart.index == 308 && glow.phaseStart.blendedBefore == 1);

    // Roadmap M4, blended cutouts: WoW's foliage (fs 2ef17a4e, Shadowglen scan frame-scan-54178-2: 70 a frame) is alpha blended,
    // alpha tested (GEQUAL) and depth writing - ALPHA_TEST, high with the alpha reference known (render_state push word 6).
    {
        auto leaf = world(true, true);
        leaf.alphaTest = 1; leaf.alphaCompare = 6;
        EXPECT(leaf, AlphaTest, Medium);                  // reference not known
        leaf.alphaRefKnown = true; leaf.alphaRef = 128;
        EXPECT(leaf, AlphaTest, High);
        CHECK(classify_draw(leaf).evidence & EvCutout);
        auto glass = world(true, true);                   // blended + depth write, no alpha test: transparent as before
        glass.alphaTest = 0;
        EXPECT(glass, Transparent, Medium);
        CHECK(!(classify_draw(glass).evidence & EvCutout));
        auto particle = world(false, true);               // alpha tested but no depth write: nothing is cut out of the depth
        particle.alphaTest = 1;
        EXPECT(particle, Transparent, High);
        auto glow = world(true, true, SrcAlpha, One);     // additive stays additive
        glow.alphaTest = 1;
        EXPECT(glow, Additive, Medium);
        // Segments: opaque, then the foliage (blended cutouts), then water - the blended phase starts at the foliage (rule C) and
        // the cutouts are not "solid draws after it".
        SegmentFrame f;
        for (int i = 0; i < 5; ++i) f.add(DrawClass::Opaque, 1, i == 0, true, false);
        for (int i = 0; i < 3; ++i) f.add(DrawClass::AlphaTest, 1, false, true, true);
        f.add(DrawClass::Water, 1, false, false, true);
        CHECK(f.phaseStart.found && f.phaseStart.index == 5 && f.phaseStart.solidsAfter == 0);
        CHECK(f.alphaBlended.found && f.alphaBlended.index == 5 && f.anyBlended.index == 5);
        SegmentFrame unblended;                           // an UNblended alpha-tested draw is solid: after the split it counts
        unblended.add(DrawClass::Water, 1, true, false, true);
        unblended.add(DrawClass::AlphaTest, 1, false, true, false);
        CHECK(unblended.phaseStart.solidsAfter == 1);
    }
    std::puts("PASS: blended cutouts (M4), draw semantics - WoW terrain / water / sky / UI states, blends, alpha test, conservative UNKNOWN corners, no unverified PARTICLE; segmentation on real frame sequences");
}
