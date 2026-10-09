#pragma once
#include "overlay_menu.hpp"
#include <cstdint>

// Toggle/marker state for the Camera Telemetry HUD and the 3D camera gizmo -
// entirely independent of OverlayMenu/F10 (see docs/inspector.md): F9
// shows/hides a small read-only text overlay, F8 shows/hides a world-space
// line gizmo at the active camera's anchor, F7 pins that gizmo in the world,
// Shift+F8 cycles the gizmo's camera source, Shift+F7 starts a memory-camera
// signature scan, Shift+F9 starts/stops a telemetry recording session, F6
// marks the next labelled event of the test sequence (Ctrl+1..9 a specific one),
// Shift+F6 cycles the depth -> world reconstruction debug views, Shift+F5 the volumetric fog (off, final, debug views). All of these are detected from the SAME passive
// (non-grabbing) keymap poll F10 already uses (overlay_input.cpp), so the
// game keyboard/mouse are never touched by any of this - closing F10 (or
// never opening it) does not affect any of it.
namespace depth_probe {
struct CameraHudInput {
    bool visible = false;     // F9: text HUD
    bool gizmoVisible = false; // F8: 3D world-space gizmo
    bool recording = false;
    bool gizmoLocked = false;  // F7: gizmo frozen at a world position, only the camera moves
    enum class GizmoSource : uint32_t { Auto, Water, Memory };
    GizmoSource gizmoSource = GizmoSource::Auto; // Shift+F8 cycles; DEPTH_PROBE_GIZMO_SOURCE sets the start value
    uint32_t fogMode = 0;      // Shift+F5: volumetric fog 0 off, 1 final, 2..6 debug views (volumetric_fog.cpp)
    static constexpr uint32_t fogModeCount = 8;
    uint32_t reconMode = 0;    // Shift+F6: depth -> world reconstruction debug view (0 off, 1..7, see shaders/depth_world.glsl)
    static constexpr uint32_t reconModeCount = 8;
    uint32_t nextMarker = 1;   // marker F6 queues next (1..9); restarts at 1 when a recording starts
private:
    bool f9WasDown = false, f8WasDown = false, f7WasDown = false, f6WasDown = false, shiftF9WasDown = false, shiftF8WasDown = false, shiftF7WasDown = false, shiftF6WasDown = false, shiftF5WasDown = false;
    bool scanRequested = false, sourceInitialized = false;
    uint32_t lastCtrlDigit = 0, pendingMarkerDigit = 0;
public:
    // Called once per polled input snapshot (same stream OverlayMenu::input
    // consumes - see poll_menu_input in overlay_input.cpp).
    void input(const MenuInput&);
    // True whenever the underlying camera capture/derivation pipeline should
    // stay active even though the F10 Camera Candidates page isn't open -
    // consulted by discovery::scene_debugger_active().
    bool wants_capture() const { return visible || gizmoVisible || recording || reconMode || fogMode; }
    bool panel_visible() const { return visible || gizmoVisible || recording || reconMode || (fogMode > 1); } // debug fog views show the panel; FINAL fog does not
    // Returns the most recently queued marker (F6 or Ctrl+<digit>, 1..9) since the
    // last call, or 0 if none - consuming (read-once) so it fires exactly
    // one recorded event per press, not once per frame while held.
    uint32_t take_marker();
    // Read-once: true once per Shift+F7 press.
    bool take_scan_request();
};
const char* gizmo_source_name(CameraHudInput::GizmoSource);
CameraHudInput& camera_hud_input();
}
