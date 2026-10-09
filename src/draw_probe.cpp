#include "draw_probe.hpp"
#include "overlay_settings.hpp"
#include <array>
#include <charconv>
#include <type_traits>

namespace depth_probe {
namespace {
std::mutex probeMutex;
DeviceHandleMap<std::vector<uint32_t>> depthClears;
std::atomic<unsigned> depthLogCount{0};
struct PipelineRecord {
    uint64_t hash=14695981039346656037ull, vertexHash=0, fragmentHash=0, layout=0, renderPass=0;
    uint32_t depthTest=0, depthWrite=0, compare=0, blend=0, topology=0;
    bool dynamic=false, shaderKnown=true;
};
struct CommandRecord {
    VkCommandPool pool{};
    uint64_t pipeline=0, pass=0, framebuffer=0, ordinal=0;
    VkViewport viewport{};VkRect2D scissor{};
    uint64_t descriptorSet=0;uint32_t descriptorFirst=0, descriptorCount=0;
};
DeviceHandleMap<uint64_t> shaderHashes;
DeviceHandleMap<PipelineRecord> pipelines;
DeviceHandleMap<CommandRecord> commands;
bool captureWasEnabled=false;
unsigned captureCount=0;
bool draw_enabled() {
    static const bool value=[] {const auto flag=std::getenv("DEPTH_PROBE_DRAW_PROBE");return flag&&!std::strcmp(flag,"1");}();
    return value;
}
void hash_value(uint64_t& hash,uint64_t value) {
    for(unsigned i=0;i<8;++i){hash=(hash^(value&255))*1099511628211ull;value>>=8;}
}
bool enabled() {
    static const bool value = [] {
        for (const auto key : {"DEPTH_PROBE_DEPTH_DIAGNOSTICS", "DEPTH_PROBE_DRAW_PROBE"}) {
            const char* flag = std::getenv(key);
            if (flag && !std::strcmp(flag, "1")) return true;
        }
        return false;
    }();
    return value;
}
template<class T> uint64_t id(T value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(value);
    else return value;
}
template<class F> F next(const DeviceData& data, const char* name) {
    return reinterpret_cast<F>(data.next_gdpa(data.device, name));
}
std::optional<DeviceData> command_data(VkCommandBuffer cmd) {
    // Dispatchable command buffers have the device's dispatch key.
    std::lock_guard lock(g_state_mutex);
    auto it = g_devices.find(dispatch_key(cmd));
    return it == g_devices.end() ? std::nullopt : std::optional<DeviceData>(it->second);
}
void emit(const std::string& message) {
    const auto n = depthLogCount.fetch_add(1, std::memory_order_relaxed);
    if (n < 256) log_line("[DEPTH-DIAGNOSTIC] record-time-only " + message);
    if (n == 255) log_line("[DEPTH-DIAGNOSTIC] log budget exhausted; Auto remains unknown");
}
void remember_pipeline(VkDevice device,const VkGraphicsPipelineCreateInfo& info,VkPipeline pipeline) {
    if(!draw_enabled())return;
    PipelineRecord record;
    record.layout=id(info.layout);record.renderPass=id(info.renderPass);
    std::lock_guard lock(probeMutex);
    std::array<std::pair<uint32_t,uint64_t>,8> stages{};
    if(info.stageCount>stages.size())record.shaderKnown=false;
    else for(uint32_t i=0;i<info.stageCount;++i) {
        const auto found=shaderHashes.find({device,id(info.pStages[i].module)});
        uint64_t hash=found==shaderHashes.end()?0:found->second;
        if(!hash)record.shaderKnown=false;
        stages[i]={info.pStages[i].stage,hash};
        if(info.pStages[i].stage==VK_SHADER_STAGE_VERTEX_BIT)record.vertexHash=hash;
        if(info.pStages[i].stage==VK_SHADER_STAGE_FRAGMENT_BIT)record.fragmentHash=hash;
        // Specialization can materially change shader behavior. Include its bytes.
        if(const auto* spec=info.pStages[i].pSpecializationInfo) {
            for(uint32_t j=0;j<spec->mapEntryCount;++j){hash_value(stages[i].second,spec->pMapEntries[j].constantID);hash_value(stages[i].second,spec->pMapEntries[j].offset);hash_value(stages[i].second,spec->pMapEntries[j].size);}
            const auto* bytes=static_cast<const unsigned char*>(spec->pData);
            for(size_t j=0;j<spec->dataSize;++j)stages[i].second=(stages[i].second^bytes[j])*1099511628211ull;
        }
    }
    std::sort(stages.begin(),stages.end());
    for(const auto& [stage,hash]:stages){hash_value(record.hash,stage);hash_value(record.hash,hash);}
    if(info.pDepthStencilState) {
        record.depthTest=info.pDepthStencilState->depthTestEnable;record.depthWrite=info.pDepthStencilState->depthWriteEnable;
        record.compare=info.pDepthStencilState->depthCompareOp;
    }
    if(info.pInputAssemblyState)record.topology=info.pInputAssemblyState->topology;
    if(info.pColorBlendState)for(uint32_t i=0;i<info.pColorBlendState->attachmentCount;++i) {
        const auto& blend=info.pColorBlendState->pAttachments[i];record.blend|=blend.blendEnable;
        for(auto value : {uint32_t(blend.blendEnable),uint32_t(blend.srcColorBlendFactor),uint32_t(blend.dstColorBlendFactor),
            uint32_t(blend.colorBlendOp),uint32_t(blend.srcAlphaBlendFactor),uint32_t(blend.dstAlphaBlendFactor),
            uint32_t(blend.alphaBlendOp),uint32_t(blend.colorWriteMask)})hash_value(record.hash,value);
    }
    if(info.pDynamicState)for(uint32_t i=0;i<info.pDynamicState->dynamicStateCount;++i) {
        const auto state=info.pDynamicState->pDynamicStates[i];hash_value(record.hash,state);
        if(state==VK_DYNAMIC_STATE_DEPTH_COMPARE_OP || state==VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE || state==VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE)record.dynamic=true;
    }
    for(auto value : {record.depthTest,record.depthWrite,record.compare,record.topology})hash_value(record.hash,value);
    // This is a fingerprint of captured fields, not a complete Vulkan pipeline identity.
    if(pipelines.size()<8192)pipelines[{device,id(pipeline)}]=record;
}
VKAPI_ATTR VkResult VKAPI_CALL graphics(VkDevice d, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* info, const VkAllocationCallbacks* alloc, VkPipeline* output) {
    const auto data = get_device_data(d); if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = next<PFN_vkCreateGraphicsPipelines>(*data, "vkCreateGraphicsPipelines")(d, cache, count, info, alloc, output);
    // Vulkan permits partial success; only non-null returned objects have state.
    for (uint32_t i = 0; i < count; ++i) if (output[i]) {
        remember_pipeline(d,info[i],output[i]);
        std::ostringstream line;
        line << "device=" << d << " pipeline=" << id(output[i]);
        if (auto* depth = info[i].pDepthStencilState)
            line << " depthTest=" << depth->depthTestEnable << " depthWrite=" << depth->depthWriteEnable << " compareOp=" << depth->depthCompareOp;
        else line << " depthState=unavailable";
        if (info[i].pDynamicState) for (uint32_t j = 0; j < info[i].pDynamicState->dynamicStateCount; ++j)
            line << " dynamicState=" << info[i].pDynamicState->pDynamicStates[j];
        emit(line.str());
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    const auto data = command_data(cmd); if (!data) return;
    next<PFN_vkCmdBindPipeline>(*data, "vkCmdBindPipeline")(cmd, point, pipeline);
    if(draw_enabled() && point==VK_PIPELINE_BIND_POINT_GRAPHICS) {
        std::lock_guard lock(probeMutex);if(commands.size()<16384)commands[{data->device,id(cmd)}].pipeline=id(pipeline);
    }
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS && depthLogCount.load() < 256)
        emit("cmd=" + std::to_string(id(cmd)) + " bindPipeline=" + std::to_string(id(pipeline)));
}
VKAPI_ATTR void VKAPI_CALL viewport(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkViewport* values) {
    const auto data = command_data(cmd); if (!data) return;
    next<PFN_vkCmdSetViewport>(*data, "vkCmdSetViewport")(cmd, first, count, values);
    if(draw_enabled() && first==0 && count) {std::lock_guard lock(probeMutex);if(commands.size()<16384)commands[{data->device,id(cmd)}].viewport=values[0];}
    if (depthLogCount.load() >= 256) return;
    for (uint32_t i = 0; i < count; ++i) {
        std::ostringstream line; line << "cmd=" << id(cmd) << " viewport=" << first+i << " minDepth=" << values[i].minDepth
            << " maxDepth=" << values[i].maxDepth << " extent=" << values[i].width << 'x' << values[i].height;
        emit(line.str());
    }
}
VKAPI_ATTR void VKAPI_CALL clearImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
    const VkClearDepthStencilValue* value, uint32_t count, const VkImageSubresourceRange* ranges) {
    const auto data = command_data(cmd); if (!data) return;
    next<PFN_vkCmdClearDepthStencilImage>(*data, "vkCmdClearDepthStencilImage")(cmd, image, layout, value, count, ranges);
    if (depthLogCount.load() < 256) emit("cmd=" + std::to_string(id(cmd)) + " clearImage=" + std::to_string(id(image)) + " depth=" + std::to_string(value->depth));
}
VKAPI_ATTR void VKAPI_CALL clearAttachments(VkCommandBuffer cmd, uint32_t count, const VkClearAttachment* attachments,
    uint32_t rectCount, const VkClearRect* rects) {
    const auto data = command_data(cmd); if (!data) return;
    next<PFN_vkCmdClearAttachments>(*data, "vkCmdClearAttachments")(cmd, count, attachments, rectCount, rects);
    for (uint32_t i = 0; i < count && depthLogCount.load() < 256; ++i)
        if (attachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
            emit("cmd=" + std::to_string(id(cmd)) + " clearAttachment depth=" + std::to_string(attachments[i].clearValue.depthStencil.depth));
}
VKAPI_ATTR VkResult VKAPI_CALL renderPass(VkDevice d, const VkRenderPassCreateInfo* info,
    const VkAllocationCallbacks* alloc, VkRenderPass* result) {
    const auto data = get_device_data(d); if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    const auto status = next<PFN_vkCreateRenderPass>(*data, "vkCreateRenderPass")(d, info, alloc, result);
    if (status == VK_SUCCESS) {
        std::vector<uint32_t> clears;
        for (uint32_t i = 0; i < info->attachmentCount; ++i)
            if (is_depth_or_stencil_format(info->pAttachments[i].format) && info->pAttachments[i].format != VK_FORMAT_S8_UINT &&
                info->pAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) clears.push_back(i);
        std::lock_guard lock(probeMutex);
        if (depthClears.size() < 4096) depthClears[{d, id(*result)}] = std::move(clears);
    }
    return status;
}
VKAPI_ATTR void VKAPI_CALL destroyPass(VkDevice d, VkRenderPass pass, const VkAllocationCallbacks* alloc) {
    const auto data = get_device_data(d); if (!data) return;
    { std::lock_guard lock(probeMutex); depthClears.erase({d, id(pass)}); }
    next<PFN_vkDestroyRenderPass>(*data, "vkDestroyRenderPass")(d, pass, alloc);
}
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    const auto data = command_data(cmd); if (!data) return;
    next<PFN_vkCmdBeginRenderPass>(*data, "vkCmdBeginRenderPass")(cmd, info, contents);
    if(draw_enabled()) {std::lock_guard lock(probeMutex);if(commands.size()<16384){auto& c=commands[{data->device,id(cmd)}];c.pass=id(info->renderPass);c.framebuffer=id(info->framebuffer);}}
    if (depthLogCount.load() >= 256) return;
    std::lock_guard lock(probeMutex);
    const auto it = depthClears.find({data->device, id(info->renderPass)});
    if (it == depthClears.end()) return;
    for (auto index : it->second) if (index < info->clearValueCount)
        emit("cmd=" + std::to_string(id(cmd)) + " renderPass=" + std::to_string(id(info->renderPass)) +
             " framebuffer=" + std::to_string(id(info->framebuffer)) + " clearDepth=" + std::to_string(info->pClearValues[index].depthStencil.depth));
}
VKAPI_ATTR VkResult VKAPI_CALL shader(VkDevice d,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks* alloc,VkShaderModule* output) {
    const auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next<PFN_vkCreateShaderModule>(*data,"vkCreateShaderModule")(d,info,alloc,output);
    if(result==VK_SUCCESS) {
        uint64_t hash=14695981039346656037ull;const auto* bytes=reinterpret_cast<const unsigned char*>(info->pCode);
        for(size_t i=0;i<info->codeSize;++i)hash=(hash^bytes[i])*1099511628211ull;
        std::lock_guard lock(probeMutex);if(shaderHashes.size()<8192)shaderHashes[{d,id(*output)}]=hash;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroyShader(VkDevice d,VkShaderModule module,const VkAllocationCallbacks* alloc) {
    const auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(probeMutex);shaderHashes.erase({d,id(module)});}
    next<PFN_vkDestroyShaderModule>(*data,"vkDestroyShaderModule")(d,module,alloc);
}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice d,VkPipeline pipeline,const VkAllocationCallbacks* alloc) {
    const auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(probeMutex);pipelines.erase({d,id(pipeline)});}
    next<PFN_vkDestroyPipeline>(*data,"vkDestroyPipeline")(d,pipeline,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL allocateCommands(VkDevice d,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* output) {
    const auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next<PFN_vkAllocateCommandBuffers>(*data,"vkAllocateCommandBuffers")(d,info,output);
    if(result==VK_SUCCESS){std::lock_guard lock(probeMutex);for(uint32_t i=0;i<info->commandBufferCount && commands.size()<16384;++i)commands[{d,id(output[i])}].pool=info->commandPool;}
    return result;
}
VKAPI_ATTR void VKAPI_CALL freeCommands(VkDevice d,VkCommandPool pool,uint32_t count,const VkCommandBuffer* buffers) {
    const auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(probeMutex);for(uint32_t i=0;i<count;++i)commands.erase({d,id(buffers[i])});}
    next<PFN_vkFreeCommandBuffers>(*data,"vkFreeCommandBuffers")(d,pool,count,buffers);
}
void reset_record(VkDevice d,VkCommandBuffer cmd) {
    std::lock_guard lock(probeMutex);
    auto it=commands.find({d,id(cmd)});
    if(it!=commands.end()){const auto pool=it->second.pool;it->second={};it->second.pool=pool;}
    else if(commands.size()<16384)commands[{d,id(cmd)}]={};
}
VKAPI_ATTR VkResult VKAPI_CALL beginCommands(VkCommandBuffer cmd,const VkCommandBufferBeginInfo* info) {
    const auto data=command_data(cmd);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next<PFN_vkBeginCommandBuffer>(*data,"vkBeginCommandBuffer")(cmd,info);
    if(result==VK_SUCCESS)reset_record(data->device,cmd);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL resetCommands(VkCommandBuffer cmd,VkCommandBufferResetFlags flags) {
    const auto data=command_data(cmd);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next<PFN_vkResetCommandBuffer>(*data,"vkResetCommandBuffer")(cmd,flags);
    if(result==VK_SUCCESS)reset_record(data->device,cmd);
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice d,VkCommandPool pool,const VkAllocationCallbacks* alloc) {
    const auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(probeMutex);std::erase_if(commands,[&](const auto& item){return item.first.device==d && item.second.pool==pool;});}
    next<PFN_vkDestroyCommandPool>(*data,"vkDestroyCommandPool")(d,pool,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice d,VkCommandPool pool,VkCommandPoolResetFlags flags) {
    const auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next<PFN_vkResetCommandPool>(*data,"vkResetCommandPool")(d,pool,flags);
    if(result==VK_SUCCESS){std::lock_guard lock(probeMutex);for(auto& [key,c]:commands)if(key.device==d && c.pool==pool){c={};c.pool=pool;}}
    return result;
}
VKAPI_ATTR void VKAPI_CALL scissor(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkRect2D* values) {
    const auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdSetScissor>(*data,"vkCmdSetScissor")(cmd,first,count,values);
    if(first==0 && count){std::lock_guard lock(probeMutex);if(commands.size()<16384)commands[{data->device,id(cmd)}].scissor=values[0];}
}
VKAPI_ATTR void VKAPI_CALL descriptors(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,
    uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* offsets) {
    const auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdBindDescriptorSets>(*data,"vkCmdBindDescriptorSets")(cmd,point,layout,first,count,sets,dynamicCount,offsets);
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS && count){std::lock_guard lock(probeMutex);if(commands.size()<16384){auto& c=commands[{data->device,id(cmd)}];c.descriptorFirst=first;c.descriptorCount=count;c.descriptorSet=id(sets[0]);}}
}
void observe_draw(const DeviceData& data,VkCommandBuffer cmd,const char* kind,uint32_t count,uint32_t instances) {
    const bool capture=overlay_settings().drawProbeCapture;
    std::lock_guard lock(probeMutex);
    if(capture && !captureWasEnabled){captureCount=0;log_line("[DRAW-PROBE] capture-start record-time-only budget=256");}
    captureWasEnabled=capture;
    const auto it=commands.find({data.device,id(cmd)});if(it==commands.end())return;
    auto& c=it->second;++c.ordinal;
    if(!capture || captureCount>=256)return;
    const auto found=pipelines.find({data.device,c.pipeline});
    const PipelineRecord p=found==pipelines.end()?PipelineRecord{}:found->second;
    static const uint64_t filter=[] {
        uint64_t hash=0;const auto* text=std::getenv("DEPTH_PROBE_DRAW_PROBE_HASH");
        if(text){if(text[0]=='0' && (text[1]=='x'||text[1]=='X'))text+=2;auto end=text+std::strlen(text);auto result=std::from_chars(text,end,hash,16);if(result.ec!=std::errc{} || result.ptr!=end)hash=0;}
        return hash;
    }();
    if(filter && filter!=p.hash)return;
    ++captureCount;
    std::ostringstream line;line<<"[DRAW-PROBE] record-time-only cmd="<<id(cmd)<<" drawIndex="<<c.ordinal<<" kind="<<kind
        <<" count="<<count<<" instances="<<instances<<" pipeline="<<c.pipeline<<" pipelineHash="<<std::hex<<p.hash
        <<" vsHash="<<p.vertexHash<<" fsHash="<<p.fragmentHash<<std::dec<<" shaderKnown="<<(found!=pipelines.end() && p.shaderKnown)
        <<" depthTestStatic="<<p.depthTest<<" depthWriteStatic="<<p.depthWrite<<" compareOpStatic="<<p.compare<<" dynamicDepth="<<p.dynamic
        <<" blend="<<p.blend<<" topology="<<p.topology<<" pipelineLayout="<<p.layout<<" pipelineRenderPass="<<p.renderPass<<" lastObservedLegacyPass="<<c.pass<<" framebuffer="<<c.framebuffer
        <<" viewport="<<c.viewport.x<<','<<c.viewport.y<<','<<c.viewport.width<<','<<c.viewport.height<<" depthRange="<<c.viewport.minDepth<<','<<c.viewport.maxDepth
        <<" scissor="<<c.scissor.offset.x<<','<<c.scissor.offset.y<<','<<c.scissor.extent.width<<','<<c.scissor.extent.height
        <<" descriptorFirst="<<c.descriptorFirst<<" descriptorCount="<<c.descriptorCount<<" firstDescriptorSet="<<c.descriptorSet
        <<" textures=unresolved water=unknown";
    log_line(line.str());
    if(captureCount==256)log_line("[DRAW-PROBE] capture-budget-exhausted toggle draw_probe_capture off/on to rearm");
}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer cmd,uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance) {
    const auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdDraw>(*data,"vkCmdDraw")(cmd,vertices,instances,firstVertex,firstInstance);observe_draw(*data,cmd,"draw",vertices,instances);
}
VKAPI_ATTR void VKAPI_CALL drawIndexed(VkCommandBuffer cmd,uint32_t indices,uint32_t instances,uint32_t firstIndex,int32_t offset,uint32_t firstInstance) {
    const auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdDrawIndexed>(*data,"vkCmdDrawIndexed")(cmd,indices,instances,firstIndex,offset,firstInstance);observe_draw(*data,cmd,"indexed",indices,instances);
}
VKAPI_ATTR void VKAPI_CALL drawIndirect(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset,uint32_t count,uint32_t stride) {
    const auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdDrawIndirect>(*data,"vkCmdDrawIndirect")(cmd,buffer,offset,count,stride);observe_draw(*data,cmd,"indirect-count-not-vertices",count,0);
}
VKAPI_ATTR void VKAPI_CALL drawIndexedIndirect(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset,uint32_t count,uint32_t stride) {
    const auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdDrawIndexedIndirect>(*data,"vkCmdDrawIndexedIndirect")(cmd,buffer,offset,count,stride);observe_draw(*data,cmd,"indexed-indirect-count-not-indices",count,0);
}

}
void destroy_device_probe(VkDevice device) {
    std::lock_guard lock(probeMutex);
    std::erase_if(shaderHashes,[&](const auto& item){return item.first.device==device;});
    std::erase_if(pipelines,[&](const auto& item){return item.first.device==device;});
    std::erase_if(commands,[&](const auto& item){return item.first.device==device;});
    std::erase_if(depthClears, [&](const auto& item) { return item.first.device == device; });
}
PFN_vkVoidFunction probe_hook(const char* name) {
    if (!enabled()) return nullptr;
#define HOOK(api, function) if (!std::strcmp(name, #api)) return reinterpret_cast<PFN_vkVoidFunction>(function)
    HOOK(vkCreateGraphicsPipelines, graphics);
    HOOK(vkCmdBindPipeline, bind);
    HOOK(vkCmdSetViewport, viewport);
    HOOK(vkCmdClearDepthStencilImage, clearImage);
    HOOK(vkCmdClearAttachments, clearAttachments);
    HOOK(vkCreateRenderPass, renderPass);
    HOOK(vkDestroyRenderPass, destroyPass);
    HOOK(vkCmdBeginRenderPass, beginPass);
    if(draw_enabled()) {
        HOOK(vkCreateShaderModule,shader);HOOK(vkDestroyShaderModule,destroyShader);HOOK(vkDestroyPipeline,destroyPipeline);
        HOOK(vkAllocateCommandBuffers,allocateCommands);HOOK(vkFreeCommandBuffers,freeCommands);
        HOOK(vkBeginCommandBuffer,beginCommands);HOOK(vkResetCommandBuffer,resetCommands);
        HOOK(vkDestroyCommandPool,destroyPool);HOOK(vkResetCommandPool,resetPool);
        HOOK(vkCmdSetScissor,scissor);HOOK(vkCmdBindDescriptorSets,descriptors);
        HOOK(vkCmdDraw,draw);HOOK(vkCmdDrawIndexed,drawIndexed);
        HOOK(vkCmdDrawIndirect,drawIndirect);HOOK(vkCmdDrawIndexedIndirect,drawIndexedIndirect);
    }
#undef HOOK
    return nullptr;
}
} // namespace depth_probe
