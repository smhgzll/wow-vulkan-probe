#version 450
// An effect blended over the water later in the pass (a fishing pool's swirl): additive, its own fragment shader (not the water's pair).
layout(push_constant) uniform Push {vec4 factor;} pc;
layout(location=0) out vec4 color;
void main(){color=vec4(.20,.10,.06,1.)*pc.factor;}
