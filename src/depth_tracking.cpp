#include "layer_state.hpp"
#include "overlay_renderer.hpp"
#include "depth_tracking.hpp"

using namespace depth_probe;

extern "C" {

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device, const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    auto data = get_device_data(device);
    if (!data || !data->next_create_image) return VK_ERROR_INITIALIZATION_FAILED;

    const auto preparation = pCreateInfo ? prepare_depth_candidate(*data, *pCreateInfo) : DepthPreparation{};
    VkImageCreateInfo patched{};
    const VkImageCreateInfo* actual = pCreateInfo;
    bool sampledPatched = false;
    if (preparation.sampledSupported && !(pCreateInfo->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
        patched = *pCreateInfo;
        patched.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        actual = &patched;
        sampledPatched = true;
    }
    VkResult result = data->next_create_image(device, actual, pAllocator, pImage);
    if (result != VK_SUCCESS && sampledPatched) {
        log_line("[PATCH-FAILED] sampled depth creation failed result=" + std::to_string(result) +
                 " - retrying original");
        actual = pCreateInfo;
        result = data->next_create_image(device, actual, pAllocator, pImage);
        sampledPatched = false;
    }
    if (result != VK_SUCCESS || !pCreateInfo || !pImage) return result;

    if (preparation.eligible) {
        std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
        static uint64_t nextGeneration = 0; // protected by lifetime mutex
        DepthCandidate candidate{};
        candidate.device = device;
        candidate.image = *pImage;
        candidate.format = pCreateInfo->format;
        candidate.extent = pCreateInfo->extent;
        candidate.samples = pCreateInfo->samples;
        candidate.originalUsage = pCreateInfo->usage;
        candidate.flags = pCreateInfo->flags;
        candidate.mipLevels = pCreateInfo->mipLevels;
        candidate.arrayLayers = pCreateInfo->arrayLayers;
        candidate.sampledSupported = preparation.sampledSupported;
        candidate.sampledPatched = sampledPatched;
        candidate.sampledReady = preparation.sampledSupported && (actual->usage & VK_IMAGE_USAGE_SAMPLED_BIT);
        candidate.generation = ++nextGeneration;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_depth_candidates[{device, image_handle_u64(*pImage)}] = candidate;
        }
        std::ostringstream log;
        log << "[DEPTH-CANDIDATE-TRACK] device=" << device
            << " image=0x" << std::hex << image_handle_u64(*pImage) << std::dec
            << " extent=" << candidate.extent.width << 'x' << candidate.extent.height
            << " format=" << format_name(candidate.format)
            << " samples=" << sample_count_string(candidate.samples)
            << " usage=" << usage_string(candidate.originalUsage)
            << " flags=" << candidate.flags << " generation=" << candidate.generation
            << " sampledSupported=" << candidate.sampledSupported
            << " patched=" << candidate.sampledPatched << " sampledReady=" << candidate.sampledReady
            << " reason=\"" << preparation.reason << '\"';
        log_line(log.str());
    }
    if (sampledPatched) {
        std::ostringstream log;
        log << "[PATCHED-DEPTH] " << pCreateInfo->extent.width << 'x' << pCreateInfo->extent.height
            << " format=" << format_name(pCreateInfo->format)
            << " samples=1 oldUsage=" << usage_string(pCreateInfo->usage)
            << " newUsage=" << usage_string(actual->usage);
        log_line(log.str());
    }
    const bool depth = (pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
                       is_depth_or_stencil_format(pCreateInfo->format);
    if ((depth && !preparation.eligible) || log_all_images()) {
        std::ostringstream log;
        log << (depth ? "[DEPTH-CANDIDATE-SKIP]" : "[image]")
            << " device=" << device << " image=0x" << std::hex << image_handle_u64(*pImage) << std::dec
            << " extent=" << pCreateInfo->extent.width << 'x' << pCreateInfo->extent.height
            << " format=" << static_cast<int>(pCreateInfo->format)
            << " samples=" << sample_count_string(pCreateInfo->samples)
            << " mips=" << pCreateInfo->mipLevels << " layers=" << pCreateInfo->arrayLayers
            << " flags=" << pCreateInfo->flags
            << " usage=" << usage_string(pCreateInfo->usage)
            << " reason=\"" << preparation.reason << '\"';
        log_line(log.str());
    }
    return result;
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    auto data = get_device_data(device);
    if (!data || !data->next_create_image_view) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = data->next_create_image_view(device, pCreateInfo, pAllocator, pView);
    if (result != VK_SUCCESS || !pCreateInfo || !data->next_destroy_image_view) return result;

    DepthCandidate candidate{};
    const DeviceHandleKey key{device, image_handle_u64(pCreateInfo->image)};
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_depth_candidates.find(key);
        if (it == g_depth_candidates.end() || !it->second.sampledReady ||
            it->second.depthView != VkImageView{}) return result;
        candidate = it->second;
    }
    // A successful application view proves the image is bound. Build a canonical
    // mip-0/layer-0 native-format depth view; do not inherit swizzles or a view's
    // restricted usage/pNext chain. The preparation policy validated this shape.
    VkImageViewCreateInfo depthInfo{};
    depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthInfo.image = candidate.image;
    depthInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthInfo.format = candidate.format;
    depthInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView depthView{};
    const VkResult customResult = data->next_create_image_view(device, &depthInfo, nullptr, &depthView);
    if (customResult == VK_SUCCESS) {
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_depth_candidates.at(key).depthView = depthView;
        }
        std::ostringstream log;
        log << "[CUSTOM-DEPTH-VIEW] device=" << device
            << " image=0x" << std::hex << key.handle
            << " view=0x" << image_view_handle_u64(depthView) << std::dec
            << " generation=" << candidate.generation << " format=" << format_name(candidate.format)
            << " aspect=DEPTH";
        log_line(log.str());
    } else {
        log_line("[CUSTOM-DEPTH-VIEW-FAILED] result=" + std::to_string(customResult));
    }
    return result;
}

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount,
    const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers) {

    auto data = get_device_data(
        reinterpret_cast<VkDevice>(commandBuffer));

    // command buffer ile device ayni dispatch table'i kullanir;
    // normal lookup olmazsa dogrudan dispatch key ile dene.
    if (!data) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_devices.find(dispatch_key(commandBuffer));
        if (it != g_devices.end())
            data = it->second;
    }

    for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
        const auto& b = pImageMemoryBarriers[i];

        if (data && should_log_depth_barrier(data->device, b.image)) {
            std::ostringstream os;
            os << "[DEPTH-BARRIER]"
               << " old=" << layout_name(b.oldLayout)
               << "(" << static_cast<int>(b.oldLayout) << ")"
               << " new=" << layout_name(b.newLayout)
               << "(" << static_cast<int>(b.newLayout) << ")"
               << " aspect="
               << aspect_string(b.subresourceRange.aspectMask);

            log_line(os.str());
        }
    }

    if (data && data->next_cmd_pipeline_barrier) {
        data->next_cmd_pipeline_barrier(
            commandBuffer,
            srcStageMask,
            dstStageMask,
            dependencyFlags,
            memoryBarrierCount,
            pMemoryBarriers,
            bufferMemoryBarrierCount,
            pBufferMemoryBarriers,
            imageMemoryBarrierCount,
            pImageMemoryBarriers);
    }
}

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(
    VkCommandBuffer commandBuffer,
    const VkDependencyInfo* pDependencyInfo) {

    std::optional<DeviceData> data;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_devices.find(dispatch_key(commandBuffer));
        if (it != g_devices.end())
            data = it->second;
    }

    if (pDependencyInfo) {
        for (uint32_t i = 0;
             i < pDependencyInfo->imageMemoryBarrierCount;
             ++i) {

            const auto& b =
                pDependencyInfo->pImageMemoryBarriers[i];

            if (data && should_log_depth_barrier(data->device, b.image)) {
                std::ostringstream os;

                os << "[DEPTH-BARRIER2]"
                   << " old=" << layout_name(b.oldLayout)
                   << "(" << static_cast<int>(b.oldLayout) << ")"
                   << " new=" << layout_name(b.newLayout)
                   << "(" << static_cast<int>(b.newLayout) << ")"
                   << " aspect="
                   << aspect_string(
                          b.subresourceRange.aspectMask);

                log_line(os.str());
            }
        }
    }

    if (data && data->next_cmd_pipeline_barrier2)
        data->next_cmd_pipeline_barrier2(
            commandBuffer,
            pDependencyInfo);
}

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    auto data = get_device_data(device);
    if (!data || !data->next_destroy_image) return;
    if (is_target_depth(device, image)) {
        VkImageView view{};
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            const auto it = g_depth_candidates.find({device, image_handle_u64(image)});
            view = it->second.depthView;
            g_depth_candidates.erase(it);
        }
        invalidate_scene_depth_image(device, image);
        // No new selector can borrow the removed image; retire GPU users before
        // destroying the layer view, then forward the application's image destroy.
        destroy_depth_overlays_for_image(*data, image);
        if (view != VkImageView{} && data->next_destroy_image_view)
            data->next_destroy_image_view(device, view, nullptr);
        log_line("[DEPTH-DESTROY] retired candidate, selection and custom view");
    }
    data->next_destroy_image(device, image, pAllocator);
}

} // extern "C"

namespace depth_probe {
void destroy_device_depth_views(const DeviceData& data) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    std::vector<VkImageView> views;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (auto it = g_depth_candidates.begin(); it != g_depth_candidates.end();) {
            if (it->first.device == data.device) {
                if (it->second.depthView != VkImageView{}) views.push_back(it->second.depthView);
                it = g_depth_candidates.erase(it);
            } else ++it;
        }
    }
    invalidate_scene_depth_device(data.device);
    for (VkImageView view : views) {
        if (data.next_destroy_image_view) data.next_destroy_image_view(data.device, view, nullptr);
    }
    if (!views.empty())
        log_line("[DEPTH-DEVICE-CLEANUP] custom views=" + std::to_string(views.size()));
}
} // namespace depth_probe
