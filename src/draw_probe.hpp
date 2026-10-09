#pragma once
#include "layer_state.hpp"
namespace depth_probe {
PFN_vkVoidFunction probe_hook(const char*);
void destroy_device_probe(VkDevice);
}
