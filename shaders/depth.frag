#version 450

layout(set = 0, binding = 2) uniform sampler2D sceneTexture;

layout(set = 0, binding = 0) uniform sampler2D depthTexture;

#extension GL_GOOGLE_include_directive : require
#include "overlay_uniform.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

float rawDepth(vec2 p)
{
    return texture(depthTexture, clamp(p, vec2(0.0), vec2(1.0))).r;
}

float getDepth(vec2 p) {
    float d = rawDepth(p);
    return fx.depthConfig.x == 2u ? 1.0-d : d;
}
#include "depth_reconstruction.glsl"
#include "depth_world.glsl"

#include "legacy_rainbow.glsl"
#include "ssao.glsl"
#include "color_effects.glsl"
#include "ssr.glsl"
void main() {
    // Reconstruction debug views (Shift+F6) replace every effect while on, independent of effectEnabled.
    if(fx.recon.x!=0u) { outColor=fx.recon.y!=0u?reconDebug(ivec2(gl_FragCoord.xy)):vec4(0); return; }
    if(fx.effectEnabled==0u) { outColor=vec4(0); return; }
    if(fx.debugMode==13u){outColor=vec4(0);return;}
    bool ssaoReady=fx.aoConfig.x!=0u && positionReady();
    if(fx.debugMode==9u) {
        outColor=positionReady()?vec4(vec3(1.0-(ssaoReady?computeSSAO(uv):0.0)),1):vec4(0);
        return;
    }
    bool ssrReady=fx.ssrConfig.x!=0u && positionReady() && fx.viewport.w>.5;
    ReflectionHit reflection=ReflectionHit(false,vec2(0),0.0);
    if(ssrReady && ((fx.debugMode>=10u && fx.debugMode<=12u) || (fx.debugMode==3u && fx.ssrBlend.x>0.0)))
        reflection=traceReflection(uv);
    if(fx.debugMode>=10u && fx.debugMode<=12u) {
        if(!ssrReady){outColor=vec4(0);return;}
        vec3 color=vec3(0);
        if(reflection.valid) {
            if(fx.debugMode==10u)color=vec3(1);
            else if(fx.debugMode==11u)color=vec3(reflection.uv,0);
            else color=texture(sceneTexture,reflection.uv).rgb;
        }
        outColor=vec4(color,1);return;
    }
    legacyEffect();
    if(fx.debugMode==3u && fx.depthConfig.z!=0u && fx.aoConfig.w!=0u) {
        float alpha=1.0-(1.0-outColor.a)*(1.0-legacyOcclusion*fx.opacity);
        outColor=vec4(alpha>1e-6?outColor.rgb*outColor.a/alpha:vec3(0),alpha);
    }
    if(ssaoReady && (fx.debugMode==0u || fx.debugMode==3u)) {
        float ao=computeSSAO(uv);
        float alpha=1.0-(1.0-outColor.a)*(1.0-ao);
        outColor=vec4(alpha>1e-6?outColor.rgb*outColor.a*(1.0-ao)/alpha:vec3(0),alpha);
    }
    if(fx.debugMode==3u && fx.viewport.w>.5 && (colorEffectsEnabled() || (ssrReady && fx.ssrBlend.x>0.0))) {
        vec3 color=mix(sourceColor(uv),outColor.rgb,outColor.a);
        color=depthColorEffects(uv,color);
        if(reflection.valid)color=mix(color,texture(sceneTexture,reflection.uv).rgb,reflection.weight*fx.ssrBlend.x);
        outColor=vec4(color,1);
    }
}
