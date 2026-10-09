#pragma once
#include <cstdint>
#include <string>

// Semantic draw classification (roadmap milestone 1): what a draw is in the scene - sky, opaque world, alpha-tested
// world, water, transparent, additive, UI - from the state the layer already tracks. Pure logic on plain facts (no Vulkan
// objects), so the rules are unit tested (tests/draw_semantics_test.cpp); draw_classifier.cpp gathers the facts per draw.
//
// Conservative: a rule fires only on evidence that was verified against WoW 3.3.5a under DXVK (real draw exports,
// docs/draw-classes.md); everything else stays UNKNOWN with the reason in `evidence`. PARTICLE has no rule yet: nothing
// verified separates WoW's (fixed-function) particles from other blended world draws - they count as TRANSPARENT /
// ADDITIVE until a signature is established from the logged blended-draw signatures.
namespace depth_probe {

// DepthOnly: writes depth, no colour (WoW 3.3.5a: its four 2048^2 sun shadow cascades, ~950 draws a frame in Booty Bay -
// tools/frame-scan.py shows the opaque world sampling them with a comparison; the class itself does not claim "shadow").
enum class DrawClass : uint8_t { Unknown, Sky, Opaque, AlphaTest, Water, Transparent, Particle, Additive, Ui, DepthOnly, Count };
enum class DrawConfidence : uint8_t { None, Low, Medium, High };

// Evidence bits: what the classification rests on (and, for UNKNOWN, why nothing fired).
enum DrawEvidence : uint32_t {
    EvWorldRange = 1u << 0,        // viewport depth range 0..<1 (WoW world: 0..0.94)
    EvSkyRange = 1u << 1,          // viewport minDepth >= 0.99 (WoW sky: 0.998/0.999..1)
    EvFullRange = 1u << 2,         // viewport depth range 0..1
    EvOddRange = 1u << 3,          // any other depth range
    EvDepthTest = 1u << 4,
    EvDepthWrite = 1u << 5,
    EvBlend = 1u << 6,
    EvAlphaBlend = 1u << 7,        // src SRC_ALPHA, dst ONE_MINUS_SRC_ALPHA (or ONE, ONE_MINUS_SRC_ALPHA: premultiplied)
    EvAdditiveBlend = 1u << 8,     // dst ONE
    EvModulateBlend = 1u << 9,     // src DST_COLOR / ZERO with dst ZERO / SRC_COLOR: multiplies the scene
    EvOtherBlend = 1u << 10,       // blending with factors none of the above
    EvWaterShader = 1u << 11,      // the known WoW water fragment shader
    EvAlphaTestOn = 1u << 12,      // fragment stage specialised with an alpha test (DXVK compare op != ALWAYS)
    EvAlphaTestOff = 1u << 13,
    EvAlphaTestUnknown = 1u << 14, // not specialised (fast-link / no pattern): opaque or alpha-tested, not known which
    EvSceneTarget = 1u << 15,      // drawn into the scene's depth target
    EvNoColorWrites = 1u << 16,
    EvStateUnknown = 1u << 17,     // a state the rule needs was not known (dynamic state never set, blend factors unknown)
    EvRasterDiscard = 1u << 18,
    EvNoDepthTarget = 1u << 19,
    EvReplaceBlend = 1u << 20,     // dst ZERO with a source factor that does not read the destination: overwrites like no blend
    EvCutout = 1u << 21,           // blended AND alpha-tested AND depth-writing: the alpha test decides where depth is written
};

struct DrawFacts {
    bool viewportKnown = false;
    float minDepth = 0, maxDepth = 1;
    bool depthTestKnown = false, depthTest = false;
    bool depthWriteKnown = false, depthWrite = false;
    bool compareKnown = false;
    uint32_t depthCompare = 0;                     // VkCompareOp (not used by the rules; the transparent-aware fog skips EQUAL layers)
    bool blendKnown = false, blend = false;
    bool blendFactorsKnown = false;
    uint32_t blendSrc = 0, blendDst = 0;           // VkBlendFactor of the first blended attachment
    bool colorMaskKnown = false;
    uint32_t colorMask = 15;
    bool rasterDiscard = false;
    bool hasDepthTarget = false, sceneTarget = false;
    bool waterShader = false;
    int alphaTest = -1;                            // 1 on, 0 off, -1 unknown
    uint32_t alphaCompare = 0xff;                  // VkCompareOp of the alpha test (0xff unknown; 7 ALWAYS = off)
    bool alphaRefKnown = false;
    uint32_t alphaRef = 0;                         // D3DRS_ALPHAREF 0..255 (DXVK render_state push word 6, fragment stage)
};

struct DrawSemantic {
    DrawClass cls = DrawClass::Unknown;
    DrawConfidence confidence = DrawConfidence::None;
    uint32_t evidence = 0;
    const char* reason = "";                       // one line, upper case (HUD / Inspector font)
};

DrawSemantic classify_draw(const DrawFacts&);
const char* draw_class_name(DrawClass);
const char* draw_confidence_name(DrawConfidence);
std::string draw_evidence_text(uint32_t evidence);   // "WORLD-RANGE DEPTH-WRITE ..." for logs / exports
}
