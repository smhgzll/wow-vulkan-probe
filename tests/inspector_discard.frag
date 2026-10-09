#version 450
layout(location=0) out vec4 color;
void main(){if(gl_FragCoord.x<48.)discard;color=vec4(.2,.7,1,1);}
