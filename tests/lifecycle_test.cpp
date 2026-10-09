#include "layer_state.hpp"
#include "overlay_renderer.hpp"
#include "heartbeat.hpp"
#include "depth_tracking.hpp"
#include "sync_tracking.hpp"
#include "sync_capabilities.hpp"
#include "overlay_internal.hpp"
#include "overlay_menu.hpp"
#include "draw_probe.hpp"

#include <cmath>
#include <filesystem>
#include <cstdio>
#include <set>
#include <type_traits>

using namespace depth_probe;
namespace {
void check(bool ok, const char* expression, int line) {
    if (!ok) {
        std::fprintf(stderr, "line %d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(x) check(bool(x), #x, __LINE__)

template <typename T> T handle(uint64_t n) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(static_cast<uintptr_t>(n));
    else return static_cast<T>(n);
}
template <typename T> uint64_t number(T h) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(h);
    else return h;
}

uint64_t nextId = 100;
int operation = 0, failAt = 0, submits = 0;
std::set<uint64_t> live;
std::vector<std::string> order;
VkImage lastDestroyedImage{};
const VkAllocationCallbacks* lastImageAllocator = nullptr;
HeartbeatConstants lastHeartbeat{};
unsigned uniformReads = 0, recordings = 0, pipelineCreates = 0, descriptorUpdates = 0, flushes = 0;
OverlayFrameData lastUniform{};
bool coherentMemory = true;
VkResult flushResult = VK_SUCCESS;
std::unordered_map<VkDeviceMemory, std::vector<unsigned char>> allocations;
std::set<VkDeviceMemory> mappedAllocations;
std::unordered_map<VkBuffer, VkDeviceMemory> bufferMemory;
std::unordered_map<VkDescriptorSet, VkBuffer> descriptorBuffers;
std::unordered_map<VkCommandBuffer, VkDescriptorSet> commandDescriptors;
bool sampledFormatSupported = true, sampledCombinationSupported = true, rejectSampledCreate = false;
std::vector<VkImageUsageFlags> imageCreateUsages;
const void* lastCreateNext = nullptr;
const void* lastQueryNext = nullptr;
VkImageView lastDescriptorView{};
VkImage lastReadDepth{};
VkExtent2D lastViewport{};
VkImageAspectFlags expectedDepthAspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
int queueIdleCalls = 0, deviceIdleCalls = 0, driverPresents = 0;
uint32_t acquireIndex = 0, fakeImageCount = 3;
VkResult acquireResult = VK_SUCCESS, submitResult = VK_SUCCESS, presentResult = VK_SUCCESS;
std::unordered_map<VkSemaphore, int> semaphoreState; // 0 free, 1 signaled, 2 presentation owns it
std::unordered_map<VkFence, VkSemaphore> presentFences;
std::unordered_map<VkFence, VkCommandBuffer> submitFences;
std::set<VkCommandBuffer> pendingCommands;
std::vector<VkSemaphore> capturedSubmitWaits, capturedPresentWaits;
VkSemaphore capturedSignal{};
VkFence capturedSubmitFence{};
const VkPresentInfoKHR* capturedPresentPointer = nullptr;
const void* capturedOriginalNext = nullptr;
VkResult* capturedResults = nullptr;
const VkSwapchainKHR* capturedSwapchains = nullptr;
const uint32_t* capturedIndices = nullptr;
float expectedAspect = 1920.0f / 1080.0f;


bool fail() { return ++operation == failAt; }
template <typename Info, typename Handle>
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice, const Info*, const VkAllocationCallbacks*, Handle* out) {
    if (fail()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = handle<Handle>(++nextId);
    CHECK(live.insert(nextId).second);
    return VK_SUCCESS;
}
template <typename Handle>
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice, Handle object, const VkAllocationCallbacks*) {
    CHECK(live.erase(number(object)) == 1);
}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice device, const VkImageCreateInfo* info,
    const VkAllocationCallbacks* alloc, VkImage* out) {
    imageCreateUsages.push_back(info->usage);
    lastCreateNext = info->pNext;
    if (rejectSampledCreate && (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return create(device, info, alloc, out);
}
VKAPI_ATTR VkResult VKAPI_CALL swapchainImages(VkDevice, VkSwapchainKHR, uint32_t* count, VkImage* images) {
    if (!images) { *count = fakeImageCount; return VK_SUCCESS; }
    *count = std::min(*count, fakeImageCount);
    for (uint32_t i = 0; i < *count; ++i) images[i] = handle<VkImage>(10 + i);
    return *count == fakeImageCount ? VK_SUCCESS : VK_INCOMPLETE;
}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* alloc) {
    lastDestroyedImage = image;
    lastImageAllocator = alloc;
    destroy(device, image, alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL createRenderPass(VkDevice device, const VkRenderPassCreateInfo* info,
                                               const VkAllocationCallbacks* alloc, VkRenderPass* out) {
    CHECK(info->pAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
    CHECK(info->pAttachments[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
    return create(device, info, alloc, out);
}
VKAPI_ATTR VkResult VKAPI_CALL createPipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* info,
                                                   const VkAllocationCallbacks* alloc, VkPipelineLayout* out) {
    CHECK(info->pushConstantRangeCount == 0);
    return create(device, info, alloc, out);
}
VKAPI_ATTR VkResult VKAPI_CALL createCommandPool(VkDevice device, const VkCommandPoolCreateInfo* info,
                                               const VkAllocationCallbacks* alloc, VkCommandPool* out) {
    CHECK(info->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    return create(device, info, alloc, out);
}
VKAPI_ATTR VkResult VKAPI_CALL createPipelines(VkDevice device, VkPipelineCache, uint32_t count,
                                              const VkGraphicsPipelineCreateInfo* info,
                                              const VkAllocationCallbacks* alloc, VkPipeline* out) {
    ++pipelineCreates;
    CHECK(count == 1);
    lastViewport = {static_cast<uint32_t>(info->pViewportState->pViewports[0].width),
                    static_cast<uint32_t>(info->pViewportState->pViewports[0].height)};
    const auto& blend = info->pColorBlendState->pAttachments[0];
    // effect/menu blend over the game; the gizmo pipeline (LINE_LIST) draws
    // opaque lines instead (see overlay_pipeline.cpp) - a real, deliberate
    // difference, not a relaxed check.
    if (blend.blendEnable == VK_TRUE) {
        CHECK(blend.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        CHECK(blend.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    } else {
        CHECK(blend.blendEnable == VK_FALSE);
        CHECK(info->pInputAssemblyState->topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
    }
    return create(device, info, alloc, out);
}
VKAPI_ATTR VkResult VKAPI_CALL allocateSets(VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out) {
    if (fail()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) out[i] = handle<VkDescriptorSet>(++nextId); // owned by pool
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL allocateCommands(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
    if (fail()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i)
        out[i] = handle<VkCommandBuffer>(++nextId); // owned by pool
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL beginCommand(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info) {
    CHECK(info->flags == 0);
    ++recordings;
    CHECK(!pendingCommands.count(cmd)); // only this image's submit fence permits re-record
    order.push_back("record");
    return fail() ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL endCommand(VkCommandBuffer) {
    return fail() ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL updateSets(VkDevice, uint32_t count, const VkWriteDescriptorSet* writes,
                                     uint32_t, const VkCopyDescriptorSet*) {
    ++descriptorUpdates;
    CHECK(count == 3);
    CHECK(writes[0].dstBinding == 0 && writes[0].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    CHECK(writes[0].pImageInfo->imageLayout == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(writes[1].dstBinding == 1 && writes[1].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    CHECK(writes[1].pBufferInfo->offset == 0 && writes[1].pBufferInfo->range == sizeof(OverlayUniform));
    CHECK(writes[2].dstBinding == 2 && writes[2].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    CHECK(writes[2].pImageInfo->imageView != VkImageView{});
    lastDescriptorView = writes[0].pImageInfo->imageView;
    descriptorBuffers[writes[1].dstSet] = writes[1].pBufferInfo->buffer;
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                                   VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t,
                                   const VkBufferMemoryBarrier*, uint32_t count, const VkImageMemoryBarrier* barriers) {
    if (count == 0) return; // trailing depth-read -> next depth-write execution dependency
    CHECK(count == 1);
    const auto& b = barriers[0];
    if (b.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        CHECK(b.oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || b.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        CHECK(b.srcAccessMask & (VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT));
        CHECK(b.dstAccessMask & VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
        CHECK(b.dstAccessMask & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    } else if (b.newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        CHECK(b.oldLayout == VK_IMAGE_LAYOUT_GENERAL);
        CHECK(b.subresourceRange.aspectMask == expectedDepthAspects);
        lastReadDepth = b.image;
    }
}
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents) {}
VKAPI_ATTR void VKAPI_CALL endPass(VkCommandBuffer) {}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL bindSets(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipelineLayout,
                                  uint32_t, uint32_t count, const VkDescriptorSet* sets, uint32_t, const uint32_t*) {
    CHECK(count == 1); commandDescriptors[cmd] = sets[0];
}
VKAPI_ATTR VkResult VKAPI_CALL createDescriptorLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
    const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
    CHECK(info->bindingCount == 3);
    CHECK(info->pBindings[0].binding == 0 && info->pBindings[0].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    CHECK(info->pBindings[1].binding == 1 && info->pBindings[1].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    return create(device, info, alloc, out);
}
VKAPI_ATTR void VKAPI_CALL memoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* properties) {
    *properties = {};
    properties->memoryTypeCount = 2;
    properties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    properties->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        (coherentMemory ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
}
VKAPI_ATTR void VKAPI_CALL bufferRequirements(VkDevice, VkBuffer, VkMemoryRequirements* req) {
    *req = {8192, 256, 3};   // at least sizeof(OverlayFrameData) (4240 with the point-light gizmo fields)
}
VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks* alloc, VkDeviceMemory* out) {
    CHECK((info->memoryTypeIndex == 1 && info->allocationSize == 8192) || (info->memoryTypeIndex == 0 && info->allocationSize == 256));
    auto result = create(device, info, alloc, out);
    if (result == VK_SUCCESS) allocations[*out].resize(static_cast<size_t>(info->allocationSize));
    return result;
}
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* alloc) {
    CHECK(!mappedAllocations.count(memory));
    CHECK(allocations.erase(memory) == 1);
    destroy(device, memory, alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL bindBuffer(VkDevice, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    CHECK(offset == 0);
    if (fail()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    bufferMemory[buffer] = memory;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL mapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags, void** out) {
    CHECK(offset == 0 && size == VK_WHOLE_SIZE);
    if (fail()) return VK_ERROR_MEMORY_MAP_FAILED;
    CHECK(mappedAllocations.insert(memory).second);
    *out = allocations.at(memory).data();
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL unmapMemory(VkDevice, VkDeviceMemory memory) { CHECK(mappedAllocations.erase(memory) == 1); }
VKAPI_ATTR VkResult VKAPI_CALL flushMemory(VkDevice, uint32_t count, const VkMappedMemoryRange* ranges) {
    CHECK(count == 1 && ranges[0].offset == 0 && ranges[0].size == VK_WHOLE_SIZE);
    CHECK(mappedAllocations.count(ranges[0].memory));
    ++flushes;
    return flushResult;
}
VKAPI_ATTR void VKAPI_CALL drawIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t count, uint32_t stride) {
    CHECK(buffer == descriptorBuffers[commandDescriptors[cmd]]);
    // Three indirect draws now share this recording: the menu panel, the
    // independent Camera Telemetry HUD, and the 3D camera gizmo (see
    // overlay_renderer.cpp).
    CHECK((offset == offsetof(OverlayFrameData, menuDraw) || offset == offsetof(OverlayFrameData, hudDraw) || offset == offsetof(OverlayFrameData, gizmoDraw))
          && count == 1 && stride == sizeof(VkDrawIndirectCommand));
}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer, uint32_t vertices, uint32_t, uint32_t, uint32_t) {
    CHECK(vertices == 3);
}
VKAPI_ATTR VkResult VKAPI_CALL queueIdle(VkQueue) { ++queueIdleCalls; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL deviceIdle(VkDevice) { ++deviceIdleCalls; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL waitFences(VkDevice, uint32_t count, const VkFence* fences, VkBool32, uint64_t) {
    for (uint32_t i = 0; i < count; ++i) {
        if (submitFences.count(fences[i])) {
            pendingCommands.erase(submitFences.at(fences[i]));
            submitFences.erase(fences[i]);
            order.push_back("wait-submit");
        } else if (presentFences.count(fences[i])) {
            semaphoreState[presentFences.at(fences[i])] = 0;
            presentFences.erase(fences[i]);
            order.push_back("wait-present");
        } else CHECK(false); // never wait on an unsubmitted/unsignaled fence
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL resetFences(VkDevice, uint32_t count, const VkFence* fences) {
    for (uint32_t i = 0; i < count; ++i) CHECK(!submitFences.count(fences[i]) && !presentFences.count(fences[i]));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroySemaphore(VkDevice device, VkSemaphore sem, const VkAllocationCallbacks* alloc) {
    CHECK(semaphoreState[sem] != 2);
    semaphoreState.erase(sem);
    destroy(device, sem, alloc);
}
VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* alloc) {
    CHECK(!presentFences.count(fence) && !submitFences.count(fence));
    destroy(device, fence, alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL acquire(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore sem, VkFence, uint32_t* index) {
    if (acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR) {
        *index = acquireIndex;
        if (sem) semaphoreState[sem] = 1;
    }
    return acquireResult;
}
VKAPI_ATTR VkResult VKAPI_CALL acquire2(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* index) {
    return acquire(device, info->swapchain, info->timeout, info->semaphore, info->fence, index);
}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue, uint32_t count, const VkSubmitInfo* info, VkFence fence) {
    CHECK(count == 1);
    const bool overlay = info->commandBufferCount != 0;
    if (overlay && submitResult != VK_SUCCESS) return submitResult;
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) {
        CHECK(semaphoreState[info->pWaitSemaphores[i]] == 1);
        semaphoreState[info->pWaitSemaphores[i]] = 0;
        if (overlay) CHECK(info->pWaitDstStageMask[i] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    for (uint32_t i = 0; i < info->signalSemaphoreCount; ++i) {
        CHECK(semaphoreState[info->pSignalSemaphores[i]] == 0);
        semaphoreState[info->pSignalSemaphores[i]] = 1;
    }
    if (overlay) {
        CHECK(info->commandBufferCount == 1 && info->signalSemaphoreCount == 1 && fence);
        CHECK(!pendingCommands.count(info->pCommandBuffers[0]));
        pendingCommands.insert(info->pCommandBuffers[0]);
        submitFences[fence] = info->pCommandBuffers[0];
        capturedSubmitWaits.assign(info->pWaitSemaphores, info->pWaitSemaphores + info->waitSemaphoreCount);
        const auto buffer = descriptorBuffers.at(commandDescriptors.at(info->pCommandBuffers[0]));
        std::memcpy(&lastUniform, allocations.at(bufferMemory.at(buffer)).data(), sizeof(lastUniform));
        lastHeartbeat = lastUniform.uniform.heartbeat;
        CHECK(std::isfinite(lastHeartbeat.phase) && lastHeartbeat.phase >= 0 && lastHeartbeat.phase <= 1);
        CHECK(lastHeartbeat.strength == 1.0f);
        CHECK(std::abs(lastHeartbeat.aspectRatio - expectedAspect) < .00001f);
        ++uniformReads; // number of uniform snapshots consumed by the mock GPU
        capturedSignal = info->pSignalSemaphores[0]; capturedSubmitFence = fence;
        ++submits;
        order.push_back("submit");
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL driverSubmit2(VkQueue q, uint32_t count, const VkSubmitInfo2* infos, VkFence fence) {
    CHECK(count == 1);
    std::vector<VkSemaphore> waits, signals;
    std::vector<VkPipelineStageFlags> stages;
    for (uint32_t i = 0; i < infos->waitSemaphoreInfoCount; ++i) {
        waits.push_back(infos->pWaitSemaphoreInfos[i].semaphore);
        stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    for (uint32_t i = 0; i < infos->signalSemaphoreInfoCount; ++i) signals.push_back(infos->pSignalSemaphoreInfos[i].semaphore);
    VkSubmitInfo legacy{};
    legacy.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    legacy.waitSemaphoreCount = static_cast<uint32_t>(waits.size()); legacy.pWaitSemaphores = waits.data(); legacy.pWaitDstStageMask = stages.data();
    legacy.signalSemaphoreCount = static_cast<uint32_t>(signals.size()); legacy.pSignalSemaphores = signals.data();
    return submit(q, 1, &legacy, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL driverPresent(VkQueue, const VkPresentInfoKHR* info) {
    ++driverPresents;
    capturedPresentPointer = info;
    capturedPresentWaits.clear();
    if (info->waitSemaphoreCount) capturedPresentWaits.assign(info->pWaitSemaphores, info->pWaitSemaphores + info->waitSemaphoreCount);
    capturedResults = info->pResults; capturedSwapchains = info->pSwapchains; capturedIndices = info->pImageIndices;
    capturedOriginalNext = info->pNext;
    VkFence fence{};
    for (auto* n = static_cast<const VkBaseInStructure*>(info->pNext); n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT) {
            fence = reinterpret_cast<const VkSwapchainPresentFenceInfoEXT*>(n)->pFences[0];
            capturedOriginalNext = n->pNext;
        }
    }
    if (present_operations_enqueued(presentResult)) {
        for (auto sem : capturedPresentWaits) {
            CHECK(semaphoreState[sem] == 1);
            semaphoreState[sem] = fence ? 2 : 0;
            if (fence) presentFences[fence] = sem;
        }
    }
    if (info->pResults) for (uint32_t i = 0; i < info->swapchainCount; ++i) info->pResults[i] = presentResult;
    order.push_back("present");
    return presentResult;
}
VKAPI_ATTR VkResult VKAPI_CALL setLoaderData(VkDevice, void*) {
    return fail() ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL sentinel() {}
bool failSampler=false;
VKAPI_ATTR VkResult VKAPI_CALL createSampler(VkDevice d,const VkSamplerCreateInfo* info,const VkAllocationCallbacks* a,VkSampler* out) {
    return failSampler?VK_ERROR_OUT_OF_DEVICE_MEMORY:create(d,info,a,out);
}
unsigned queryReads=0, timestampWrites=0;
std::unordered_map<VkQueryPool,uint32_t> queryCounts;
VkResult queryCreationResult=VK_SUCCESS, queryReadResult=VK_SUCCESS;
uint32_t fakeTimestampBits=8;
VKAPI_ATTR void VKAPI_CALL timingProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* props) {
    *props={};props->limits.timestampPeriod=2;
}
VKAPI_ATTR void VKAPI_CALL timingQueues(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* props) {
    *count=1;if(props){*props={};props->queueFlags=VK_QUEUE_GRAPHICS_BIT;props->timestampValidBits=fakeTimestampBits;}
}
VKAPI_ATTR VkResult VKAPI_CALL queryPool(VkDevice d,const VkQueryPoolCreateInfo* info,const VkAllocationCallbacks* a,VkQueryPool* out) {
    CHECK(info->queryType==VK_QUERY_TYPE_TIMESTAMP && info->queryCount>0 && info->queryCount<=6 && info->queryCount%2==0);
    if(queryCreationResult!=VK_SUCCESS)return queryCreationResult;
    const auto result=create(d,info,a,out);if(result==VK_SUCCESS)queryCounts[*out]=info->queryCount;return result;
}
VKAPI_ATTR void VKAPI_CALL destroyQueryPool(VkDevice d,VkQueryPool pool,const VkAllocationCallbacks* a){CHECK(queryCounts.erase(pool)==1);destroy(d,pool,a);}
VKAPI_ATTR void VKAPI_CALL resetQueries(VkCommandBuffer,VkQueryPool pool,uint32_t first,uint32_t count){CHECK(first%2==0 && count==2 && first+count<=queryCounts.at(pool));}
VKAPI_ATTR void VKAPI_CALL timestamp(VkCommandBuffer,VkPipelineStageFlagBits stage,VkQueryPool pool,uint32_t query){
    CHECK(query<queryCounts.at(pool));
    CHECK(stage==(query%2?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));++timestampWrites;
}
VKAPI_ATTR VkResult VKAPI_CALL queryResults(VkDevice,VkQueryPool pool,uint32_t first,uint32_t count,size_t size,void* output,VkDeviceSize stride,VkQueryResultFlags flags) {
    CHECK(first%2==0 && count==2 && first+count<=queryCounts.at(pool) && size==32 && stride==16);
    CHECK(flags==(VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));++queryReads;
    auto* values=static_cast<uint64_t*>(output);values[0]=250;values[1]=1;values[2]=5;values[3]=1;
    return queryReadResult;
}
unsigned sceneCopies = 0;
VKAPI_ATTR void VKAPI_CALL copyImage(VkCommandBuffer, VkImage src, VkImageLayout sl, VkImage dst,
    VkImageLayout dl, uint32_t count, const VkImageCopy* copy) {
    CHECK(src != dst && sl == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && dl == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(count == 1 && copy->extent.width == lastViewport.width && copy->extent.height == lastViewport.height);
    ++sceneCopies;
}
VKAPI_ATTR void VKAPI_CALL imageRequirements(VkDevice, VkImage, VkMemoryRequirements* req) { *req = {256, 256, 1}; }
VKAPI_ATTR VkResult VKAPI_CALL bindImage(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
    return fail() ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice, const char* name) {
#define MAP(n, ...) if (!std::strcmp(name, "vk" #n)) return reinterpret_cast<PFN_vkVoidFunction>(__VA_ARGS__)
    MAP(CreateQueryPool, queryPool);
    MAP(DestroyQueryPool, destroyQueryPool);
    MAP(CmdResetQueryPool, resetQueries);
    MAP(CmdWriteTimestamp, timestamp);
    MAP(GetQueryPoolResults, queryResults);
    MAP(CreateImage, create<VkImageCreateInfo, VkImage>);
    MAP(DestroyImage, destroy<VkImage>);
    MAP(GetImageMemoryRequirements, imageRequirements);
    MAP(BindImageMemory, bindImage);
    MAP(CmdCopyImage, copyImage);
    MAP(CreateSampler, createSampler);
    MAP(DestroySampler, destroy<VkSampler>);
    MAP(CreateDescriptorSetLayout, createDescriptorLayout);
    MAP(DestroyDescriptorSetLayout, destroy<VkDescriptorSetLayout>);
    MAP(CreatePipelineLayout, createPipelineLayout);
    MAP(DestroyPipelineLayout, destroy<VkPipelineLayout>);
    MAP(CreateRenderPass, createRenderPass);
    MAP(DestroyRenderPass, destroy<VkRenderPass>);
    MAP(CreateShaderModule, create<VkShaderModuleCreateInfo, VkShaderModule>);
    MAP(DestroyShaderModule, destroy<VkShaderModule>);
    MAP(CreateGraphicsPipelines, createPipelines);
    MAP(DestroyPipeline, destroy<VkPipeline>);
    MAP(CreateDescriptorPool, create<VkDescriptorPoolCreateInfo, VkDescriptorPool>);
    MAP(DestroyDescriptorPool, destroy<VkDescriptorPool>);
    MAP(AllocateDescriptorSets, allocateSets);
    MAP(UpdateDescriptorSets, updateSets);
    MAP(CreateImageView, create<VkImageViewCreateInfo, VkImageView>);
    MAP(DestroyImageView, destroy<VkImageView>);
    MAP(CreateFramebuffer, create<VkFramebufferCreateInfo, VkFramebuffer>);
    MAP(DestroyFramebuffer, destroy<VkFramebuffer>);
    MAP(CreateCommandPool, createCommandPool);
    MAP(DestroyCommandPool, destroy<VkCommandPool>);
    MAP(AllocateCommandBuffers, allocateCommands);
    MAP(BeginCommandBuffer, beginCommand);
    MAP(EndCommandBuffer, endCommand);
    MAP(CmdPipelineBarrier, barrier);
    MAP(CmdBeginRenderPass, beginPass);
    MAP(CmdEndRenderPass, endPass);
    MAP(CmdBindPipeline, bindPipeline);
    MAP(CmdBindDescriptorSets, bindSets);
    MAP(CreateBuffer, create<VkBufferCreateInfo, VkBuffer>);
    MAP(DestroyBuffer, destroy<VkBuffer>);
    MAP(GetBufferMemoryRequirements, bufferRequirements);
    MAP(AllocateMemory, allocateMemory);
    MAP(FreeMemory, freeMemory);
    MAP(BindBufferMemory, bindBuffer);
    MAP(MapMemory, mapMemory);
    MAP(UnmapMemory, unmapMemory);
    MAP(FlushMappedMemoryRanges, flushMemory);
    MAP(CmdDrawIndirect, drawIndirect);
    MAP(CmdDraw, draw);
    MAP(CreateSemaphore, create<VkSemaphoreCreateInfo, VkSemaphore>);
    MAP(DestroySemaphore, destroySemaphore);
    MAP(CreateFence, create<VkFenceCreateInfo, VkFence>);
    MAP(DestroyFence, destroyFence);
    MAP(WaitForFences, waitFences);
    MAP(ResetFences, resetFences);
    MAP(AcquireNextImageKHR, acquire);
    MAP(AcquireNextImage2KHR, acquire2);
    MAP(QueuePresentKHR, driverPresent);
    MAP(QueueSubmit, submit);
    MAP(QueueSubmit2, driverSubmit2);
    MAP(QueueSubmit2KHR, driverSubmit2);
    MAP(QueueWaitIdle, queueIdle);
    MAP(DeviceWaitIdle, deviceIdle);
    MAP(TestUnknownExtension, sentinel);
#undef MAP
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance, const char* name) { return gdpa({}, name); }
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice, const VkAllocationCallbacks*) { order.push_back("destroyDevice"); }

struct Dispatchable { void* dispatch; } deviceObject{&deviceObject}, otherDeviceObject{&otherDeviceObject};
VkDevice device = reinterpret_cast<VkDevice>(&deviceObject);
VkDevice otherDevice = reinterpret_cast<VkDevice>(&otherDeviceObject);
VkQueue queue = reinterpret_cast<VkQueue>(&deviceObject);
VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(55);

VKAPI_ATTR void VKAPI_CALL formatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties* properties) {
    *properties = {};
    properties->optimalTilingFeatures = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        (sampledFormatSupported ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT : 0);
}
VKAPI_ATTR VkResult VKAPI_CALL imageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
    VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties* properties) {
    if (!sampledCombinationSupported) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *properties = {};
    properties->maxExtent = {16384, 16384, 1};
    properties->maxMipLevels = 1; properties->maxArrayLayers = 1;
    properties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL imageFormatProperties2(VkPhysicalDevice physical,
    const VkPhysicalDeviceImageFormatInfo2* info, VkImageFormatProperties2* properties) {
    lastQueryNext = info->pNext;
    CHECK(info->usage & VK_IMAGE_USAGE_SAMPLED_BIT);
    return imageFormatProperties(physical, info->format, info->type, info->tiling,
                                 info->usage, info->flags, &properties->imageFormatProperties);
}
DeviceData deviceData() {
    DeviceData data{};
    data.device = device;
    data.sync = std::make_shared<SyncDeviceState>();
    data.sync->maintenance = true;
    data.sync->singleGraphicsQueue = true;
    data.sync->queues[queue] = {0, 0, true};
    data.next_queue_present = driverPresent;
    data.physical_device = reinterpret_cast<VkPhysicalDevice>(&deviceObject);
    data.get_properties=timingProperties;data.get_queue_properties=timingQueues;
    data.get_format_properties = formatProperties;
    data.get_memory_properties = memoryProperties;
    data.get_image_format_properties = imageFormatProperties;
    data.next_gdpa = gdpa;
    data.set_device_loader_data = setLoaderData;
    data.next_create_image = createImage;
    data.next_create_swapchain = create<VkSwapchainCreateInfoKHR, VkSwapchainKHR>;
    data.next_destroy_swapchain = destroy<VkSwapchainKHR>;
    data.next_get_swapchain_images = swapchainImages;
    data.next_create_image_view = create<VkImageViewCreateInfo, VkImageView>;
    data.next_destroy_image = destroyImage;
    data.next_destroy_image_view = destroy<VkImageView>;
    data.next_destroy_device = destroyDevice;
    return data;
}
void seedSwapchain() {
    SwapchainInfo sc{};
    sc.overlaySyncSupported = true;
    sc.width = 1920; sc.height = 1080; sc.format = VK_FORMAT_B8G8R8A8_UNORM;
    sc.images = {handle<VkImage>(10), handle<VkImage>(11), handle<VkImage>(12)};
    g_swapchains[{device, 55}] = sc;
}
VkImage makeDepth(uint32_t width = 1920, uint32_t height = 1080, bool ready = true,
                  VkFormat format = VK_FORMAT_D24_UNORM_S8_UINT,
                  VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                  const void* next = nullptr) {
    VkImageCreateInfo info{};
    info.imageType = VK_IMAGE_TYPE_2D; info.mipLevels = 1; info.arrayLayers = 1;
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; info.pNext = next;
    info.extent = {width, height, 1}; info.format = format;
    info.samples = VK_SAMPLE_COUNT_1_BIT; info.usage = usage;
    VkImage image{};
    CHECK(vkCreateImage(device, &info, nullptr, &image) == VK_SUCCESS);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image; vi.format = info.format; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange = {depth_barrier_aspects(format), 0, 1, 0, 1};
    VkImageView appView{};
    CHECK(vkCreateImageView(device, &vi, nullptr, &appView) == VK_SUCCESS);
    destroy(device, appView, nullptr); // application's view; layer's view stays alive
    CHECK((g_depth_candidates.at({device, number(image)}).depthView != VkImageView{}) == ready);
    return image;
}
void prepareFrame(uint32_t imageIndex, const std::vector<VkSemaphore>& waits) {
    acquireIndex = imageIndex;
    uint32_t acquiredIndex = UINT32_MAX;
    const VkSemaphore available = handle<VkSemaphore>(0xffffffff00000070ull);
    CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, available, {}, &acquiredIndex) == VK_SUCCESS);
    CHECK(acquiredIndex == imageIndex);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo render{};
    render.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    render.waitSemaphoreCount = 1; render.pWaitSemaphores = &available; render.pWaitDstStageMask = &stage;
    render.signalSemaphoreCount = static_cast<uint32_t>(waits.size()); render.pSignalSemaphores = waits.data();
    CHECK(vkQueueSubmit(queue, 1, &render, {}) == VK_SUCCESS);
}
bool present(const DeviceData& data, uint32_t index = 0) {
    const VkSemaphore ready = handle<VkSemaphore>(0xffffffff00000080ull);
    prepareFrame(index, {ready});
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.swapchainCount = 1; info.pSwapchains = &swapchain; info.pImageIndices = &index;
    info.waitSemaphoreCount = 1; info.pWaitSemaphores = &ready;
    const auto result = depth_overlay_present(data, queue, &info, 1);
    if (!result.handled) {
        auto r = driverPresent(queue, &info);
        consume_present_waits(data, &info, r);
    }
    return result.drawn;
}
VkSwapchainKHR makeSwapchain(uint32_t width, uint32_t height, VkSwapchainKHR old = {}) {
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.imageExtent = {width, height}; info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (const char* flag=std::getenv("DEPTH_PROBE_SCENE_COLOR");flag && !std::strcmp(flag,"1"))info.imageUsage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.minImageCount = 3; info.imageArrayLayers = 1; info.oldSwapchain = old;
    VkSwapchainKHR result{};
    CHECK(vkCreateSwapchainKHR(device, &info, nullptr, &result) == VK_SUCCESS);
    uint32_t count = 3;
    VkImage images[3]{};
    CHECK(vkGetSwapchainImagesKHR(device, result, &count, images) == VK_SUCCESS);
    swapchain = result;
    expectedAspect = static_cast<float>(width) / height;
    return result;
}
DepthCandidate candidate(VkImage image) { return g_depth_candidates.at({device, number(image)}); }
std::string logText() {
    std::ifstream file(log_path());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void sceneDepthScenarios(const DeviceData& data) {
    CHECK(live.empty());
    g_depth_candidates.clear(); g_swapchains.clear();
    invalidate_scene_depth_device(device); invalidate_scene_depth_device(otherDevice);

    // 1/2: scene and shadow are created before a swapchain exists.
    const auto scene = makeDepth();
    const auto shadow = makeDepth(2048, 2048);
    const auto oldSc = makeSwapchain(1920, 1080);
    CHECK(resolve_scene_depth(device, oldSc)->candidate.image == scene);
    CHECK(present(data));
    CHECK(lastReadDepth == scene && lastDescriptorView == candidate(scene).depthView);

    // 5: two exact matches, independent of hash insertion order; one ambiguity log.
    const auto second = makeDepth();
    const auto offset = logText().size();
    auto choice = resolve_scene_depth(device, oldSc);
    CHECK(choice && choice->candidate.image == scene && choice->matchingCandidates == 2);
    CHECK(logText().substr(offset).find("[SCENE-DEPTH-CANDIDATES]") != std::string::npos);
    const auto after = logText().size();
    for (unsigned i = 0; i < 30; ++i) CHECK(resolve_scene_depth(device, oldSc)->candidate.image == scene);
    CHECK(logText().size() == after);
    DeviceHandleMap<DepthCandidate> reversed;
    reversed[{device, number(second)}] = candidate(second);
    reversed[{device, number(scene)}] = candidate(scene);
    CHECK(select_scene_depth_candidate(reversed, device, {1920, 1080})->candidate.image == scene);
    reversed.rehash(97);
    CHECK(select_scene_depth_candidate(reversed, device, {1920, 1080})->candidate.image == scene);
    vkDestroyImage(device, second, nullptr);

    // 4: retire old swapchain, keep old scene alive, and select a new extent.
    auto resizeSettings = overlay_settings(); resizeSettings.aoRadius = 23.5f;
    set_overlay_settings(resizeSettings);
    auto resizedDepth = makeDepth(2560, 1440);
    const auto oldGeneration = candidate(resizedDepth).generation;
    const auto resizedSc = makeSwapchain(2560, 1440, oldSc);
    CHECK(!resolve_scene_depth(device, oldSc));
    CHECK(g_swapchains.at({device, number(oldSc)}).retired);
    CHECK(is_target_depth(device, scene));
    const auto withoutRenderer = live;
    CHECK(resolve_scene_depth(device, resizedSc)->candidate.image == resizedDepth);
    CHECK(present(data));
    CHECK(lastReadDepth == resizedDepth && lastDescriptorView == candidate(resizedDepth).depthView);
    CHECK(lastViewport.width == 2560 && lastViewport.height == 1440);
    CHECK(lastUniform.uniform.settings == resizeSettings && overlay_settings() == resizeSettings);
    reset_overlay_settings();

    // 6: destroying selected depth retires descriptors/renderer; old extent can't win.
    const auto deadView = candidate(resizedDepth).depthView;
    vkDestroyImage(device, resizedDepth, nullptr);
    auto expected = withoutRenderer;
    expected.erase(number(resizedDepth)); expected.erase(number(deadView));
    CHECK(live == expected);
    const int submitted = submits;
    CHECK(!resolve_scene_depth(device, resizedSc));
    CHECK(!present(data) && submits == submitted);
    CHECK(is_target_depth(device, scene));

    // Reuse numeric handle; a new generation must rebuild its descriptor.
    const auto highId = nextId;
    nextId = number(resizedDepth) - 1;
    CHECK(makeDepth(2560, 1440) == resizedDepth);
    nextId = std::max(highId, nextId);
    CHECK(candidate(resizedDepth).generation > oldGeneration);
    CHECK(present(data));
    CHECK(lastDescriptorView == candidate(resizedDepth).depthView);
    vkDestroySwapchainKHR(device, oldSc, nullptr);
    vkDestroySwapchainKHR(device, resizedSc, nullptr);
    vkDestroyImage(device, scene, nullptr);
    vkDestroyImage(device, shadow, nullptr);
    vkDestroyImage(device, resizedDepth, nullptr);
    CHECK(live.empty());

    // 3/10: swapchain first; a ready shadow alone is never a fallback.
    const auto smallSc = makeSwapchain(1280, 720);
    CHECK(!resolve_scene_depth(device, smallSc));
    const auto noMatchLogSize = logText().size();
    for (unsigned i = 0; i < 30; ++i) CHECK(!resolve_scene_depth(device, smallSc));
    CHECK(logText().size() == noMatchLogSize);
    const auto shadowOnly = makeDepth(2048, 2048);
    CHECK(!resolve_scene_depth(device, smallSc));
    CHECK(!select_scene_depth_candidate(g_depth_candidates, device, {1920, 1080}));
    const auto smallDepth = makeDepth(1280, 720);
    CHECK(resolve_scene_depth(device, smallSc)->candidate.image == smallDepth);
    CHECK(present(data) && lastReadDepth == smallDepth);

    // 7: identical numeric handles on another device remain isolated, even with
    // an older generation that would otherwise beat the current selection.
    auto foreign = candidate(smallDepth);
    foreign.device = otherDevice; foreign.generation = 0;
    foreign.depthView = handle<VkImageView>(999999);
    g_depth_candidates[{otherDevice, number(smallDepth)}] = foreign;
    CHECK(resolve_scene_depth(device, smallSc)->candidate.depthView != foreign.depthView);
    CHECK(select_scene_depth_candidate(g_depth_candidates, otherDevice, {1280, 720})->candidate.depthView == foreign.depthView);
    vkDestroyImage(device, smallDepth, nullptr);
    CHECK(g_depth_candidates.count({otherDevice, number(smallDepth)}) == 1);
    CHECK(!resolve_scene_depth(device, smallSc));
    g_depth_candidates.erase({otherDevice, number(smallDepth)});
    vkDestroyImage(device, shadowOnly, nullptr);
    vkDestroySwapchainKHR(device, smallSc, nullptr);
    CHECK(live.empty());

    // 8: sampled feature missing -> unmodified create, tracked but unselectable.
    sampledFormatSupported = false;
    imageCreateUsages.clear();
    auto unsupported = makeDepth(1920, 1080, false);
    CHECK(imageCreateUsages.size() == 1 && !(imageCreateUsages[0] & VK_IMAGE_USAGE_SAMPLED_BIT));
    CHECK(!candidate(unsupported).sampledSupported && !candidate(unsupported).sampledReady);
    vkDestroyImage(device, unsupported, nullptr);
    sampledFormatSupported = true;
    // A supported format alone is insufficient: usage combination must succeed.
    sampledCombinationSupported = false;
    unsupported = makeDepth(1920, 1080, false);
    CHECK(!candidate(unsupported).sampledSupported);
    vkDestroyImage(device, unsupported, nullptr);
    sampledCombinationSupported = true;

    // 9: driver rejects modified create -> exact original usage is retried.
    rejectSampledCreate = true;
    imageCreateUsages.clear();
    const auto fallback = makeDepth(1920, 1080, false);
    CHECK(imageCreateUsages.size() == 2);
    CHECK(imageCreateUsages[0] & VK_IMAGE_USAGE_SAMPLED_BIT);
    CHECK(imageCreateUsages[1] == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    CHECK(!candidate(fallback).sampledPatched && !candidate(fallback).sampledReady);
    CHECK(!select_scene_depth_candidate(g_depth_candidates, device, {1920, 1080}));
    rejectSampledCreate = false;
    vkDestroyImage(device, fallback, nullptr);

    // Already sampled is ready without a patch, including a depth-only format.
    const auto native = makeDepth(1280, 720, true, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    CHECK(candidate(native).sampledReady && !candidate(native).sampledPatched);
    const auto nativeSc = makeSwapchain(1280, 720);
    expectedDepthAspects = VK_IMAGE_ASPECT_DEPTH_BIT;
    CHECK(present(data) && lastReadDepth == native);
    vkDestroySwapchainKHR(device, nativeSc, nullptr);
    vkDestroyImage(device, native, nullptr);
    expectedDepthAspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    // The next equal-extent candidate takes over when the winning generation dies.
    const auto winner = makeDepth();
    const auto survivor = makeDepth();
    const auto tiedSc = makeSwapchain(1920, 1080);
    CHECK(present(data) && lastReadDepth == winner);
    vkDestroyImage(device, winner, nullptr);
    CHECK(resolve_scene_depth(device, tiedSc)->candidate.image == survivor);
    CHECK(present(data) && lastReadDepth == survivor);
    CHECK(lastDescriptorView == candidate(survivor).depthView);

    // A failed replacement still retires oldSwapchain and invalidates its renderer.
    VkSwapchainCreateInfoKHR replacement{};
    replacement.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    replacement.oldSwapchain = tiedSc; replacement.imageExtent = {2560, 1440};
    VkSwapchainKHR failedSc{};
    operation = 0; failAt = 1;
    CHECK(vkCreateSwapchainKHR(device, &replacement, nullptr, &failedSc) != VK_SUCCESS);
    failAt = 0;
    CHECK(!resolve_scene_depth(device, tiedSc));
    const auto submitCount = submits;
    CHECK(!present(data) && submits == submitCount);
    vkDestroySwapchainKHR(device, tiedSc, nullptr);
    vkDestroyImage(device, survivor, nullptr);
    CHECK(live.empty());

    // Known pNext structures are queried intact; no dangling caller pointers stored.
    auto query2Data = data;
    query2Data.get_image_format_properties2 = imageFormatProperties2;
    g_devices[dispatch_key(device)] = query2Data;
    VkFormat viewFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    VkImageStencilUsageCreateInfo stencil{};
    stencil.sType = VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO;
    stencil.stencilUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImageFormatListCreateInfo list{};
    list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    list.pNext = &stencil; list.viewFormatCount = 1; list.pViewFormats = &viewFormat;
    const auto chained = makeDepth(1920, 1080, true, viewFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &list);
    CHECK(lastQueryNext == &list && lastCreateNext == &list);
    vkDestroyImage(device, chained, nullptr);
    g_devices[dispatch_key(device)] = data;

    // Shape/usage/flag policy never alters risky creations or tracks them as ready.
    VkImageCreateInfo base{};
    base.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    base.imageType = VK_IMAGE_TYPE_2D; base.extent = {1920, 1080, 1};
    base.format = VK_FORMAT_D24_UNORM_S8_UINT; base.samples = VK_SAMPLE_COUNT_1_BIT;
    base.mipLevels = 1; base.arrayLayers = 1; base.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    std::vector<VkImageCreateInfo> excluded;
    for (auto flag : {VK_IMAGE_CREATE_SPARSE_BINDING_BIT, VK_IMAGE_CREATE_PROTECTED_BIT, VK_IMAGE_CREATE_ALIAS_BIT}) {
        auto info = base; info.flags = flag; excluded.push_back(info);
    }
    { auto info = base; info.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT; excluded.push_back(info); }
    { auto info = base; info.samples = VK_SAMPLE_COUNT_8_BIT; excluded.push_back(info); }
    { auto info = base; info.arrayLayers = 2; excluded.push_back(info); }
    { auto info = base; info.mipLevels = 2; excluded.push_back(info); }
    { auto info = base; info.imageType = VK_IMAGE_TYPE_3D; excluded.push_back(info); }
    { auto info = base; info.extent.depth = 2; excluded.push_back(info); }
    { auto info = base; info.tiling = VK_IMAGE_TILING_LINEAR; excluded.push_back(info); }
    { auto info = base; info.extent.width = 32; excluded.push_back(info); }
    { auto info = base; info.extent.width = 32768; excluded.push_back(info); }
    { auto info = base; info.format = VK_FORMAT_S8_UINT; excluded.push_back(info); }
    VkExternalMemoryImageCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    { auto info = base; info.pNext = &external; excluded.push_back(info); }
    for (const auto& info : excluded) {
        CHECK(!prepare_depth_candidate(data, info).eligible);
        VkImage image{};
        imageCreateUsages.clear();
        CHECK(vkCreateImage(device, &info, nullptr, &image) == VK_SUCCESS);
        CHECK(imageCreateUsages.size() == 1 && imageCreateUsages[0] == info.usage);
        CHECK(!is_target_depth(device, image));
        vkDestroyImage(device, image, nullptr);
    }

    // Missing capability entry points fail closed rather than guessing support.
    auto missing = data; missing.get_format_properties = nullptr;
    CHECK(!prepare_depth_candidate(missing, base).sampledSupported);
    missing = data; missing.get_image_format_properties = nullptr;
    CHECK(!prepare_depth_candidate(missing, base).sampledSupported);
    CHECK(live.empty() && g_depth_candidates.empty() && g_swapchains.empty());
    std::puts("PASS: all 10 scene-depth scenarios, pure-depth format, capability/pNext policy, deterministic ties and bounded logs");
}
VKAPI_ATTR VkResult VKAPI_CALL instanceExtensions(const char*, uint32_t* count, VkExtensionProperties* out) {
    if (!out) { *count = 2; return VK_SUCCESS; }
    CHECK(*count >= 2); *count = 2;
    std::strcpy(out[0].extensionName, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    std::strcpy(out[1].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL deviceExtensions(VkPhysicalDevice, const char*, uint32_t* count, VkExtensionProperties* out) {
    if (!out) { *count = 1; return VK_SUCCESS; }
    CHECK(*count >= 1); *count = 1;
    std::strcpy(out[0].extensionName, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL syncFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures2* out) {
    auto* feature = static_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(out->pNext);
    CHECK(feature->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT);
    feature->swapchainMaintenance1 = VK_TRUE;
}
VKAPI_ATTR void VKAPI_CALL queueFamilies(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* out) {
    if (!out) { *count = 2; return; }
    CHECK(*count >= 2); *count = 2;
    out[0] = {}; out[0].queueFlags = VK_QUEUE_GRAPHICS_BIT; out[0].queueCount = 2;
    out[1] = {}; out[1].queueFlags = VK_QUEUE_TRANSFER_BIT; out[1].queueCount = 1;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL capabilityGipa(VkInstance, const char* name) {
#define CAP(n, f) if (!std::strcmp(name, #n)) return reinterpret_cast<PFN_vkVoidFunction>(f)
    CAP(vkEnumerateInstanceExtensionProperties, instanceExtensions);
    CAP(vkEnumerateDeviceExtensionProperties, deviceExtensions);
    CAP(vkGetPhysicalDeviceFeatures2, syncFeatures);
    CAP(vkGetPhysicalDeviceQueueFamilyProperties, queueFamilies);
#undef CAP
    return nullptr;
}
void capabilityScenarios() {
    setenv("DEPTH_PROBE_DEPTH_PRESENT", "1", 1);
    const char* surface = VK_KHR_SURFACE_EXTENSION_NAME;
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app; ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = &surface;
    SyncInstanceCreate instance(ci, capabilityGipa);
    CHECK(instance.surfaceMaintenance && instance.info.enabledExtensionCount == 3);
    CHECK(ci.enabledExtensionCount == 1 && ci.ppEnabledExtensionNames == &surface);
    SyncInstanceCreate missingInstance(ci, gipa);
    CHECK(!missingInstance.surfaceMaintenance && missingInstance.info.enabledExtensionCount == 1);
    const char* swap = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo di{}; di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = &swap;
    VkPhysicalDeviceFeatures2 originalFeatures{}; originalFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    di.pNext = &originalFeatures;
    InstanceData id{}; id.apiVersion = app.apiVersion; id.surfaceMaintenance = true; id.next_gipa = capabilityGipa;
    SyncDeviceCreate added(di, {}, id);
    CHECK(added.maintenance && added.info.pNext == &added.feature && added.feature.pNext == &originalFeatures);
    CHECK(di.pNext == &originalFeatures && originalFeatures.pNext == nullptr && di.enabledExtensionCount == 1);
    id.surfaceMaintenance = false;
    CHECK(!SyncDeviceCreate(di, {}, id).maintenance);
    id.surfaceMaintenance = true;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT disabled{};
    disabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    di.pNext = &disabled;
    CHECK(!SyncDeviceCreate(di, {}, id).maintenance && disabled.swapchainMaintenance1 == VK_FALSE);
    const char* enabledExtensions[]{swap, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
    disabled.swapchainMaintenance1 = VK_TRUE;
    di.enabledExtensionCount = 2; di.ppEnabledExtensionNames = enabledExtensions;
    CHECK(SyncDeviceCreate(di, {}, id).maintenance);
    VkDeviceQueueCreateInfo queues[2]{};
    queues[0].queueFamilyIndex = 0; queues[0].queueCount = 1;
    queues[1].queueFamilyIndex = 1; queues[1].queueCount = 1;
    di.queueCreateInfoCount = 2; di.pQueueCreateInfos = queues;
    CHECK(single_graphics_queue(di, {}, id));
    queues[0].queueCount = 2; CHECK(!single_graphics_queue(di, {}, id));
    queues[0].queueCount = 1; queues[0].flags = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
    CHECK(!single_graphics_queue(di, {}, id));
    unsetenv("DEPTH_PROBE_DEPTH_PRESENT");
    std::puts("PASS: maintenance capability negotiation, original create-info preservation, explicit opt-out and graphics queue scope");
}
void synchronizationScenarios(const DeviceData& data) {
    CHECK(live.empty());
    g_devices[dispatch_key(device)] = data;
    data.sync->deviceLost = false;
    data.sync->signals.clear(); data.sync->acquired.clear();
    auto depth = makeDepth();
    makeSwapchain(1920, 1080);
    setenv("DEPTH_PROBE_DEPTH_PRESENT", "1", 1);
    unsetenv("DEPTH_PROBE_CLEAR_PRESENT");
    const std::vector<VkSemaphore> waits{handle<VkSemaphore>(0xffffffff00000081ull),
        handle<VkSemaphore>(0xffffffff00000082ull), handle<VkSemaphore>(0xffffffff00000083ull)};
    uint32_t index = 0;
    VkResult imageResult = VK_NOT_READY;
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.swapchainCount = 1; info.pSwapchains = &swapchain; info.pImageIndices = &index; info.pResults = &imageResult;
    info.waitSemaphoreCount = 1; info.pWaitSemaphores = waits.data();
    const auto initialIdle = deviceIdleCalls;
    std::vector<VkSemaphore> signals(3);
    // Real exported present hook: A -> overlay B -> present B, with caller memory intact.
    for (unsigned frame = 0; frame < 3000; ++frame) {
        index = frame % 3;
        info.waitSemaphoreCount = frame % 2 ? 3 : 1;
        prepareFrame(index, {waits.begin(), waits.begin() + info.waitSemaphoreCount});
        const auto before = info;
        CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
        CHECK(capturedSubmitWaits == std::vector<VkSemaphore>(waits.begin(), waits.begin() + info.waitSemaphoreCount));
        CHECK(capturedPresentWaits == std::vector<VkSemaphore>{capturedSignal});
        CHECK(info.pWaitSemaphores == before.pWaitSemaphores && info.waitSemaphoreCount == before.waitSemaphoreCount);
        CHECK(capturedSwapchains == info.pSwapchains && capturedIndices == info.pImageIndices && capturedResults == &imageResult);
        CHECK(imageResult == VK_SUCCESS);
        if (frame < 3) signals[index] = capturedSignal;
        else CHECK(signals[index] == capturedSignal);
    }
    CHECK(std::set<VkSemaphore>(signals.begin(), signals.end()).size() == 3);
    CHECK(queueIdleCalls == 0 && deviceIdleCalls == initialIdle && data.sync->overlayQueueWaitIdleCount == 0);

    info.waitSemaphoreCount = 3;
    // Allowed pNext remains intact, including application-owned present fences.
    uint64_t id = 123;
    VkPresentIdKHR presentId{};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR; presentId.swapchainCount = 1; presentId.pPresentIds = &id;
    info.pNext = &presentId;
    prepareFrame(index, waits);
    CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
    CHECK(capturedOriginalNext == &presentId && info.pNext == &presentId);
    VkFence appFence{};
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    CHECK(create(device, &fi, nullptr, &appFence) == VK_SUCCESS);
    VkSwapchainPresentFenceInfoEXT appFenceInfo{};
    appFenceInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
    appFenceInfo.swapchainCount = 1; appFenceInfo.pFences = &appFence; appFenceInfo.pNext = &presentId;
    info.pNext = &appFenceInfo;
    prepareFrame(index, waits);
    CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
    CHECK(info.pNext == &appFenceInfo && appFenceInfo.pFences == &appFence);
    CHECK(presentFences.count(appFence));
    CHECK(vkResetFences(device, 1, &appFence) == VK_SUCCESS);
    CHECK(!presentFences.count(appFence));
    prepareFrame(index, waits);
    CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
    vkDestroyFence(device, appFence, nullptr); // detaches borrowed reference before destruction
    CHECK(!presentFences.count(appFence));
    info.pNext = nullptr;

    // OOM submit leaves A untouched: safe original-present forwarding.
    for (auto failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        prepareFrame(index, waits);
        submitResult = failure;
        const int before = submits;
        CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
        CHECK(submits == before && capturedPresentPointer == &info && capturedPresentWaits == waits);
        submitResult = VK_SUCCESS;
    }
    // Post-submit present OOM must retry B, without resubmitting/consuming A twice.
    prepareFrame(index, waits);
    presentResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(vkQueuePresentKHR(queue, &info) == presentResult && imageResult == presentResult);
    const auto heldSignal = capturedSignal;
    const auto beforeRetry = submits;
    CHECK(semaphoreState[heldSignal] == 1);
    presentResult = VK_SUCCESS;
    CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
    CHECK(submits == beforeRetry && capturedPresentWaits == std::vector<VkSemaphore>{heldSignal});

    for (auto r : {VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR}) {
        prepareFrame(index, waits);
        presentResult = r;
        CHECK(vkQueuePresentKHR(queue, &info) == r && imageResult == r);
        CHECK(capturedPresentWaits.size() == 1 && capturedPresentWaits[0] == capturedSignal);
    }
    presentResult = VK_SUCCESS;

    // Unsupported calls keep the exact original present pointer, arrays and pNext.
    auto skipped = [&] {
        const auto before = submits;
        CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
        CHECK(submits == before && capturedPresentPointer == &info);
    };
    prepareFrame(index, waits);
    VkSwapchainKHR chains[]{swapchain, handle<VkSwapchainKHR>(777)};
    uint32_t indices[]{index, 0}; VkResult results[2]{};
    info.swapchainCount = 2; info.pSwapchains = chains; info.pImageIndices = indices; info.pResults = results;
    skipped(); CHECK(capturedResults == results && results[1] == VK_SUCCESS);
    info.swapchainCount = 1; info.pSwapchains = &swapchain; info.pImageIndices = &index; info.pResults = &imageResult;
    info.waitSemaphoreCount = 0; info.pWaitSemaphores = nullptr;
    prepareFrame(index, {}); skipped();
    info.waitSemaphoreCount = 3; info.pWaitSemaphores = waits.data();
    VkPresentRegionsKHR regions{}; regions.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR;
    info.pNext = &regions; prepareFrame(index, waits); skipped(); CHECK(info.pNext == &regions);
    info.pNext = nullptr;
    prepareFrame(index, waits); data.sync->maintenance = false; skipped(); data.sync->maintenance = true;
    prepareFrame(index, waits); data.sync->singleGraphicsQueue = false; skipped(); data.sync->singleGraphicsQueue = true;
    prepareFrame(index, waits); data.sync->signals[waits[0]].queue = reinterpret_cast<VkQueue>(&otherDeviceObject); skipped();
    prepareFrame(index, waits); data.sync->acquired[swapchain][index].waitedQueue = {}; skipped();

    // Recreate while submissions/presents remain outstanding: both kinds of fence must settle.
    const auto old = swapchain;
    const auto oldSignals = signals;
    fakeImageCount = 2;
    makeSwapchain(1920, 1080, old);
    CHECK(pendingCommands.empty() && presentFences.empty());
    vkDestroySwapchainKHR(device, old, nullptr);
    index = 0;
    prepareFrame(index, waits);
    CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
    CHECK(std::find(oldSignals.begin(), oldSignals.end(), capturedSignal) == oldSignals.end());
    CHECK(g_depth_overlay_states.at({device, number(swapchain)}).sync.size() == 2);
    fakeImageCount = 3;

    // No depth / partial renderer failure forwards original waits without a submit.
    vkDestroyImage(device, depth, nullptr);
    prepareFrame(index, waits); skipped();
    depth = makeDepth();
    prepareFrame(index, waits); failSampler = true; skipped(); failSampler = false;
    prepareFrame(index, waits); CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);

    // Acquire2 + submit2/KHR: only ALL_COMMANDS signal scope qualifies.
    for (bool khr : {false, true}) {
        auto available = handle<VkSemaphore>(0xffffffff00000071ull);
        VkAcquireNextImageInfoKHR ai{}; ai.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR;
        ai.swapchain = swapchain; ai.semaphore = available; ai.deviceMask = 1;
        CHECK(vkAcquireNextImage2KHR(device, &ai, &index) == VK_SUCCESS);
        VkSemaphoreSubmitInfo wait{}, signal{};
        wait.sType = signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = available; wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        signal.semaphore = waits[0]; signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submitInfo{}; submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = 1; submitInfo.pWaitSemaphoreInfos = &wait;
        submitInfo.signalSemaphoreInfoCount = 1; submitInfo.pSignalSemaphoreInfos = &signal;
        CHECK((khr ? vkQueueSubmit2KHR(queue, 1, &submitInfo, {}) : vkQueueSubmit2(queue, 1, &submitInfo, {})) == VK_SUCCESS);
        info.waitSemaphoreCount = 1;
        CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
        CHECK(capturedPresentWaits[0] == capturedSignal);
    }
    acquireResult = VK_TIMEOUT;
    const auto serial = data.sync->acquired[swapchain][index].serial;
    uint32_t untouched = 999;
    CHECK(vkAcquireNextImageKHR(device, swapchain, 0, {}, {}, &untouched) == VK_TIMEOUT);
    CHECK(untouched == 999 && data.sync->acquired[swapchain][index].serial == serial);
    acquireResult = VK_SUCCESS;

    // A narrow submit2 signal does not establish the full depth+color scope.
    {
        auto available = handle<VkSemaphore>(0xffffffff00000072ull);
        CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, available, {}, &index) == VK_SUCCESS);
        VkSemaphoreSubmitInfo wait{}, signal{};
        wait.sType = signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = available; wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        signal.semaphore = waits[0]; signal.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo2 narrow{}; narrow.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        narrow.waitSemaphoreInfoCount = 1; narrow.pWaitSemaphoreInfos = &wait;
        narrow.signalSemaphoreInfoCount = 1; narrow.pSignalSemaphoreInfos = &signal;
        CHECK(vkQueueSubmit2(queue, 1, &narrow, {}) == VK_SUCCESS);
        skipped();
    }
    // Cleanup after a post-submit present OOM must not restore consumed A waits.
    prepareFrame(index, {waits[0]});
    presentResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    CHECK(vkQueuePresentKHR(queue, &info) == presentResult);
    vkDestroyImage(device, depth, nullptr);
    presentResult = VK_SUCCESS;
    const int priorCalls = driverPresents;
    CHECK(vkQueuePresentKHR(queue, &info) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(driverPresents == priorCalls);
    auto failedSwapchain = swapchain;
    makeSwapchain(1920, 1080, failedSwapchain);
    vkDestroySwapchainKHR(device, failedSwapchain, nullptr);
    CHECK(!data.sync->abandonedPresents.count(failedSwapchain));
    depth = makeDepth();

    // Same numeric application semaphore/swapchain on another device is isolated.
    auto foreign = data;
    foreign.device = otherDevice; foreign.sync = std::make_shared<SyncDeviceState>();
    foreign.sync->signals[waits[0]] = {reinterpret_cast<VkQueue>(&otherDeviceObject), 77};
    foreign.sync->abandonedPresents[swapchain][0] = VK_ERROR_OUT_OF_HOST_MEMORY;
    g_devices[dispatch_key(otherDevice)] = foreign;
    prepareFrame(index, {waits[0]});
    CHECK(vkQueuePresentKHR(queue, &info) == VK_SUCCESS);
    CHECK(foreign.sync->signals.at(waits[0]).serial == 77);
    CHECK(foreign.sync->abandonedPresents.at(swapchain).at(0) == VK_ERROR_OUT_OF_HOST_MEMORY);
    g_devices.erase(dispatch_key(otherDevice));

    // Fatal submit never calls original present with potentially consumed waits.
    prepareFrame(index, {waits[0]});
    const auto previousPresents = driverPresents;
    submitResult = VK_ERROR_DEVICE_LOST;
    CHECK(vkQueuePresentKHR(queue, &info) == VK_ERROR_DEVICE_LOST);
    CHECK(driverPresents == previousPresents && data.sync->deviceLost);
    submitResult = VK_SUCCESS;
    data.sync->deviceLost = false; // mock recovery only; real device cannot recover
    driverPresent(queue, &info); // discard fake app signal; no overlay submission happened
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyImage(device, depth, nullptr);
    // Present device loss follows a successful submit: never call present twice.
    depth = makeDepth(); makeSwapchain(1920, 1080);
    prepareFrame(index, {waits[0]});
    presentResult = VK_ERROR_DEVICE_LOST;
    const int priorLostCalls = driverPresents;
    CHECK(vkQueuePresentKHR(queue, &info) == VK_ERROR_DEVICE_LOST);
    CHECK(driverPresents == priorLostCalls + 1 && capturedPresentWaits[0] == capturedSignal);
    CHECK(data.sync->deviceLost);
    // Fake driver models the lost device's discarded work for destruction checks.
    submitFences.clear(); presentFences.clear(); pendingCommands.clear();
    for (auto& [sem, state] : semaphoreState) { (void)sem; state = 0; }
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyImage(device, depth, nullptr);
    data.sync->deviceLost = false; presentResult = VK_SUCCESS;
    CHECK(live.empty() && pendingCommands.empty() && presentFences.empty());
    CHECK(queueIdleCalls == 0 && data.sync->overlayQueueWaitIdleCount == 0);
    for (const char* name : {"vkAcquireNextImageKHR", "vkAcquireNextImage2KHR", "vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR", "vkResetFences", "vkDestroyFence", "vkDestroySemaphore"}) {
        CHECK(vkGetDeviceProcAddr(device, name) == sync_hook(name));
        CHECK(vkGetInstanceProcAddr(reinterpret_cast<VkInstance>(&deviceObject), name) == sync_hook(name));
    }
    unsetenv("DEPTH_PROBE_DEPTH_PRESENT");
    std::puts("PASS: semaphore transfer, 3-image reuse, borrowed fences, OOM retries, present errors, acquire/submit hooks, skips, recreate and zero queue-idle");
}
unsigned probeCalls = 0;
VKAPI_ATTR void VKAPI_CALL probeViewport(VkCommandBuffer, uint32_t first, uint32_t count, const VkViewport* v) {
    CHECK(first == 0 && count == 1 && v[0].minDepth == 0 && v[0].maxDepth == 1); ++probeCalls;
}
VKAPI_ATTR void VKAPI_CALL probeClear(VkCommandBuffer, VkImage image, VkImageLayout layout,
    const VkClearDepthStencilValue* value, uint32_t count, const VkImageSubresourceRange*) {
    CHECK(image == handle<VkImage>(444) && layout == VK_IMAGE_LAYOUT_GENERAL && value->depth == 1 && count == 1); ++probeCalls;
}
VKAPI_ATTR void VKAPI_CALL probeBeginPass(VkCommandBuffer, const VkRenderPassBeginInfo* info, VkSubpassContents) {
    CHECK(info->clearValueCount == 2 && info->pClearValues[1].depthStencil.depth == .25f); ++probeCalls;
}
unsigned probeDrawCalls=0;
VKAPI_ATTR void VKAPI_CALL probeDraw(VkCommandBuffer,uint32_t vertices,uint32_t instances,uint32_t first,uint32_t firstInstance) {
    CHECK(vertices==42 && instances==2 && first==7 && firstInstance==3);++probeDrawCalls;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL probeDriver(VkDevice d, const char* name) {
    if (!std::strcmp(name, "vkCmdDraw")) return reinterpret_cast<PFN_vkVoidFunction>(probeDraw);
    if (!std::strcmp(name, "vkCmdSetViewport")) return reinterpret_cast<PFN_vkVoidFunction>(probeViewport);
    if (!std::strcmp(name, "vkCmdClearDepthStencilImage")) return reinterpret_cast<PFN_vkVoidFunction>(probeClear);
    if (!std::strcmp(name, "vkCmdBeginRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(probeBeginPass);
    if (!std::strcmp(name, "vkCreateRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(create<VkRenderPassCreateInfo, VkRenderPass>);
    return gdpa(d, name);
}
void testProbe() {
    setenv("DEPTH_PROBE_DEPTH_DIAGNOSTICS", "1", 1);
    setenv("DEPTH_PROBE_DRAW_PROBE", "1", 1);
    setenv("XDG_CONFIG_HOME",("/tmp/depth-probe-probe-config-"+std::to_string(getpid())).c_str(),1);
    setenv("DEPTH_PROBE_LAYER_LOG", "/tmp/depth-probe-probe-test.log", 1);
    std::remove("/tmp/depth-probe-probe-test.log");
    auto data = deviceData(); data.next_gdpa = probeDriver; g_devices[dispatch_key(device)] = data;
    const auto cmd = reinterpret_cast<VkCommandBuffer>(&deviceObject);
    auto viewportHook = reinterpret_cast<PFN_vkCmdSetViewport>(probe_hook("vkCmdSetViewport")); CHECK(viewportHook);
    VkViewport vp{0,0,1920,1080,0,1}; viewportHook(cmd,0,1,&vp);
    auto clearHook = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(probe_hook("vkCmdClearDepthStencilImage")); CHECK(clearHook);
    VkClearDepthStencilValue value{1,0}; VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    clearHook(cmd,handle<VkImage>(444),VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
    VkAttachmentDescription attachments[2]{};
    attachments[0].format=VK_FORMAT_R8G8B8A8_UNORM; attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].format=VK_FORMAT_D24_UNORM_S8_UINT; attachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkRenderPassCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount=2;info.pAttachments=attachments;VkRenderPass pass{};
    CHECK(reinterpret_cast<PFN_vkCreateRenderPass>(probe_hook("vkCreateRenderPass"))(device,&info,nullptr,&pass)==VK_SUCCESS);
    VkClearValue values[2]{}; values[0].color={{.7f,0,0,1}}; values[1].depthStencil={.25f,0};
    VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass=pass;begin.clearValueCount=2;begin.pClearValues=values;
    reinterpret_cast<PFN_vkCmdBeginRenderPass>(probe_hook("vkCmdBeginRenderPass"))(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
    for (int i=0;i<300;++i) viewportHook(cmd,0,1,&vp);
    CHECK(probeCalls==303);
    reinterpret_cast<PFN_vkDestroyRenderPass>(probe_hook("vkDestroyRenderPass"))(device,pass,nullptr);
    const uint32_t code[]{0x07230203,0x00010000,0,1,0};
    VkShaderModuleCreateInfo shaderInfo{};shaderInfo.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;shaderInfo.codeSize=sizeof(code);shaderInfo.pCode=code;
    VkShaderModule module{};
    CHECK(reinterpret_cast<PFN_vkCreateShaderModule>(probe_hook("vkCreateShaderModule"))(device,&shaderInfo,nullptr,&module)==VK_SUCCESS);
    VkPipelineShaderStageCreateInfo stages[2]{};
    for(auto& stage:stages){stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.module=module;stage.pName="main";}
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkPipelineViewportStateCreateInfo vpState{};vpState.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vpState.viewportCount=1;vpState.pViewports=&vp;
    VkPipelineColorBlendAttachmentState blend{};blend.blendEnable=VK_TRUE;blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkPipelineColorBlendStateCreateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb.attachmentCount=1;cb.pAttachments=&blend;
    VkPipelineDepthStencilStateCreateInfo depthState{};depthState.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;depthState.depthTestEnable=1;depthState.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    VkGraphicsPipelineCreateInfo pipelineInfo{};pipelineInfo.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;pipelineInfo.stageCount=2;pipelineInfo.pStages=stages;
    pipelineInfo.pViewportState=&vpState;pipelineInfo.pColorBlendState=&cb;pipelineInfo.pDepthStencilState=&depthState;VkPipeline pipeline{};
    CHECK(reinterpret_cast<PFN_vkCreateGraphicsPipelines>(probe_hook("vkCreateGraphicsPipelines"))(device,{},1,&pipelineInfo,nullptr,&pipeline)==VK_SUCCESS);
    reinterpret_cast<PFN_vkDestroyShaderModule>(probe_hook("vkDestroyShaderModule"))(device,module,nullptr);
    VkCommandBufferBeginInfo commandBegin{};commandBegin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CHECK(reinterpret_cast<PFN_vkBeginCommandBuffer>(probe_hook("vkBeginCommandBuffer"))(cmd,&commandBegin)==VK_SUCCESS);
    reinterpret_cast<PFN_vkCmdBindPipeline>(probe_hook("vkCmdBindPipeline"))(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    auto drawHook=reinterpret_cast<PFN_vkCmdDraw>(probe_hook("vkCmdDraw"));CHECK(drawHook);
    auto capture=overlay_settings();capture.drawProbeCapture=1;set_overlay_settings(capture);
    for(int i=0;i<300;++i)drawHook(cmd,42,2,7,3);
    capture.drawProbeCapture=0;set_overlay_settings(capture);drawHook(cmd,42,2,7,3);
    capture.drawProbeCapture=1;set_overlay_settings(capture);drawHook(cmd,42,2,7,3);
    CHECK(probeDrawCalls==302);
    reinterpret_cast<PFN_vkDestroyPipeline>(probe_hook("vkDestroyPipeline"))(device,pipeline,nullptr);
    destroy_device_probe(device);g_devices.clear();CHECK(live.empty());
    std::ifstream log("/tmp/depth-probe-probe-test.log");std::string text((std::istreambuf_iterator<char>(log)),{});
    CHECK(text.find("clearDepth=0.250000")!=std::string::npos && text.find("clearDepth=0.700000")==std::string::npos);
    CHECK(text.find("log budget exhausted")!=std::string::npos && text.size()<250000);
    CHECK(text.find("pipelineHash=")!=std::string::npos && text.find("shaderKnown=1")!=std::string::npos);
    CHECK(text.find("water=unknown")!=std::string::npos && text.find("capture-budget-exhausted")!=std::string::npos);
    const auto firstCapture=text.find("capture-start");CHECK(firstCapture!=std::string::npos && text.find("capture-start",firstCapture+1)!=std::string::npos);
    std::puts("PASS: shader hashing, pipeline snapshot after module destruction, draw forwarding, capture budget/rearm and ownership");
    std::puts("PASS: depth probe forwards unchanged, interprets depth attachment only, bounded logs and cleanup");
}
} // namespace

int main(int argc, char**) {
    if (argc > 1) { testProbe(); return 0; }
    setenv("DEPTH_PROBE_LAYER_LOG", "/tmp/depth-probe-lifecycle-test.log", 1);
    setenv("XDG_CONFIG_HOME", ("/tmp/depth-probe-lifecycle-config-" + std::to_string(getpid())).c_str(), 1);
    unsetenv("DEPTH_PROBE_HEARTBEAT");
    unsetenv("DEPTH_PROBE_HEARTBEAT_BPM");
    unsetenv("DEPTH_PROBE_HEARTBEAT_STRENGTH");
    auto config = read_heartbeat_config();
    CHECK(config.enabled && config.bpm == 72 && config.strength == 1);
    // Tempo is measured in real seconds and preserves precision after long sessions.
    CHECK(std::abs(heartbeat_at(config, 0.25, 1920, 1080).phase - 0.30f) < 0.00001f);
    CHECK(std::abs(heartbeat_at(config, 86400.0 * 30 + 0.25, 1920, 1080).phase - 0.30f) < 0.00001f);
    setenv("DEPTH_PROBE_HEARTBEAT", "0", 1);
    CHECK(heartbeat_at(read_heartbeat_config(), 1.0, 1920, 1080).strength == 0);
    setenv("DEPTH_PROBE_HEARTBEAT_BPM", "nan", 1);
    CHECK(read_heartbeat_config().bpm == 72);
    setenv("DEPTH_PROBE_HEARTBEAT_BPM", "garbage", 1);
    CHECK(read_heartbeat_config().bpm == 72);
    setenv("DEPTH_PROBE_HEARTBEAT_BPM", "10000", 1);
    setenv("DEPTH_PROBE_HEARTBEAT_STRENGTH", "-1", 1);
    CHECK(read_heartbeat_config().bpm == 240 && read_heartbeat_config().strength == 0);
    unsetenv("DEPTH_PROBE_HEARTBEAT");
    unsetenv("DEPTH_PROBE_HEARTBEAT_BPM");
    unsetenv("DEPTH_PROBE_HEARTBEAT_STRENGTH");
    // Older-loader fallback must initialize the dispatch word as documented.
    Dispatchable commandObject{};
    auto fallback = deviceData();
    fallback.set_device_loader_data = nullptr;
    CHECK(initialize_layer_command_buffer(fallback, reinterpret_cast<VkCommandBuffer>(&commandObject)));
    CHECK(commandObject.dispatch == deviceObject.dispatch);
    const auto data = deviceData();
    g_devices[dispatch_key(device)] = data;
    seedSwapchain();
    auto image = makeDepth();
    const auto baseline = live;

    // A first present defines construction + recording failure points.
    operation = 0;
    CHECK(present(data));
    const int failurePoints = operation;
    CHECK(failurePoints > 20);
    CHECK((order == std::vector<std::string>{"record", "submit", "present"}));
    CHECK(queueIdleCalls == 0);
    const auto resident = live;
    // Repeated present must upload constants again without allocating resources.
    const auto oldUniformReads = uniformReads;
    const auto oldRecordings = recordings;
    const auto oldPipelines = pipelineCreates;
    const auto oldDescriptors = descriptorUpdates;
    auto settings = overlay_settings(); settings.aoStrength = 2.25f;
    set_overlay_settings(settings);
    CHECK(present(data));
    CHECK(uniformReads == oldUniformReads + 1 && live == resident);
    CHECK(lastUniform.uniform.settings.aoStrength == 2.25f);
    CHECK(recordings == oldRecordings && pipelineCreates == oldPipelines && descriptorUpdates == oldDescriptors);
    CHECK(lastUniform.menuDraw.vertexCount == 0);
    overlay_menu().visible = true;
    CHECK(present(data));
    CHECK(lastUniform.menuDraw.vertexCount == 6 && lastUniform.uniform.menu[0] == 1);
    overlay_menu().visible = false;
    CHECK(present(data));
    CHECK(lastUniform.menuDraw.vertexCount == 0);
    CHECK(recordings == oldRecordings && pipelineCreates == oldPipelines && descriptorUpdates == oldDescriptors);
    CHECK(flushes == 0);
    reset_overlay_settings();
    destroy_depth_overlay_state(data, swapchain);
    CHECK(live == baseline);
    for (int step = 1; step <= failurePoints; ++step) {
        failAt = step; operation = 0; submits = 0;
        CHECK(!present(data));
        CHECK(submits == 0);
        // A recording error keeps the successfully constructed renderer for retry.
        destroy_depth_overlay_state(data, swapchain);
        CHECK(live == baseline);
    }
    failAt = 0;
    // Transactional hot reload; old generations survive until every image has
    // passed its fence and replaced its executable recording.
    const auto reloadDir = std::filesystem::path("/tmp/depth-probe-reload-" + std::to_string(getpid()));
    std::filesystem::create_directories(reloadDir);
    for (const char* name : {"depth.vert.spv", "depth.frag.spv", "menu.vert.spv", "menu.frag.spv", "gizmo.vert.spv", "gizmo.frag.spv"})
        std::filesystem::copy_file(std::filesystem::path(DEPTH_PROBE_TEST_SHADER_DIR) / name, reloadDir / name,
                                  std::filesystem::copy_options::overwrite_existing);
    setenv("DEPTH_PROBE_SHADER_DIR", reloadDir.c_str(), 1);
    setenv("DEPTH_PROBE_SHADER_HOT_RELOAD", "1", 1);
    for (unsigned i = 0; i < 3; ++i) CHECK(present(data, i));
    auto& reload = g_depth_overlay_states.at({device, number(swapchain)});
    const auto oldEffect = reload.pipeline;
    auto touchShader = [&] {
        const auto path = reloadDir / "depth.frag.spv";
        std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(1));
        reload.shaderPollAfter = {};
    };
    touchShader();
    CHECK(present(data, 0));
    CHECK(reload.pipeline != oldEffect && reload.pipelineGeneration == 2 && live.count(number(oldEffect)));
    CHECK(present(data, 1));
    CHECK(live.count(number(oldEffect)));
    CHECK(present(data, 2));
    CHECK(present(data, 0));
    CHECK(!live.count(number(oldEffect)) && reload.retiredPipelines.empty());
    const auto goodPipeline = reload.pipeline;
    const auto reloadLive = live;
    // Both pipeline builds and every shader-module failure are transactional.
    for (int step = 1; step <= 6; ++step) {
        touchShader(); operation = 0; failAt = step;
        CHECK(present(data)); failAt = 0;
        CHECK(reload.pipeline == goodPipeline && reload.shaderReloadStatus == 2 && live == reloadLive);
    }
    { std::ofstream broken(reloadDir / "depth.frag.spv", std::ios::binary); broken << "broken"; }
    reload.shaderPollAfter = {};
    CHECK(present(data));
    CHECK(reload.pipeline == goodPipeline && live == reloadLive);
    destroy_depth_overlay_state(data, swapchain);
    CHECK(live == baseline && queueIdleCalls == 0);
    unsetenv("DEPTH_PROBE_SHADER_HOT_RELOAD");
    std::filesystem::remove_all(reloadDir);
    std::puts("PASS: shader reload success, six creation failures, malformed file, per-image generation retirement");
    setenv("DEPTH_PROBE_SHADER_DIR", DEPTH_PROBE_TEST_SHADER_DIR, 1);
    setenv("DEPTH_PROBE_SCENE_COLOR", "1", 1);
    CHECK(present(data)); // Unsupported swapchain usage retains legacy rendering.
    CHECK(g_depth_overlay_states.at({device, number(swapchain)}).sceneColor.empty());
    destroy_depth_overlay_state(data, swapchain);
    g_swapchains.at({device, number(swapchain)}).usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    CHECK(present(data));
    CHECK(g_depth_overlay_states.at({device, number(swapchain)}).sceneColor.size() == 3);
    CHECK(sceneCopies > 0 && lastUniform.uniform.viewport[3] == 1);
    destroy_depth_overlay_state(data, swapchain);
    CHECK(live == baseline);
    // Four fallible allocations/binds/views per image; partial cleanup falls back.
    for (int step = 1; step <= 12; ++step) {
        operation = 0; failAt = step;
        CHECK(present(data)); failAt = 0;
        CHECK(g_depth_overlay_states.at({device, number(swapchain)}).sceneColor.empty());
        destroy_depth_overlay_state(data, swapchain);
        CHECK(live == baseline);
    }
    sampledCombinationSupported = false;
    CHECK(present(data));
    CHECK(g_depth_overlay_states.at({device, number(swapchain)}).sceneColor.empty());
    destroy_depth_overlay_state(data, swapchain); sampledCombinationSupported = true;
    unsetenv("DEPTH_PROBE_SCENE_COLOR");
    std::puts("PASS: scene-color copy, descriptor, per-image allocation, 12 failures, unavailable capability fallback");
    setenv("DEPTH_PROBE_GPU_TIMING","1",1);
    auto timingSettings=overlay_settings();timingSettings.gpuTimingEnabled=1;set_overlay_settings(timingSettings);
    const auto readsBefore=queryReads;CHECK(present(data));CHECK(queryReads==readsBefore);
    auto& timed=g_depth_overlay_states.at({device,number(swapchain)});
    CHECK(timed.timestampPool && timestampWrites>=2 && timed.queryPending[0]);
    CHECK(present(data));CHECK(queryReads==readsBefore+1 && std::abs(timed.overlayGpuMs-.000022f)<1e-8f);
    queryReadResult=VK_NOT_READY;CHECK(present(data));CHECK(!timed.timingReadFailed);
    queryReadResult=VK_ERROR_OUT_OF_HOST_MEMORY;CHECK(present(data));CHECK(timed.timingReadFailed);
    queryReadResult=VK_SUCCESS;destroy_depth_overlay_state(data,swapchain);CHECK(live==baseline);
    fakeTimestampBits=0;CHECK(present(data));CHECK(!g_depth_overlay_states.at({device,number(swapchain)}).timestampPool);
    destroy_depth_overlay_state(data,swapchain);fakeTimestampBits=8;
    queryCreationResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;CHECK(present(data));
    CHECK(!g_depth_overlay_states.at({device,number(swapchain)}).timestampPool);
    destroy_depth_overlay_state(data,swapchain);queryCreationResult=VK_SUCCESS;CHECK(live==baseline);
    unsetenv("DEPTH_PROBE_GPU_TIMING");reset_overlay_settings();
    std::puts("PASS: per-image timestamp lifecycle, no WAIT_BIT, wrap arithmetic, unavailable queue, allocation/read failure fallback");
    // Missing SPIR-V used to leak the sampler, layouts, pool and render pass.
    setenv("DEPTH_PROBE_SHADER_DIR", "/definitely/missing/depth-probe-shaders", 1);
    CHECK(!present(data));
    CHECK(live == baseline);
    setenv("DEPTH_PROBE_SHADER_DIR", DEPTH_PROBE_TEST_SHADER_DIR, 1);
    CHECK(present(data));

    // Begin/end failures on an image's first recording skip submit and recover.
    for (int step = 1; step <= 2; ++step) {
        const auto before = live;
        operation = 0; failAt = step; submits = 0;
        CHECK(!present(data, static_cast<uint32_t>(step)));
        CHECK(submits == 0 && live == before);
        failAt = 0;
        CHECK(present(data, static_cast<uint32_t>(step)));
    }
    destroy_depth_overlay_state(data, swapchain);
    CHECK(allocations.empty() && mappedAllocations.empty());
    coherentMemory = false;
    const auto beforeFlush = flushes;
    CHECK(present(data));
    CHECK(flushes == beforeFlush + 1);
    CHECK(present(data));
    CHECK(flushes == beforeFlush + 2);
    const auto priorSubmitCount = submits;
    flushResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(!present(data) && submits == priorSubmitCount);
    flushResult = VK_SUCCESS;
    CHECK(present(data));
    coherentMemory = true;

    // Independent mapped allocations for two devices; destroying one leaves the
    // other's bytes and mapping alive. Persistent user settings are process-wide.
    {
        auto foreignData = data; foreignData.device = otherDevice;
        DepthOverlayState a, b; a.colorImages.resize(1); b.colorImages.resize(1);
        const auto f = load_depth_overlay_fns(data);
        CHECK(create_overlay_uniforms(data, f, a));
        CHECK(create_overlay_uniforms(foreignData, f, b));
        CHECK(a.uniforms[0].memory != b.uniforms[0].memory && a.uniforms[0].mapped != b.uniforms[0].mapped);
        OverlayFrameData first{}, second{};
        first.uniform.settings.aoStrength = .5f; second.uniform.settings.aoStrength = 2.5f;
        CHECK(write_overlay_uniform(data, f, a.uniforms[0], first) == VK_SUCCESS);
        CHECK(write_overlay_uniform(foreignData, f, b.uniforms[0], second) == VK_SUCCESS);
        CHECK(std::memcmp(a.uniforms[0].mapped, &first, sizeof(first)) == 0);
        destroy_overlay_uniforms(data, f, a);
        CHECK(mappedAllocations.count(b.uniforms[0].memory));
        CHECK(std::memcmp(b.uniforms[0].mapped, &second, sizeof(second)) == 0);
        destroy_overlay_uniforms(foreignData, f, b);
    }

    // The same numeric image handle on another device must remain untouched.
    auto other = g_depth_candidates.at({device, number(image)});
    other.device = otherDevice;
    other.depthView = handle<VkImageView>(9999);
    g_depth_candidates[{otherDevice, number(image)}] = other;
    VkAllocationCallbacks allocator{};
    vkDestroyImage(device, image, &allocator);
    CHECK(lastDestroyedImage == image && lastImageAllocator == &allocator);
    CHECK(!is_target_depth(device, image));
    CHECK(g_depth_candidates.count({device, number(image)}) == 0);
    CHECK(is_target_depth(otherDevice, image));
    CHECK(g_depth_candidates.count({otherDevice, number(image)}) == 1);
    CHECK(live.empty());
    CHECK(!present(data)); // no stale descriptor or command buffer submitted

    // Reuse the destroyed image handle; it must create a fresh custom view.
    nextId = number(image) - 1;
    CHECK(makeDepth() == image);
    CHECK(present(data));
    g_swapchains[{otherDevice, 55}] = {};
    vkDestroyDevice(device, nullptr);
    // The layer owns the extra view/overlay, never the application's image.
    CHECK((live == std::set<uint64_t>{number(image)}));
    live.clear(); // fake driver discards the application-owned image at shutdown

    CHECK(g_devices.empty());
    CHECK(g_swapchains.count({device, 55}) == 0);
    CHECK(g_swapchains.count({otherDevice, 55}) == 1);
    CHECK(g_depth_candidates.count({otherDevice, number(image)}) == 1);
    CHECK(order.back() == "destroyDevice");

    // Unknown entry points still forward to the next implementation.
    g_devices[dispatch_key(device)] = data;
    CHECK(vkGetDeviceProcAddr(device, "vkTestUnknownExtension") == sentinel);
    CHECK(vkGetDeviceProcAddr(device, "vkNotImplemented") == nullptr);
    CHECK(vkGetDeviceProcAddr(device, "vkDestroyImage") == reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImage));
    VkInstance instance = reinterpret_cast<VkInstance>(&deviceObject);
    InstanceData id{}; id.instance = instance; id.next_gipa = gipa;
    g_instances[dispatch_key(instance)] = id;
    CHECK(vkGetInstanceProcAddr(instance, "vkTestUnknownExtension") == sentinel);
    sceneDepthScenarios(data);
    capabilityScenarios();
    synchronizationScenarios(data);
    setenv("DEPTH_PROBE_SCENE_COLOR","1",1);setenv("DEPTH_PROBE_GPU_TIMING","1",1);
    auto featureSettings=overlay_settings();featureSettings.gpuTimingEnabled=1;featureSettings.sharpenEnabled=1;featureSettings.ssaoEnabled=1;featureSettings.ssrEnabled=1;
    set_overlay_settings(featureSettings);
    const auto beforeCopies=sceneCopies,beforeQueries=queryReads;
    sceneDepthScenarios(data); // active color/query resources across 1080p/1440p/720p and generation changes
    CHECK(sceneCopies>beforeCopies);
    set_overlay_settings(featureSettings);
    synchronizationScenarios(data); // same 3,000-frame protocol with optional resources active
    CHECK(queryReads>beforeQueries && queueIdleCalls==0 && queryCounts.empty());
    unsetenv("DEPTH_PROBE_SCENE_COLOR");unsetenv("DEPTH_PROBE_GPU_TIMING");reset_overlay_settings();
    std::puts("PASS: optional color/timestamp resources through multi-extent recreate and another 3000-frame semaphore stress");
    std::printf("PASS: %d initialization/recording failures, heartbeat UBO, coherent/noncoherent uploads, cached recording, record recovery, resource cleanup, dispatch forwarding\n", failurePoints);
}
