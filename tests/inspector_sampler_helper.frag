#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=0,binding=0) uniform sampler heap[];
layout(set=1,binding=0) uniform texture2D sourceImage;
layout(push_constant) uniform Indices { layout(offset=4) uint packed; } pc;
layout(location=0) out vec4 color;
void localWrite(inout uint x) { x &= 65535u; }
void main() {
    uint index = pc.packed;
    localWrite(index);
    color = texture(sampler2D(sourceImage,heap[index]),vec2(.5));
}
