#include "camera_hud_input.hpp"
#include "layer_state.hpp"
#include <cstdlib>
#include <cstring>

namespace depth_probe {
void CameraHudInput::input(const MenuInput& in) {
    if (!sourceInitialized) {
        sourceInitialized = true;
        if (const char* v = std::getenv("DEPTH_PROBE_GIZMO_SOURCE"); v && *v)
            gizmoSource = !std::strcmp(v, "water") ? GizmoSource::Water : !std::strcmp(v, "memory") ? GizmoSource::Memory : GizmoSource::Auto;
    }
    if (!in.focused) return; // mirrors OverlayMenu::input - do nothing while the game window isn't focused

    // Ctrl+F-keys are KDE global shortcuts (Ctrl+F7/F9 Present Windows, Ctrl+F1..F4 desktops): KWin takes
    // them before the game sees them, so every chord here is plain or Shift+F-key.
    auto edge = [](bool down, bool& wasDown) { const bool e = down && !wasDown; wasDown = down; return e; };
    const bool plain = !in.ctrl && !in.shift, shifted = in.shift && !in.ctrl;

    if (edge(in.f9 && plain, f9WasDown)) { visible = !visible; log_line("[UI-HOTKEY] key=F9 visible=" + std::to_string(visible)); }
    if (edge(in.f9 && shifted, shiftF9WasDown)) {
        recording = !recording;
        if (recording) nextMarker = 1;
        log_line("[UI-HOTKEY] key=SHIFT+F9 recording=" + std::to_string(recording));
    }
    if (edge(in.f8 && plain, f8WasDown)) { gizmoVisible = !gizmoVisible; log_line("[UI-HOTKEY] key=F8 gizmoVisible=" + std::to_string(gizmoVisible)); }
    if (edge(in.f8 && shifted, shiftF8WasDown)) {
        gizmoSource = GizmoSource((uint32_t(gizmoSource) + 1) % 3);
        log_line(std::string("[UI-HOTKEY] key=SHIFT+F8 gizmoSource=") + gizmo_source_name(gizmoSource));
    }
    if (edge(in.f7 && plain, f7WasDown)) { gizmoLocked = !gizmoLocked; log_line("[UI-HOTKEY] key=F7 gizmoLocked=" + std::to_string(gizmoLocked)); }
    if (edge(in.f7 && shifted, shiftF7WasDown)) { scanRequested = true; log_line("[UI-HOTKEY] key=SHIFT+F7 memory-camera-scan-requested"); }
    if (edge(in.f5 && shifted, shiftF5WasDown)) {
        fogMode = (fogMode + 1) % fogModeCount;
        log_line("[UI-HOTKEY] key=SHIFT+F5 fogMode=" + std::to_string(fogMode));
    }
    if (edge(in.f6 && shifted, shiftF6WasDown)) {
        reconMode = (reconMode + 1) % reconModeCount;
        log_line("[UI-HOTKEY] key=SHIFT+F6 reconMode=" + std::to_string(reconMode));
    }
    if (edge(in.f6 && plain, f6WasDown)) {
        pendingMarkerDigit = nextMarker;
        log_line("[UI-HOTKEY] key=F6 marker=" + std::to_string(nextMarker) + " queued");
        nextMarker = nextMarker % 9 + 1;
    }

    if (in.ctrlDigit != 0 && in.ctrlDigit != lastCtrlDigit) {
        pendingMarkerDigit = in.ctrlDigit;
        nextMarker = in.ctrlDigit % 9 + 1;
        log_line("[UI-HOTKEY] key=CTRL+" + std::to_string(in.ctrlDigit) + " marker-queued");
    }
    lastCtrlDigit = in.ctrlDigit;
}
uint32_t CameraHudInput::take_marker() {
    const auto d = pendingMarkerDigit;
    pendingMarkerDigit = 0;
    return d;
}
bool CameraHudInput::take_scan_request() {
    const bool requested = scanRequested;
    scanRequested = false;
    return requested;
}
const char* gizmo_source_name(CameraHudInput::GizmoSource s) {
    return s == CameraHudInput::GizmoSource::Water ? "WATER" : s == CameraHudInput::GizmoSource::Memory ? "MEMORY" : "AUTO";
}
CameraHudInput& camera_hud_input() {
    // DEPTH_PROBE_FOG_START=1 (tools/run-inspector.sh sets it unless told otherwise): the volumetric fog starts on (FINAL). Not a code default, so
    // tests and other hosts start with it off.
    static CameraHudInput state = [] {
        CameraHudInput s;
        if (const char* e = std::getenv("DEPTH_PROBE_FOG_START"); e && !std::strcmp(e, "1")) {
            s.fogMode = 1;
            log_line("[UI-HOTKEY] fog on at start (DEPTH_PROBE_FOG_START=1)");
        }
        return s;
    }();
    return state;
}
}
