#pragma once
// Soft sun shadows inside WoW's own pixel shaders (fog.ini soft_shadows / shadow_fade, docs/fog.md). Every shadow-receiving
// fragment shader DXVK builds for WoW 3.3.5a samples its four cascades with depth-compare samples (OpImageSampleDrefExplicitLod):
// the finest 9 or 10 times, the others 5 times, each tap in a basic block of its own, and averages them; its lighting is
// linear in that average (tools/spirv-eval.py: colour(vis) = colour(0) + vis (colour(1) - colour(0))). So a softer value per
// tap gives a correctly lit soft shadow. Each tap is rewritten in place, inside its own block (no control flow changes):
//   a blocker search (2 raw depth fetches of the same map around the tap), the receiver-blocker distance in yards, a
//   penumbra growing with it (percentage-closer soft shadows), 3 depth-compare samples over that penumbra, the shadow lifted
//   with the distance (shadow_fade) - and the game's own sample where no blocker was found or the size is 0.
// The taps of one pixel each use another part of the pattern: together a 20-point search and a 30-point filter for the finest
// cascade. Pure code (tests/spirv_soft_shadows_test.cpp on the game's shaders).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace depth_probe::softshadow {

struct Params {
    float size = 1;        // penumbra per yard between receiver and blocker: size x .05 yd (1 ~ a 3 degree sun)
    float fade = 0;        // how much of a shadow is gone at fadeEnd yards from its caster (0 = none, 1 = all)
    float fadeStart = .5f, fadeEnd = 10;   // yards between receiver and blocker: the lift grows linearly between them
    float cascadeYards[4]{40, 80, 320, 1280};   // the cascades' widths (WoW's, logged by [FOG-CASCADES])
    float indoorStrength = 1;  // indoor shadow shaders: how much of the game's shadow stays (1 = all, 0 = none)
    float depthYards = 4000;                    // yards per unit of cascade depth (the shared depth row's scale)
};

// Live settings: DXVK's shared pixel data (D3D9SharedPS, set 2 / binding 4 in WoW's shaders; found by its layout) holds per texture stage the fixed-function
// constant and bump-environment values; stage 7's bump environment is never used by WoW's shaders. When its words hold
// liveMagic, the patched code takes size / fade / fadeEnd from there instead of the values baked into it (Params), so the
// layer can change them while the game runs (game_soft_shadows.cpp writes them at record time).
constexpr uint32_t liveOffset = 360;          // bytes into D3D9SharedPS: magic, fadeEnd, size, fade (stage 7 BumpEnvMat[1], LScale, LOffset)
constexpr uint32_t liveIndoorOffset = 352;    // and the indoor shadow strength (stage 7 BumpEnvMat[0].x)
constexpr float liveMagic = 1234.5678f;

struct Result {
    std::vector<uint32_t> code;   // empty: not a shadow receiver (untouched)
    uint32_t taps = 0;            // samples rewritten
    bool indoor = false;          // an indoor shadow shader (one map): its samples scaled by the indoor strength, no soft shadows
    bool live = false;            // reads the live settings (the shader declares D3D9SharedPS as expected) ...
    uint32_t liveSet = 0, liveBinding = 0;   // ... at this descriptor set / binding (the layer writes them there)
    std::string liveWhy;          // when not live: why
    std::string why;              // when empty code: why not
};

// words: a SPIR-V module. A fragment shader with 4 images each sampled at least 5 times with OpImageSampleDrefExplicitLod
// (the cascades, finest = lowest binding) is rewritten; so is one whose depth compares all read one image, 8 times or more
// (WoW indoors: one 2048 map, 14 taps - frame-scan-49703-1, the Den): each of those becomes 1 - strength (1 - the game's).
// Anything else is left alone.
Result patch(const uint32_t* words, size_t count, const Params&);

}  // namespace depth_probe::softshadow
