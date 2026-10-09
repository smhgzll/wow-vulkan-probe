#include <chrono>
#include "overlay_renderer.hpp"
#include "overlay_internal.hpp"
#include "shader_resources.hpp"
#include "depth_tracking.hpp"
#include "sync_tracking.hpp"
#include "overlay_menu.hpp"
#include "surface_inspector.hpp"

namespace depth_probe {

DeviceHandleMap<DepthOverlayState> g_depth_overlay_states;

// This function never waits: callers either own an unsubmitted partial state,
// or have already waited for all submitted work to finish.
static void destroy_depth_overlay_resources(
    const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st) {
    release_menu_input();
    if(inspector_enabled())inspector_cancel(data.device);
    destroy_inspector_gpu(data,f,st);
    for (const auto& old : st.retiredPipelines) {
        f.destroyPipeline(data.device, old.effect, nullptr);
        f.destroyPipeline(data.device, old.menu, nullptr);
        f.destroyPipeline(data.device, old.gizmo, nullptr);
    }
    if (f.destroyPipeline && st.menuPipeline) f.destroyPipeline(data.device, st.menuPipeline, nullptr);
    if (f.destroyPipeline && st.gizmoPipeline) f.destroyPipeline(data.device, st.gizmoPipeline, nullptr);
    if (f.destroyCommandPool &&
        st.commandPool != VkCommandPool{}) {
        f.destroyCommandPool(
            data.device,
            st.commandPool,
            nullptr);
    }

    if (f.destroyFramebuffer) {
        for (VkFramebuffer fb : st.framebuffers) {
            if (fb != VkFramebuffer{})
                f.destroyFramebuffer(data.device, fb, nullptr);
        }
    }

    if (f.destroyImageView) {
        for (VkImageView view : st.colorViews) {
            if (view != VkImageView{})
                f.destroyImageView(data.device, view, nullptr);
        }
    }

    if (f.destroyPipeline &&
        st.pipeline != VkPipeline{}) {
        f.destroyPipeline(
            data.device,
            st.pipeline,
            nullptr);
    }

    if (f.destroyRenderPass &&
        st.renderPass != VkRenderPass{}) {
        f.destroyRenderPass(
            data.device,
            st.renderPass,
            nullptr);
    }

    if (f.destroyDescriptorPool &&
        st.descriptorPool != VkDescriptorPool{}) {
        f.destroyDescriptorPool(
            data.device,
            st.descriptorPool,
            nullptr);
    }

    if (f.destroyPipelineLayout &&
        st.pipelineLayout != VkPipelineLayout{}) {
        f.destroyPipelineLayout(
            data.device,
            st.pipelineLayout,
            nullptr);
    }

    if (f.destroyDescriptorSetLayout &&
        st.descriptorSetLayout != VkDescriptorSetLayout{}) {
        f.destroyDescriptorSetLayout(
            data.device,
            st.descriptorSetLayout,
            nullptr);
    }

    if (f.destroySampler &&
        st.sampler != VkSampler{}) {
        f.destroySampler(
            data.device,
            st.sampler,
            nullptr);
    }

    if(st.timestampPool)f.destroyQueryPool(data.device,st.timestampPool,nullptr);
    destroy_scene_color(data, f, st);
    destroy_overlay_uniforms(data, f, st);
    destroy_overlay_sync(data, f, st);
    st = DepthOverlayState{};
}

struct OverlayConstructionGuard {
    const DeviceData& data;
    const DepthOverlayFns& f;
    DepthOverlayState& state;
    bool committed = false;
    ~OverlayConstructionGuard() {
        if (!committed) {
            destroy_depth_overlay_resources(data, f, state);
            log_line("[DEPTH-OVERLAY-ROLLBACK] partial resources released");
        }
    }
};

// Command buffers come from a RESET_COMMAND_BUFFER pool. vkBeginCommandBuffer
// implicitly resets an old recording; the submit policy waits before calling us.
bool record_depth_overlay_commands(const DepthOverlayFns& f, DepthOverlayState& st,
                                   uint32_t imageIndex) {
    const size_t i = imageIndex;
    if (i >= st.commandBuffers.size() || i >= st.colorImages.size() || i >= st.framebuffers.size())
        return false;
    VkCommandBuffer cmd = st.commandBuffers[i];
    VkCommandBufferBeginInfo bi{};
    bi.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags =
        0; // Recorded once per image; fence-protected resubmission is intentional.

    VkResult r =
        f.beginCommandBuffer(cmd, &bi);

    if (r != VK_SUCCESS) {
        log_line("[DEPTH-OVERLAY-ERROR] vkBeginCommandBuffer result=" + std::to_string(r));
        return false;
    }

    if(st.timestampPool) {
        f.cmdResetQueryPool(cmd,st.timestampPool,imageIndex*2,2);
        f.cmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,st.timestampPool,imageIndex*2);
    }
    VkImageMemoryBarrier depthBarrier{};
    depthBarrier.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthBarrier.srcAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    depthBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT;
    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = st.depthImage;
    // Metadata determines whether stencil exists; the sampled view is DEPTH-only.
    depthBarrier.subresourceRange.aspectMask = depth_barrier_aspects(st.depthFormat);
    depthBarrier.subresourceRange.baseMipLevel = 0;
    depthBarrier.subresourceRange.levelCount = 1;
    depthBarrier.subresourceRange.baseArrayLayer = 0;
    depthBarrier.subresourceRange.layerCount = 1;

    f.cmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &depthBarrier);

    record_inspector_gpu(f,st,imageIndex);
    VkImageSubresourceRange colorRange{};
    colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorRange.baseMipLevel = 0;
    colorRange.levelCount = 1;
    colorRange.baseArrayLayer = 0;
    colorRange.layerCount = 1;

    if (!st.sceneColor.empty()) record_scene_color(f, st, imageIndex);
    VkImageMemoryBarrier toColor{};
    toColor.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toColor.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    // LOAD and destination blending read the game's existing color values.
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = st.sceneColor.empty() ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!st.sceneColor.empty()) toColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toColor.newLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.image = st.colorImages[i];
    toColor.subresourceRange = colorRange;

    f.cmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &toColor);

    VkRenderPassBeginInfo rbi{};
    rbi.sType =
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = st.renderPass;
    rbi.framebuffer = st.framebuffers[i];
    rbi.renderArea.offset = {0, 0};
    rbi.renderArea.extent = {st.width, st.height};

    f.cmdBeginRenderPass(
        cmd,
        &rbi,
        VK_SUBPASS_CONTENTS_INLINE);

    f.cmdBindPipeline(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        st.pipeline);

    f.cmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        st.pipelineLayout,
        0,
        1,
        &st.descriptorSets[i],
        0,
        nullptr);

    f.cmdDraw(cmd, 3, 1, 0, 0);
    f.cmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, st.menuPipeline);
    f.cmdDrawIndirect(cmd, st.uniforms[i].buffer, offsetof(OverlayFrameData, menuDraw), 1, sizeof(VkDrawIndirectCommand));
    // Camera Telemetry HUD: same pipeline/descriptor set, a second indirect
    // draw distinguished only by VkDrawIndirectCommand.firstVertex (see
    // menu.vert) - independent of menu.visible, so it keeps rendering after
    // F10 closes.
    f.cmdDrawIndirect(cmd, st.uniforms[i].buffer, offsetof(OverlayFrameData, hudDraw), 1, sizeof(VkDrawIndirectCommand));
    // 3D camera gizmo (F8): same pipelineLayout/descriptor set (already
    // bound above), different pipeline (LINE_LIST, see overlay_pipeline.cpp)
    // and its own indirect draw - independent of the menu/HUD.
    f.cmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, st.gizmoPipeline);
    f.cmdDrawIndirect(cmd, st.uniforms[i].buffer, offsetof(OverlayFrameData, gizmoDraw), 1, sizeof(VkDrawIndirectCommand));
    record_inspector_preview(f,st,imageIndex);
    f.cmdEndRenderPass(cmd);

    VkImageMemoryBarrier toPresent{};
    toPresent.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toPresent.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout =
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = st.colorImages[i];
    toPresent.subresourceRange = colorRange;

    f.cmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &toPresent);

    // Keep this frame's depth reads ahead of later same-queue depth writes.
    VkMemoryBarrier depthReadComplete{};
    depthReadComplete.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    depthReadComplete.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthReadComplete.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    f.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 1, &depthReadComplete, 0, nullptr, 0, nullptr);

    if(st.timestampPool)f.cmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,st.timestampPool,imageIndex*2+1);
    r =
        f.endCommandBuffer(cmd);

    if (r != VK_SUCCESS) {
        log_line("[DEPTH-OVERLAY-ERROR] vkEndCommandBuffer");
        return false;
    }
    st.recordedGeneration[i] = st.pipelineGeneration;
    return true;
}

bool create_depth_overlay_state(
    const DeviceData& data,
    VkSwapchainKHR swapchain,
    const SceneDepthChoice& selected,
    DepthOverlayState& outState) {

    const DepthOverlayFns f =
        load_depth_overlay_fns(data);

    if (!depth_overlay_fns_ok(f)) {
        log_line("[DEPTH-OVERLAY-ERROR] eksik Vulkan function pointer");
        return false;
    }

    SwapchainInfo sc{};
    const auto& candidate = selected.candidate;
    const uint64_t depthImageKey = image_handle_u64(candidate.image);
    const VkImageView depthView = candidate.depthView;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);

        auto sit =
            g_swapchains.find({data.device, swapchain_handle_u64(swapchain)});

        if (sit == g_swapchains.end() ||
            sit->second.images.empty()) {
            return false;
        }

        sc = sit->second;

    }

    if (candidate.device != data.device || sc.retired ||
        score_scene_depth_candidate(candidate, data.device, {sc.width, sc.height}) < 0) {
        return false;
    }

    DepthOverlayState st{};
    OverlayConstructionGuard rollback{data, f, st};
    st.swapchain = swapchain;
    st.depthImage = candidate.image;
    st.depthGeneration = candidate.generation;
    st.depthFormat = candidate.format;
    st.depthView = depthView;
    st.width = sc.width;
    st.height = sc.height;
    st.format = sc.format;
    st.colorImages = sc.images;
    create_scene_color(data, f, st, sc);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.compareEnable = VK_FALSE;

    VkResult r =
        f.createSampler(
            data.device,
            &samplerInfo,
            nullptr,
            &st.sampler);

    if (r != VK_SUCCESS) {
        log_line("[DEPTH-OVERLAY-ERROR] vkCreateSampler");
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2] = bindings[0]; bindings[2].binding = 2;
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 3; dslInfo.pBindings = bindings;
    r = f.createDescriptorSetLayout(data.device, &dslInfo, nullptr, &st.descriptorSetLayout);
    if (r != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1; plInfo.pSetLayouts = &st.descriptorSetLayout;
    r = f.createPipelineLayout(data.device, &plInfo, nullptr, &st.pipelineLayout);
    if (r != VK_SUCCESS) return false;

    if (!create_overlay_uniforms(data, f, st)) return false;
    const uint32_t imageCount = static_cast<uint32_t>(sc.images.size());
    VkDescriptorPoolSize poolSizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageCount * 2},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount}};
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets = imageCount; dpInfo.poolSizeCount = 2; dpInfo.pPoolSizes = poolSizes;
    r = f.createDescriptorPool(data.device, &dpInfo, nullptr, &st.descriptorPool);
    if (r != VK_SUCCESS) return false;
    st.descriptorSets.resize(imageCount);
    const std::vector<VkDescriptorSetLayout> layouts(imageCount, st.descriptorSetLayout);
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = st.descriptorPool;
    dsAlloc.descriptorSetCount = imageCount; dsAlloc.pSetLayouts = layouts.data();
    r = f.allocateDescriptorSets(data.device, &dsAlloc, st.descriptorSets.data());
    if (r != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkDescriptorImageInfo imageInfo{st.sampler, st.depthView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo bufferInfo{st.uniforms[i].buffer, 0, sizeof(OverlayUniform)};
        VkDescriptorImageInfo sceneInfo = imageInfo;
        if (!st.sceneColor.empty()) sceneInfo = {st.sampler, st.sceneColor[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[3]{};
        for (auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = st.descriptorSets[i]; write.descriptorCount = 1; }
        writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &imageInfo;
        writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[1].pBufferInfo = &bufferInfo;
        writes[2] = writes[0]; writes[2].dstBinding = 2; writes[2].pImageInfo = &sceneInfo;
        f.updateDescriptorSets(data.device, 3, writes, 0, nullptr);
    }

    VkAttachmentDescription attachment{};
    attachment.format = st.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint =
        VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType =
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;

    r =
        f.createRenderPass(
            data.device,
            &rpInfo,
            nullptr,
            &st.renderPass);

    if (r != VK_SUCCESS) {
        log_line("[DEPTH-OVERLAY-ERROR] vkCreateRenderPass");
        return false;
    }

    if (!create_overlay_pipelines(data, f, st, st.pipeline, st.menuPipeline, st.gizmoPipeline)) return false;
    if(inspector_enabled() && !create_inspector_gpu(data,f,st)) {
        destroy_inspector_gpu(data,f,st);
        log_line("[INSPECTOR-GPU-UNAVAILABLE] private resources failed; original overlay retained");
    }
    initialize_shader_reload(st);
    create_gpu_timing(data,f,st);

    st.colorViews.resize(sc.images.size());
    st.framebuffers.resize(sc.images.size());

    for (size_t i = 0; i < sc.images.size(); ++i) {
        VkImageViewCreateInfo iv{};
        iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image = sc.images[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = st.format;
        iv.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv.subresourceRange.baseMipLevel = 0;
        iv.subresourceRange.levelCount = 1;
        iv.subresourceRange.baseArrayLayer = 0;
        iv.subresourceRange.layerCount = 1;

        r =
            f.createImageView(
                data.device,
                &iv,
                nullptr,
                &st.colorViews[i]);

        if (r != VK_SUCCESS) {
            log_line("[DEPTH-OVERLAY-ERROR] swapchain vkCreateImageView");
            return false;
        }

        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = st.renderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &st.colorViews[i];
        fb.width = st.width;
        fb.height = st.height;
        fb.layers = 1;

        r =
            f.createFramebuffer(
                data.device,
                &fb,
                nullptr,
                &st.framebuffers[i]);

        if (r != VK_SUCCESS) {
            log_line("[DEPTH-OVERLAY-ERROR] vkCreateFramebuffer");
            return false;
        }
    }

    VkCommandPoolCreateInfo cp{};
    cp.sType =
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = 0;

    r =
        f.createCommandPool(
            data.device,
            &cp,
            nullptr,
            &st.commandPool);

    if (r != VK_SUCCESS) {
        log_line("[DEPTH-OVERLAY-ERROR] vkCreateCommandPool");
        return false;
    }

    st.commandBuffers.resize(sc.images.size());
    st.commandRecorded.resize(sc.images.size(), false);

    VkCommandBufferAllocateInfo ca{};
    ca.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = st.commandPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount =
        static_cast<uint32_t>(st.commandBuffers.size());

    r =
        f.allocateCommandBuffers(
            data.device,
            &ca,
            st.commandBuffers.data());

    if (r != VK_SUCCESS) {
        log_line("[DEPTH-OVERLAY-ERROR] vkAllocateCommandBuffers");
        return false;
    }

    for (VkCommandBuffer cmd : st.commandBuffers) {
        if (!initialize_layer_command_buffer(data, cmd))
            return false;
    }

    if (!create_overlay_sync(data, f, st)) return false;

    outState = std::move(st);
    rollback.committed = true;

    std::ostringstream ready;
    ready
        << "[DEPTH-OVERLAY-READY]"
        << " extent=" << outState.width << "x" << outState.height
        << " format=" << static_cast<int>(outState.format)
        << " images=" << outState.commandBuffers.size()
        << " depthImage=0x" << std::hex << depthImageKey
        << " depthView=0x"
        << image_view_handle_u64(outState.depthView)
        << std::dec;

    log_line(ready.str());

    return true;
}


void destroy_depth_overlay_state(
    const DeviceData& data,
    VkSwapchainKHR swapchain) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);

    const uint64_t key =
        swapchain_handle_u64(swapchain);

    DepthOverlayState st{};
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);

        auto it =
            g_depth_overlay_states.find({data.device, key});

        if (it != g_depth_overlay_states.end()) {
            st = std::move(it->second);
            g_depth_overlay_states.erase(it);
            found = true;
        }
    }

    if (!found)
        return;

    const DepthOverlayFns f =
        load_depth_overlay_fns(data);

    wait_overlay_sync(data, f, st); // completion or device loss, including presentation resources
    if (f.deviceWaitIdle) f.deviceWaitIdle(data.device);

    destroy_depth_overlay_resources(data, f, st);

    std::ostringstream os;
    os << "[DEPTH-OVERLAY-DESTROY]"
       << " swapchain=0x"
       << std::hex << key << std::dec;

    log_line(os.str());
}

namespace {
// Present-to-present time, logged every 300 frames next to the water timings so a slowdown can be read straight from the log, with the
// layer's own blocking time (mutex waits, fence waits, the driver's present/submit) and the draw count.
// Performance test (Water Reflections menu -> PERFORMANCE TEST): once the menu is closed the logger walks through five phases by itself, each
// 120 warm-up + 360 measured frames, so what each part costs shows up in one run while the player stands still:
//   0 as configured | 1 water replay off | 2 + overlay effect off | 3 + command tracking off (the layer's per-command bookkeeping) | 4 as configured again.
struct FramePerf {
    std::chrono::steady_clock::time_point last=std::chrono::steady_clock::now();
    double total=0,worst=0;unsigned count=0;
    bool ab=false;int phase=0;unsigned phaseFrame=0;double abTotal=0;unsigned abCount=0;double abMs[5]{},abDraws[5]{};
};
constexpr unsigned abWarmup=120,abMeasure=360;constexpr int abPhases=5;
void ab_apply(int phase) {
    g_perf_water_off.store(phase>=1 && phase<=3);g_perf_effect_off.store(phase>=2 && phase<=3);g_perf_tracking_off.store(phase==3);
}
void log_frame_performance(float overlayGpuMs,uint64_t frame) {
    static FramePerf p;
    const auto now=std::chrono::steady_clock::now();const double ms=std::chrono::duration<double,std::milli>(now-p.last).count();p.last=now;
    if(p.count>0 || ms<1000.){p.total+=ms;p.worst=std::max(p.worst,ms);}
    if(!p.ab && g_perf_ab_request.load() && !overlay_menu().visible) {   // started from the menu; waits for the menu to be closed
        g_perf_ab_request.store(false);p.ab=true;p.phase=0;p.phaseFrame=0;p.abTotal=0;p.abCount=0;g_perf.draws.store(0);
        log_line("[PERF-AB] started: keep still for about 40 seconds");
    }
    if(p.ab) {
        if(p.phaseFrame==0){ab_apply(p.phase);g_perf.draws.store(0);}
        if(p.phaseFrame>=abWarmup && ms<1000.){p.abTotal+=ms;++p.abCount;}
        if(++p.phaseFrame>=abWarmup+abMeasure) {
            const char* names[]{"as configured","water replay off","+ overlay effect off","+ command tracking off","as configured (again)"};
            p.abMs[p.phase]=p.abCount?p.abTotal/p.abCount:0;p.abDraws[p.phase]=double(g_perf.draws.exchange(0))/(abWarmup+abMeasure);
            log_line(std::string("[PERF-AB] phase ")+std::to_string(p.phase)+" ("+names[p.phase]+"): fps="+std::to_string(p.abMs[p.phase]>0?1000./p.abMs[p.phase]:0.)+" frame="+std::to_string(p.abMs[p.phase])+" ms draws/frame="+std::to_string(p.abDraws[p.phase])+" (n="+std::to_string(p.abCount)+")");
            p.abTotal=0;p.abCount=0;p.phaseFrame=0;
            if(++p.phase>=abPhases) {
                p.ab=false;ab_apply(-1);
                const auto d=[&](int a,int b){return std::to_string(p.abMs[a]-p.abMs[b]);};
                log_line("[PERF-AB] summary (ms per frame): configured "+std::to_string(p.abMs[0])+" | water replay costs "+d(0,1)+" | overlay effect costs "+d(1,2)+" | command tracking costs "+d(2,3)+" | left when all off "+std::to_string(p.abMs[3])+
                         " | configured again "+std::to_string(p.abMs[4])+" (differs from the first = the scene changed or the game was loading: stand still and repeat)");
            }
        }
    }
    if(++p.count>=300) {
        const auto take=[&](std::atomic<uint64_t>& v){return double(v.exchange(0,std::memory_order_relaxed))/1e6/p.count;};   // ms per frame
        const double present=take(g_perf.presentNs),presentLock=take(g_perf.presentLockNs),overlay=take(g_perf.overlayNs),fence=take(g_perf.fenceWaitNs),downstream=take(g_perf.downstreamPresentNs);
        const double submit=take(g_perf.submitNs),submitLock=take(g_perf.submitLockNs),driver=take(g_perf.submitDriverNs);
        const double submits=p.ab?0:double(g_perf.submits.exchange(0))/p.count,draws=p.ab?0:double(g_perf.draws.exchange(0))/p.count;
        const auto n=[](double v){char b[32];std::snprintf(b,sizeof(b),"%.2f",v);return std::string(b);};
        log_line("[FRAME-PERF] fps="+std::to_string(p.total>0?p.count*1000./p.total:0.)+" avg="+std::to_string(p.total/p.count)+" ms worst="+std::to_string(p.worst)+" ms (n="+std::to_string(p.count)+") water="+
            [](){const auto h=g_water_hint.load(std::memory_order_relaxed);return h==UINT32_MAX?std::string("n/a"):std::string(h&15?((h&15)==1?"ssr":"planar"):"off")+" layers="+std::to_string((h>>4)&255)+" quality="+std::to_string(h>>12);}()+
            " overlayGpu="+std::to_string(overlayGpuMs)+" ms frame="+std::to_string(frame));
        log_line("[LAYER-PERF] per frame: present hook "+n(present)+" ms (waiting for the mutex "+n(presentLock)+", overlay "+n(overlay)+" of which fence waits "+n(fence)+", driver present "+n(downstream)+") | submit hooks "+n(submit)+" ms in "+n(submits)+" submits (mutex wait "+n(submitLock)+", driver submit "+n(driver)+") | draws "+n(draws));
        p.total=p.worst=0;p.count=0;
    }
}
}
OverlayPresentResult depth_overlay_present(
    const DeviceData& data,
    VkQueue queue,
    const VkPresentInfoKHR* presentInfo,
    uint64_t frame) {
    PerfTimer overlayTimer(g_perf.overlayNs);
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);

    if (data.sync && data.sync->deviceLost) return {true, VK_ERROR_DEVICE_LOST, false};
    if (data.sync && data.sync->terminalError != VK_SUCCESS) return {true, data.sync->terminalError, false};
    // A failed present enqueue may leave a submitted overlay signal waiting for
    // retry. Handle this before normal support checks or selection can change.
    if (presentInfo && presentInfo->pSwapchains && presentInfo->pImageIndices) {
        for (uint32_t i = 0; i < presentInfo->swapchainCount; ++i) {
            if (data.sync) {
                auto sc = data.sync->abandonedPresents.find(presentInfo->pSwapchains[i]);
                if (sc != data.sync->abandonedPresents.end()) {
                    auto failure = sc->second.find(presentInfo->pImageIndices[i]);
                    if (failure != sc->second.end()) return {true, failure->second, false};
                }
            }
            auto it = g_depth_overlay_states.find({data.device, swapchain_handle_u64(presentInfo->pSwapchains[i])});
            if (it != g_depth_overlay_states.end() && presentInfo->pImageIndices[i] < it->second.sync.size() &&
                it->second.sync[presentInfo->pImageIndices[i]].retryPresent) {
                if (presentInfo->swapchainCount != 1)
                    return {true, it->second.sync[presentInfo->pImageIndices[i]].retryResult, false};
                return submit_depth_overlay(data, load_depth_overlay_fns(data), queue, it->second, presentInfo, frame, true);
            }
        }
    }
    if (const char* reason = overlay_sync_support(data, queue, presentInfo)) {
        sync_skip(frame, reason);
        return {};
    }

    const VkSwapchainKHR swapchain =
        presentInfo->pSwapchains[0];

    const uint64_t scKey =
        swapchain_handle_u64(swapchain);

    const auto selected = resolve_scene_depth(data.device, swapchain);
    if (!selected) {
        destroy_depth_overlay_state(data, swapchain);
        sync_skip(frame, "scene depth not ready");
        return {};
    }
    bool needCreate = false;
    bool stale = false;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);

        const auto it = g_depth_overlay_states.find({data.device, scKey});
        needCreate = it == g_depth_overlay_states.end();
        if (!needCreate) {
            const auto& state = it->second;
            const auto& candidate = selected->candidate;
            stale = state.depthGeneration != candidate.generation || state.depthImage != candidate.image ||
                    state.depthView != candidate.depthView || state.width != candidate.extent.width ||
                    state.height != candidate.extent.height;
        }
    }

    if (stale) {
        destroy_depth_overlay_state(data, swapchain);
        needCreate = true;
    }
    if (needCreate) {
        DepthOverlayState newState{};

        if (!create_depth_overlay_state(
                data,
                swapchain,
                *selected,
                newState)) {
            sync_skip(frame, "overlay resources not ready");
            return {};
        }

        bool inserted;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            inserted = g_depth_overlay_states.try_emplace(
                DeviceHandleKey{data.device, scKey}, std::move(newState)).second;
        }
        if (!inserted) {
            const auto f = load_depth_overlay_fns(data);
            destroy_depth_overlay_resources(data, f, newState);
        }
    }

    auto it = g_depth_overlay_states.find({data.device, scKey});
    if (it == g_depth_overlay_states.end()) return {};
    auto& state=it->second;
    state.frameNumber=frame;state.originalWaitCount=presentInfo->waitSemaphoreCount;
    log_frame_performance(state.overlayGpuMs,frame);
    const auto result=submit_depth_overlay(data, load_depth_overlay_fns(data), queue, state, presentInfo, frame);
    const auto index=presentInfo->pImageIndices[0];
    if(state.timestampPool && index<state.sync.size() && state.sync[index].submitted)state.queryPending[index]=true;
    return result;
}

void destroy_depth_overlays_for_image(const DeviceData& data, VkImage image) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    std::vector<VkSwapchainKHR> swapchains;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (const auto& [key, state] : g_depth_overlay_states) {
            if (key.device == data.device && state.depthImage == image)
                swapchains.push_back(state.swapchain);
        }
    }
    for (VkSwapchainKHR swapchain : swapchains)
        destroy_depth_overlay_state(data, swapchain);
}

void destroy_device_depth_overlays(const DeviceData& data) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    std::vector<VkSwapchainKHR> swapchains;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (const auto& [key, state] : g_depth_overlay_states) {
            if (key.device == data.device)
                swapchains.push_back(state.swapchain);
        }
    }
    for (VkSwapchainKHR swapchain : swapchains)
        destroy_depth_overlay_state(data, swapchain);
}

} // namespace depth_probe
