#version 450
#extension GL_GOOGLE_include_directive : require
#include "overlay_uniform.glsl"
// 3D world-space debug gizmo (F8): 3 arrows along the active camera's
// right/up/forward basis (from its anchor point), plus a small world-axis
// cross marking the anchor itself. Procedural - no vertex buffer, mirroring
// menu.vert's corners[] pattern. 24 vertices, VK_PRIMITIVE_TOPOLOGY_LINE_LIST
// (each consecutive pair is one segment): 3 arrows x (1 main + 2 head
// segments) = 18, + 3 world-axis segments = 6. The light gizmo's markers follow (markerVertex).
layout(location = 0) out vec3 lineColor;

vec3 arrowVertex(vec3 anchor, vec3 dir, vec3 side, float armLength, float headSize, int i) {
    vec3 tip = anchor + dir * armLength;
    if (i == 0) return anchor;
    if (i == 1) return tip;
    if (i == 2) return tip;
    if (i == 3) return tip - dir * headSize + side * headSize * .5;
    if (i == 4) return tip;
    return tip - dir * headSize - side * headSize * .5;
}
// Light gizmo: 40 vertices per marker after the camera gizmo's 24 - [0,1] a line from the screen centre to where the
// light's direction lands (it shrinks to a point when you look straight at the light), [2..33] a circle there,
// [34..37] a cross in it (on the screen) or an arrowhead at the screen edge (off the screen / behind), [38,39] unused.
const int markerVertices = 40;
vec3 markerColor(float kind) {
    if (kind < 1.5) return vec3(1.0, .85, .2);    // the fog's sun (the shafts come from here)
    if (kind < 2.5) return vec3(.3, .9, 1.0);     // the game's lighting direction
    if (kind < 3.5) return vec3(1.0, .35, 1.0);   // the sky address (0x00D43180)
    return vec3(1.0);                             // a disc seen on the screen
}
vec4 markerVertex(int marker, int k, out vec3 color) {
    vec4 m = fx.lightMarkers[marker];
    color = markerColor(m.w);
    const vec4 none = vec4(2.0, 2.0, 0.0, 1.0);   // both ends of the segment off the screen: nothing drawn
    if (fx.lightGizmo.x < .5 || m.w <= 0.0 || k >= 38) return none;
    vec4 clip = fx.gizmoVP * vec4(m.xyz, 0.0);    // a direction: the point at infinity
    clip.y = -clip.y;                             // same flip as the camera gizmo below
    bool behind = clip.w <= 1e-4;
    vec2 ndc = behind ? -clip.xy : clip.xy / clip.w;
    bool on = !behind && all(lessThanEqual(abs(ndc), vec2(1.0)));
    vec2 target = ndc;
    if (!on) { float edge = max(max(abs(ndc.x), abs(ndc.y)), 1e-6); target = ndc / edge * .94; if (length(ndc) < 1e-6) target = vec2(0.0, .94); }
    vec2 aspect = vec2(fx.viewport.y / max(fx.viewport.x, 1.0), 1.0);   // round circles
    if (k < 2) return vec4(k == 0 ? vec2(0.0) : target, 0.0, 1.0);
    if (k < 34) {
        if (!on) return none;
        int s = (k - 2) / 2 + ((k - 2) & 1);      // segment start/end around the circle
        float a = 6.2831853 * float(s) / 16.0;
        return vec4(target + vec2(cos(a), sin(a)) * (m.w < 1.5 ? .065 : .045) * aspect, 0.0, 1.0);   // the fog's sun: a larger circle
    }
    if (on) {                                      // a cross in the circle
        vec2 d = k < 36 ? vec2(.02, 0.0) : vec2(0.0, .02);
        return vec4(target + ((k & 1) == 0 ? -d : d) * aspect, 0.0, 1.0);
    }
    vec2 dir = normalize(target), side = vec2(-dir.y, dir.x);   // an arrowhead at the edge
    if ((k & 1) == 0) return vec4(target, 0.0, 1.0);
    return vec4(target - dir * .05 + side * (k < 36 ? .03 : -.03), 0.0, 1.0);
}
// Point lights (fog menu LIGHT SOURCES): 40 vertices each after the 4 direction markers - [0..31] a circle of radius
// pointLightColors w yards around the light, facing the camera, [32..35] a cross in it, [36,37] a line a yard down (the
// light's height over what is below reads better), [38,39] unused. Nothing when the light is behind the camera.
vec4 pointVertex(int n, int k, out vec3 color) {
    vec4 l = fx.pointLights[n], c = fx.pointLightColors[n];
    color = c.rgb;
    const vec4 none = vec4(2.0, 2.0, 0.0, 1.0);
    if (l.w <= 0.0 || k >= 38) return none;
    vec4 centre = fx.gizmoVP * vec4(l.xyz, 1.0);
    if (centre.w <= .1) return none;
    vec3 right = fx.lightCameraRight.xyz * c.w, up = fx.lightCameraUp.xyz * c.w, p;
    if (k < 32) {
        int s = k / 2 + (k & 1);
        float a = 6.2831853 * float(s) / 16.0;
        p = l.xyz + right * cos(a) + up * sin(a);
    } else if (k < 36) {
        vec3 d = (k < 34 ? right : up) * .5;
        p = l.xyz + ((k & 1) == 0 ? -d : d);
    } else p = l.xyz - vec3(0.0, 0.0, k == 36 ? 0.0 : 1.0);
    vec4 clip = fx.gizmoVP * vec4(p, 1.0);
    clip.y = -clip.y;
    return clip;
}
void main() {
    if (gl_VertexIndex >= 24) {
        int i = gl_VertexIndex - 24;
        if (i < 4 * markerVertices) gl_Position = markerVertex(i / markerVertices, i % markerVertices, lineColor);
        else { i -= 4 * markerVertices; gl_Position = pointVertex(i / markerVertices, i % markerVertices, lineColor); }
        return;
    }
    if (fx.gizmoAnchor.w == 0.0) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); lineColor = vec3(0.0); return; }   // camera gizmo off, light gizmo on
    vec3 anchor = fx.gizmoAnchor.xyz;
    vec3 right = fx.gizmoRight.xyz, up = fx.gizmoUp.xyz, forward = fx.gizmoForward.xyz;
    float armLength = fx.gizmoRight.w, headSize = fx.gizmoUp.w, crossSize = fx.gizmoForward.w;
    int idx = gl_VertexIndex;
    vec3 pos;
    if (idx < 6) { pos = arrowVertex(anchor, right, up, armLength, headSize, idx); lineColor = vec3(1, .25, .25); }
    else if (idx < 12) { pos = arrowVertex(anchor, up, forward, armLength, headSize, idx - 6); lineColor = vec3(.3, 1, .3); }
    else if (idx < 18) { pos = arrowVertex(anchor, forward, right, armLength, headSize, idx - 12); lineColor = vec3(.35, .6, 1); }
    else {
        int c = idx - 18;
        vec3 axis = c < 2 ? vec3(1, 0, 0) : c < 4 ? vec3(0, 1, 0) : vec3(0, 0, 1);
        pos = anchor + axis * crossSize * (mod(float(c), 2.) < .5 ? -1. : 1.);
        lineColor = vec3(.95, .9, .25);
    }
    gl_Position = fx.gizmoVP * vec4(pos, 1.0);
    // gizmoVP is the game's D3D-style (+Y up) clip space; WoW draws it with a negative-height
    // viewport (docs/water.md: [0.5, 1080.5, 1920, -1080]) while this overlay's viewport is positive.
    gl_Position.y = -gl_Position.y;
}
