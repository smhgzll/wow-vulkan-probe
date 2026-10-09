#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <unistd.h>

#if defined(__GNUC__)
#define LAYER_EXPORT __attribute__((visibility("default")))
#else
#define LAYER_EXPORT
#endif

namespace depth_probe {

constexpr const char* kLayerName = "VK_LAYER_depth_probe";

template <typename T>
void* dispatch_key(T handle) {
    if (handle == VK_NULL_HANDLE) return nullptr;
    return *reinterpret_cast<void**>(handle);
}

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    PFN_vkDestroyInstance next_destroy_instance = nullptr;
    PFN_vkEnumeratePhysicalDevices next_enumerate_physical_devices = nullptr;
    PFN_vkEnumeratePhysicalDeviceGroups next_enumerate_physical_device_groups = nullptr;
    bool surfaceMaintenance = false;
    uint32_t apiVersion = VK_API_VERSION_1_0;
};

struct SyncDeviceState;
// Command-recording entry points the water replay uses hundreds of times per frame. vkGetDeviceProcAddr is a string lookup through
// the loader, so they are resolved once per device (water_reflections.cpp fills this on first use).
struct WaterReplayFunctions {
    bool resolved = false;
    PFN_vkCmdSetViewport setViewport = nullptr;
    PFN_vkCmdSetScissor setScissor = nullptr;
    PFN_vkCmdDrawIndexed drawIndexed = nullptr;
    PFN_vkCmdPushConstants pushConstants = nullptr;
    PFN_vkCmdBindVertexBuffers bindVertexBuffers = nullptr;
    PFN_vkCmdBindVertexBuffers2 bindVertexBuffers2 = nullptr;
    PFN_vkCmdBindIndexBuffer bindIndexBuffer = nullptr;
    PFN_vkCmdBindDescriptorSets bindDescriptorSets = nullptr;
    PFN_vkCmdUpdateBuffer updateBuffer = nullptr;
    PFN_vkCmdCopyImage copyImage = nullptr;
    PFN_vkCmdClearColorImage clearColorImage = nullptr;
};

struct DeviceData {
    VkDevice device = VkDevice{};
    mutable WaterReplayFunctions waterFns;
    std::shared_ptr<SyncDeviceState> sync;
    VkPhysicalDevice physical_device = VkPhysicalDevice{};
    PFN_vkGetPhysicalDeviceProperties get_properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_properties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties get_format_properties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties get_image_format_properties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image_format_properties2 = nullptr;
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
    PFN_vkSetDeviceLoaderData set_device_loader_data = nullptr;
    PFN_vkDestroyDevice next_destroy_device = nullptr;
    PFN_vkCreateImage next_create_image = nullptr;
    PFN_vkDestroyImage next_destroy_image = nullptr;
    PFN_vkDestroyImageView next_destroy_image_view = nullptr;
    PFN_vkCreateImageView next_create_image_view = nullptr;

    PFN_vkGetDeviceQueue next_get_device_queue = nullptr;
    PFN_vkGetDeviceQueue2 next_get_device_queue2 = nullptr;

    PFN_vkCreateSwapchainKHR next_create_swapchain = nullptr;
    PFN_vkDestroySwapchainKHR next_destroy_swapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR next_get_swapchain_images = nullptr;
    PFN_vkQueuePresentKHR next_queue_present = nullptr;

    PFN_vkCreateCommandPool next_create_command_pool = nullptr;
    PFN_vkDestroyCommandPool next_destroy_command_pool = nullptr;
    PFN_vkAllocateCommandBuffers next_allocate_command_buffers = nullptr;
    PFN_vkBeginCommandBuffer next_begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer next_end_command_buffer = nullptr;
    PFN_vkQueueSubmit next_queue_submit = nullptr;
    PFN_vkQueueWaitIdle next_queue_wait_idle = nullptr;
    PFN_vkCmdPipelineBarrier next_cmd_pipeline_barrier = nullptr;
    PFN_vkCmdPipelineBarrier2 next_cmd_pipeline_barrier2 = nullptr;
    PFN_vkCmdClearColorImage next_cmd_clear_color_image = nullptr;
};

struct SwapchainInfo {
    VkImageUsageFlags usage = 0;
    bool retired = false;
    bool overlaySyncSupported = false;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t image_count = 0;
    std::vector<VkImage> images;
};

// Non-dispatchable handles need not be unique across logical devices.
struct DeviceHandleKey {
    VkDevice device = VkDevice{};
    uint64_t handle = 0;
    bool operator==(const DeviceHandleKey&) const = default;
};
struct DeviceHandleHash {
    size_t operator()(const DeviceHandleKey& key) const {
        return std::hash<VkDevice>{}(key.device) ^ (std::hash<uint64_t>{}(key.handle) << 1);
    }
};
template <typename T>
using DeviceHandleMap = std::unordered_map<DeviceHandleKey, T, DeviceHandleHash>;

extern std::mutex g_state_mutex;
extern std::unordered_map<void*, InstanceData> g_instances;
extern std::unordered_map<void*, DeviceData> g_devices;
extern std::unordered_map<VkPhysicalDevice, VkInstance> g_physical_to_instance;

extern DeviceHandleMap<SwapchainInfo> g_swapchains;
extern std::atomic<uint64_t> g_present_counter;


template <typename T>
T* find_loader_create_info(const void* pNext, VkStructureType wanted_stype, VkLayerFunction function) {
    struct Header {
        VkStructureType sType;
        const void* pNext;
    };

    const Header* current = reinterpret_cast<const Header*>(pNext);
    while (current) {
        if (current->sType == wanted_stype) {
            auto* info = reinterpret_cast<T*>(const_cast<Header*>(current));
            if (info->function == function) return info;
        }
        current = reinterpret_cast<const Header*>(current->pNext);
    }
    return nullptr;
}

bool initialize_layer_command_buffer(const DeviceData& data, VkCommandBuffer cmd);
std::string log_path();
void log_line(const std::string& text);
// Water mode / layers / quality as of the last recorded pass end (UINT32_MAX = the water code never ran); printed with the frame rate so a
// log line says which water setting the fps belongs to. mode | layers<<4 | quality<<12.
inline std::atomic<uint32_t> g_water_hint{UINT32_MAX};
// Where the layer spends CPU time in the game's present/submit path (nanoseconds since the last [FRAME-PERF] line; read and reset by it).
// Only the hooks that can block or take the global lifecycle mutex are timed; the per-command hooks are counted (`draws`) instead.
struct LayerPerf {
    std::atomic<uint64_t> presentNs{0},presentLockNs{0},overlayNs{0},fenceWaitNs{0},downstreamPresentNs{0};   // vkQueuePresentKHR hook
    std::atomic<uint64_t> submitNs{0},submitLockNs{0},submitDriverNs{0},submits{0},draws{0};                  // vkQueueSubmit hooks, draw calls seen
};
inline LayerPerf g_perf;
// A/B test switches (started from the Water Reflections menu: PERFORMANCE TEST; the frame-rate logger flips them in turn): water replay off,
// overlay effect off, command tracking off; g_perf_ab_request asks the logger to start a run once the menu is closed.
inline std::atomic<bool> g_perf_water_off{false},g_perf_effect_off{false},g_perf_tracking_off{false},g_perf_ab_request{false};   // tracking off: command hooks skip all bookkeeping (measurement only)
struct PerfTimer {   // adds the elapsed time to `sink` on destruction
    std::atomic<uint64_t>& sink;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    explicit PerfTimer(std::atomic<uint64_t>& s):sink(s){}
    ~PerfTimer(){sink.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count()),std::memory_order_relaxed);}
};
// Takes `m` and, if it was contended, adds how long the caller waited to `waitSink`.
template<class Mutex> std::unique_lock<Mutex> lock_measured(Mutex& m,std::atomic<uint64_t>& waitSink) {
    std::unique_lock<Mutex> lock(m,std::defer_lock);
    if(!lock.try_lock()){
        const auto start=std::chrono::steady_clock::now();lock.lock();
        waitSink.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count()),std::memory_order_relaxed);
    }
    return lock;
}
uint64_t image_handle_u64(VkImage image);
uint64_t image_view_handle_u64(VkImageView view);
uint64_t queue_handle_u64(VkQueue queue);
uint64_t swapchain_handle_u64(VkSwapchainKHR swapchain);
const char* layout_name(VkImageLayout layout);
bool is_target_depth(VkDevice device, VkImage image);
std::string aspect_string(VkImageAspectFlags aspect);
const char* format_name(VkFormat format);
bool is_depth_or_stencil_format(VkFormat format);
std::string usage_string(VkImageUsageFlags usage);
std::string sample_count_string(VkSampleCountFlagBits samples);
std::string api_version_string(uint32_t version);
std::optional<InstanceData> get_instance_data(VkInstance instance);
std::optional<DeviceData> get_device_data(VkDevice device);
std::optional<VkInstance> instance_for_physical_device(VkPhysicalDevice physicalDevice);
void remember_physical_device(VkInstance instance, VkPhysicalDevice physicalDevice, PFN_vkGetInstanceProcAddr next_gipa);
bool log_all_images();

} // namespace depth_probe
