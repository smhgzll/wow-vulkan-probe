#include "sync_tracking.hpp"
#include "depth_tracking.hpp"

namespace depth_probe {
void sync_skip(uint64_t frame, const char* reason) {
    if (frame <= 10 || frame % 120 == 0)
        log_line(std::string("[OVERLAY-SYNC-SKIP] reason=\"") + reason + '"');
}
bool present_operations_enqueued(VkResult r) {
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR ||
           r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;
}
void remember_sync_queue(const DeviceData& data, VkQueue queue, uint32_t family, uint32_t index, VkDeviceQueueCreateFlags flags) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    if (data.sync) data.sync->queues[queue] = {family, index, flags == 0};
}
void forget_sync_swapchain(const DeviceData& data, VkSwapchainKHR sc) {
    if (data.sync) { data.sync->acquired.erase(sc); data.sync->abandonedPresents.erase(sc); }
}
void remember_acquire(const DeviceData& data, VkSwapchainKHR sc, uint32_t index, VkSemaphore semaphore, VkResult result) {
    if (!data.sync || (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)) return;
    auto& state = *data.sync;
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_swapchains.find({data.device, swapchain_handle_u64(sc)});
        if (it == g_swapchains.end()) return;
        count = static_cast<uint32_t>(it->second.images.size());
    }
    if (index >= count) return;
    auto& images = state.acquired[sc];
    images.resize(count);
    images[index] = {++state.serial, semaphore, {}, 0, false};
    state.signals.erase(semaphore);
}
void consume_present_waits(const DeviceData& data, const VkPresentInfoKHR* info, VkResult result) {
    if (!data.sync || !info) return;
    if (result == VK_ERROR_DEVICE_LOST) data.sync->deviceLost = true;
    if (!present_operations_enqueued(result)) return;
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) data.sync->signals.erase(info->pWaitSemaphores[i]);
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        auto it = data.sync->acquired.find(info->pSwapchains[i]);
        if (it != data.sync->acquired.end() && info->pImageIndices[i] < it->second.size())
            it->second[info->pImageIndices[i]].presented = true;
    }
}
const char* overlay_sync_support(const DeviceData& data, VkQueue queue, const VkPresentInfoKHR* info) {
    if (!info || !info->pSwapchains || !info->pImageIndices) return "missing present info";
    if (info->swapchainCount != 1) return "multiple swapchains";
    if (!data.sync || !data.sync->maintenance) return "present completion requires swapchainMaintenance1";
    if (data.sync->deviceLost) return "device lost";
    if (!data.sync->singleGraphicsQueue) return "requires one graphics queue in family 0";
    auto q = data.sync->queues.find(queue);
    if (q == data.sync->queues.end() || q->second.family != 0 || q->second.index != 0 || !q->second.ordinary)
        return "present queue is not the supported graphics queue";
    if (!info->waitSemaphoreCount) return "zero original waits: frame completion chain unverified";
    if (!info->pWaitSemaphores) return "missing original wait array";
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto sc = g_swapchains.find({data.device, swapchain_handle_u64(info->pSwapchains[0])});
        if (sc == g_swapchains.end() || sc->second.retired || !sc->second.overlaySyncSupported)
            return "swapchain usage/sharing/layers/mode unsupported or retired";
    }
    auto sc = data.sync->acquired.find(info->pSwapchains[0]);
    if (sc == data.sync->acquired.end() || info->pImageIndices[0] >= sc->second.size()) return "image acquire not observed";
    const auto& image = sc->second[info->pImageIndices[0]];
    if (!image.serial || image.presented || image.waitedQueue != queue || !image.waitSerial)
        return "acquire semaphore wait not observed on graphics/present queue";
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) {
        auto signal = data.sync->signals.find(info->pWaitSemaphores[i]);
        if (signal == data.sync->signals.end() || signal->second.queue != queue || signal->second.serial < image.waitSerial)
            return "render-complete signal chain unverified (requires same queue, ALL_COMMANDS)";
    }
    return present_extension_support(info);
}
const char* present_extension_support(const VkPresentInfoKHR* info) {
    for (auto* n = static_cast<const VkBaseInStructure*>(info->pNext); n; n = n->pNext) {
        switch (n->sType) {
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR:
        case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR:
        case VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE: break;
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT: {
            auto* fence = reinterpret_cast<const VkSwapchainPresentFenceInfoEXT*>(n);
            if (fence->swapchainCount != 1 || !fence->pFences || !fence->pFences[0]) return "null application present fence";
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT: {
            auto* mode = reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(n);
            if (mode->swapchainCount != 1 || !mode->pPresentModes ||
                mode->pPresentModes[0] > VK_PRESENT_MODE_FIFO_RELAXED_KHR) return "shared/unknown present mode";
            break;
        }
        default: return "unsupported present pNext (including incremental regions/device groups)";
        }
    }
    return nullptr;
}
namespace {
template<class T> T next(const DeviceData& data, const char* name) {
    return reinterpret_cast<T>(data.next_gdpa ? data.next_gdpa(data.device, name) : nullptr);
}
void wait_semaphore(SyncDeviceState& s, VkQueue queue, VkSemaphore semaphore, bool ordinary) {
    s.signals.erase(semaphore);
    for (auto& [sc, images] : s.acquired) {
        (void)sc;
        for (auto& image : images) if (!image.presented && image.semaphore == semaphore && !image.waitSerial) {
            image.waitedQueue = ordinary ? queue : VkQueue{};
            image.waitSerial = s.serial;
        }
    }
}
}
} // namespace depth_probe

using namespace depth_probe;
extern "C" {
LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR sc,
    uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    // Do not hold the lifecycle lock while acquire blocks: another thread may present.
    auto data = get_device_data(device);
    auto fn = data ? next<PFN_vkAcquireNextImageKHR>(*data, "vkAcquireNextImageKHR") : nullptr;
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    auto result = fn(device, sc, timeout, semaphore, fence, index);
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    if (index && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR))
        remember_acquire(*data, sc, *index, semaphore, result);
    return result;
}
LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device,
    const VkAcquireNextImageInfoKHR* info, uint32_t* index) {
    auto data = get_device_data(device);
    auto fn = data ? next<PFN_vkAcquireNextImage2KHR>(*data, "vkAcquireNextImage2KHR") : nullptr;
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    auto result = fn(device, info, index);
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    if (info && index && info->deviceMask == 1 && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) remember_acquire(*data, info->swapchain, *index, info->semaphore, result);
    return result;
}
LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
    PerfTimer submitTimer(g_perf.submitNs);g_perf.submits.fetch_add(1,std::memory_order_relaxed);
    auto lifetime = lock_measured(g_depth_lifecycle_mutex, g_perf.submitLockNs);
    auto data = get_device_data(reinterpret_cast<VkDevice>(queue));
    auto fn = data ? next<PFN_vkQueueSubmit>(*data, "vkQueueSubmit") : nullptr;
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    {PerfTimer driverTimer(g_perf.submitDriverNs);result = fn(queue, count, infos, fence);}
    if (data->sync && result == VK_ERROR_DEVICE_LOST) data->sync->deviceLost = true;
    if (result != VK_SUCCESS || !data->sync) return result;
    auto& s = *data->sync;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& info = infos[i];
        ++s.serial;
        bool ordinary = true;
        for (auto* n = static_cast<const VkBaseInStructure*>(info.pNext); n; n = n->pNext)
            if (n->sType != VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) ordinary = false;
        for (uint32_t j = 0; j < info.waitSemaphoreCount; ++j) wait_semaphore(s, queue, info.pWaitSemaphores[j], ordinary);
        for (uint32_t j = 0; j < info.signalSemaphoreCount; ++j) {
            s.signals.erase(info.pSignalSemaphores[j]);
            if (ordinary) s.signals[info.pSignalSemaphores[j]] = {queue, s.serial};
        }
    }
    return result;
}
static VkResult submit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence, const char* name) {
    PerfTimer submitTimer(g_perf.submitNs);g_perf.submits.fetch_add(1,std::memory_order_relaxed);
    auto lifetime = lock_measured(g_depth_lifecycle_mutex, g_perf.submitLockNs);
    auto data = get_device_data(reinterpret_cast<VkDevice>(queue));
    auto fn = data ? next<PFN_vkQueueSubmit2>(*data, name) : nullptr;
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    {PerfTimer driverTimer(g_perf.submitDriverNs);result = fn(queue, count, infos, fence);}
    if (data->sync && result == VK_ERROR_DEVICE_LOST) data->sync->deviceLost = true;
    if (result != VK_SUCCESS || !data->sync) return result;
    auto& s = *data->sync;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& info = infos[i];
        ++s.serial;
        bool ordinary = !info.flags && !info.pNext;
        for (uint32_t j = 0; j < info.commandBufferInfoCount; ++j)
            ordinary &= info.pCommandBufferInfos[j].deviceMask <= 1 && !info.pCommandBufferInfos[j].pNext;
        for (uint32_t j = 0; j < info.waitSemaphoreInfoCount; ++j) {
            const auto& w = info.pWaitSemaphoreInfos[j];
            wait_semaphore(s, queue, w.semaphore, ordinary && !w.deviceIndex && !w.pNext);
        }
        for (uint32_t j = 0; j < info.signalSemaphoreInfoCount; ++j) {
            const auto& signal = info.pSignalSemaphoreInfos[j];
            s.signals.erase(signal.semaphore);
            if (ordinary && !signal.deviceIndex && !signal.pNext &&
                (signal.stageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) s.signals[signal.semaphore] = {queue, s.serial};
        }
    }
    return result;
}
LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    return submit2(q, n, s, f, "vkQueueSubmit2");
}
LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    return submit2(q, n, s, f, "vkQueueSubmit2KHR");
}
LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t count, const VkFence* fences) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    auto data = get_device_data(device);
    auto fn = data ? next<PFN_vkResetFences>(*data, "vkResetFences") : nullptr;
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < count; ++i) {
        auto r = settle_borrowed_present_fence(*data, fences[i]);
        if (r != VK_SUCCESS) return r;
    }
    return fn(device, count, fences);
}
LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* alloc) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    auto data = get_device_data(device);
    auto fn = data ? next<PFN_vkDestroyFence>(*data, "vkDestroyFence") : nullptr;
    if (!fn) return;
    settle_borrowed_present_fence(*data, fence);
    fn(device, fence, alloc);
}
LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* alloc) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    auto data = get_device_data(device);
    auto fn = data ? next<PFN_vkDestroySemaphore>(*data, "vkDestroySemaphore") : nullptr;
    if (!fn) return;
    if (data->sync) {
        data->sync->signals.erase(semaphore);
        for (auto& [sc, images] : data->sync->acquired) {
            (void)sc;
            for (auto& image : images) if (image.semaphore == semaphore) image.semaphore = {};
        }
    }
    fn(device, semaphore, alloc);
}
}
namespace depth_probe {
PFN_vkVoidFunction sync_hook(const char* name) {
#define HOOK(n) if (!std::strcmp(name, #n)) return reinterpret_cast<PFN_vkVoidFunction>(n)
    HOOK(vkAcquireNextImageKHR); HOOK(vkAcquireNextImage2KHR);
    HOOK(vkQueueSubmit); HOOK(vkQueueSubmit2); HOOK(vkQueueSubmit2KHR);
    HOOK(vkResetFences); HOOK(vkDestroyFence); HOOK(vkDestroySemaphore);
#undef HOOK
    return nullptr;
}
} // namespace depth_probe
