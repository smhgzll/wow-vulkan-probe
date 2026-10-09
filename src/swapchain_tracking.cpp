#include "layer_state.hpp"
#include "overlay_renderer.hpp"
#include "depth_tracking.hpp"
#include "sync_tracking.hpp"

using namespace depth_probe;

extern "C" {

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue) {

    auto data = get_device_data(device);

    if (!data || !data->next_get_device_queue) {
        return;
    }

    data->next_get_device_queue(
        device, queueFamilyIndex, queueIndex, pQueue);

    if (pQueue && *pQueue) {
        remember_sync_queue(*data, *pQueue, queueFamilyIndex, queueIndex, 0);
        std::ostringstream os;
        os << "[QUEUE]"
           << " queue=0x" << std::hex
           << queue_handle_u64(*pQueue)
           << std::dec
           << " family=" << queueFamilyIndex
           << " index=" << queueIndex;

        log_line(os.str());
    }
}

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device,
    const VkDeviceQueueInfo2* pQueueInfo,
    VkQueue* pQueue) {

    auto data = get_device_data(device);

    if (!data || !data->next_get_device_queue2) {
        return;
    }

    data->next_get_device_queue2(device, pQueueInfo, pQueue);

    if (pQueue && *pQueue) {
        if (pQueueInfo) remember_sync_queue(*data, *pQueue, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex, pQueueInfo->flags);
        std::ostringstream os;
        os << "[QUEUE2]"
           << " queue=0x" << std::hex
           << queue_handle_u64(*pQueue)
           << std::dec;

        if (pQueueInfo) {
            os << " family=" << pQueueInfo->queueFamilyIndex
               << " index=" << pQueueInfo->queueIndex;
        }

        log_line(os.str());
    }
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);

    auto data = get_device_data(device);

    if (!data || !data->next_create_swapchain) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = data->next_create_swapchain(
        device, pCreateInfo, pAllocator, pSwapchain);

    // oldSwapchain is retired by the create call, including a failed create.
    if (pCreateInfo && pCreateInfo->oldSwapchain != VkSwapchainKHR{}) {
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            auto old = g_swapchains.find({device, swapchain_handle_u64(pCreateInfo->oldSwapchain)});
            if (old != g_swapchains.end()) old->second.retired = true;
        }
        invalidate_scene_depth_swapchain(device, pCreateInfo->oldSwapchain, "swapchain retired");
        destroy_depth_overlay_state(*data, pCreateInfo->oldSwapchain);
    }

    if (result != VK_SUCCESS ||
        !pCreateInfo ||
        !pSwapchain ||
        *pSwapchain == VK_NULL_HANDLE) {
        return result;
    }

    const uint64_t key =
        swapchain_handle_u64(*pSwapchain);

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);

        SwapchainInfo info{};
        info.usage = pCreateInfo->imageUsage;
        info.overlaySyncSupported = (pCreateInfo->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
            pCreateInfo->imageArrayLayers == 1 && pCreateInfo->imageSharingMode == VK_SHARING_MODE_EXCLUSIVE &&
            !(pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) &&
            pCreateInfo->presentMode <= VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        info.width = pCreateInfo->imageExtent.width;
        info.height = pCreateInfo->imageExtent.height;
        info.format = pCreateInfo->imageFormat;
        info.image_count = pCreateInfo->minImageCount;

        g_swapchains[{device, key}] = info;
    }

    std::ostringstream os;

    os << "[SWAPCHAIN-CREATED]"
       << " swapchain=0x"
       << std::hex << key << std::dec
       << " extent="
       << pCreateInfo->imageExtent.width
       << "x"
       << pCreateInfo->imageExtent.height
       << " format="
       << static_cast<int>(pCreateInfo->imageFormat)
       << " minImages="
       << pCreateInfo->minImageCount
       << " usage="
       << usage_string(pCreateInfo->imageUsage)
       << " presentMode="
       << static_cast<int>(pCreateInfo->presentMode);

    log_line(os.str());

    return result;
}

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);

    auto data = get_device_data(device);

    if (!data || !data->next_destroy_swapchain) {
        return;
    }

    invalidate_scene_depth_swapchain(device, swapchain, "swapchain destroyed");
    destroy_depth_overlay_state(
        *data,
        swapchain);
    forget_sync_swapchain(*data, swapchain);

    const uint64_t key =
        swapchain_handle_u64(swapchain);

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_swapchains.erase({device, key});
    }

    {
        std::ostringstream os;
        os << "[SWAPCHAIN-DESTROY]"
           << " swapchain=0x"
           << std::hex << key << std::dec;
        log_line(os.str());
    }

    data->next_destroy_swapchain(
        device, swapchain, pAllocator);
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages) {

    auto data = get_device_data(device);

    if (!data || !data->next_get_swapchain_images) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result =
        data->next_get_swapchain_images(
            device,
            swapchain,
            pSwapchainImageCount,
            pSwapchainImages);

    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
        pSwapchainImageCount) {

        const uint64_t key =
            swapchain_handle_u64(swapchain);

        {
            std::lock_guard<std::mutex> lock(g_state_mutex);

            auto it = g_swapchains.find({device, key});
            if (it != g_swapchains.end()) {
                it->second.image_count =
                    *pSwapchainImageCount;

                if (pSwapchainImages) {
                    it->second.images.assign(
                        pSwapchainImages,
                        pSwapchainImages + *pSwapchainImageCount);
                }
            }
        }

        if (pSwapchainImages) {
            std::ostringstream os;

            os << "[SWAPCHAIN-IMAGES]"
               << " swapchain=0x"
               << std::hex << key << std::dec
               << " count="
               << *pSwapchainImageCount;

            for (uint32_t i = 0;
                 i < *pSwapchainImageCount;
                 ++i) {

                os << " image[" << i << "]=0x"
                   << std::hex
                   << image_handle_u64(
                          pSwapchainImages[i])
                   << std::dec;
            }

            log_line(os.str());
        }
    }

    return result;
}

} // extern "C"
