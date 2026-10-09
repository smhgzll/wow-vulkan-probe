#include "overlay_internal.hpp"
#include "sync_tracking.hpp"
#include "depth_tracking.hpp"
#include <thread>
#include <chrono>

namespace depth_probe {
namespace {
void error(const char* stage, VkResult r) {
    log_line(std::string("[OVERLAY-SYNC-ERROR] stage=\"") + stage + "\" result=" + std::to_string(r));
}
VkResult wait_fence(const DeviceData& data, const DepthOverlayFns& f, VkFence fence) {
    if (data.sync && data.sync->deviceLost) return VK_ERROR_DEVICE_LOST;
    VkResult result;
    {PerfTimer waitTimer(g_perf.fenceWaitNs);result = f.waitForFences(data.device, 1, &fence, VK_TRUE, UINT64_MAX);}
    if (result == VK_ERROR_DEVICE_LOST && data.sync) data.sync->deviceLost = true;
    if (result != VK_SUCCESS) error("fence wait", result);
    return result;
}
// Destruction cannot abandon a borrowed fence or free in-use resources on an
// allocation error. Retry exceptional host wait failures; never infer completion.
VkResult wait_for_cleanup(const DeviceData& data, const DepthOverlayFns& f, VkFence fence) {
    VkResult result;
    do {
        result = wait_fence(data, f, fence);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST);
    return result;
}
bool oom(VkResult result) { return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY; }
OverlayPresentResult before_submit_error(const DeviceData& data, VkResult r, const char* stage) {
    error(stage, r);
    if (r == VK_ERROR_DEVICE_LOST) {
        if (data.sync) data.sync->deviceLost = true;
        return {true, r, false};
    }
    return {}; // no submission: caller can forward the original present
}
}
bool create_overlay_sync(const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st) {
    st.sync.resize(st.commandBuffers.size());
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (auto& frame : st.sync) {
        VkResult r = f.createSemaphore(data.device, &semaphore, nullptr, &frame.finishedSemaphore);
        if (r == VK_SUCCESS) r = f.createFence(data.device, &fence, nullptr, &frame.submitFence);
        if (r == VK_SUCCESS) r = f.createFence(data.device, &fence, nullptr, &frame.ownPresentFence);
        if (r != VK_SUCCESS) { error("create sync", r); return false; }
    }
    return true;
}
VkResult wait_overlay_sync(const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st) {
    for (auto& frame : st.sync) {
        if (frame.submitted) {
            auto r = wait_for_cleanup(data, f, frame.submitFence);
            if (r != VK_SUCCESS) return r;
            frame.submitted = false;
        }
        if (frame.pendingPresentFence) {
            auto r = wait_for_cleanup(data, f, frame.pendingPresentFence);
            if (r != VK_SUCCESS) return r;
            frame.pendingPresentFence = {};
        }
    }
    return VK_SUCCESS;
}
void destroy_overlay_sync(const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st) {
    for (uint32_t index = 0; index < st.sync.size(); ++index) {
        auto& frame = st.sync[index];
        if (frame.retryPresent && data.sync)
            data.sync->abandonedPresents[st.swapchain][index] = frame.retryResult;
        if (frame.finishedSemaphore) f.destroySemaphore(data.device, frame.finishedSemaphore, nullptr);
        if (frame.submitFence) f.destroyFence(data.device, frame.submitFence, nullptr);
        if (frame.ownPresentFence) f.destroyFence(data.device, frame.ownPresentFence, nullptr);
    }
    st.sync.clear();
}
VkResult settle_borrowed_present_fence(const DeviceData& data, VkFence fence) {
    if (!fence) return VK_SUCCESS;
    const auto f = load_depth_overlay_fns(data);
    for (auto& [key, st] : g_depth_overlay_states) {
        if (key.device != data.device) continue;
        for (auto& frame : st.sync) {
            if (frame.pendingPresentFence == fence) {
                const auto r = wait_for_cleanup(data, f, fence);
                if (r != VK_SUCCESS && r != VK_ERROR_DEVICE_LOST) return r;
                frame.pendingPresentFence = {};
                if (r != VK_SUCCESS) return r;
            }
        }
    }
    return VK_SUCCESS;
}
OverlayPresentResult submit_depth_overlay(const DeviceData& data, const DepthOverlayFns& f, VkQueue queue,
    DepthOverlayState& st, const VkPresentInfoKHR* info, uint64_t frameNumber, bool retryOnly) {
    const uint32_t index = info->pImageIndices[0];
    if (index >= st.sync.size()) return {};
    auto& sync = st.sync[index];
    const bool retry = sync.retryPresent;
    if (retry && (present_extension_support(info) || (info->waitSemaphoreCount && !info->pWaitSemaphores)))
        return {true, sync.retryResult, false};
    if (retryOnly && !retry) return {};
    const bool logFrame = frameNumber <= 10 || frameNumber % 120 == 0;
    VkFence presentFence = sync.ownPresentFence;
    bool applicationFence = false;
    for (auto* n = static_cast<const VkBaseInStructure*>(info->pNext); n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT) {
            const auto* fences = reinterpret_cast<const VkSwapchainPresentFenceInfoEXT*>(n);
            presentFence = fences->pFences[0];
            applicationFence = true;
        }
    }
    if (retry) {
        // The original binary waits were consumed already. Only retry present,
        // using the same completed signal. Never draw or consume the waits again.
        if (info->waitSemaphoreCount != sync.consumedWaits.size() ||
            !std::equal(sync.consumedWaits.begin(), sync.consumedWaits.end(), info->pWaitSemaphores)) {
            error("present retry wait list changed", sync.retryResult);
            return {true, sync.retryResult, false};
        }
    } else {
        if (sync.submitted) {
            auto r = wait_fence(data, f, sync.submitFence);
            if (r != VK_SUCCESS) return before_submit_error(data, r, "submit fence");
            sync.submitted = false;
        }
        // Submit completion alone does NOT make the present semaphore reusable.
        // The acquire/submit chain is checked before this function. The present
        // fence also supplies explicit host proof and handles renderer teardown.
        if (sync.pendingPresentFence) {
            auto r = wait_fence(data, f, sync.pendingPresentFence);
            if (r != VK_SUCCESS) return before_submit_error(data, r, "present fence");
            sync.pendingPresentFence = {};
            if (logFrame) log_line("[OVERLAY-SYNC-REUSE] imageIndex=" + std::to_string(index));
        }
        const auto updated = update_overlay_uniform(data, f, st, index);
        if (updated != VK_SUCCESS) return before_submit_error(data, updated, "uniform update");
        if (!st.commandRecorded[index]) {
            if (!record_depth_overlay_commands(f, st, index)) return {};
            st.commandRecorded[index] = true;
        }
        auto r = f.resetFences(data.device, 1, &sync.submitFence);
        if (r != VK_SUCCESS) return before_submit_error(data, r, "reset submit fence");
        if (!applicationFence) {
            r = f.resetFences(data.device, 1, &sync.ownPresentFence);
            if (r != VK_SUCCESS) return before_submit_error(data, r, "reset present fence");
        }
        // Allocate all host storage before consuming any application semaphore.
        sync.consumedWaits.assign(info->pWaitSemaphores, info->pWaitSemaphores + info->waitSemaphoreCount);
        std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = info->waitSemaphoreCount;
        submit.pWaitSemaphores = info->pWaitSemaphores;
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &st.commandBuffers[index];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &sync.finishedSemaphore;
        r = f.queueSubmit(queue, 1, &submit, sync.submitFence);
        if (r != VK_SUCCESS) {
            error("submit", r);
            if (oom(r)) return {}; // Vulkan guarantees no referenced semaphore was changed.
            if (data.sync) {
                data.sync->deviceLost = r == VK_ERROR_DEVICE_LOST;
                if (r != VK_ERROR_DEVICE_LOST) data.sync->terminalError = r;
            }
            return {true, r, false}; // Unknown consumption/device lost: never forward original waits.
        }
        sync.submitted = true;
        if (data.sync) {
            ++data.sync->overlaySubmits;
            for (auto semaphore : sync.consumedWaits) data.sync->signals.erase(semaphore);
        }
        if (logFrame) {
            std::ostringstream log;
            log << "[OVERLAY-SUBMIT] frame=" << frameNumber << " imageIndex=" << index
                << " originalWaitCount=" << info->waitSemaphoreCount << " signalSemaphore=" << sync.finishedSemaphore
                << " overlayQueueWaitIdleCount=" << (data.sync ? data.sync->overlayQueueWaitIdleCount : 0);
            log_line(log.str());
        }
    }
    VkPresentInfoKHR copy = *info;
    copy.waitSemaphoreCount = 1;
    copy.pWaitSemaphores = &sync.finishedSemaphore;
    VkSwapchainPresentFenceInfoEXT completion{};
    completion.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
    completion.pNext = copy.pNext;
    completion.swapchainCount = 1;
    completion.pFences = &presentFence;
    if (!applicationFence) copy.pNext = &completion; // Original chain is retained intact.
    VkResult result;
    {PerfTimer presentTimer(g_perf.downstreamPresentNs);result = data.next_queue_present(queue, &copy);}
    consume_present_waits(data, info, result);
    sync.retryPresent = oom(result);
    sync.retryResult = result;
    if (present_operations_enqueued(result)) sync.pendingPresentFence = presentFence;
    else if (!oom(result) && data.sync) {
        data.sync->deviceLost = result == VK_ERROR_DEVICE_LOST;
        if (result != VK_ERROR_DEVICE_LOST) data.sync->terminalError = result;
        // An unexpected driver error cannot be used as completion evidence.
        // Retain the present fence until completion or actual device loss.
        if (!data.sync->deviceLost) sync.pendingPresentFence = presentFence;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) error("present", result);
    if (logFrame) log_line("[OVERLAY-PRESENT] frame=" + std::to_string(frameNumber) + " wait=overlaySemaphore result=" + std::to_string(result));
    return {true, result, !retry};
}
} // namespace depth_probe
