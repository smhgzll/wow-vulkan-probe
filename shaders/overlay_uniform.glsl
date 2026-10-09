// std140 matches OverlayUniform in src/overlay_uniform.hpp.
layout(std140, set = 0, binding = 1) uniform OverlayParameters {
    float aoStrength;
    float aoRadius;
    float aoBias;
    float rainbowStrength;
    float hueFrequency;
    float opacity;
    uint effectEnabled;
    uint debugMode;
    uvec4 depthConfig; // explicit convention, reserved
    vec4 projection; // near, far, vertical FOV degrees, ortho height
    uvec4 aoConfig; // enabled, samples, blur, reserved
    vec4 ssao; // strength, radius, bias, reserved
    uvec4 colorFlags; // sharpen, chromatic, fog, contours
    uvec4 edgeFlags; // outline, reserved
    vec4 colorParams; // sharpen, chromatic pixels, contour frequency/intensity
    vec4 fog; // start, end, density, red
    vec4 fogEdge; // fog green, blue, edge strength, width pixels
    uvec4 ssrConfig; // enabled, max steps, reserved
    vec4 ssrRay; // step size, thickness, max distance, edge fade
    vec4 ssrBlend; // reflection strength, reserved
    vec4 heartbeat; // phase, strength, aspectRatio, padding
    vec4 viewport;
    uvec4 menu;
    vec4 pointer;
    uvec4 panel;
    vec4 rows[10];
    uvec4 explorer[4];
    uvec4 inspector;
    uvec4 pick;
    uvec4 inspectorText[96];
    uvec4 hud;
    uvec4 hudText[72];
    vec4 gizmoAnchor;
    vec4 gizmoRight;
    vec4 gizmoUp;
    vec4 gizmoForward;
    mat4 gizmoVP;
    uvec4 recon;
    vec4 reconViewport;
    vec4 reconDepth;
    vec4 reconProj;
    vec4 reconRight;
    vec4 reconUp;
    vec4 reconForward;
    vec4 reconCameraMod;
    vec4 reconProbe;
    vec4 lightMarkers[4]; // xyz world direction toward a light, w = kind (0 none, 1 fog sun, 2 game light, 3 sky address, 4 seen disc)
    vec4 lightGizmo;      // x: on, y: point lights shown
    vec4 pointLights[16];      // xyz world position, w on (game_lights_capture.cpp)
    vec4 pointLightColors[16]; // rgb, w circle radius (yards)
    vec4 lightCameraRight;     // xyz display camera right
    vec4 lightCameraUp;        // xyz display camera up
} fx;
