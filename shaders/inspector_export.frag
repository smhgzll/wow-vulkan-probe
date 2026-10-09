#version 450
layout(set=0,binding=0) uniform sampler2D sourceTexture;
layout(constant_id=0) const uint conversion=0u;
layout(location=0) out vec4 color;
void main() {
    vec4 t=texelFetch(sourceTexture,ivec2(gl_FragCoord.xy),0);
    if((conversion&1u)!=0u) {
        vec3 v=max(t.rgb,vec3(0));
        t.rgb=mix(12.92*v,1.055*pow(v,vec3(1./2.4))-.055,greaterThan(v,vec3(.0031308)));
    }
    for(int i=0;i<4;++i)if((conversion&(2u<<i))!=0u)t[i]=t[i]*.5+.5;
    color=clamp(t,0.,1.);
}
