#pragma once
#include "overlay_settings.hpp"
#include "heartbeat.hpp"
#include "layer_state.hpp"

namespace depth_probe {
struct DepthOverlayFns;
struct DepthOverlayState;
struct alignas(16) OverlayUniform {
    OverlaySettings settings;
    HeartbeatConstants heartbeat;
    float viewport[4]{}; // width, height, menu scale, scene-color available
    uint32_t menu[4]{}; // visible, selected control, status, input available
    float pointer[4]{}; // panel-local x/y, pointer valid, reserved
    uint32_t panel[4]{}; // page, page count, manual projection readiness, reserved
    float rows[10][4]{}; // value, fraction, label ID, kind (1 toggle,2 slider,3 enum,4 number,5 unavailable,6 depth sample,7 label)
    uint32_t explorer[16]{}; // exact 64-bit identities as pairs; last word: capability
    uint32_t inspector[4]{}; // enabled, page, selection ready, highlight
    uint32_t pick[4]{}; // exact pixel x/y, readback ticket, reserved
    uint32_t inspectorText[384]{}; // 32 x 48 ASCII bytes, packed four per word
    uint32_t hud[4]{}; // visible, reserved, reserved, reserved
    uint32_t hudText[288]{}; // 24 x 48 ASCII bytes, packed four per word - Camera Telemetry HUD, independent of the menu/inspector panels above
    // 3D camera gizmo (F8, scene_debugger.cpp): world-space line primitives
    // drawn by shaders/gizmo.vert/.frag using a SEPARATE line-list pipeline
    // (see overlay_pipeline.cpp) - independent of the 2D text panels above.
    float gizmoAnchor[4]{};  // xyz world position, w = visible (0/1)
    float gizmoRight[4]{};   // xyz active camera right (unit), w = arm length (world units)
    float gizmoUp[4]{};      // xyz active camera up (unit), w = arrowhead size (world units)
    float gizmoForward[4]{}; // xyz active camera forward (unit), w = cross size (world units)
    float gizmoVP[16]{};     // ViewProjection, column-major (same convention as shaders/water_camera.frag)
    // Depth -> world reconstruction debug views (Shift+F6, shaders/depth_world.glsl), camera-relative
    // so WoW's ~17000-yard coordinates never enter the shader.
    uint32_t recon[4]{};       // mode (0 off), setup valid, reserved, reserved
    float reconViewport[4]{};  // the game's world-draw viewport: x, y, width, height (height < 0 in WoW)
    float reconDepth[4]{};     // viewport minDepth, maxDepth, P22, P23
    float reconProj[4]{};      // P00, P11, grid spacing (yd), probe radius (yd)
    float reconRight[4]{};     // xyz camera right; w = camera world z
    float reconUp[4]{};        // xyz camera up
    float reconForward[4]{};   // xyz camera forward
    float reconCameraMod[4]{}; // camera position modulo 1000 yd (world-locked patterns); w = probe enabled
    float reconProbe[4]{};     // probe (gizmo anchor) minus camera position
    // Light gizmo (fog menu LIGHT GIZMO, scene_debugger.cpp): screen-space markers for light directions, drawn by
    // shaders/gizmo.vert after the camera gizmo's 24 vertices - a line from the screen centre to where the direction
    // points, a circle there (an arrow at the edge when it is off the screen). gizmoVP is filled whenever it is on.
    float lightMarkers[4][4]{};  // xyz world direction toward the light, w = kind (0 none, 1 fog sun, 2 game light, 3 sky address, 4 seen disc)
    float lightGizmo[4]{};       // x on (0/1), y point lights shown (0..16), reserved x2
    // Point lights (fog menu LIGHT SOURCES, game_lights_capture.cpp): the game's nearest 16, drawn by shaders/gizmo.vert after
    // the direction markers - a circle facing the camera (radius pointLightColors w, yards) and a cross in it.
    float pointLights[16][4]{};      // xyz world position, w = on (0/1)
    float pointLightColors[16][4]{}; // rgb (brightest channel 1), w = circle radius (yards)
    float lightCameraRight[4]{};     // xyz the display camera's right (the circles face it)
    float lightCameraUp[4]{};        // xyz its up
};
static_assert(sizeof(OverlayUniform) == sizeof(OverlaySettings) + 1872 + 16 + 1152 + 128 + 144 + 80 + 544);
static_assert(offsetof(OverlayUniform, heartbeat) == sizeof(OverlaySettings) && offsetof(OverlayUniform, menu) == sizeof(OverlaySettings)+32);
struct alignas(16) OverlayFrameData {
    OverlayUniform uniform;
    VkDrawIndirectCommand menuDraw{};
    VkDrawIndirectCommand pickDraw{};
    VkDrawIndirectCommand previewDraw{};
    VkDrawIndirectCommand hudDraw{};
    VkDrawIndirectCommand gizmoDraw{};
};
static_assert(offsetof(OverlayFrameData, menuDraw) == sizeof(OverlayUniform) && sizeof(OverlayFrameData) == sizeof(OverlayUniform)+80);
struct MappedUniform {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped = nullptr;
    bool coherent = false;
};
bool create_overlay_uniforms(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&);
void destroy_overlay_uniforms(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&);
VkResult update_overlay_uniform(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&, uint32_t imageIndex);
// Pure upload helper also exercised against a real Vulkan device by smoke tests.
VkResult write_overlay_uniform(const DeviceData&, const DepthOverlayFns&, const MappedUniform&, const OverlayFrameData&);
} // namespace depth_probe
