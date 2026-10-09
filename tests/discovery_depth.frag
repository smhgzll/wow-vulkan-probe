#version 450
layout(set=0,binding=0) uniform sampler2DShadow shadowTexture;
layout(location=0) out vec4 color;
void main(){color=vec4(vec3(texture(shadowTexture,vec3(.5,.5,.5))),1);}
