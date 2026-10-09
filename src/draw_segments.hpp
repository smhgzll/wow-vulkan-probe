#pragma once
#include "draw_semantics.hpp"
#include <array>
#include <cstdint>

// Pass segmentation (roadmap milestone 2, observational): the order of the semantic classes on the scene target within one
// frame, and how good a single "fog goes here" split point between the solid world and the blended world would be.
//
// WoW 3.3.5a (frame scans 2026-09-27): the whole world - terrain, sky, models, then foliage, water, effects - is ONE DXVK
// render pass on the scene targets, the UI a later pass on the same targets. WoW draws each object's own blended batches with
// the object inside the opaque phase (WMO second material layers, lamp glows, an M2's alpha batch), then the transparent phase:
// foliage (alpha blend + alpha test + depth write), water, effects. Three candidate split rules:
//   A: before the first blended draw of any kind (unusable: WMO second layers come early),
//   B: before the first alpha-blended world draw (TRANSPARENT / WATER) - failed once: an M2 alpha batch inside the opaque phase,
//   C: before the first depth-writing TRANSPARENT (foliage) or the first WATER - 0 solid draws after it in all 26 scanned
//      frames of 2026-09-27 (Booty Bay, two sessions), 0..7 non-depth-writing blended draws before it.
// For each: the solid draws that would come after the split (drawn after the fog: not fogged - a visible error) and the
// blended draws before it (fogged with the depth behind them, as today). Pure logic: tests/draw_semantics_test.cpp.
namespace depth_probe {

struct SegmentSplit {
    bool found = false;
    uint32_t index = 0;             // scene-target draw index of the split (the first draw after it)
    uint32_t passOrdinal = 0;       // render pass instance that draw belongs to
    bool atPassStart = false;       // that draw is the first of its render pass: no pass has to be split there
    uint32_t solidsAfter = 0;       // SKY / OPAQUE / ALPHA_TEST draws after the split (depth-only ones change no colour)
    uint32_t blendedBefore = 0;     // blended draws WITHOUT depth write before it: fogged with the depth behind them (a depth-
                                    // writing blended draw - WoW's second material layers - leaves its own depth: fogged right)
};

struct SegmentFrame {
    static constexpr size_t classes = size_t(DrawClass::Count);
    uint32_t draws = 0;                                     // scene-target draws seen this frame
    std::array<uint32_t, classes> count{}, first{}, last{}; // first/last index per class (valid where count > 0)
    SegmentSplit anyBlended, alphaBlended, phaseStart;      // rules A, B, C
    // blendedDraw: the draw's blend state is on. A blended ALPHA_TEST draw is a cutout (WoW's foliage, roadmap M4): it belongs
    // to the blended phase like the depth-writing transparents it used to be classified as, not to the solids before it.
    void add(DrawClass cls, uint32_t passOrdinal, bool passStart, bool writesDepth, bool blendedDraw = false);
};

bool draw_class_solid(DrawClass);     // writes the world's depth before the blended phase: sky, opaque, alpha-tested, depth-only
bool draw_class_blended(DrawClass);   // transparent, water, additive, particle
}
