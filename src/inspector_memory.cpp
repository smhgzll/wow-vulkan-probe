#include "surface_inspector.hpp"
#include "inspector_uniform_gpu.hpp"
#include <limits>

namespace depth_probe::discovery {
namespace {
constexpr uint64_t maxUniformBytes=64*1024, maxPickBytes=8*1024*1024;
constexpr size_t maxPickRecords=8192;
constexpr VkBufferUsageFlags readOnlyUsage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;

VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice d,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* a,VkDeviceMemory* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    const auto r=next<PFN_vkAllocateMemory>(*data,"vkAllocateMemory")(d,info,a,out);
    if(r!=VK_SUCCESS)return r;
    VkPhysicalDeviceMemoryProperties properties{};
    if(data->get_memory_properties)data->get_memory_properties(data->physical_device,&properties);
    std::lock_guard lock(mutex);auto& s=device_state(d);
    if(s.memories.size()>=maxObjects && !s.memories.count(id(*out))){s.memoryTrackingIncomplete=true;return r;}
    auto m=std::make_shared<Memory>();m->handle=id(*out);m->generation=++s.serial;m->size=info->allocationSize;
    if(info->memoryTypeIndex<properties.memoryTypeCount)m->properties=properties.memoryTypes[info->memoryTypeIndex].propertyFlags;
    for(auto* n=static_cast<const VkBaseInStructure*>(info->pNext);n;n=n->pNext) {
        if(n->sType==VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO || n->sType==VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT)continue;
        if(n->sType==VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
            const auto flags=reinterpret_cast<const VkMemoryAllocateFlagsInfo*>(n)->flags;
            if(!(flags&~VkMemoryAllocateFlags(VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT|VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT)))continue;
        }
        m->compatible=false; // imported/external or unknown allocation path
    }
    s.memories[id(*out)]=std::move(m);return r;
}
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice d,VkDeviceMemory memory,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    uint64_t generation=0;{std::lock_guard lock(mutex);if(auto m=find_ptr(device_state(d).memories,id(memory)))generation=m->generation;}
    if(generation)retire_uniform_source(d,0,generation);
    // Erase the borrowed pointer before downstream unmaps/frees it. A snapshot
    // already in progress holds this same mutex and finishes its copy first.
    {std::lock_guard lock(mutex);device_state(d).memories.erase(id(memory));}
    next<PFN_vkFreeMemory>(*data,"vkFreeMemory")(d,memory,a);
}
void mapped(VkDevice d,VkDeviceMemory memory,uint64_t offset,uint64_t size,const void* ptr,bool supported) {
    std::lock_guard lock(mutex);auto m=find_ptr(device_state(d).memories,id(memory));if(!m)return;
    m->mapped=nullptr;m->mapSize=0;
    if(!supported || !ptr || offset>=m->size)return;
    if(size==VK_WHOLE_SIZE)size=m->size-offset;
    if(!size || size>m->size-offset)return;
    m->mapped=static_cast<const uint8_t*>(ptr);m->mapOffset=offset;m->mapSize=size;
}
void unmapped(VkDevice d,VkDeviceMemory memory) {
    std::lock_guard lock(mutex);auto m=find_ptr(device_state(d).memories,id(memory));if(m){m->mapped=nullptr;m->mapSize=0;}
}
VKAPI_ATTR VkResult VKAPI_CALL map(VkDevice d,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void** out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkMapMemory>(*data,"vkMapMemory")(d,memory,offset,size,flags,out);
    if(r==VK_SUCCESS)mapped(d,memory,offset,size,*out,!flags);
    return r;
}
template<bool khr> VKAPI_ATTR VkResult VKAPI_CALL map2(VkDevice d,const VkMemoryMapInfo* info,void** out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkMapMemory2>(*data,khr?"vkMapMemory2KHR":"vkMapMemory2")(d,info,out);
    if(r==VK_SUCCESS)mapped(d,info->memory,info->offset,info->size,*out,!info->flags && !info->pNext);
    return r;
}
VKAPI_ATTR void VKAPI_CALL unmap(VkDevice d,VkDeviceMemory memory) {
    auto data=get_device_data(d);if(!data)return;
    unmapped(d,memory);next<PFN_vkUnmapMemory>(*data,"vkUnmapMemory")(d,memory);
}
template<bool khr> VKAPI_ATTR VkResult VKAPI_CALL unmap2(VkDevice d,const VkMemoryUnmapInfo* info) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    // Failure conservatively loses capture eligibility, never keeps a pointer
    // across a potentially successful unmap.
    unmapped(d,info->memory);
    return next<PFN_vkUnmapMemory2>(*data,khr?"vkUnmapMemory2KHR":"vkUnmapMemory2")(d,info);
}
void bind_buffer(Device& s,VkBuffer buffer,VkDeviceMemory memory,uint64_t offset,bool supported) {
    auto b=find_ptr(s.buffers,id(buffer));auto m=find_ptr(s.memories,id(memory));
    if(m && (!b || !supported || !b->captureCompatible || (b->usage&~readOnlyUsage)))m->gpuWritable=true;
    if(!b)return;
    b->memory=id(memory);b->memoryGeneration=m?m->generation:0;b->memoryOffset=offset;
    b->captureCompatible &= supported;
}
void bind_image(Device& s,VkDeviceMemory memory) {
    // An aliased image could write this allocation without going through a
    // buffer command. Reject the whole allocation, including future reads.
    if(auto m=find_ptr(s.memories,id(memory)))m->gpuWritable=true;
}
VKAPI_ATTR VkResult VKAPI_CALL bindBuffer(VkDevice d,VkBuffer b,VkDeviceMemory memory,VkDeviceSize offset) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    // Publish hazards before forwarding so another queue cannot race a host
    // snapshot with a newly bound GPU-writable alias.
    {std::lock_guard lock(mutex);bind_buffer(device_state(d),b,memory,offset,true);}
    auto r=next<PFN_vkBindBufferMemory>(*data,"vkBindBufferMemory")(d,b,memory,offset);
    if(r!=VK_SUCCESS){std::lock_guard lock(mutex);if(auto value=find_ptr(device_state(d).buffers,id(b)))value->memoryGeneration=0;}
    return r;
}
template<bool khr> VKAPI_ATTR VkResult VKAPI_CALL bindBuffers(VkDevice d,uint32_t count,const VkBindBufferMemoryInfo* infos) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    {std::lock_guard lock(mutex);auto& s=device_state(d);for(uint32_t i=0;i<count;++i)bind_buffer(s,infos[i].buffer,infos[i].memory,infos[i].memoryOffset,!infos[i].pNext);}
    auto r=next<PFN_vkBindBufferMemory2>(*data,khr?"vkBindBufferMemory2KHR":"vkBindBufferMemory2")(d,count,infos);
    if(r!=VK_SUCCESS){std::lock_guard lock(mutex);auto& s=device_state(d);for(uint32_t i=0;i<count;++i)if(auto b=find_ptr(s.buffers,id(infos[i].buffer)))b->memoryGeneration=0;}
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL bindImage(VkDevice d,VkImage image,VkDeviceMemory memory,VkDeviceSize offset) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    {std::lock_guard lock(mutex);bind_image(device_state(d),memory);}
    return next<PFN_vkBindImageMemory>(*data,"vkBindImageMemory")(d,image,memory,offset);
}
template<bool khr> VKAPI_ATTR VkResult VKAPI_CALL bindImages(VkDevice d,uint32_t count,const VkBindImageMemoryInfo* infos) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    {std::lock_guard lock(mutex);auto& s=device_state(d);for(uint32_t i=0;i<count;++i)bind_image(s,infos[i].memory);}
    return next<PFN_vkBindImageMemory2>(*data,khr?"vkBindImageMemory2KHR":"vkBindImageMemory2")(d,count,infos);
}
VKAPI_ATTR VkResult VKAPI_CALL sparse(VkQueue queue,uint32_t count,const VkBindSparseInfo* infos,VkFence fence) {
    auto data=get_device_data(reinterpret_cast<VkDevice>(queue));if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    {std::lock_guard lock(mutex);auto& s=device_state(data->device);s.memoryTrackingIncomplete=true;}
    return next<PFN_vkQueueBindSparse>(*data,"vkQueueBindSparse")(queue,count,infos,fence);
}
}
bool uniform_dynamic_offset(const Draw& d,const ResourceUse& r,uint64_t& offset) {
    offset=0;if(r.descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)return true;
    if(r.set>=d.sets.size())return false;
    const auto& bound=d.sets[r.set];if(!bound.layout || r.set>=bound.layout->sets.size())return false;
    auto layout=bound.layout->sets[r.set];if(!layout)return false;
    size_t index=0;
    for(const auto& [binding,b]:layout->bindings) {
        if(b.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && b.type!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)continue;
        if(binding==r.binding){if(r.element>=b.count || index+r.element>=bound.offsets.size())return false;offset=bound.offsets[index+r.element];return true;}
        index+=b.count;
    }
    return false;
}
bool read_bound_uniform_word(const Device& s,const Command& c,uint32_t setIndex,uint32_t binding,uint32_t byteOffset,uint32_t& value) {
    return read_bound_uniform_bytes(s,c,setIndex,binding,byteOffset,4,&value);
}
bool read_bound_uniform_bytes(const Device& s,const Command& c,uint32_t setIndex,uint32_t binding,uint32_t byteOffset,uint32_t size,void* out) {
    uint64_t bytes=0;uint8_t* p=bound_uniform_span(s,c,setIndex,binding,bytes);
    if(!p || uint64_t(byteOffset)+size>bytes)return false;
    std::memcpy(out,p+byteOffset,size);
    return true;
}
uint8_t* bound_uniform_span(const Device& s,const Command& c,uint32_t setIndex,uint32_t binding,uint64_t& bytes) {
    bytes=0;
    if(setIndex>=c.sets.size() || s.memoryTrackingIncomplete)return nullptr;
    const auto& bound=c.sets[setIndex];
    std::shared_ptr<Set> set=bound.pushed;
    if(!set){auto it=s.sets.find(bound.handle);if(it==s.sets.end() || !it->second || it->second->generation!=bound.generation)return nullptr;set=it->second;}
    auto found=set->descriptors.find({binding,0});
    if(found==set->descriptors.end())return nullptr;
    const auto& d=found->second;
    if(!d.known || (d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC))return nullptr;
    uint64_t dynamic=0;
    if(d.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {   // uniform_dynamic_offset, on the command's bound state
        if(!bound.layout || setIndex>=bound.layout->sets.size() || !bound.layout->sets[setIndex])return nullptr;
        size_t index=0;bool ok=false;
        for(const auto& [b,info]:bound.layout->sets[setIndex]->bindings) {
            if(info.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && info.type!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)continue;
            if(b==binding){if(index>=bound.offsets.size())return nullptr;dynamic=bound.offsets[index];ok=true;break;}
            index+=info.count;
        }
        if(!ok)return nullptr;
    }
    auto buffer=s.buffers.find(d.buffer);
    if(buffer==s.buffers.end() || !buffer->second || buffer->second->generation!=d.bufferGeneration)return nullptr;
    const auto& b=*buffer->second;
    auto memory=s.memories.find(b.memory);
    if(memory==s.memories.end() || !memory->second || memory->second->generation!=b.memoryGeneration || !memory->second->mapped)return nullptr;
    const auto& m=*memory->second;
    const auto required=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if((m.properties&required)!=required)return nullptr;
    const uint64_t start=b.memoryOffset+d.offset+dynamic;
    if(d.offset+dynamic>=b.size || start<m.mapOffset || start>=m.mapOffset+m.mapSize)return nullptr;
    bytes=std::min<uint64_t>(b.size-(d.offset+dynamic),m.mapOffset+m.mapSize-start);
    if(d.range && d.range!=VK_WHOLE_SIZE)bytes=std::min<uint64_t>(bytes,d.range);
    return const_cast<uint8_t*>(m.mapped)+(start-m.mapOffset);   // the application's own mapping: host-writable
}
PFN_vkVoidFunction memory_hook(const char* name) {
#define HOOK(api,fn) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(fn)
    HOOK(vkAllocateMemory,allocate);HOOK(vkFreeMemory,freeMemory);
    HOOK(vkMapMemory,map);HOOK(vkUnmapMemory,unmap);
    HOOK(vkMapMemory2,map2<false>);HOOK(vkMapMemory2KHR,map2<true>);
    HOOK(vkUnmapMemory2,unmap2<false>);HOOK(vkUnmapMemory2KHR,unmap2<true>);
    HOOK(vkBindBufferMemory,bindBuffer);HOOK(vkBindBufferMemory2,bindBuffers<false>);HOOK(vkBindBufferMemory2KHR,bindBuffers<true>);
    HOOK(vkBindImageMemory,bindImage);HOOK(vkBindImageMemory2,bindImages<false>);HOOK(vkBindImageMemory2KHR,bindImages<true>);
    HOOK(vkQueueBindSparse,sparse);
#undef HOOK
    return nullptr;
}
// Shared by capture_pick_uniforms (Inspector, ticket-gated) and
// capture_scene_debugger_uniforms (opt-in automatic sampling): validates that
// a bound uniform-buffer descriptor is safe to read from host-mapped memory
// (tracked, generation-current, host-coherent, not GPU-writable/aliased,
// within the mapped range and the caller's byte budget) and copies it.
// `budgetUsed` is the caller's own running byte counter, checked against
// `budgetMax`; callers with independent budgets must pass their own counter.
std::shared_ptr<UniformSnapshot> try_capture_uniform(Device& s,const SubmittedDraw& draw,const ResourceUse& r,size_t& budgetUsed,size_t budgetMax,bool allowWritableUsage) {
    auto out=std::make_shared<UniformSnapshot>();out->set=r.set;out->binding=r.binding;out->element=r.element;
    out->buffer=r.descriptor.buffer;out->range=r.descriptor.range;out->submit=draw.submit;out->ticket=draw.draw.pickTicket;
    auto capture=[&]() -> const char* {
        if(s.memoryTrackingIncomplete)return "memory-tracking-incomplete";
        // allowWritableUsage callers (frame scan, the fog's cascade matrices) need only this descriptor resolved, not the whole draw.
        if((draw.descriptorsIncomplete && !allowWritableUsage) || !r.descriptor.known || r.updateAfterBind)return "descriptor-unresolved-or-update-after-bind";
        auto b=find_ptr(s.buffers,r.descriptor.buffer);if(!b)return "buffer-untracked";
        if(!r.descriptor.bufferGeneration || r.descriptor.bufferGeneration!=b->generation)return "buffer-generation-stale-or-untracked";
        out->bufferGeneration=b->generation;out->memoryGeneration=b->memoryGeneration;out->bufferUsage=b->usage;
        // allowWritableUsage (frame scan "uniforms", research only): DXVK's constant rings carry transfer/storage usage bits although
        // only the CPU writes them; the host copy at submit is then read, not proven - the capture reports the usage.
        if(!(b->usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || (!allowWritableUsage && (!b->captureCompatible || (b->usage&~readOnlyUsage))))return "buffer-not-proven-gpu-read-only";
        auto m=find_ptr(s.memories,b->memory);
        if(!m || !b->memoryGeneration || m->generation!=b->memoryGeneration)return "memory-binding-stale-or-untracked";
        if(!m->compatible || (m->gpuWritable && !allowWritableUsage))return "allocation-may-have-gpu-writes-or-aliases";
        const auto required=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if((m->properties&required)!=required)return "memory-not-host-coherent";
        if(!m->mapped)return "memory-not-mapped-at-submit";
        uint64_t dynamic=0;if(!uniform_dynamic_offset(draw.draw,r,dynamic))return "dynamic-offset-unresolved";
        if(r.descriptor.offset>b->size || dynamic>b->size-r.descriptor.offset)return "buffer-offset-out-of-range";
        out->offset=r.descriptor.offset+dynamic;
        if(out->range==VK_WHOLE_SIZE){if(dynamic)return "whole-size-with-dynamic-offset";out->range=b->size-out->offset;}
        if(!out->range || out->range>b->size-out->offset)return "buffer-range-out-of-range";
        if(out->range>maxUniformBytes || out->range>budgetMax-budgetUsed)return "uniform-snapshot-budget";
        if(b->memoryOffset>m->size || out->offset>m->size-b->memoryOffset)return "allocation-offset-out-of-range";
        const auto start=b->memoryOffset+out->offset;
        if(start<m->mapOffset || start-m->mapOffset>m->mapSize || out->range>m->mapSize-(start-m->mapOffset))return "descriptor-outside-mapped-range";
        const auto relative=start-m->mapOffset;
        if(relative>std::numeric_limits<size_t>::max()-out->range)return "host-address-overflow";
        out->bytes.assign(m->mapped+size_t(relative),m->mapped+size_t(relative+out->range));
        budgetUsed+=out->range;
        return "";
    };
    out->reason=capture();
    return out;
}
void capture_pick_uniforms(Device& s,SubmittedDraw& draw) {
    if(!s.inspector || !s.inspector->armed || !draw.draw.pick || draw.draw.pickTicket!=s.inspector->ticket || draw.draw.query==UINT32_MAX)return;
    auto& in=*s.inspector;
    for(const auto& r:draw.resources) {
        if(r.descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && r.descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)continue;
        // Bound layouts may include descriptors unrelated to this shader. Their
        // contents need not be stable for this draw, so do not inspect them.
        bool declared=false;
        if(auto p=draw.draw.pipeline)for(auto shader:{p->vs,p->fs})if(shader)for(const auto& binding:shader->bindings)
            declared |= binding.storage==2 && binding.set==r.set && binding.binding==r.binding;
        if(!declared)continue;
        if(in.uniformCaptureRecords>=maxPickRecords)break;
        ++in.uniformCaptureRecords;
        auto out=try_capture_uniform(s,draw,r,in.uniformCaptureBytes,maxPickBytes);
        if(!out->reason.empty())for(const auto& gpu:draw.draw.gpuUniforms) {
            auto& value=*gpu->snapshot;
            if(value.set==r.set && value.binding==r.binding && value.element==r.element && value.buffer==r.descriptor.buffer && value.bufferGeneration==r.descriptor.bufferGeneration) {
                value.submit=draw.submit;out=gpu->snapshot;break;
            }
        }
        draw.uniforms.push_back(std::move(out));
    }
}
}
