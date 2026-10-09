#include "overlay_internal.hpp"

namespace depth_probe {

template <typename T>
T overlay_load(const DeviceData& data, const char* name) {
    if (!data.next_gdpa || data.device == VkDevice{})
        return nullptr;

    return reinterpret_cast<T>(
        data.next_gdpa(data.device, name));
}

DepthOverlayFns load_depth_overlay_fns(const DeviceData& data) {
    DepthOverlayFns f{};
    f.cmdCopyImageToBuffer=overlay_load<PFN_vkCmdCopyImageToBuffer>(data,"vkCmdCopyImageToBuffer");
    f.invalidateMappedMemoryRanges=overlay_load<PFN_vkInvalidateMappedMemoryRanges>(data,"vkInvalidateMappedMemoryRanges");
    f.createQueryPool=overlay_load<PFN_vkCreateQueryPool>(data,"vkCreateQueryPool");
    f.destroyQueryPool=overlay_load<PFN_vkDestroyQueryPool>(data,"vkDestroyQueryPool");
    f.cmdResetQueryPool=overlay_load<PFN_vkCmdResetQueryPool>(data,"vkCmdResetQueryPool");
    f.cmdWriteTimestamp=overlay_load<PFN_vkCmdWriteTimestamp>(data,"vkCmdWriteTimestamp");
    f.getQueryPoolResults=overlay_load<PFN_vkGetQueryPoolResults>(data,"vkGetQueryPoolResults");
    f.createImage = overlay_load<PFN_vkCreateImage>(data, "vkCreateImage");
    f.destroyImage = overlay_load<PFN_vkDestroyImage>(data, "vkDestroyImage");
    f.getImageMemoryRequirements = overlay_load<PFN_vkGetImageMemoryRequirements>(data, "vkGetImageMemoryRequirements");
    f.bindImageMemory = overlay_load<PFN_vkBindImageMemory>(data, "vkBindImageMemory");
    f.cmdCopyImage = overlay_load<PFN_vkCmdCopyImage>(data, "vkCmdCopyImage");

    f.createSampler = overlay_load<PFN_vkCreateSampler>(data, "vkCreateSampler");
    f.destroySampler = overlay_load<PFN_vkDestroySampler>(data, "vkDestroySampler");
    f.createDescriptorSetLayout = overlay_load<PFN_vkCreateDescriptorSetLayout>(data, "vkCreateDescriptorSetLayout");
    f.destroyDescriptorSetLayout = overlay_load<PFN_vkDestroyDescriptorSetLayout>(data, "vkDestroyDescriptorSetLayout");
    f.createPipelineLayout = overlay_load<PFN_vkCreatePipelineLayout>(data, "vkCreatePipelineLayout");
    f.destroyPipelineLayout = overlay_load<PFN_vkDestroyPipelineLayout>(data, "vkDestroyPipelineLayout");
    f.createRenderPass = overlay_load<PFN_vkCreateRenderPass>(data, "vkCreateRenderPass");
    f.destroyRenderPass = overlay_load<PFN_vkDestroyRenderPass>(data, "vkDestroyRenderPass");
    f.createShaderModule = overlay_load<PFN_vkCreateShaderModule>(data, "vkCreateShaderModule");
    f.destroyShaderModule = overlay_load<PFN_vkDestroyShaderModule>(data, "vkDestroyShaderModule");
    f.createGraphicsPipelines = overlay_load<PFN_vkCreateGraphicsPipelines>(data, "vkCreateGraphicsPipelines");
    f.destroyPipeline = overlay_load<PFN_vkDestroyPipeline>(data, "vkDestroyPipeline");
    f.createDescriptorPool = overlay_load<PFN_vkCreateDescriptorPool>(data, "vkCreateDescriptorPool");
    f.destroyDescriptorPool = overlay_load<PFN_vkDestroyDescriptorPool>(data, "vkDestroyDescriptorPool");
    f.allocateDescriptorSets = overlay_load<PFN_vkAllocateDescriptorSets>(data, "vkAllocateDescriptorSets");
    f.updateDescriptorSets = overlay_load<PFN_vkUpdateDescriptorSets>(data, "vkUpdateDescriptorSets");
    f.createImageView = overlay_load<PFN_vkCreateImageView>(data, "vkCreateImageView");
    f.destroyImageView = overlay_load<PFN_vkDestroyImageView>(data, "vkDestroyImageView");
    f.createFramebuffer = overlay_load<PFN_vkCreateFramebuffer>(data, "vkCreateFramebuffer");
    f.destroyFramebuffer = overlay_load<PFN_vkDestroyFramebuffer>(data, "vkDestroyFramebuffer");
    f.createCommandPool = overlay_load<PFN_vkCreateCommandPool>(data, "vkCreateCommandPool");
    f.destroyCommandPool = overlay_load<PFN_vkDestroyCommandPool>(data, "vkDestroyCommandPool");
    f.allocateCommandBuffers = overlay_load<PFN_vkAllocateCommandBuffers>(data, "vkAllocateCommandBuffers");
    f.beginCommandBuffer = overlay_load<PFN_vkBeginCommandBuffer>(data, "vkBeginCommandBuffer");
    f.endCommandBuffer = overlay_load<PFN_vkEndCommandBuffer>(data, "vkEndCommandBuffer");
    f.cmdPipelineBarrier = overlay_load<PFN_vkCmdPipelineBarrier>(data, "vkCmdPipelineBarrier");
    f.cmdBeginRenderPass = overlay_load<PFN_vkCmdBeginRenderPass>(data, "vkCmdBeginRenderPass");
    f.cmdEndRenderPass = overlay_load<PFN_vkCmdEndRenderPass>(data, "vkCmdEndRenderPass");
    f.cmdBindPipeline = overlay_load<PFN_vkCmdBindPipeline>(data, "vkCmdBindPipeline");
    f.cmdBindDescriptorSets = overlay_load<PFN_vkCmdBindDescriptorSets>(data, "vkCmdBindDescriptorSets");
    f.createBuffer = overlay_load<PFN_vkCreateBuffer>(data, "vkCreateBuffer");
    f.destroyBuffer = overlay_load<PFN_vkDestroyBuffer>(data, "vkDestroyBuffer");
    f.getBufferMemoryRequirements = overlay_load<PFN_vkGetBufferMemoryRequirements>(data, "vkGetBufferMemoryRequirements");
    f.allocateMemory = overlay_load<PFN_vkAllocateMemory>(data, "vkAllocateMemory");
    f.freeMemory = overlay_load<PFN_vkFreeMemory>(data, "vkFreeMemory");
    f.bindBufferMemory = overlay_load<PFN_vkBindBufferMemory>(data, "vkBindBufferMemory");
    f.mapMemory = overlay_load<PFN_vkMapMemory>(data, "vkMapMemory");
    f.unmapMemory = overlay_load<PFN_vkUnmapMemory>(data, "vkUnmapMemory");
    f.flushMappedMemoryRanges = overlay_load<PFN_vkFlushMappedMemoryRanges>(data, "vkFlushMappedMemoryRanges");
    f.cmdDrawIndirect = overlay_load<PFN_vkCmdDrawIndirect>(data, "vkCmdDrawIndirect");
    f.cmdDraw = overlay_load<PFN_vkCmdDraw>(data, "vkCmdDraw");
    f.queueSubmit = overlay_load<PFN_vkQueueSubmit>(data, "vkQueueSubmit");
    f.createSemaphore = overlay_load<PFN_vkCreateSemaphore>(data, "vkCreateSemaphore");
    f.destroySemaphore = overlay_load<PFN_vkDestroySemaphore>(data, "vkDestroySemaphore");
    f.createFence = overlay_load<PFN_vkCreateFence>(data, "vkCreateFence");
    f.destroyFence = overlay_load<PFN_vkDestroyFence>(data, "vkDestroyFence");
    f.waitForFences = overlay_load<PFN_vkWaitForFences>(data, "vkWaitForFences");
    f.resetFences = overlay_load<PFN_vkResetFences>(data, "vkResetFences");
    f.deviceWaitIdle = overlay_load<PFN_vkDeviceWaitIdle>(data, "vkDeviceWaitIdle");

    return f;
}

bool depth_overlay_fns_ok(const DepthOverlayFns& f) {
    return f.destroySampler &&
           f.destroyDescriptorSetLayout &&
           f.destroyPipelineLayout &&
           f.destroyRenderPass &&
           f.destroyPipeline &&
           f.destroyDescriptorPool &&
           f.destroyImageView &&
           f.destroyFramebuffer &&
           f.destroyCommandPool &&
           f.deviceWaitIdle &&
           f.createSampler &&
           f.createDescriptorSetLayout &&
           f.createPipelineLayout &&
           f.createRenderPass &&
           f.createShaderModule &&
           f.destroyShaderModule &&
           f.createGraphicsPipelines &&
           f.createDescriptorPool &&
           f.allocateDescriptorSets &&
           f.updateDescriptorSets &&
           f.createImageView &&
           f.createFramebuffer &&
           f.createCommandPool &&
           f.allocateCommandBuffers &&
           f.beginCommandBuffer &&
           f.endCommandBuffer &&
           f.cmdPipelineBarrier &&
           f.cmdBeginRenderPass &&
           f.cmdEndRenderPass &&
           f.cmdBindPipeline &&
           f.cmdBindDescriptorSets &&
           f.createBuffer &&
           f.destroyBuffer &&
           f.getBufferMemoryRequirements &&
           f.allocateMemory &&
           f.freeMemory &&
           f.bindBufferMemory &&
           f.mapMemory &&
           f.unmapMemory &&
           f.flushMappedMemoryRanges &&
           f.cmdDrawIndirect &&
           f.cmdDraw &&
           f.queueSubmit &&
           f.createSemaphore && f.destroySemaphore && f.createFence && f.destroyFence &&
           f.waitForFences && f.resetFences;
}

} // namespace depth_probe
