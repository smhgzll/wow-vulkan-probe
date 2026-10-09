#include "inspector_uniform_gpu.hpp"

namespace depth_probe::discovery {
namespace {
constexpr uint64_t batchBytes=4*1024*1024, pickBytes=8*1024*1024, uniformBytes=64*1024;
constexpr size_t maxBatches=8,maxRecords=8192;
template<class T> T raw(const DeviceData& d,const char* name){return reinterpret_cast<T>(d.next_gdpa(d.device,name));}
constexpr VkBufferUsageFlags hostReadOnly=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
bool can_timestamp(const DeviceData& data,const Device& s,const Command& c) {
    const auto pool=s.commandPools.find(c.pool);
    if(pool==s.commandPools.end() || (pool->second.flags&VK_COMMAND_POOL_CREATE_PROTECTED_BIT) || !data.get_queue_properties)return false;
    uint32_t count=0;data.get_queue_properties(data.physical_device,&count,nullptr);
    if(!count || count>64 || pool->second.queueFamilyIndex>=count)return false;
    std::vector<VkQueueFamilyProperties> properties(count);data.get_queue_properties(data.physical_device,&count,properties.data());
    const auto& family=properties[pool->second.queueFamilyIndex];
    return family.timestampValidBits && (family.queueFlags&VK_QUEUE_GRAPHICS_BIT);
}
std::shared_ptr<GpuUniformBatch> create_batch(const DeviceData& data,Device& s,const Command& c) {
    std::erase_if(s.uniformBatches,[](auto& w){return w.expired();});
    if(s.uniformBatches.size()>=maxBatches || !can_timestamp(data,s,c) || !data.get_memory_properties)return {};
    for(const auto name:{"vkCreateBuffer","vkDestroyBuffer","vkGetBufferMemoryRequirements","vkAllocateMemory","vkFreeMemory",
            "vkBindBufferMemory","vkMapMemory","vkUnmapMemory","vkInvalidateMappedMemoryRanges","vkCreateQueryPool","vkDestroyQueryPool",
            "vkGetQueryPoolResults","vkCmdResetQueryPool","vkCmdWriteTimestamp","vkCmdCopyBuffer","vkCmdPipelineBarrier"})
        if(!data.next_gdpa(data.device,name))return {};
    auto result=std::make_shared<GpuUniformBatch>();result->data=data;
    VkBufferCreateInfo bi{};bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=batchBytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(raw<PFN_vkCreateBuffer>(data,"vkCreateBuffer")(data.device,&bi,nullptr,&result->buffer)!=VK_SUCCESS)return {};
    VkMemoryRequirements requirements{};raw<PFN_vkGetBufferMemoryRequirements>(data,"vkGetBufferMemoryRequirements")(data.device,result->buffer,&requirements);
    VkPhysicalDeviceMemoryProperties properties{};data.get_memory_properties(data.physical_device,&properties);
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<properties.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        type=i;if(properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)break;
    }
    if(type==UINT32_MAX)return {};
    result->coherent=properties.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
    if(raw<PFN_vkAllocateMemory>(data,"vkAllocateMemory")(data.device,&ai,nullptr,&result->memory)!=VK_SUCCESS)return {};
    if(raw<PFN_vkBindBufferMemory>(data,"vkBindBufferMemory")(data.device,result->buffer,result->memory,0)!=VK_SUCCESS)return {};
    if(raw<PFN_vkMapMemory>(data,"vkMapMemory")(data.device,result->memory,0,VK_WHOLE_SIZE,0,&result->mapped)!=VK_SUCCESS)return {};
    VkQueryPoolCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=1;
    if(raw<PFN_vkCreateQueryPool>(data,"vkCreateQueryPool")(data.device,&qi,nullptr,&result->marker)!=VK_SUCCESS)return {};
    s.uniformBatches.push_back(result);return result;
}
}
GpuUniformBatch::~GpuUniformBatch(){release();}
void GpuUniformBatch::release() {
    if(mapped)raw<PFN_vkUnmapMemory>(data,"vkUnmapMemory")(data.device,memory);
    if(marker)raw<PFN_vkDestroyQueryPool>(data,"vkDestroyQueryPool")(data.device,marker,nullptr);
    if(buffer)raw<PFN_vkDestroyBuffer>(data,"vkDestroyBuffer")(data.device,buffer,nullptr);
    if(memory)raw<PFN_vkFreeMemory>(data,"vkFreeMemory")(data.device,memory,nullptr);
    mapped=nullptr;marker=VK_NULL_HANDLE;buffer=VK_NULL_HANDLE;memory=VK_NULL_HANDLE;
}
bool GpuUniformBatch::poll() {
    if(completed)return true;
    {std::lock_guard lock(submitMutex);if(submitting || !submitted)return false;}
    if(!recorded || !marker)return false;
    uint64_t values[2]{};
    auto r=raw<PFN_vkGetQueryPoolResults>(data,"vkGetQueryPoolResults")(data.device,marker,0,1,sizeof(values),values,sizeof(values),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if(r==VK_NOT_READY || (r==VK_SUCCESS && !values[1]))return false;
    if(r==VK_SUCCESS && !coherent) {
        VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=memory;range.size=VK_WHOLE_SIZE;
        r=raw<PFN_vkInvalidateMappedMemoryRanges>(data,"vkInvalidateMappedMemoryRanges")(data.device,1,&range);
    }
    for(auto& weak:reads)if(auto read=weak.lock()) {
        auto& value=*read->snapshot;
        if(value.reason!="gpu-readback-pending")continue;
        if(r!=VK_SUCCESS){value.reason="gpu-readback-result:"+std::to_string(r);continue;}
        const auto p=static_cast<const uint8_t*>(mapped)+read->destination;
        value.bytes.assign(p,p+value.range);value.reason.clear();
    }
    completed=true;return true;
}
std::shared_ptr<GpuUniformRead> schedule_gpu_uniform_read(const DeviceData& data,Device& s,Command& c,const Draw& draw,const ResourceUse& resource,size_t& budgetUsed,size_t budgetMax) {
    const auto& descriptor=resource.descriptor;
    auto read=std::make_shared<GpuUniformRead>();read->snapshot=std::make_shared<UniformSnapshot>();auto& out=*read->snapshot;
    out.set=resource.set;out.binding=resource.binding;out.element=resource.element;out.range=descriptor.range;
    out.captureMoment="gpu-end-of-render-pass; original draw values not guaranteed";
    auto schedule=[&]() -> const char* {
        if((descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) || !descriptor.known || resource.updateAfterBind)return "gpu-resource-not-a-known-uniform-descriptor";
        auto b=find_ptr(s.buffers,descriptor.buffer);
        if(!b || b->generation!=descriptor.bufferGeneration)return "gpu-buffer-untracked-or-stale";
        out.buffer=b->handle;out.bufferGeneration=b->generation;out.bufferUsage=b->usage;
        if(!b->captureCompatible)return "gpu-buffer-create-or-binding-unsupported";
        if(!(b->usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT))return "gpu-buffer-missing-transfer-src";
        auto memory=find_ptr(s.memories,b->memory);
        if(!memory || memory->generation!=b->memoryGeneration || !b->memoryGeneration)return "gpu-memory-binding-stale-or-untracked";
        out.memoryGeneration=b->memoryGeneration;
        uint64_t dynamic=0;if(!uniform_dynamic_offset(draw,resource,dynamic))return "gpu-dynamic-offset-unresolved";
        if(descriptor.offset>b->size || dynamic>b->size-descriptor.offset)return "gpu-buffer-offset-out-of-range";
        out.offset=descriptor.offset+dynamic;
        if(out.range==VK_WHOLE_SIZE){if(dynamic)return "gpu-whole-size-with-dynamic-offset";out.range=b->size-out.offset;}
        if(!out.range || out.range>b->size-out.offset || out.range>uniformBytes)return "gpu-uniform-range-out-of-range";
        if(out.range>budgetMax-budgetUsed)return "gpu-uniform-budget";
        if(!c.uniformPass){c.uniformPass=create_batch(data,s,c);if(c.uniformPass)c.uniformBatches.push_back(c.uniformPass);}
        if(!c.uniformPass)return "gpu-uniform-allocation-or-timestamp-unavailable";
        auto& batch=*c.uniformPass;
        if(out.range>batchBytes-batch.used)return "gpu-uniform-pass-budget";
        read->batch=c.uniformPass;read->destination=batch.used;batch.used=(batch.used+out.range+3)&~uint64_t(3);batch.reads.push_back(read);
        budgetUsed+=out.range;return "gpu-readback-pending";
    };
    out.reason=schedule();
    return read;
}
void schedule_gpu_uniforms(const DeviceData& data,Device& s,Command& c,Draw& draw,const std::vector<ResourceUse>& resources) {
    if(!s.inspector || !s.inspector->armed || !draw.pick || draw.query==UINT32_MAX || !draw.target.known ||
        c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY || !(c.usage&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) || c.conditional || c.transformFeedback || c.activeQueries)return;
    auto& in=*s.inspector;
    for(const auto& resource:resources) {
        const auto& descriptor=resource.descriptor;
        if((descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) || !descriptor.known || resource.updateAfterBind)continue;
        bool declared=false;
        if(auto p=draw.pipeline)for(auto shader:{p->vs,p->fs})if(shader)for(const auto& binding:shader->bindings)
            declared |= binding.storage==2 && binding.set==resource.set && binding.binding==resource.binding;
        if(!declared || in.uniformGpuRecords>=maxRecords)continue;
        auto b=find_ptr(s.buffers,descriptor.buffer);
        if(!b || b->generation!=descriptor.bufferGeneration)continue;
        auto memory=find_ptr(s.memories,b->memory);
        const auto hostFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if(b->captureCompatible && !(b->usage&~hostReadOnly) && memory && memory->generation==b->memoryGeneration && memory->compatible && !memory->gpuWritable && memory->mapped && (memory->properties&hostFlags)==hostFlags)continue;
        auto read=schedule_gpu_uniform_read(data,s,c,draw,resource,in.uniformGpuBytes,pickBytes);
        read->snapshot->ticket=in.ticket;
        ++in.uniformGpuRecords;draw.gpuUniforms.push_back(std::move(read));
    }
}
void record_gpu_uniforms(const DeviceData& data,Device& s,Command& c,VkCommandBuffer cmd) {
    auto batch=std::move(c.uniformPass);if(!batch)return;
    auto barrier=raw<PFN_vkCmdPipelineBarrier>(data,"vkCmdPipelineBarrier");
    raw<PFN_vkCmdResetQueryPool>(data,"vkCmdResetQueryPool")(cmd,batch->marker,0,1);
    VkMemoryBarrier before{};before.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;before.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
    for(const auto& weak:batch->reads)if(auto read=weak.lock()) {
        auto& value=*read->snapshot;auto b=find_ptr(s.buffers,value.buffer);
        if(!b || b->generation!=value.bufferGeneration){value.reason="gpu-buffer-destroyed-before-pass-end";continue;}
        VkBuffer source=(VkBuffer)value.buffer;
        VkBufferCopy copy{value.offset,read->destination,value.range};raw<PFN_vkCmdCopyBuffer>(data,"vkCmdCopyBuffer")(cmd,source,batch->buffer,1,&copy);
    }
    VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=batch->buffer;host.size=VK_WHOLE_SIZE;
    VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;after.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;after.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,1,&host,0,nullptr);
    raw<PFN_vkCmdWriteTimestamp>(data,"vkCmdWriteTimestamp")(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,batch->marker,0);batch->recorded=true;
}
bool poll_gpu_uniforms(Frame& frame) {
    bool ready=true;std::set<GpuUniformBatch*> seen;
    for(auto& record:frame.draws)for(auto& read:record.draw.gpuUniforms)if(read->batch && seen.insert(read->batch.get()).second)ready &= read->batch->poll();
    return ready;
}
void release_gpu_uniforms(Device& s) {
    for(auto& weak:s.uniformBatches)if(auto batch=weak.lock())batch->release();
    s.uniformBatches.clear();
}
std::vector<std::shared_ptr<GpuUniformBatch>> begin_uniform_submit(const Frame& frame) {
    std::vector<std::shared_ptr<GpuUniformBatch>> batches;
    for(const auto& batch:frame.uniformBatches)if(batch && std::find(batches.begin(),batches.end(),batch)==batches.end()) {
        batches.push_back(batch);std::lock_guard lock(batch->submitMutex);batch->submitting=true;
    }
    return batches;
}
void end_uniform_submit(const std::vector<std::shared_ptr<GpuUniformBatch>>& batches,VkResult result) {
    for(const auto& batch:batches) {
        {std::lock_guard lock(batch->submitMutex);batch->submitted=result==VK_SUCCESS;batch->submitting=false;}
        batch->submitChanged.notify_all();
    }
}
void retire_uniform_source(VkDevice device,uint64_t buffer,uint64_t memoryGeneration) {
    std::vector<std::shared_ptr<GpuUniformBatch>> batches;
    {
        std::lock_guard lock(mutex);auto found=devices.find(device);if(found==devices.end())return;
        for(const auto& weak:found->second.uniformBatches)if(auto batch=weak.lock())if(!batch->completed && batch->recorded) {
            for(const auto& w:batch->reads)if(auto read=w.lock())if((buffer && read->snapshot->buffer==buffer) || (memoryGeneration && read->snapshot->memoryGeneration==memoryGeneration)) {batches.push_back(batch);break;}
        }
    }
    for(const auto& batch:batches) {
        // Never hold the discovery mutex while waiting for an in-progress
        // driver submit or for the GPU. Failed/unsubmitted commands cannot hang.
        {std::unique_lock lock(batch->submitMutex);batch->submitChanged.wait(lock,[&]{return !batch->submitting;});if(!batch->submitted)continue;}
        uint64_t value=0;const auto r=raw<PFN_vkGetQueryPoolResults>(batch->data,"vkGetQueryPoolResults")(device,batch->marker,0,1,sizeof(value),&value,sizeof(value),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);
        {std::lock_guard lock(mutex);if(r==VK_SUCCESS)batch->poll();}
    }
}
}
