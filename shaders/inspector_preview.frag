#version 450
#extension GL_GOOGLE_include_directive : require
#include "overlay_uniform.glsl"
layout(set=0,binding=0) uniform sampler2D previewTexture;
layout(location=0) out vec4 color;
void main() {
    vec2 p=gl_FragCoord.xy/fx.viewport.z-vec2(16);
    vec2 uv=(p-vec2(416,128))/256.;
    if(fx.pick.w==0u || any(lessThan(uv,vec2(0))) || any(greaterThanEqual(uv,vec2(1))))discard;
    vec4 t=textureLod(previewTexture,uv,0.);
    // Inspect RGB even when alpha is zero (common for data/normal textures).
    // Bitmap export separately preserves the alpha channel.
    color=vec4(t.rgb,1.);
}
