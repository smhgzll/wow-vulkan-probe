#pragma once
#include <cstdint>
// Test-only fake server, loaded by the production input worker via libxcb.so.1.
// It never connects to a display, grabs a desktop device or sends user input.
extern "C" {
void test_xcb_grab_status(uint8_t keyboard, uint8_t pointer);
void test_xcb_focus(bool owned);
void test_xcb_key(uint8_t code, bool down);
void test_xcb_pointer(int16_t x, int16_t y, bool down);
void test_xcb_button_event(bool down, int16_t x, int16_t y);
void test_xcb_key_event(uint8_t code);
unsigned test_xcb_queries();
unsigned test_xcb_grabs(); // live keyboard bit 1, pointer bit 2
unsigned test_xcb_attempts();
}
