#version 450
layout(location=0) in vec3 position;
layout(std140,set=2,binding=0) uniform Camera { vec4 padding[16]; vec4 c[9]; } camera;
out gl_PerVertex {vec4 gl_Position;float gl_ClipDistance[1];};
void main(){mat4 p=mat4(camera.c[0],camera.c[1],camera.c[2],camera.c[3]);mat4 m=mat4(camera.c[5],camera.c[6],camera.c[7],camera.c[8]);gl_Position=p*m*vec4(position,1);gl_ClipDistance[0]=1;}
