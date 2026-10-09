#version 450
#extension GL_EXT_demote_to_helper_invocation : require
// Mirrors the alpha test DXVK 2.x puts into every translated D3D9 fragment shader (seen in WoW 3.3.5a, 2026-09-27): the
// compare op is bits 21..23 of spec word 1 when spec constant 20 is set, else of the spec-state UBO's word 1 (fast link);
// a switch over VkCompareOp values, demote when the comparison fails. find_dxvk_alpha_test must find it.
layout(constant_id = 20) const uint specEnabled = 0u;
layout(constant_id = 1) const uint specWord1 = 0u;
layout(set = 2, binding = 5) uniform SpecState { uint dwords[8]; } spec_state;
layout(push_constant) uniform RenderState { float alphaRef; } render_state;
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 oC0;
void main() {
    oC0 = color;
    uint op = bitfieldExtract(mix(spec_state.dwords[1], specWord1, specEnabled != 0u), 21, 3);
    bool pass;
    switch (op) {
    case 0u: pass = false; break;
    case 1u: pass = oC0.a < render_state.alphaRef; break;
    case 2u: pass = oC0.a == render_state.alphaRef; break;
    case 3u: pass = oC0.a <= render_state.alphaRef; break;
    case 4u: pass = oC0.a > render_state.alphaRef; break;
    case 5u: pass = oC0.a != render_state.alphaRef; break;
    case 6u: pass = oC0.a >= render_state.alphaRef; break;
    default: pass = true; break;
    }
    if (!pass) demote;
}
