#pragma once
#include "draw_semantics.hpp"
#include "draw_segments.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>

// Layer side of the semantic draw classification (draw_semantics.hpp holds the rules): the facts of a draw from the
// tracked Vulkan state, per-draw classification at record time (observational only - nothing is drawn differently),
// counters, and the [DRAW-CLASS] log. DEPTH_PROBE_DRAW_CLASSES=0 turns it off.
namespace depth_probe::discovery {
struct Device;struct Command;struct Draw;struct Pipeline;struct Dynamic;struct Target;

struct DrawClassStats {
    static constexpr size_t classes = size_t(DrawClass::Count);
    uint64_t frames = 0;                                  // presents in the current log interval
    std::array<uint64_t, classes> scene{}, other{};       // draws on the scene's depth target / elsewhere, this interval
    std::array<uint32_t, classes> lastScene{}, lastOther{}, currentScene{}, currentOther{};   // the last whole frame / the one being recorded
    std::map<const char*, uint64_t> unknownReasons;       // why UNKNOWN (reason strings are static)
    std::map<const char*, uint64_t> alphaSources;         // solid world draws (depth write, no blend): where their alpha test came from
    std::map<uint32_t, uint64_t> cutoutRefs;              // blended cutouts (roadmap M4): their alpha reference (256 = not known)
    struct Signature { uint64_t count = 0; DrawClass cls = DrawClass::Unknown; uint32_t src = 0, dst = 0, depthWrite = 0, topology = 0; uint64_t fs = 0; };
    std::unordered_map<uint64_t, Signature> signatures;   // blended / unknown draws on the scene target, by state + fragment shader
    // Milestone 2 (observational): the scene-target class order of the frame being recorded, and the interval's totals of
    // the two split rules (draw_segments.hpp).
    SegmentFrame segments;
    struct SplitTotals { uint64_t found = 0, clean = 0, solidsAfter = 0, solidsAfterMax = 0, blendedBefore = 0, blendedBeforeMax = 0, atPassStart = 0, index = 0; };
    uint64_t segmentFrames = 0;                           // frames with scene-target draws
    SplitTotals anyBlended, alphaBlended, phaseStart;
};

bool draw_classes_enabled();
struct PushState;
// fragmentPush: the fragment stage's push words (DXVK's render_state: the alpha reference in word 6), nullptr = not known.
DrawFacts draw_facts(const Device&, const Pipeline*, const Dynamic&, const Target&, uint32_t* topology = nullptr, const PushState* fragmentPush = nullptr);
DrawSemantic classify_recorded(Device&, const Command&, DrawFacts* facts = nullptr);   // record time: counts it
DrawSemantic classify_snapshot(const Device&, const Draw&); // a snapshotted draw (Inspector, exports): no counting
void draw_classes_present(Device&);                         // once per present: closes the frame, logs every 900 frames ([DRAW-CLASS], [DRAW-SEGMENTS])
}
