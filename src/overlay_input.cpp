#include "overlay_menu.hpp"
#include "camera_hud_input.hpp"
#include "layer_state.hpp"
#include <chrono>
#include <array>
#include <condition_variable>
#include <thread>
#include <dlfcn.h>
#if __has_include(<xcb/xcb.h>)
#include <xcb/xcb.h>
#define DEPTH_PROBE_HAS_XCB_HEADERS 1
#else
#define DEPTH_PROBE_HAS_XCB_HEADERS 0
#endif
namespace depth_probe {
namespace {
#if DEPTH_PROBE_HAS_XCB_HEADERS
// A private connection never consumes Wine/SDL's event queue. Only the worker
// touches XCB; render frames exchange small input snapshots, without X roundtrips.
#define XCB_FUNCTIONS(X) \
 X(connect) X(disconnect) X(connection_has_error) X(get_setup) X(flush) X(poll_for_event) \
 X(intern_atom) X(intern_atom_reply) X(get_input_focus) X(get_input_focus_reply) \
 X(get_property) X(get_property_reply) X(get_property_value) X(get_property_value_length) \
 X(query_tree) X(query_tree_reply) X(query_keymap) X(query_keymap_reply) \
 X(get_keyboard_mapping) X(get_keyboard_mapping_reply) X(get_keyboard_mapping_keysyms) \
 X(grab_keyboard) X(grab_keyboard_reply) X(ungrab_keyboard) \
 X(grab_pointer) X(grab_pointer_reply) X(ungrab_pointer) \
 X(query_pointer) X(query_pointer_reply) X(get_geometry) X(get_geometry_reply)
struct Xcb {
    void* library = nullptr;
#define FIELD(name) decltype(&xcb_##name) name = nullptr;
    XCB_FUNCTIONS(FIELD)
#undef FIELD
    bool load() {
        library = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library) return false;
#define LOAD(name) name = reinterpret_cast<decltype(name)>(dlsym(library, "xcb_" #name)); if (!name) return false;
        XCB_FUNCTIONS(LOAD)
#undef LOAD
        return true;
    }
    ~Xcb() { if (library) dlclose(library); }
};
template<class T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
template<class T> Reply<T> reply(T* p) { return Reply<T>(p, &std::free); }
#endif
struct InputBackend {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false, requested = false, captured = false;
    uint32_t inputMode = 0; // 0 unavailable, 1 captured, 2 shared with game
    std::vector<MenuInput> events;
    std::chrono::steady_clock::time_point lastFrame{};
    std::thread worker;
    InputBackend() : worker([this] { run(); }) {}
    ~InputBackend() {
        { std::lock_guard lock(mutex); stopping = true; wake.notify_all(); }
        worker.join();
    }
    void run() {
        const char* enabled = std::getenv("DEPTH_PROBE_UI_INPUT");
        const char* display = std::getenv("DISPLAY");
        if ((enabled && std::string(enabled) == "0") || !display || !*display) {
            log_line("[UI-HOTKEY-UNAVAILABLE] reason=disabled-or-no-XWayland-DISPLAY config-reload=1");
            return;
        }
#if DEPTH_PROBE_HAS_XCB_HEADERS
        // Do not connect a render process to a remote X server.
        if (display[0] != ':' && std::string(display).rfind("unix:", 0) != 0) {
            log_line("[UI-HOTKEY-UNAVAILABLE] reason=nonlocal-DISPLAY config-reload=1"); return;
        }
        Xcb x;
        if (!x.load()) { log_line("[UI-HOTKEY-UNAVAILABLE] reason=libxcb-unavailable"); return; }
        auto* connection = x.connect(nullptr, nullptr);
        if (!connection || x.connection_has_error(connection)) {
            if (connection) x.disconnect(connection);
            log_line("[UI-HOTKEY-UNAVAILABLE] reason=XWayland-connection-failed"); return;
        }
        const char property[] = "_NET_WM_PID";
        auto atom = reply(x.intern_atom_reply(connection, x.intern_atom(connection, 1, sizeof(property)-1, property), nullptr));
        const auto* setup = x.get_setup(connection);
        auto keys = reply(x.get_keyboard_mapping_reply(connection, x.get_keyboard_mapping(connection,
            setup->min_keycode, setup->max_keycode - setup->min_keycode + 1), nullptr));
        if (!atom || !atom->atom || !keys) {
            x.disconnect(connection); log_line("[UI-HOTKEY-UNAVAILABLE] reason=missing-window-PID-or-keymap"); return;
        }
        const auto* symbols = x.get_keyboard_mapping_keysyms(keys.get());
        uint8_t f10 = 0, f9 = 0, f8 = 0, f7 = 0, f6 = 0, f5 = 0, digitCode[10] = {};
        auto keySymbol = [&](uint8_t code) -> uint32_t {
            if (code < setup->min_keycode || code > setup->max_keycode) return 0;
            return symbols[(code-setup->min_keycode)*keys->keysyms_per_keycode];
        };
        for (unsigned code = setup->min_keycode; code <= setup->max_keycode; ++code) {
            const auto sym = keySymbol(code);
            if (sym == 0xffc7) f10 = code;
            if (sym == 0xffc6) f9 = code; // Camera Telemetry HUD toggle - optional, degrades quietly if unmapped
            if (sym == 0xffc5) f8 = code; // 3D camera gizmo toggle - optional, degrades quietly if unmapped
            if (sym == 0xffc4) f7 = code; // gizmo world lock - optional, degrades quietly if unmapped
            if (sym == 0xffc3) f6 = code; // telemetry marker - optional, degrades quietly if unmapped
            if (sym == 0xffc2) f5 = code; // Shift+F5 volumetric fog views - optional
            if (sym >= 0x31 && sym <= 0x39) digitCode[sym-0x30] = code; // '1'..'9', for Ctrl+<digit> markers
        }
        if (!f10) { x.disconnect(connection); log_line("[UI-HOTKEY-UNAVAILABLE] reason=F10-unmapped"); return; }
        if (!f9) log_line("[UI-HOTKEY-UNAVAILABLE] reason=F9-unmapped (Camera Telemetry HUD hotkey unavailable, F10 menu unaffected)");
        auto ownFocusedWindow = [&]() -> xcb_window_t {
            auto focus = reply(x.get_input_focus_reply(connection, x.get_input_focus(connection), nullptr));
            xcb_window_t window = focus ? focus->focus : XCB_NONE;
            for (unsigned i = 0; window > XCB_INPUT_FOCUS_POINTER_ROOT && i < 16; ++i) {
                auto pid = reply(x.get_property_reply(connection, x.get_property(connection, 0, window, atom->atom,
                    XCB_ATOM_CARDINAL, 0, 1), nullptr));
                if (pid && pid->format == 32 && x.get_property_value_length(pid.get()) == 4) {
                    uint32_t value = 0; std::memcpy(&value, x.get_property_value(pid.get()), 4);
                    return value == static_cast<uint32_t>(getpid()) ? window : xcb_window_t(XCB_NONE);
                }
                auto tree = reply(x.query_tree_reply(connection, x.query_tree(connection, window), nullptr));
                if (!tree || tree->parent == window || tree->root == window) break;
                window = tree->parent;
            }
            return XCB_NONE;
        };
        bool keyboardGrabbed = false, pointerGrabbed = false, armed = false;
        bool wasOwned = false, announced = false, unavailable = false;
        xcb_window_t activeWindow = XCB_NONE;
        const char* sharedPointer = std::getenv("DEPTH_PROBE_UI_SHARED_POINTER");
        const bool allowSharedPointer = !sharedPointer || std::string(sharedPointer) != "0";
        std::array<bool, 256> previousKeys{};
        std::array<std::chrono::steady_clock::time_point, 256> repeatAt{};
        const auto started = std::chrono::steady_clock::now();
        auto release = [&] {
            if (pointerGrabbed) x.ungrab_pointer(connection, XCB_CURRENT_TIME);
            if (keyboardGrabbed) x.ungrab_keyboard(connection, XCB_CURRENT_TIME);
            if (pointerGrabbed || keyboardGrabbed) x.flush(connection);
            pointerGrabbed = keyboardGrabbed = armed = false;
            activeWindow = XCB_NONE;
            std::lock_guard lock(mutex);
            captured = false; inputMode = 0; wake.notify_all();
        };
        auto navigation = [](uint32_t symbol, bool shift) {
            switch (symbol) {
            case 0xff1b: return MenuKey::Escape;
            case 0xff52: return MenuKey::Up;
            case 0xff54: return MenuKey::Down;
            case 0xff09: return shift ? MenuKey::Up : MenuKey::Down;
            case 0xff51: return MenuKey::Left;
            case 0xff53: return MenuKey::Right;
            case 0xff0d: case 0x20: return MenuKey::Activate;
            default: return MenuKey::None;
            }
        };
        for (;;) {
            bool visible, stale;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(16));
                if (stopping) break;
                visible = requested;
                stale = std::chrono::steady_clock::now() - lastFrame > std::chrono::milliseconds(500);
            }
            if (x.connection_has_error(connection)) break;
            if (stale) { release(); continue; }
            const auto window = ownFocusedWindow();
            if (!window) {
                release(); previousKeys.fill(false);
                if (wasOwned) {
                    MenuInput lost; lost.focused = false;
                    std::lock_guard lock(mutex); events.push_back(lost);
                }
                wasOwned = false;
                if (!announced && !unavailable && std::chrono::steady_clock::now()-started > std::chrono::seconds(2)) {
                    log_line("[UI-HOTKEY-UNAVAILABLE] reason=no-focused-window-with-current-process-PID config-reload=1");
                    unavailable = true;
                }
                continue;
            }
            wasOwned = true;
            if (!announced) {
                log_line("[UI-HOTKEY-READY] backend=XWayland own-process-window=1 key=F10"); announced = true;
            }
            if (!visible || (activeWindow && window != activeWindow)) release();
            auto keymap = reply(x.query_keymap_reply(connection, x.query_keymap(connection), nullptr));
            if (!keymap) { release(); continue; }
            auto isDown = [&](unsigned code) {
                return (static_cast<unsigned char>(keymap->keys[code/8]) & (1u << (code%8))) != 0;
            };
            bool keysReleased = true, shift = false, ctrl = false;
            for (unsigned code = setup->min_keycode; code <= setup->max_keycode; ++code) {
                if (isDown(code)) {
                    keysReleased = false;
                    shift |= keySymbol(code) == 0xffe1 || keySymbol(code) == 0xffe2;
                    ctrl |= keySymbol(code) == 0xffe3 || keySymbol(code) == 0xffe4;
                }
            }
            // QueryPointer does not require an active grab. In particular, a
            // Wine-owned grab must not disable menu pointer coordinates/buttons.
            auto pointer = visible ? reply(x.query_pointer_reply(connection, x.query_pointer(connection, window), nullptr))
                                   : reply<xcb_query_pointer_reply_t>(nullptr);
            constexpr unsigned buttons = XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3 |
                XCB_BUTTON_MASK_4 | XCB_BUTTON_MASK_5;
            const bool buttonsReleased = pointer && pointer->same_screen && !(pointer->mask & buttons);
            // First let the game receive releases for controls held when F10
            // opened the menu. Afterwards keyboard and pointer are independent.
            if (visible && !armed && keysReleased && buttonsReleased) {
                auto keyboard = reply(x.grab_keyboard_reply(connection, x.grab_keyboard(connection, 0, window,
                    XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), nullptr));
                keyboardGrabbed = keyboard && keyboard->status == XCB_GRAB_STATUS_SUCCESS;
                auto mouse = reply(x.grab_pointer_reply(connection, x.grab_pointer(connection, 0, window,
                    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), nullptr));
                pointerGrabbed = mouse && mouse->status == XCB_GRAB_STATUS_SUCCESS;
                activeWindow = window; armed = true;
                // Keep a successful grab even if the other device is busy. We
                // never release/replace the game's grab or retry it every frame.
                log_line(std::string("[UI-INPUT] keyboard=") + (keyboardGrabbed ? "captured" : "polled") +
                    " pointer=" + (pointerGrabbed ? "captured" : allowSharedPointer ? "polled" : "disabled") +
                    " keyboardGrabStatus=" + std::to_string(keyboard ? keyboard->status : -1) +
                    " pointerGrabStatus=" + std::to_string(mouse ? mouse->status : -1) +
                    " sharedWithGame=" + std::to_string(!keyboardGrabbed || !pointerGrabbed));
            }
            MenuInput state;
            state.f10 = isDown(f10);
            state.f9 = f9 && isDown(f9);
            state.f8 = f8 && isDown(f8);
            state.f7 = f7 && isDown(f7);
            state.f6 = f6 && isDown(f6);
            state.f5 = f5 && isDown(f5);
            state.ctrl = ctrl;
            state.shift = shift;
            if (ctrl) for (unsigned d = 1; d <= 9; ++d) if (digitCode[d] && isDown(digitCode[d])) { state.ctrlDigit = d; break; }
            std::vector<MenuInput> batch;
            auto geometry = visible && armed ? reply(x.get_geometry_reply(connection, x.get_geometry(connection, window), nullptr))
                                             : reply<xcb_get_geometry_reply_t>(nullptr);
            auto coordinates = [&](MenuInput& e, int xPos, int yPos) {
                if (geometry && geometry->width && geometry->height) {
                    e.pointerValid = true;
                    // XCB gives an integer pixel. Carry its center through the
                    // normalized -> panel -> framebuffer round trip so float
                    // rounding cannot floor a boundary into the previous pixel.
                    e.x = (static_cast<float>(xPos) + .5f) / geometry->width;
                    e.y = (static_cast<float>(yPos) + .5f) / geometry->height;
                }
            };
            // Retain press/release events when we own that device. A quick click
            // may start and finish between QueryPointer snapshots.
            while (auto event = reply(x.poll_for_event(connection))) {
                if (!visible || !armed || (event->response_type & 0x80)) continue;
                const auto type = event->response_type & 0x7f;
                MenuInput action = state;
                if (keyboardGrabbed && type == XCB_KEY_PRESS) {
                    const auto* key = reinterpret_cast<const xcb_key_press_event_t*>(event.get());
                    action.key = navigation(keySymbol(key->detail), key->state & XCB_MOD_MASK_SHIFT);
                    if (action.key != MenuKey::None) batch.push_back(action);
                } else if (pointerGrabbed && (type == XCB_BUTTON_PRESS || type == XCB_BUTTON_RELEASE)) {
                    const auto* button = reinterpret_cast<const xcb_button_press_event_t*>(event.get());
                    if (button->detail == 1) {
                        action.leftDown = type == XCB_BUTTON_PRESS;
                        coordinates(action, button->event_x, button->event_y);
                        batch.push_back(action);
                    }
                }
            }
            // Shared keyboard fallback also covers Escape; no claim of exclusive
            // input is made when an existing game grab prevents capture.
            const auto now = std::chrono::steady_clock::now();
            for (unsigned code = setup->min_keycode; code <= setup->max_keycode; ++code) {
                const bool down = isDown(code);
                const auto key = navigation(keySymbol(code), shift);
                const bool edge = down && !previousKeys[code];
                const bool repeatable = key == MenuKey::Up || key == MenuKey::Down || key == MenuKey::Left || key == MenuKey::Right;
                if (visible && armed && (!keyboardGrabbed || key == MenuKey::Escape) && key != MenuKey::None &&
                    (edge || (down && repeatable && now >= repeatAt[code]))) {
                    MenuInput action = state; action.key = key; batch.push_back(action);
                    repeatAt[code] = now + std::chrono::milliseconds(edge ? 350 : 60);
                }
                previousKeys[code] = down;
            }
            if (visible && armed && (pointerGrabbed || allowSharedPointer) && pointer && pointer->same_screen) {
                coordinates(state, pointer->win_x, pointer->win_y);
                state.leftDown = pointer->mask & XCB_BUTTON_MASK_1;
            }
            bool close = false;
            for (const auto& action : batch) close |= action.key == MenuKey::Escape;
            if (close) { release(); state.pointerValid = state.leftDown = false; }
            batch.push_back(state);
            {
                std::lock_guard lock(mutex);
                captured = keyboardGrabbed || pointerGrabbed;
                inputMode = visible && armed ? (keyboardGrabbed && pointerGrabbed ? 1 : 2) : 0;
                if (events.size() + batch.size() > 256) events.clear();
                events.insert(events.end(), batch.begin(), batch.end());
            }
        }
        release(); x.disconnect(connection);
#else
        log_line("[UI-HOTKEY-UNAVAILABLE] reason=built-without-XCB-headers config-reload=1");
#endif
    }
};
// Lazily created on the first usable renderer. Cleanup of partially constructed
// renderers does not start a thread or contact a display server.
std::unique_ptr<InputBackend> backend;
}
void poll_menu_input(uint32_t width, uint32_t height) {
    if (const char* enabled = std::getenv("DEPTH_PROBE_UI_INPUT"); enabled && std::string(enabled) == "0") {
        static const bool logged = [] { log_line("[UI-HOTKEY-UNAVAILABLE] reason=disabled config-reload=1"); return true; }();
        (void)logged;
        return;
    }
    static bool workerFailed = false;
    if (workerFailed) return;
    if (!backend) {
        try { backend = std::make_unique<InputBackend>(); }
        catch (const std::system_error&) {
            workerFailed = true;
            log_line("[UI-HOTKEY-UNAVAILABLE] reason=input-thread-unavailable config-reload=1");
            return;
        }
    }
    std::vector<MenuInput> events;
    {
        std::lock_guard lock(backend->mutex);
        events.swap(backend->events);
    }
    auto& menu = overlay_menu();
    const auto scale = menu_scale(width, height);
    for (auto input : events) {
        if (input.pointerValid && scale > 0) {
            input.x = input.x * width / scale - menu_margin;
            input.y = input.y * height / scale - menu_margin;
        }
        menu.input(input);
        camera_hud_input().input(input); // coordinate-independent hotkeys - safe to run on the un-rescaled fields too
    }
    // File Save may block on disk; it never holds the input worker's mutex.
    std::lock_guard lock(backend->mutex);
    backend->lastFrame = std::chrono::steady_clock::now();
    if (backend->requested != menu.visible) { backend->requested = menu.visible; backend->wake.notify_all(); }
}
void release_menu_input() {
    if (!backend) return;
    std::unique_lock lock(backend->mutex);
    backend->requested = false;
    backend->events.clear();
    backend->wake.notify_all();
    // No Vulkan wait/submit. The worker also releases on focus loss and when
    // presents stop for 500ms, even if the render thread cannot run cleanup.
    backend->wake.wait_for(lock, std::chrono::milliseconds(100), [] { return !backend->captured; });
}
uint32_t menu_input_available() {
    if (!backend) return 0;
    std::lock_guard lock(backend->mutex);
    return backend->inputMode;
}
} // namespace depth_probe
