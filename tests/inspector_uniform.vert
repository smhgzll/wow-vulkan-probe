#version 450
layout(push_constant) uniform Position { float depth; } pos;
layout(set=0,binding=1) uniform Constants { uint marker; } constants;
void main() {
    vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(p[gl_VertexIndex],pos.depth+float(constants.marker & 255u)*0.0001,1);
}
