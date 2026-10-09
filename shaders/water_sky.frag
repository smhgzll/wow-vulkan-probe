#version 450
// Sky memory: a small equirectangular map of the sky in WORLD directions (u = azimuth, v = elevation 0..90 degrees).
// Every frame the texels whose direction is visible on screen and shows sky (depth == cleared value) are blended toward
// what the screen shows, so the map always holds the most recently seen real sky (time of day, clouds, sun). Directions
// never seen keep a default gradient. Water reflections read this instead of the screen edge, so looking down at the water
// (sky off screen) still reflects a believable sky instead of dark or garbage pixels.
layout(set=0,binding=0) uniform sampler2D camera;
layout(set=0,binding=2) uniform sampler2D sceneDepth;
layout(set=0,binding=3) uniform sampler2D sceneColor;
layout(std140,set=0,binding=6) uniform Parameters {
    vec4 viewport;vec4 depthRange;uvec4 vertex;uvec4 indices;ivec4 draw;vec4 options;vec4 tone;vec4 foam;vec4 look;vec4 extra;vec4 fx;
} params;
layout(location=0) out vec4 result;
vec4 column(int x) { return texelFetch(camera,ivec2(x,0),0); }
const vec2 skySize=vec2(128.,64.);
void main() {
    vec2 uv=gl_FragCoord.xy/skySize;
    float azimuth=(uv.x-.5)*6.2831853,elevation=uv.y*1.5707963;
    vec3 world=vec3(cos(elevation)*cos(azimuth),cos(elevation)*sin(azimuth),sin(elevation));
    // Default sky (never seen yet): warm horizon into a cooler zenith.
    vec3 fallback=params.draw.z==2?vec3(0.):mix(vec3(.80,.66,.52),vec3(.30,.45,.75),pow(uv.y,.5));   // draw.z==2: tests (black default)
    bool initialize=params.draw.z!=0;
    result=vec4(fallback,initialize?1.:0.);
    vec4 coefficients=column(13);
    if(coefficients.x<.5)return;
    // World direction -> view space with the rotation part of the view matrix (columns c5..c7 = images of the world axes).
    vec3 view=column(14).xyz*world.x+column(15).xyz*world.y+column(16).xyz*world.z;
    if(view.z<=.05)return;                                   // behind the camera
    vec4 projection=column(10),viewport=column(11),range=column(12);
    vec3 far=view*(projection.y*.5);
    vec2 ndc=far.xy*projection.zw/far.z;
    vec2 window=viewport.xy+(ndc*.5+.5)*viewport.zw;
    vec2 extent=vec2(textureSize(sceneDepth,0));
    if(any(lessThan(window,vec2(8.))) || any(greaterThanEqual(window,extent-8.)))return;
    ivec2 texel=ivec2(window);
    if(texelFetch(sceneDepth,texel,0).r<range.y-1e-6)return;   // something is drawn there, not sky
    result=vec4(texelFetch(sceneColor,texel,0).rgb,initialize?1.:.12);
}
