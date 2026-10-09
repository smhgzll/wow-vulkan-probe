#version 450
#extension GL_GOOGLE_include_directive : require
#include "overlay_uniform.glsl"
layout(location = 0) out vec2 panel;
layout(location = 1) out flat uint isHud;
// The Camera Telemetry HUD is a SEPARATE vkCmdDrawIndirect using this same
// pipeline (see overlay_renderer.cpp), distinguished purely by its
// VkDrawIndirectCommand.firstVertex offset (6, vs. 0 for the menu draw) -
// no new descriptor/push-constant plumbing needed for a second panel.
const vec2 hudSize = vec2(300, 460);
void main() {
    const vec2 corners[6] = vec2[](vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1));
    isHud = gl_VertexIndex >= 6 ? 1u : 0u;
    uint local = gl_VertexIndex - (isHud!=0u ? 6u : 0u);
    vec2 size = isHud!=0u ? hudSize : (fx.inspector.x!=0u ? fx.viewport.xy/fx.viewport.z : vec2(400,648));
    panel = corners[local] * size;
    if(fx.inspector.x!=0u && isHud==0u)panel-=vec2(16);
    vec2 origin = isHud!=0u ? vec2(fx.viewport.x/fx.viewport.z - hudSize.x - 16.0, 16.0) : vec2(16);
    vec2 pixel = (panel + origin) * fx.viewport.z;
    gl_Position = vec4(pixel / fx.viewport.xy * 2.0 - 1.0, 0, 1);
}
