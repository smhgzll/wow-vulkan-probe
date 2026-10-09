#version 450
// Pre-projected clip-space positions (the fog smoke projects on the CPU with the layer's own camera math).
layout(location = 0) in vec4 clip;
void main() { gl_Position = clip; }
