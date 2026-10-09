#include "overlay_settings.hpp"
#include "overlay_menu.hpp"
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <chrono>
#include <unistd.h>
using namespace depth_probe;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
int main(int argc, char**) {
    if (argc > 1) {
        // Exercise the optional backend without connecting to the user's desktop.
        unsetenv("DISPLAY"); unsetenv("DEPTH_PROBE_UI_INPUT");
        setenv("DEPTH_PROBE_MENU_VISIBLE", "1", 1);
        setenv("DEPTH_PROBE_LAYER_LOG", "/tmp/depth-probe-input-fallback-test.log", 1);
        poll_menu_input(1280, 720);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        poll_menu_input(1280, 720);
        CHECK(overlay_menu().visible && menu_input_available() == 0);
        release_menu_input();
        std::puts("PASS: no-DISPLAY input worker exits safely; environment-visible UI remains usable with config");
        return 0;
    }
    const std::string root = "/tmp/depth-probe-settings-test-" + std::to_string(getpid());
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    setenv("DEPTH_PROBE_LAYER_LOG", (root + ".log").c_str(), 1);
    const auto path = settings_config_path();
    CHECK(path == root + "/vulkan-depth-probe/settings.ini");
    const OverlaySettings defaults;
    CHECK(defaults.aoStrength == 1 && defaults.aoRadius == 16 && defaults.aoBias == .002f);
    CHECK(defaults.opacity == .95f && defaults.rainbowStrength == 1 && defaults.hueFrequency == 1);
    CHECK(defaults.effectEnabled == 1 && defaults.debugMode == 3);
    CHECK(load_settings_file(path) == defaults);
    auto modified = defaults; modified.aoStrength = 2.5f; modified.aoRadius = 24; modified.debugMode = 1; modified.depthConvention = 2; modified.projectionMode = 1; modified.nearPlane = 1; modified.farPlane = 101; modified.verticalFov = 60; modified.ssaoEnabled = 1; modified.ssaoSamples = 16; modified.ssaoRadius = 2; modified.aoBlur = 1; modified.sharpenEnabled=1; modified.chromaticEnabled=1; modified.fogEnabled=1; modified.contoursEnabled=1; modified.outlineEnabled=1; modified.fogR=.7f; modified.edgeWidth=3; modified.ssrEnabled=1; modified.ssrSteps=32; modified.ssrThickness=.3f; modified.reflectionStrength=.8f;
    CHECK(save_settings_file(path, modified));
    CHECK(load_settings_file(path) == modified);
    { std::ofstream out(path); out << "ao_strength=nan\nao_radius=999\nao_bias=garbage\nopacity=-2\ndebug_mode=999\neffect_enabled=-1\nbroken line\n"; }
    bool bad = false; auto parsed = load_settings_file(path, &bad);
    CHECK(bad && parsed.aoStrength == 1 && parsed.aoRadius == 32 && parsed.aoBias == .002f && parsed.opacity == 0);
    CHECK(parsed.debugMode == 3 && parsed.effectEnabled == 1);
    { std::ofstream out(path); out << std::string(70000, 'x'); }
    CHECK(load_settings_file(path, &bad) == defaults && bad);
    set_overlay_settings(modified); CHECK(overlay_settings() == modified);
    CHECK(save_overlay_settings());
    CHECK(load_settings_file(path) == modified);
    auto unsaved = modified; unsaved.aoStrength = .4f;
    set_overlay_settings(unsaved);
    poll_settings_reload(); // our own Save must not undo subsequent slider edits
    CHECK(overlay_settings() == unsaved);
    reset_overlay_settings(); CHECK(overlay_settings() == defaults);
    modified.aoStrength = std::numeric_limits<float>::infinity();
    CHECK(sanitize_settings(modified).aoStrength == defaults.aoStrength);
    OverlayMenu menu;
    MenuInput input; input.f10 = true;
    menu.input(input); CHECK(menu.visible);
    menu.input(input); CHECK(menu.visible); // held F10 cannot toggle repeatedly
    input.f10 = false; menu.input(input);
    input.f10 = true; menu.input(input); CHECK(!menu.visible);
    input.f10 = false; menu.input(input);
    input.f10 = true; menu.input(input); CHECK(menu.visible);
    input.f10 = false; input.key = MenuKey::Activate; menu.input(input);
    CHECK(!overlay_settings().effectEnabled);
    input.key = MenuKey::Down; menu.input(input); CHECK(menu.selected == 1);
    input.key = MenuKey::Right; menu.input(input); CHECK(overlay_settings().aoStrength > 1);
    input.key = MenuKey::None; input.pointerValid = input.leftDown = true;
    input.x = 376; input.y = 178; menu.input(input); CHECK(overlay_settings().aoStrength == 3);
    input.leftDown = false; menu.input(input);
    input.x = 220; input.y = 560; input.leftDown = true; menu.input(input);
    CHECK(overlay_settings() == defaults && menu.status == 3);
    input.leftDown = false; input.key = MenuKey::Escape; menu.input(input); CHECK(!menu.visible);
    menu.visible = true; input.key = MenuKey::None; input.focused = false;
    menu.input(input); CHECK(!menu.visible && menu.dragging == -1);
    CHECK(menu_scale(1280,720) == 1 && menu_scale(320,240) < 1);
    menu.visible=true;menu.page=0;input={};input.pointerValid=true;
    input.x=330;input.y=30;input.leftDown=true;menu.input(input);CHECK(menu.page==1);
    input.leftDown=false;menu.input(input);input.x=30;input.y=78;input.leftDown=true;
    menu.input(input);CHECK(overlay_settings().debugMode==4); // Enum row cycles.
    input.leftDown=false;menu.input(input);
    input.x=264;input.y=78+3*44+30;input.leftDown=true;menu.input(input);
    CHECK(overlay_settings().nearPlane>99990); // Actual drawn 240px slider extent.
    input.leftDown=false;menu.input(input);input.x=330;input.y=30;input.leftDown=true;menu.input(input);CHECK(menu.page==2);
    input.leftDown=false;menu.input(input);input.x=30;input.y=78+3*44;input.leftDown=true;menu.input(input);
    CHECK(overlay_settings().rainbowEnabled==1); // default is off; one click toggles it on
    input.leftDown=false;menu.input(input);input.x=30;input.y=560;input.leftDown=true;menu.input(input);
    CHECK(load_settings_file(path)==overlay_settings());
    input.leftDown=false;menu.input(input);input.x=30;input.y=542;input.leftDown=true;menu.input(input);CHECK(menu.page==1);
    reset_overlay_settings();
    std::filesystem::remove_all(root);
    std::puts("PASS: shader defaults, std140 ABI, config round-trip/missing/corrupt/oversized, reset, F10 edge, Escape/focus loss, keyboard and pointer controls");
}
