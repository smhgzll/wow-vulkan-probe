bool colorEffectsEnabled() {
    return any(notEqual(fx.colorFlags,uvec4(0))) || fx.edgeFlags.x!=0u;
}
vec3 sourceColor(vec2 p) {
    vec2 px=1.0/fx.viewport.xy;
    vec3 color=texture(sceneTexture,p).rgb;
    if(fx.colorFlags.y!=0u) {
        vec2 shift=(p-.5)*2.0*px*fx.colorParams.y;
        color.r=texture(sceneTexture,clamp(p+shift,vec2(0),vec2(1))).r;
        color.b=texture(sceneTexture,clamp(p-shift,vec2(0),vec2(1))).b;
    }
    if(fx.colorFlags.x!=0u) {
        vec3 neighbors=texture(sceneTexture,p+vec2(px.x,0)).rgb+texture(sceneTexture,p-vec2(px.x,0)).rgb+
                       texture(sceneTexture,p+vec2(0,px.y)).rgb+texture(sceneTexture,p-vec2(0,px.y)).rgb;
        color=clamp(color+(4.0*texture(sceneTexture,p).rgb-neighbors)*fx.colorParams.x,0.0,1.0);
    }
    return color;
}
vec3 depthColorEffects(vec2 p, vec3 color) {
    float d=getDepth(p);
    if(fx.colorFlags.z!=0u && linearDepthReady() && fx.fog.y>fx.fog.x) {
        float amount=clamp((linearDepth(p)-fx.fog.x)/(fx.fog.y-fx.fog.x),0.0,1.0);
        color=mix(color,vec3(fx.fog.w,fx.fogEdge.xy),1.0-exp(-amount*fx.fog.z));
    }
    if(fx.colorFlags.w!=0u) {
        float depth=linearDepthReady()?linearDepth(p)/fx.projection.y:d;
        float phase=fract(depth*fx.colorParams.z);
        float width=max(.02,min(.15,fwidth(depth*fx.colorParams.z)*1.5));
        float line=1.0-smoothstep(0.0,width,min(phase,1.0-phase));
        color=mix(color,hsv2rgb(vec3(fract(depth*fx.hueFrequency+fx.heartbeat.x),.9,1)),line*fx.colorParams.w);
    }
    if(fx.edgeFlags.x!=0u) {
        vec2 px=fx.fogEdge.w/fx.viewport.xy;
        float gradient=abs(getDepth(p+vec2(px.x,0))-getDepth(p-vec2(px.x,0)))+
                       abs(getDepth(p+vec2(0,px.y))-getDepth(p-vec2(0,px.y)));
        float edge=clamp(smoothstep(.01,.2,gradient/max(1.0-d,.0002))*fx.fogEdge.z,0.0,1.0);
        color=mix(color,hsv2rgb(vec3(fract(p.x*.7+p.y*.3-fx.heartbeat.x),.85,1)),edge);
    }
    return color;
}
