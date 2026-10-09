#pragma once
#include "layer_state.hpp"

namespace depth_probe {
// All maps below use g_state_mutex. The lifecycle mutex must be acquired first
// whenever a selected image/view can be borrowed, published or destroyed.
extern std::recursive_mutex g_depth_lifecycle_mutex;

struct DepthCandidate {
    VkDevice device{};
    VkImage image{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags originalUsage = 0;
    VkImageCreateFlags flags = 0;
    uint32_t mipLevels = 0;
    uint32_t arrayLayers = 0;
    bool sampledSupported = false;
    bool sampledPatched = false;
    bool sampledReady = false;
    VkImageView depthView{};
    uint64_t generation = 0;
    uint32_t barrierLogs = 0; // bounded diagnostics, not GPU activity evidence
};

struct DepthPreparation {
    bool eligible = false;
    bool sampledSupported = false;
    const char* reason = "not a scene-target shape";
};

struct SceneDepthChoice {
    DepthCandidate candidate;
    int score = 0;
    uint32_t matchingCandidates = 0;
};

extern DeviceHandleMap<DepthCandidate> g_depth_candidates;
DepthPreparation prepare_depth_candidate(const DeviceData& data, const VkImageCreateInfo& info);
VkImageAspectFlags depth_barrier_aspects(VkFormat format);
int score_scene_depth_candidate(const DepthCandidate& candidate, VkDevice device, VkExtent2D extent);
std::optional<SceneDepthChoice> select_scene_depth_candidate(
    const DeviceHandleMap<DepthCandidate>& candidates, VkDevice device, VkExtent2D extent);
std::optional<SceneDepthChoice> resolve_scene_depth(VkDevice device, VkSwapchainKHR swapchain);
void invalidate_scene_depth_swapchain(VkDevice device, VkSwapchainKHR swapchain, const char* reason);
void invalidate_scene_depth_image(VkDevice device, VkImage image);
void invalidate_scene_depth_device(VkDevice device);
bool should_log_depth_barrier(VkDevice device, VkImage image);
void destroy_device_depth_views(const DeviceData& data);
} // namespace depth_probe
