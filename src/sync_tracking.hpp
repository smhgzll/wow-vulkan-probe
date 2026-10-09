#pragma once
#include "layer_state.hpp"

namespace depth_probe {
struct AcquiredImage {
    uint64_t serial = 0;
    VkSemaphore semaphore{};
    VkQueue waitedQueue{};
    uint64_t waitSerial = 0;
    bool presented = false;
};
struct QueueSyncInfo {
    uint32_t family = UINT32_MAX;
    uint32_t index = UINT32_MAX;
    bool ordinary = false;
};
struct SemaphoreOrigin { VkQueue queue{}; uint64_t serial = 0; };
struct SyncDeviceState {
    bool maintenance = false;
    bool singleGraphicsQueue = false;
    bool deviceLost = false;
    VkResult terminalError = VK_SUCCESS;
    uint64_t serial = 0;
    uint64_t overlaySubmits = 0;
    uint64_t overlayQueueWaitIdleCount = 0;
    std::unordered_map<VkQueue, QueueSyncInfo> queues;
    std::unordered_map<VkSemaphore, SemaphoreOrigin> signals;
    std::unordered_map<VkSwapchainKHR, std::vector<AcquiredImage>> acquired;
    // A renderer may be destroyed after submit succeeded but present OOM'd.
    // Never forward that image's already-consumed original waits afterward.
    std::unordered_map<VkSwapchainKHR, std::unordered_map<uint32_t, VkResult>> abandonedPresents;
};
// Tracking is serialized by g_depth_lifecycle_mutex, never g_state_mutex.
void remember_sync_queue(const DeviceData&, VkQueue, uint32_t family, uint32_t index, VkDeviceQueueCreateFlags);
void forget_sync_swapchain(const DeviceData&, VkSwapchainKHR);
void remember_acquire(const DeviceData&, VkSwapchainKHR, uint32_t index, VkSemaphore, VkResult);
void consume_present_waits(const DeviceData&, const VkPresentInfoKHR*, VkResult);
const char* overlay_sync_support(const DeviceData&, VkQueue, const VkPresentInfoKHR*);
const char* present_extension_support(const VkPresentInfoKHR*);
void sync_skip(uint64_t frame, const char* reason);
bool present_operations_enqueued(VkResult result);
PFN_vkVoidFunction sync_hook(const char* name);
// Fence hooks settle borrowed application present fences before reset/destruction.
VkResult settle_borrowed_present_fence(const DeviceData&, VkFence);
} // namespace depth_probe
