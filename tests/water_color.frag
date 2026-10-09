#version 450
layout(constant_id=0) const float red=0;
layout(constant_id=1) const float green=0;
layout(constant_id=2) const float blue=1;
layout(constant_id=3) const uint hole=0;
layout(constant_id=4) const float alpha=1;
layout(constant_id=5) const uint pattern=0;   // 1: an 8-pixel checker in the colour (x1 / x.5), a stand-in for the game's water texture
layout(push_constant) uniform Push {vec4 factor;} pc;
layout(location=0) out vec4 color;
void main(){if(hole!=0u && gl_FragCoord.x>=60 && gl_FragCoord.x<68 && gl_FragCoord.y>55)discard;color=vec4(red,green,blue,alpha)*pc.factor;
    if(pattern!=0u && ((int(gl_FragCoord.x)/8+int(gl_FragCoord.y)/8)&1)!=0)color.rgb*=.5;}
