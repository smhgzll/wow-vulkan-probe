#pragma once
#include "layer_state.hpp"
#include "heartbeat.hpp"
#include "overlay_renderer.hpp"
#include "overlay_uniform.hpp"
#include <array>
#include <chrono>

namespace depth_probe {

struct InspectorGpu;
struct DepthOverlayFns {
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    PFN_vkInvalidateMappedMemoryRanges invalidateMappedMemoryRanges = nullptr;
    PFN_vkCreateQueryPool createQueryPool = nullptr;
    PFN_vkDestroyQueryPool destroyQueryPool = nullptr;
    PFN_vkCmdResetQueryPool cmdResetQueryPool = nullptr;
    PFN_vkCmdWriteTimestamp cmdWriteTimestamp = nullptr;
    PFN_vkGetQueryPoolResults getQueryPoolResults = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkCreateSampler createSampler = nullptr;
    PFN_vkDestroySampler destroySampler = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateRenderPass createRenderPass = nullptr;
    PFN_vkDestroyRenderPass destroyRenderPass = nullptr;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreateGraphicsPipelines createGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets allocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
    PFN_vkCreateImageView createImageView = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkCreateFramebuffer createFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer destroyFramebuffer = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdBeginRenderPass cmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass cmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges flushMappedMemoryRanges = nullptr;
    PFN_vkCmdDrawIndirect cmdDrawIndirect = nullptr;
    PFN_vkCmdDraw cmdDraw = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
};

struct OverlayFrameSync {
    VkSemaphore finishedSemaphore{};
    VkFence submitFence{};
    VkFence ownPresentFence{};
    VkFence pendingPresentFence{}; // owned or borrowed, protected by fence hooks
    bool submitted = false;
    bool retryPresent = false; // submit succeeded, present OOM did not enqueue
    VkResult retryResult = VK_SUCCESS;
    std::vector<VkSemaphore> consumedWaits;
};

struct SceneColorImage { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; };
struct ShaderStamp {
    int64_t seconds{}, nanos{};
    uint64_t size{}, inode{};
    bool operator==(const ShaderStamp&) const = default;
};
struct RetiredPipelines { VkPipeline effect{}, menu{}, gizmo{}; uint64_t generation{}; };
struct DepthOverlayState {
    std::shared_ptr<InspectorGpu> inspectorGpu;
    VkQueryPool timestampPool{};
    float timestampPeriod=0, overlayGpuMs=-1;
    uint32_t timestampBits=0, originalWaitCount=0;
    uint64_t frameNumber=0;
    bool timingReadFailed=false;
    std::vector<bool> queryPending;
    std::vector<SceneColorImage> sceneColor;
    bool shaderReloadEnabled = false;
    uint32_t shaderReloadStatus = 0; // 0 unchanged, 1 success, 2 failure
    std::array<ShaderStamp, 6> shaderStamps{};
    std::chrono::steady_clock::time_point shaderPollAfter{};
    uint64_t pipelineGeneration = 1;
    std::vector<uint64_t> recordedGeneration;
    std::vector<RetiredPipelines> retiredPipelines;
    VkSwapchainKHR swapchain = VkSwapchainKHR{};
    VkImage depthImage = VkImage{};
    VkImageView depthView = VkImageView{};
    uint64_t depthGeneration = 0;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    VkSampler sampler = VkSampler{};
    VkDescriptorSetLayout descriptorSetLayout = VkDescriptorSetLayout{};
    VkPipelineLayout pipelineLayout = VkPipelineLayout{};
    VkRenderPass renderPass = VkRenderPass{};
    VkPipeline pipeline = VkPipeline{};
    VkPipeline menuPipeline = VkPipeline{};
    VkPipeline gizmoPipeline = VkPipeline{}; // 3D camera gizmo (F8), LINE_LIST topology - see overlay_pipeline.cpp
    VkDescriptorPool descriptorPool = VkDescriptorPool{};
    std::vector<VkDescriptorSet> descriptorSets;
    std::vector<MappedUniform> uniforms;
    std::vector<bool> commandRecorded;
    VkCommandPool commandPool = VkCommandPool{};

    std::vector<VkImageView> colorViews;
    std::vector<VkImage> colorImages; // borrowed from the swapchain
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<OverlayFrameSync> sync;
};

extern DeviceHandleMap<DepthOverlayState> g_depth_overlay_states;

DepthOverlayFns load_depth_overlay_fns(const DeviceData& data);
bool depth_overlay_fns_ok(const DepthOverlayFns& f);
bool record_depth_overlay_commands(const DepthOverlayFns& f, DepthOverlayState& state,
                                   uint32_t imageIndex);
void create_gpu_timing(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&);
VkResult collect_gpu_timing(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&, uint32_t);
void populate_diagnostics(const DeviceData&, const DepthOverlayState&, OverlayUniform&);
void create_scene_color(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&, const SwapchainInfo&);
void destroy_scene_color(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&);
void record_scene_color(const DepthOverlayFns&, const DepthOverlayState&, uint32_t);
bool create_overlay_pipelines(const DeviceData&, const DepthOverlayFns&, const DepthOverlayState&, VkPipeline& effect, VkPipeline& menu, VkPipeline& gizmo);
void initialize_shader_reload(DepthOverlayState&);
void prepare_shader_frame(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&, uint32_t);
bool create_overlay_sync(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&);
VkResult wait_overlay_sync(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&);
void destroy_overlay_sync(const DeviceData&, const DepthOverlayFns&, DepthOverlayState&);
OverlayPresentResult submit_depth_overlay(const DeviceData&, const DepthOverlayFns&, VkQueue,
    DepthOverlayState&, const VkPresentInfoKHR*, uint64_t frame, bool retryOnly = false);
} // namespace depth_probe
