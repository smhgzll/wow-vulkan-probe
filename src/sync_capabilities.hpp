#pragma once
#include "sync_tracking.hpp"

namespace depth_probe {
// Local copies keep application extension arrays and feature chains untouched.
struct SyncInstanceCreate {
    VkInstanceCreateInfo info{};
    std::vector<const char*> extensions;
    bool surfaceMaintenance = false;
    explicit SyncInstanceCreate(const VkInstanceCreateInfo&, PFN_vkGetInstanceProcAddr);
};
struct SyncDeviceCreate {
    VkDeviceCreateInfo info{};
    std::vector<const char*> extensions;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT feature{};
    bool maintenance = false;
    SyncDeviceCreate(const VkDeviceCreateInfo&, VkPhysicalDevice, const InstanceData&);
};
bool single_graphics_queue(const VkDeviceCreateInfo&, VkPhysicalDevice, const InstanceData&);
} // namespace depth_probe
