#pragma once
#include "layer_state.hpp"

namespace depth_probe {
void destroy_depth_overlay_state(const DeviceData& data, VkSwapchainKHR swapchain);
void destroy_device_depth_overlays(const DeviceData& data);
void destroy_depth_overlays_for_image(const DeviceData& data, VkImage image);
struct OverlayPresentResult {
    bool handled = false; // true: driver present already called or terminal error
    VkResult result = VK_SUCCESS;
    bool drawn = false;
};
OverlayPresentResult depth_overlay_present(const DeviceData& data, VkQueue queue,
                           const VkPresentInfoKHR* presentInfo, uint64_t frame);
} // namespace depth_probe
