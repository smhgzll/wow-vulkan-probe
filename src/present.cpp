#include "layer_state.hpp"
#include "overlay_renderer.hpp"
#include "depth_tracking.hpp"
#include "sync_tracking.hpp"

using namespace depth_probe;

extern "C" {

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo) {

    PerfTimer presentTimer(g_perf.presentNs);
    auto lifetime = lock_measured(g_depth_lifecycle_mutex, g_perf.presentLockNs);
    std::optional<DeviceData> dataOpt;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);

        auto it = g_devices.find(dispatch_key(queue));

        if (it != g_devices.end()) {
            dataOpt = it->second;
        }
    }

    if (!dataOpt || !dataOpt->next_queue_present) {
        log_line("[PRESENT-ERROR] device/nextPresent bulunamadi");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    DeviceData data = *dataOpt;

    const uint64_t frame =
        ++g_present_counter;

    const char* env =
        std::getenv("DEPTH_PROBE_CLEAR_PRESENT");

    const bool clearEnabled =
        env && *env && std::strcmp(env, "0") != 0;

    const char* depthEnv =
        std::getenv("DEPTH_PROBE_DEPTH_PRESENT");

    const bool depthEnabled =
        depthEnv &&
        *depthEnv &&
        std::strcmp(depthEnv, "0") != 0;

    if (depthEnabled) {
        const auto result = depth_overlay_present(data, queue, pPresentInfo, frame);
        if (result.handled) return result.result;
    }

    if (clearEnabled &&
        pPresentInfo &&
        pPresentInfo->swapchainCount > 0 &&
        pPresentInfo->pImageIndices &&
        data.next_create_command_pool &&
        data.next_destroy_command_pool &&
        data.next_allocate_command_buffers &&
        data.next_begin_command_buffer &&
        data.next_end_command_buffer &&
        data.next_queue_submit &&
        data.next_queue_wait_idle &&
        data.next_cmd_pipeline_barrier &&
        data.next_cmd_clear_color_image) {

        // DXVK'nin onceki render isleri bitsin.
        VkResult r =
            data.next_queue_wait_idle(queue);

        if (r == VK_SUCCESS) {

            for (uint32_t scIndex = 0;
                 scIndex < pPresentInfo->swapchainCount;
                 ++scIndex) {

                const VkSwapchainKHR swapchain =
                    pPresentInfo->pSwapchains[scIndex];

                const uint32_t imageIndex =
                    pPresentInfo->pImageIndices[scIndex];

                VkImage targetImage = VkImage{};

                {
                    std::lock_guard<std::mutex>
                        lock(g_state_mutex);

                    auto it =
                        g_swapchains.find(
                            {data.device, swapchain_handle_u64(swapchain)});

                    if (it != g_swapchains.end() &&
                        imageIndex < it->second.images.size()) {

                        targetImage =
                            it->second.images[imageIndex];
                    }
                }

                if (targetImage == VkImage{}) {
                    log_line(
                        "[CLEAR-SKIP] swapchain image bulunamadi");
                    continue;
                }

                // Logdan biliyoruz: present queue family = 0.
                VkCommandPoolCreateInfo poolInfo{};
                poolInfo.sType =
                    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                poolInfo.flags =
                    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                poolInfo.queueFamilyIndex = 0;

                VkCommandPool pool = VkCommandPool{};

                r = data.next_create_command_pool(
                    data.device,
                    &poolInfo,
                    nullptr,
                    &pool);

                if (r != VK_SUCCESS) {
                    log_line(
                        "[CLEAR-ERROR] vkCreateCommandPool");
                    continue;
                }

                VkCommandBufferAllocateInfo allocInfo{};
                allocInfo.sType =
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocInfo.commandPool = pool;
                allocInfo.level =
                    VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocInfo.commandBufferCount = 1;

                VkCommandBuffer cmd = VkCommandBuffer{};

                r = data.next_allocate_command_buffers(
                    data.device,
                    &allocInfo,
                    &cmd);

                if (r != VK_SUCCESS) {
                    data.next_destroy_command_pool(
                        data.device, pool, nullptr);
                    log_line(
                        "[CLEAR-ERROR] vkAllocateCommandBuffers");
                    continue;
                }

                if (!initialize_layer_command_buffer(data, cmd)) {
                    data.next_destroy_command_pool(data.device, pool, nullptr);
                    continue;
                }

                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType =
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags =
                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

                r = data.next_begin_command_buffer(
                    cmd,
                    &beginInfo);

                if (r != VK_SUCCESS) {
                    data.next_destroy_command_pool(
                        data.device, pool, nullptr);
                    log_line(
                        "[CLEAR-ERROR] vkBeginCommandBuffer");
                    continue;
                }

                VkImageSubresourceRange range{};
                range.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                range.baseMipLevel = 0;
                range.levelCount = 1;
                range.baseArrayLayer = 0;
                range.layerCount = 1;

                VkImageMemoryBarrier toTransfer{};
                toTransfer.sType =
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

                toTransfer.srcAccessMask =
                    VK_ACCESS_MEMORY_READ_BIT |
                    VK_ACCESS_MEMORY_WRITE_BIT;

                toTransfer.dstAccessMask =
                    VK_ACCESS_TRANSFER_WRITE_BIT;

                toTransfer.oldLayout =
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

                toTransfer.newLayout =
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                toTransfer.srcQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;

                toTransfer.dstQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;

                toTransfer.image =
                    targetImage;

                toTransfer.subresourceRange =
                    range;

                data.next_cmd_pipeline_barrier(
                    cmd,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0,
                    0, nullptr,
                    0, nullptr,
                    1, &toTransfer);

                VkClearColorValue magenta{};
                magenta.float32[0] = 1.0f;
                magenta.float32[1] = 0.0f;
                magenta.float32[2] = 1.0f;
                magenta.float32[3] = 1.0f;

                data.next_cmd_clear_color_image(
                    cmd,
                    targetImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    &magenta,
                    1,
                    &range);

                VkImageMemoryBarrier toPresent{};
                toPresent.sType =
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

                toPresent.srcAccessMask =
                    VK_ACCESS_TRANSFER_WRITE_BIT;

                toPresent.dstAccessMask = 0;

                toPresent.oldLayout =
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                toPresent.newLayout =
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

                toPresent.srcQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;

                toPresent.dstQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;

                toPresent.image =
                    targetImage;

                toPresent.subresourceRange =
                    range;

                data.next_cmd_pipeline_barrier(
                    cmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    0,
                    0, nullptr,
                    0, nullptr,
                    1, &toPresent);

                r = data.next_end_command_buffer(cmd);

                if (r == VK_SUCCESS) {
                    VkSubmitInfo submit{};
                    submit.sType =
                        VK_STRUCTURE_TYPE_SUBMIT_INFO;

                    submit.commandBufferCount = 1;
                    submit.pCommandBuffers = &cmd;

                    r = data.next_queue_submit(
                        queue,
                        1,
                        &submit,
                        VkFence{});
                }

                if (r == VK_SUCCESS) {
                    r = data.next_queue_wait_idle(queue);
                }

                if (r == VK_SUCCESS &&
                    (frame <= 10 || frame % 120 == 0)) {

                    std::ostringstream os;

                    os << "[CLEAR-PRESENT]"
                       << " frame=" << frame
                       << " imageIndex=" << imageIndex
                       << " image=0x"
                       << std::hex
                       << image_handle_u64(targetImage)
                       << std::dec;

                    log_line(os.str());
                }

                if (r != VK_SUCCESS) {
                    std::ostringstream os;
                    os << "[CLEAR-ERROR] result="
                       << static_cast<int>(r);
                    log_line(os.str());
                }

                data.next_destroy_command_pool(
                    data.device,
                    pool,
                    nullptr);
            }
        }
        else {
            log_line(
                "[CLEAR-ERROR] pre-clear vkQueueWaitIdle");
        }
    }

    VkResult result =
        data.next_queue_present(
            queue,
            pPresentInfo);

    if (frame <= 10 || frame % 120 == 0) {
        std::ostringstream os;

        os << "[PRESENT]"
           << " frame=" << frame
           << " result="
           << static_cast<int>(result);

        log_line(os.str());
    }

    consume_present_waits(data, pPresentInfo, result);
    return result;
}

} // extern "C"
