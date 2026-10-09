#pragma once
#include "layer_state.hpp"

namespace depth_probe {
struct OverlayUniform;
// Opt-in observational hooks; submit/present wrappers call the existing layer
// entry points, never create a submit or change semaphore/fence arguments.
bool discovery_enabled();
PFN_vkVoidFunction discovery_hook(const char*);
void destroy_device_discovery(VkDevice);
uint32_t discovery_first_page();
void populate_discovery_menu(VkDevice, OverlayUniform&);
bool discovery_menu_input(uint32_t page, uint32_t control, int direction, bool activate);
void stop_discovery_isolation();
// Request one bounded capture; also used by the independent regression harness.
void request_discovery_capture(VkDevice, uint32_t frames = 8);
void flush_discovery_exports();
// Cached downstream function pointers must be dropped whenever a device is created or destroyed.
void invalidate_hook_cache();
}
