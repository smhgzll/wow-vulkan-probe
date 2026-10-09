#version 450
layout(push_constant) uniform Position { float depth; } pos;
void main() {
    vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(p[gl_VertexIndex],pos.depth,1);
}
