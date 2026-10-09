#version 450
#extension GL_GOOGLE_include_directive : require
#include "overlay_uniform.glsl"
layout(set=0,binding=0) uniform sampler2D sceneDepth;
layout(location=0) out uvec4 result;
void main() {
    result=uvec4(floatBitsToUint(texelFetch(sceneDepth,ivec2(fx.pick.xy),0).r),fx.pick.xyz);
}
