#include "fake_xcb.hpp"
#include <xcb/xcb.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unistd.h>
namespace {
std::mutex mutex;
std::array<uint8_t,32> keys{};
std::deque<xcb_generic_event_t> events;
std::atomic<bool> focused{true};
std::atomic<uint8_t> keyboardStatus{1}, pointerStatus{1};
std::atomic<unsigned> queries{0}, grabs{0}, attempts{0};
int16_t pointerX=60, pointerY=105;
bool pointerDown=false;
int connection;
xcb_setup_t setup=[] { xcb_setup_t s{};s.min_keycode=8;s.max_keycode=40;return s; }();
template<class T> T* alloc(size_t extra=0) { return static_cast<T*>(std::calloc(1,sizeof(T)+extra)); }
}
extern "C" {
void test_xcb_grab_status(uint8_t keyboard,uint8_t pointer) { keyboardStatus=keyboard;pointerStatus=pointer; }
void test_xcb_focus(bool owned) { focused=owned; }
void test_xcb_key(uint8_t code,bool down) {
    std::lock_guard lock(mutex);
    if(down)keys[code/8]|=1u<<(code%8);else keys[code/8]&=~(1u<<(code%8));
}
void test_xcb_pointer(int16_t x,int16_t y,bool down) { std::lock_guard lock(mutex);pointerX=x;pointerY=y;pointerDown=down; }
void test_xcb_button_event(bool down,int16_t x,int16_t y) {
    std::lock_guard lock(mutex);xcb_button_press_event_t b{};
    b.response_type=down?XCB_BUTTON_PRESS:XCB_BUTTON_RELEASE;b.detail=1;b.event=101;b.event_x=x;b.event_y=y;
    xcb_generic_event_t event{};std::memcpy(&event,&b,sizeof(b));events.push_back(event);
}
void test_xcb_key_event(uint8_t code) {
    std::lock_guard lock(mutex);xcb_key_press_event_t k{};k.response_type=XCB_KEY_PRESS;k.detail=code;
    xcb_generic_event_t event{};std::memcpy(&event,&k,sizeof(k));events.push_back(event);
}
unsigned test_xcb_queries(){return queries;}
unsigned test_xcb_grabs(){return grabs;}
unsigned test_xcb_attempts(){return attempts;}
xcb_connection_t* xcb_connect(const char*,int*) { return reinterpret_cast<xcb_connection_t*>(&connection); }
void xcb_disconnect(xcb_connection_t*) { grabs=0; }
int xcb_connection_has_error(xcb_connection_t*) { return 0; }
const xcb_setup_t* xcb_get_setup(xcb_connection_t*) { return &setup; }
int xcb_flush(xcb_connection_t*) { return 1; }
xcb_generic_event_t* xcb_poll_for_event(xcb_connection_t*) {
    std::lock_guard lock(mutex);if(events.empty())return nullptr;
    auto* r=alloc<xcb_generic_event_t>();*r=events.front();events.pop_front();return r;
}
xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t*,uint8_t,uint16_t,const char*) { return {}; }
xcb_intern_atom_reply_t* xcb_intern_atom_reply(xcb_connection_t*,xcb_intern_atom_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_intern_atom_reply_t>();r->atom=42;return r;
}
xcb_get_input_focus_cookie_t xcb_get_input_focus(xcb_connection_t*) { return {}; }
xcb_get_input_focus_reply_t* xcb_get_input_focus_reply(xcb_connection_t*,xcb_get_input_focus_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_get_input_focus_reply_t>();r->focus=focused?101:1;return r;
}
xcb_get_property_cookie_t xcb_get_property(xcb_connection_t*,uint8_t,xcb_window_t,xcb_atom_t,xcb_atom_t,uint32_t,uint32_t){return {};}
xcb_get_property_reply_t* xcb_get_property_reply(xcb_connection_t*,xcb_get_property_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_get_property_reply_t>(4);r->format=32;r->value_len=1;
    uint32_t pid=getpid();std::memcpy(r+1,&pid,4);return r;
}
void* xcb_get_property_value(const xcb_get_property_reply_t* r){return const_cast<xcb_get_property_reply_t*>(r+1);}
int xcb_get_property_value_length(const xcb_get_property_reply_t*){return 4;}
xcb_query_tree_cookie_t xcb_query_tree(xcb_connection_t*,xcb_window_t){return {};}
xcb_query_tree_reply_t* xcb_query_tree_reply(xcb_connection_t*,xcb_query_tree_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_query_tree_reply_t>();r->parent=r->root=1;return r;
}
xcb_query_keymap_cookie_t xcb_query_keymap(xcb_connection_t*){return {};}
xcb_query_keymap_reply_t* xcb_query_keymap_reply(xcb_connection_t*,xcb_query_keymap_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_query_keymap_reply_t>();std::lock_guard lock(mutex);std::memcpy(r->keys,keys.data(),keys.size());++queries;return r;
}
xcb_get_keyboard_mapping_cookie_t xcb_get_keyboard_mapping(xcb_connection_t*,xcb_keycode_t,uint8_t){return {};}
xcb_get_keyboard_mapping_reply_t* xcb_get_keyboard_mapping_reply(xcb_connection_t*,xcb_get_keyboard_mapping_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_get_keyboard_mapping_reply_t>(33*4);r->keysyms_per_keycode=1;
    auto* symbols=reinterpret_cast<xcb_keysym_t*>(r+1);
    // keycodes 10..19: F10, Escape, Tab, Up, Down, Left, Right, Enter, Space, Shift.
    const uint32_t values[]{0xffc7,0xff1b,0xff09,0xff52,0xff54,0xff51,0xff53,0xff0d,0x20,0xffe1};
    std::memcpy(symbols+2,values,sizeof(values));return r;
}
xcb_keysym_t* xcb_get_keyboard_mapping_keysyms(const xcb_get_keyboard_mapping_reply_t* r){return reinterpret_cast<xcb_keysym_t*>(const_cast<xcb_get_keyboard_mapping_reply_t*>(r+1));}
xcb_grab_keyboard_cookie_t xcb_grab_keyboard(xcb_connection_t*,uint8_t,xcb_window_t,xcb_timestamp_t,uint8_t,uint8_t){++attempts;return {};}
xcb_grab_keyboard_reply_t* xcb_grab_keyboard_reply(xcb_connection_t*,xcb_grab_keyboard_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_grab_keyboard_reply_t>();r->status=keyboardStatus;if(!r->status)grabs.fetch_or(1);return r;
}
xcb_void_cookie_t xcb_ungrab_keyboard(xcb_connection_t*,xcb_timestamp_t){grabs.fetch_and(~1u);return {};}
xcb_grab_pointer_cookie_t xcb_grab_pointer(xcb_connection_t*,uint8_t,xcb_window_t,uint16_t,uint8_t,uint8_t,xcb_window_t,xcb_cursor_t,xcb_timestamp_t){++attempts;return {};}
xcb_grab_pointer_reply_t* xcb_grab_pointer_reply(xcb_connection_t*,xcb_grab_pointer_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_grab_pointer_reply_t>();r->status=pointerStatus;if(!r->status)grabs.fetch_or(2);return r;
}
xcb_void_cookie_t xcb_ungrab_pointer(xcb_connection_t*,xcb_timestamp_t){grabs.fetch_and(~2u);return {};}
xcb_query_pointer_cookie_t xcb_query_pointer(xcb_connection_t*,xcb_window_t){return {};}
xcb_query_pointer_reply_t* xcb_query_pointer_reply(xcb_connection_t*,xcb_query_pointer_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_query_pointer_reply_t>();std::lock_guard lock(mutex);
    r->same_screen=1;r->win_x=pointerX;r->win_y=pointerY;r->mask=pointerDown?XCB_BUTTON_MASK_1:0;return r;
}
xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t*,xcb_drawable_t){return {};}
xcb_get_geometry_reply_t* xcb_get_geometry_reply(xcb_connection_t*,xcb_get_geometry_cookie_t,xcb_generic_error_t**) {
    auto* r=alloc<xcb_get_geometry_reply_t>();r->width=1920;r->height=1080;return r;
}
}
