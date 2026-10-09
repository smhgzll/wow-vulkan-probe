#include "overlay_internal.hpp"

namespace depth_probe {
void destroy_scene_color(const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st) {
    for (auto& image : st.sceneColor) {
        if (image.view) f.destroyImageView(data.device, image.view, nullptr);
        if (image.image) f.destroyImage(data.device, image.image, nullptr);
        if (image.memory) f.freeMemory(data.device, image.memory, nullptr);
    }
    st.sceneColor.clear();
}
void create_scene_color(const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st,
                        const SwapchainInfo& sc) {
    const auto flag = std::getenv("DEPTH_PROBE_SCENE_COLOR");
    if (!flag || std::strcmp(flag, "1")) return;
    const auto unavailable = [&] {
        destroy_scene_color(data, f, st);
        log_line("[SCENE-COLOR-UNAVAILABLE] usage/format/capability/allocation; legacy path retained");
    };
    if (!(sc.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !data.get_format_properties ||
        !data.get_image_format_properties || !f.createImage || !f.destroyImage ||
        !f.getImageMemoryRequirements || !f.bindImageMemory || !f.cmdCopyImage) { unavailable(); return; }
    VkFormatProperties format{};
    data.get_format_properties(data.physical_device, st.format, &format);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & needed) != needed) { unavailable(); return; }
    VkImageFormatProperties properties{};
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (data.get_image_format_properties(data.physical_device, st.format, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, usage, 0, &properties) != VK_SUCCESS ||
        properties.maxExtent.width < st.width || properties.maxExtent.height < st.height ||
        properties.maxExtent.depth < 1 || properties.maxMipLevels < 1 || properties.maxArrayLayers < 1 ||
        !(properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) { unavailable(); return; }
    VkPhysicalDeviceMemoryProperties memory{};
    data.get_memory_properties(data.physical_device, &memory);
    st.sceneColor.resize(st.colorImages.size());
    for (auto& image : st.sceneColor) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D; info.format = st.format;
        info.extent = {st.width, st.height, 1}; info.mipLevels = 1; info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (f.createImage(data.device, &info, nullptr, &image.image) != VK_SUCCESS) { unavailable(); return; }
        VkMemoryRequirements requirements{};
        f.getImageMemoryRequirements(data.device, image.image, &requirements);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if (!(requirements.memoryTypeBits & (1u << i))) continue;
            type = i;
            if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) break;
        }
        if (type == UINT32_MAX) { unavailable(); return; }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
        if (f.allocateMemory(data.device, &allocation, nullptr, &image.memory) != VK_SUCCESS ||
            f.bindImageMemory(data.device, image.image, image.memory, 0) != VK_SUCCESS) { unavailable(); return; }
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = st.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (f.createImageView(data.device, &view, nullptr, &image.view) != VK_SUCCESS) { unavailable(); return; }
    }
    log_line("[SCENE-COLOR-READY] per-image copy; binding=2 extent=" + std::to_string(st.width) + "x" + std::to_string(st.height));
}
void record_scene_color(const DepthOverlayFns& f, const DepthOverlayState& st, uint32_t index) {
    const auto cmd = st.commandBuffers[index];
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.image = st.colorImages[index];
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    f.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    // Per-image fence completed before reuse. Discard last frame's copied pixels.
    barrier.image = st.sceneColor[index].image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    f.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {st.width, st.height, 1};
    f.cmdCopyImage(cmd, st.colorImages[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        st.sceneColor[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    f.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}
} // namespace depth_probe
