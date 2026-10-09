#pragma once
#include "overlay_settings.hpp"
#include <cstdint>
namespace depth_probe {
constexpr float menu_width = 400, menu_height = 648, menu_margin = 16;
struct MenuRect { float x, y, w, h; };
MenuRect menu_control_rect(unsigned control, unsigned page = 0);
float menu_scale(uint32_t width, uint32_t height);
float menu_slider_value(const OverlaySettings&, unsigned slider);
void menu_set_slider(OverlaySettings&, unsigned slider, float fraction);
enum class MenuKey { None, Escape, Up, Down, Left, Right, Activate };
struct MenuInput {
    bool f10 = false; // physical state, rising edge only
    // Camera Telemetry HUD / gizmo hotkeys (F9/F8/F7/F6 plain or with Shift, Ctrl+1..9) -
    // physical state, read from the SAME passive (non-grabbing) keymap poll
    // as f10, so they work whether or not the F10 menu is open and never
    // take the keyboard away from the game. See camera_hud_input.hpp.
    bool f9 = false, f8 = false, f7 = false, f6 = false, f5 = false, ctrl = false, shift = false;
    uint32_t ctrlDigit = 0; // 1..9 if Ctrl+<digit> is currently down, else 0
    bool focused = true;
    MenuKey key = MenuKey::None;
    bool pointerValid = false, leftDown = false;
    float x = 0, y = 0; // panel-local coordinates
};
// Accessed only under g_depth_lifecycle_mutex, including backend polling/cleanup.
struct OverlayMenu {
    bool visible = false, f10WasDown = false, leftWasDown = false;
    int dragging = -1;
    float pointerX = 0, pointerY = 0;
    bool pointerValid = false;
    uint32_t page = 0;
    uint32_t selected = 0, status = 0; // 0 editable, 1 saved, 2 save failed, 3 defaults
    void input(const MenuInput&);
};
OverlayMenu& overlay_menu();
struct OverlayUniform;
void populate_menu_uniform(OverlayUniform&);
uint32_t menu_page_count();
void poll_menu_input(uint32_t width, uint32_t height);
void release_menu_input();
uint32_t menu_input_available(); // 0 config fallback, 1 captured, 2 shared/polled input
} // namespace depth_probe
