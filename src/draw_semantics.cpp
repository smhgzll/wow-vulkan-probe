#include "draw_semantics.hpp"

namespace depth_probe {
namespace {
// VkBlendFactor values (no Vulkan header needed for plain numbers).
constexpr uint32_t Zero = 0, One = 1, SrcColor = 2, DstColor = 4, OneMinusDstColor = 5, SrcAlpha = 6, OneMinusSrcAlpha = 7, DstAlpha = 8, OneMinusDstAlpha = 9;

uint32_t range_evidence(const DrawFacts& f) {
    if (f.minDepth >= .99f) return EvSkyRange;
    if (f.minDepth == 0.f && f.maxDepth >= .999f) return EvFullRange;
    if (f.minDepth == 0.f && f.maxDepth > .5f) return EvWorldRange;
    return EvOddRange;
}
uint32_t blend_evidence(const DrawFacts& f) {
    if (f.blendSrc == SrcAlpha && f.blendDst == OneMinusSrcAlpha) return EvAlphaBlend;
    if (f.blendSrc == One && f.blendDst == OneMinusSrcAlpha) return EvAlphaBlend;   // premultiplied alpha
    if (f.blendDst == One) return EvAdditiveBlend;
    if ((f.blendSrc == DstColor && f.blendDst == Zero) || (f.blendSrc == Zero && f.blendDst == SrcColor) ||
        (f.blendSrc == DstColor && f.blendDst == SrcColor)) return EvModulateBlend;
    // dst ZERO, source not reading the destination: the result replaces what was there (WoW's WMO two-layer materials draw
    // their first layer with SRC_ALPHA/ZERO and depth write, then an additive second layer on the same geometry).
    if (f.blendDst == Zero && f.blendSrc != DstColor && f.blendSrc != OneMinusDstColor && f.blendSrc != DstAlpha && f.blendSrc != OneMinusDstAlpha)
        return EvReplaceBlend;
    return EvOtherBlend;
}
DrawSemantic result(DrawClass c, DrawConfidence k, uint32_t evidence, const char* reason) { return {c, k, evidence, reason}; }
}

DrawSemantic classify_draw(const DrawFacts& f) {
    uint32_t ev = 0;
    if (f.rasterDiscard) return result(DrawClass::Unknown, DrawConfidence::None, EvRasterDiscard, "RASTERIZER DISCARD");
    if (f.colorMaskKnown && f.colorMask == 0) {
        // Depth-only: a depth pass (WoW: the sun shadow cascades). Only when it really writes depth; colour-less draws that
        // do not (stencil / occlusion queries) are not claimed.
        if (f.depthWriteKnown && f.depthWrite && f.depthTestKnown && f.depthTest)
            return result(DrawClass::DepthOnly, f.sceneTarget ? DrawConfidence::Medium : DrawConfidence::High,
                          EvNoColorWrites | EvDepthTest | EvDepthWrite | (f.sceneTarget ? EvSceneTarget : 0u), "DEPTH ONLY (NO COLOR WRITES)");
        return result(DrawClass::Unknown, DrawConfidence::None, EvNoColorWrites, "NO COLOR WRITES, NO DEPTH WRITE");
    }
    if (!f.viewportKnown) return result(DrawClass::Unknown, DrawConfidence::None, EvStateUnknown, "VIEWPORT NOT KNOWN");
    ev |= range_evidence(f);
    if (f.depthTestKnown && f.depthTest) ev |= EvDepthTest;
    if (f.depthWriteKnown && f.depthWrite) ev |= EvDepthWrite;
    if (f.sceneTarget) ev |= EvSceneTarget;
    if (!f.hasDepthTarget) ev |= EvNoDepthTarget;
    if (f.blendKnown && f.blend) {
        ev |= EvBlend;
        if (f.blendFactorsKnown) ev |= blend_evidence(f);
    }
    if (f.waterShader) ev |= EvWaterShader;
    ev |= f.alphaTest > 0 ? EvAlphaTestOn : f.alphaTest == 0 ? EvAlphaTestOff : EvAlphaTestUnknown;

    // Water first: the shader identity is the strongest evidence there is (exact hash of WoW's water FS).
    if (f.waterShader && (ev & (EvWorldRange | EvSceneTarget)))
        return result(DrawClass::Water, DrawConfidence::High, ev, "KNOWN WOW WATER SHADER");

    // UI: WoW draws its interface with the full 0..1 depth range and no depth test (onto the scene's own targets or the
    // swapchain). A full-range draw that depth tests is a 3D model in a UI frame (portrait, preview) or unknown: not UI.
    if (ev & EvFullRange) {
        if (!f.depthTestKnown) return result(DrawClass::Unknown, DrawConfidence::None, ev | EvStateUnknown, "FULL RANGE, DEPTH TEST NOT KNOWN");
        if (!f.depthTest) return result(DrawClass::Ui, DrawConfidence::Medium, ev, "FULL DEPTH RANGE, NO DEPTH TEST");
        return result(DrawClass::Unknown, DrawConfidence::None, ev, "FULL RANGE WITH DEPTH TEST (UI MODEL?)");
    }
    // Sky: WoW's sky dome / celestial draws use a viewport squeezed to the far end (0.998..1). High only with the depth
    // test on (it is drawn behind the world); a sky-range draw without it is unusual - medium.
    if (ev & EvSkyRange) {
        if (!(ev & EvSceneTarget) && f.hasDepthTarget) return result(DrawClass::Unknown, DrawConfidence::None, ev, "SKY RANGE OFF THE SCENE TARGET");
        return result(DrawClass::Sky, f.depthTestKnown && f.depthTest ? DrawConfidence::High : DrawConfidence::Medium, ev, "SKY DEPTH RANGE (0.99..1)");
    }
    if (!(ev & EvWorldRange)) return result(DrawClass::Unknown, DrawConfidence::None, ev, "UNUSUAL DEPTH RANGE");
    // World range from here on.
    if (!f.depthTestKnown || !f.blendKnown) return result(DrawClass::Unknown, DrawConfidence::None, ev | EvStateUnknown, "DEPTH TEST / BLEND NOT KNOWN");
    if (!f.depthTest) return result(DrawClass::Unknown, DrawConfidence::None, ev, "WORLD RANGE WITHOUT DEPTH TEST");
    if (!f.blend) {
        if (!f.depthWriteKnown) return result(DrawClass::Unknown, DrawConfidence::None, ev | EvStateUnknown, "UNBLENDED, DEPTH WRITE NOT KNOWN");
        if (!f.depthWrite) return result(DrawClass::Unknown, DrawConfidence::None, ev, "UNBLENDED WITHOUT DEPTH WRITE");
        if (f.alphaTest > 0) return result(DrawClass::AlphaTest, DrawConfidence::High, ev, "ALPHA TEST IN FRAGMENT SPECIALIZATION");
        if (f.alphaTest == 0) return result(DrawClass::Opaque, DrawConfidence::High, ev, "DEPTH WRITE, NO BLEND, NO ALPHA TEST");
        // Depth-writing and unblended: surely solid geometry, only opaque vs cutout is open. Both write depth, so both are
        // "before the transparents" for pass segmentation; medium confidence and the evidence bit say which part is open.
        return result(DrawClass::Opaque, DrawConfidence::Medium, ev, "DEPTH WRITE, NO BLEND (ALPHA TEST UNKNOWN)");
    }
    if (!f.blendFactorsKnown) return result(DrawClass::Unknown, DrawConfidence::None, ev | EvStateUnknown, "BLENDED, FACTORS NOT KNOWN");
    if (ev & EvReplaceBlend) {
        if (!f.depthWrite) return result(DrawClass::Unknown, DrawConfidence::None, ev, "REPLACE BLEND (DST ZERO) WITHOUT DEPTH WRITE");
        if (f.alphaTest > 0) return result(DrawClass::AlphaTest, DrawConfidence::Medium, ev, "REPLACE BLEND, DEPTH WRITE, ALPHA TEST");
        return result(DrawClass::Opaque, DrawConfidence::Medium, ev, "REPLACE BLEND (DST ZERO) WITH DEPTH WRITE");
    }
    // Cutout (roadmap M4): alpha-blended (or modulating), alpha-tested and depth-writing - WoW's foliage (fs 2ef17a4e: 70 draws a
    // frame in Shadowglen, alpha test GEQUAL): the alpha test decides where it writes depth, the blend only softens the edges, so
    // for depth, fog and light it is solid with holes. High with the alpha reference known (push constants), else medium.
    if ((ev & (EvAlphaBlend | EvModulateBlend)) && f.alphaTest > 0 && f.depthWriteKnown && f.depthWrite)
        return result(DrawClass::AlphaTest, f.alphaRefKnown ? DrawConfidence::High : DrawConfidence::Medium, ev | EvCutout, "BLENDED CUTOUT: ALPHA TEST + DEPTH WRITE");
    if (ev & EvAdditiveBlend) return result(DrawClass::Additive, f.depthWrite ? DrawConfidence::Medium : DrawConfidence::High, ev, "ADDITIVE BLEND (DST ONE)");
    if (ev & (EvAlphaBlend | EvModulateBlend))
        return result(DrawClass::Transparent, f.depthWrite ? DrawConfidence::Medium : DrawConfidence::High, ev,
                      (ev & EvModulateBlend) ? "MODULATE BLEND (MULTIPLIES SCENE)" : "ALPHA BLEND");
    return result(DrawClass::Unknown, DrawConfidence::None, ev, "BLENDED WITH UNRECOGNIZED FACTORS");
}

const char* draw_class_name(DrawClass c) {
    switch (c) {
    case DrawClass::Sky: return "SKY";
    case DrawClass::Opaque: return "OPAQUE";
    case DrawClass::AlphaTest: return "ALPHA_TEST";
    case DrawClass::Water: return "WATER";
    case DrawClass::Transparent: return "TRANSPARENT";
    case DrawClass::Particle: return "PARTICLE";
    case DrawClass::Additive: return "ADDITIVE";
    case DrawClass::Ui: return "UI";
    case DrawClass::DepthOnly: return "DEPTH_ONLY";
    default: return "UNKNOWN";
    }
}
const char* draw_confidence_name(DrawConfidence c) {
    switch (c) {
    case DrawConfidence::High: return "HIGH";
    case DrawConfidence::Medium: return "MEDIUM";
    case DrawConfidence::Low: return "LOW";
    default: return "NONE";
    }
}
std::string draw_evidence_text(uint32_t e) {
    static const char* names[]{"WORLD-RANGE", "SKY-RANGE", "FULL-RANGE", "ODD-RANGE", "DEPTH-TEST", "DEPTH-WRITE", "BLEND", "ALPHA-BLEND",
                               "ADDITIVE", "MODULATE", "OTHER-BLEND", "WATER-FS", "ALPHA-TEST-ON", "ALPHA-TEST-OFF", "ALPHA-TEST-UNKNOWN",
                               "SCENE-TARGET", "NO-COLOR-WRITES", "STATE-UNKNOWN", "RASTER-DISCARD", "NO-DEPTH-TARGET", "REPLACE-BLEND", "CUTOUT"};
    std::string out;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (e & (1u << i)) { if (!out.empty()) out += ' '; out += names[i]; }
    return out;
}
}
