#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=0,binding=0) uniform sampler heap[];
layout(set=1,binding=0) uniform texture2D sourceImage;
layout(push_constant) uniform Indices { layout(offset=4) uint packed; } pc;
layout(location=0) out vec4 color;
void main() {
    color = .5 * (texture(sampler2D(sourceImage,heap[pc.packed & 65535u]),vec2(.5)) +
                  texture(sampler2D(sourceImage,heap[bitfieldExtract(pc.packed,16,16)]),vec2(.5)));
}
