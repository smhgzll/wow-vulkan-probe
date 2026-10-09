namespace memory_cases {
static std::array<uint8_t,2048> host;
static VkResult driverResult=VK_SUCCESS;
static uint64_t bufferHandle=51,memoryHandle=61;
static unsigned forwarded=0;
static bool rejectTransfer=false;static std::vector<uint64_t> observedUsage;
static unsigned queryReads=0;static VkResult queryResult=VK_NOT_READY;
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties* p) {
    *p={};p->memoryTypeCount=2;p->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    p->memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice,const VkBufferCreateInfo* info,const VkAllocationCallbacks*,VkBuffer* p){
    ++forwarded;uint64_t usage=info->usage;
    if(auto v=chain<VkBufferUsageFlags2CreateInfo>(info->pNext,VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO))usage=v->usage;
    observedUsage.push_back(usage);if(rejectTransfer && (usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT))return VK_ERROR_FEATURE_NOT_PRESENT;
    *p=(VkBuffer)bufferHandle;return driverResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL queries(VkDevice,VkQueryPool,uint32_t,uint32_t,size_t,void* out,VkDeviceSize,VkQueryResultFlags flags){
    ++queryReads;CHECK(!(flags&VK_QUERY_RESULT_WAIT_BIT));auto* values=static_cast<uint64_t*>(out);values[0]=12;values[1]=queryResult==VK_SUCCESS;return queryResult;
}
static VKAPI_ATTR void VKAPI_CALL destroyQuery(VkDevice,VkQueryPool,const VkAllocationCallbacks*){}
static VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory* p){++forwarded;*p=(VkDeviceMemory)memoryHandle;return driverResult;}
static VKAPI_ATTR void VKAPI_CALL free(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*){++forwarded;}
static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice,VkBuffer,const VkAllocationCallbacks*){++forwarded;}
static VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize){++forwarded;return driverResult;}
static VKAPI_ATTR VkResult VKAPI_CALL bind2(VkDevice,uint32_t,const VkBindBufferMemoryInfo*){++forwarded;return driverResult;}
static VKAPI_ATTR VkResult VKAPI_CALL bindImage(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize){++forwarded;return driverResult;}
static VKAPI_ATTR VkResult VKAPI_CALL map(VkDevice,VkDeviceMemory,VkDeviceSize offset,VkDeviceSize,VkMemoryMapFlags,void** p){++forwarded;*p=host.data()+offset;return driverResult;}
static VKAPI_ATTR VkResult VKAPI_CALL map2(VkDevice d,const VkMemoryMapInfo* i,void** p){return map(d,i->memory,i->offset,i->size,i->flags,p);}
static VKAPI_ATTR void VKAPI_CALL unmap(VkDevice,VkDeviceMemory){++forwarded;}
static VKAPI_ATTR VkResult VKAPI_CALL unmap2(VkDevice,const VkMemoryUnmapInfo*){++forwarded;return driverResult;}
static PFN_vkVoidFunction VKAPI_CALL dispatch(VkDevice,const char* name) {
#define M(api,fn) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(fn)
    M(vkCreateBuffer,create);M(vkDestroyBuffer,destroy);M(vkAllocateMemory,allocate);M(vkFreeMemory,free);
    M(vkBindBufferMemory,bind);M(vkBindBufferMemory2,bind2);M(vkBindBufferMemory2KHR,bind2);M(vkBindImageMemory,bindImage);
    M(vkMapMemory,map);M(vkMapMemory2,map2);M(vkMapMemory2KHR,map2);M(vkUnmapMemory,unmap);M(vkUnmapMemory2,unmap2);M(vkUnmapMemory2KHR,unmap2);
    M(vkGetQueryPoolResults,queries);M(vkDestroyQueryPool,destroyQuery);
#undef M
    return nullptr;
}
static void run() {
    int key;struct Dispatch {void* key;} object{&key};auto device=reinterpret_cast<VkDevice>(&object);
    DeviceData data;data.device=device;data.next_gdpa=dispatch;data.get_memory_properties=properties;g_devices[&key]=data;
#define M(api) reinterpret_cast<PFN_##api>(discovery_hook(#api))
    auto& s=device_state(device);auto& in=inspector_state(s,device);in.armed=true;in.ticket=77;
    VkBufferCreateInfo bi{};bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=1024;bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VkMemoryAllocateInfo mi{};mi.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;mi.allocationSize=2048;
    VkBuffer b;VkDeviceMemory memory;void* mapped=nullptr;
    CHECK(M(vkCreateBuffer)(device,&bi,nullptr,&b)==VK_SUCCESS);
    CHECK(M(vkAllocateMemory)(device,&mi,nullptr,&memory)==VK_SUCCESS);
    CHECK(M(vkBindBufferMemory)(device,b,memory,256)==VK_SUCCESS);
    CHECK(M(vkMapMemory)(device,memory,128,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
    for(size_t i=0;i<host.size();++i)host[i]=uint8_t(i);
    SubmittedDraw draw;draw.draw.pick=std::make_shared<PickQueries>();draw.draw.pickTicket=77;draw.draw.query=0;draw.submit=123;
    draw.draw.pipeline=std::make_shared<Pipeline>();draw.draw.pipeline->vs=std::make_shared<Shader>();
    draw.draw.pipeline->vs->bindings={{2,0,2,false,false},{2,3,2,false,false}};
    ResourceUse resource;resource.set=2;resource.binding=0;resource.descriptor.type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    resource.descriptor.known=true;resource.descriptor.buffer=id(b);resource.descriptor.bufferGeneration=s.buffers.at(id(b))->generation;resource.descriptor.offset=32;resource.descriptor.range=64;draw.resources={resource};
    auto capture=[&]{draw.uniforms.clear();capture_pick_uniforms(s,draw);CHECK(draw.uniforms.size()==1);return draw.uniforms[0];};
    auto saved=capture();CHECK(saved->bytes.size()==64 && saved->bytes[0]==32 && saved->bytes[63]==95 && saved->submit==123);
    host[288]=201;CHECK(saved->bytes[0]==32 && capture()->bytes[0]==201);
    M(vkUnmapMemory)(device,memory);CHECK(capture()->reason=="memory-not-mapped-at-submit" && saved->bytes.size()==64);
    VkMemoryMapInfo mapInfo{};mapInfo.sType=VK_STRUCTURE_TYPE_MEMORY_MAP_INFO;mapInfo.memory=memory;mapInfo.offset=128;mapInfo.size=VK_WHOLE_SIZE;
    CHECK(M(vkMapMemory2KHR)(device,&mapInfo,&mapped)==VK_SUCCESS);CHECK(capture()->bytes.size()==64);
    // Dynamic offsets are ordered across *both* uniform and storage bindings.
    auto layout=std::make_shared<Layout>();layout->sets.resize(3);layout->sets[2]=std::make_shared<SetLayout>();
    Binding before;before.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;before.count=2;layout->sets[2]->bindings[0]=before;
    Binding uniform;uniform.type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;uniform.count=2;layout->sets[2]->bindings[3]=uniform;
    draw.draw.sets[2].layout=layout;draw.draw.sets[2].offsets={1,2,16,48};draw.resources[0].binding=3;draw.resources[0].element=1;draw.resources[0].descriptor.type=uniform.type;
    CHECK(capture()->offset==80 && capture()->bytes[0]==80);
    draw.draw.sets[2].offsets.pop_back();CHECK(capture()->reason=="dynamic-offset-unresolved");draw.draw.sets[2].offsets.push_back(48);
    draw.resources[0].descriptor.offset=UINT64_MAX;CHECK(capture()->reason=="buffer-offset-out-of-range");draw.resources[0].descriptor.offset=32;
    draw.resources[0].descriptor.range=VK_WHOLE_SIZE;CHECK(capture()->reason=="whole-size-with-dynamic-offset");draw.resources[0]=resource;
    mapInfo.size=32;CHECK(M(vkMapMemory2)(device,&mapInfo,&mapped)==VK_SUCCESS);CHECK(capture()->reason=="descriptor-outside-mapped-range");
    mapInfo.size=VK_WHOLE_SIZE;CHECK(M(vkMapMemory2)(device,&mapInfo,&mapped)==VK_SUCCESS);
    draw.resources[0].updateAfterBind=true;CHECK(capture()->bytes.empty());draw.resources[0].updateAfterBind=false;
    in.uniformCaptureBytes=8*1024*1024;CHECK(capture()->reason=="uniform-snapshot-budget");in.uniformCaptureBytes=0;
    in.armed=false;draw.uniforms.clear();capture_pick_uniforms(s,draw);CHECK(draw.uniforms.empty());in.armed=true;
    draw.resources[0].binding=7;capture_pick_uniforms(s,draw);CHECK(draw.uniforms.empty());draw.resources[0]=resource;
    // Core/KHR bind failure must not publish a usable binding.
    VkBindBufferMemoryInfo binding{};binding.sType=VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO;binding.buffer=b;binding.memory=memory;binding.memoryOffset=256;
    driverResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;CHECK(M(vkBindBufferMemory2KHR)(device,1,&binding)==driverResult);CHECK(capture()->reason=="memory-binding-stale-or-untracked");
    driverResult=VK_SUCCESS;CHECK(M(vkBindBufferMemory2)(device,1,&binding)==VK_SUCCESS);
    CHECK(M(vkBindImageMemory)(device,(VkImage)uint64_t(91),memory,0)==VK_SUCCESS);CHECK(capture()->reason=="allocation-may-have-gpu-writes-or-aliases");
    M(vkFreeMemory)(device,memory,nullptr);CHECK(capture()->reason=="memory-binding-stale-or-untracked");
    CHECK(M(vkAllocateMemory)(device,&mi,nullptr,&memory)==VK_SUCCESS);CHECK(capture()->reason=="memory-binding-stale-or-untracked");
    CHECK(M(vkBindBufferMemory)(device,b,memory,256)==VK_SUCCESS);CHECK(M(vkMapMemory)(device,memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
    CHECK(capture()->bytes.size()==64);
    // Another GPU-writable buffer in this allocation blocks host reads, even
    // after destruction; no fence or idle is inserted to try to make it safe.
    ++bufferHandle;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;VkBuffer alias;CHECK(M(vkCreateBuffer)(device,&bi,nullptr,&alias)==VK_SUCCESS);
    CHECK(M(vkBindBufferMemory)(device,alias,memory,0)==VK_SUCCESS);M(vkDestroyBuffer)(device,alias,nullptr);CHECK(capture()->bytes.empty());
    M(vkFreeMemory)(device,memory,nullptr);mi.memoryTypeIndex=1;CHECK(M(vkAllocateMemory)(device,&mi,nullptr,&memory)==VK_SUCCESS);
    CHECK(M(vkBindBufferMemory)(device,b,memory,256)==VK_SUCCESS);CHECK(capture()->reason=="memory-not-host-coherent");
    // Same numeric buffer handle with a new generation cannot revive a descriptor.
    M(vkDestroyBuffer)(device,b,nullptr);bufferHandle=id(b);bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;CHECK(M(vkCreateBuffer)(device,&bi,nullptr,&b)==VK_SUCCESS);
    CHECK(capture()->reason=="buffer-generation-stale-or-untracked");
    CHECK(saved->bytes[0]==32 && saved->ticket==77);CHECK(forwarded>20);
    // Patched create failure forwards the identical original usage/chain.
    M(vkDestroyBuffer)(device,b,nullptr);rejectTransfer=true;observedUsage.clear();
    CHECK(M(vkCreateBuffer)(device,&bi,nullptr,&b)==VK_SUCCESS);CHECK(observedUsage.size()==2 && observedUsage[0]==(bi.usage|VK_BUFFER_USAGE_TRANSFER_SRC_BIT) && observedUsage[1]==bi.usage);
    CHECK(!s.buffers.at(id(b))->transferPatched && s.buffers.at(id(b))->usage==bi.usage);
    VkBufferUsageFlags2CreateInfo usage2{};usage2.sType=VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO;usage2.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.pNext=&usage2;observedUsage.clear();CHECK(M(vkCreateBuffer)(device,&bi,nullptr,&b)==VK_SUCCESS);CHECK(usage2.usage==VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT && observedUsage.size()==2);
    rejectTransfer=false;CHECK(M(vkCreateBuffer)(device,&bi,nullptr,&b)==VK_SUCCESS);CHECK(s.buffers.at(id(b))->transferPatched && usage2.usage==VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    {
        auto batch=std::make_shared<GpuUniformBatch>();batch->data=data;batch->marker=(VkQueryPool)uint64_t(55);batch->coherent=true;batch->mapped=host.data();
        auto read=std::make_shared<GpuUniformRead>();read->batch=batch;read->snapshot=std::make_shared<UniformSnapshot>();read->snapshot->range=8;read->snapshot->reason="gpu-readback-pending";batch->reads.push_back(read);
        Frame frame;frame.uniformBatches.push_back(batch);frame.draws.emplace_back();frame.draws[0].draw.gpuUniforms.push_back(read);
        CHECK(!poll_gpu_uniforms(frame) && queryReads==0);batch->recorded=true;
        auto submission=begin_uniform_submit(frame);CHECK(!poll_gpu_uniforms(frame) && queryReads==0);
        end_uniform_submit(submission,VK_ERROR_OUT_OF_DEVICE_MEMORY);CHECK(!poll_gpu_uniforms(frame) && queryReads==0);
        submission=begin_uniform_submit(frame);end_uniform_submit(submission,VK_SUCCESS);
        CHECK(!poll_gpu_uniforms(frame) && queryReads==1 && read->snapshot->bytes.empty());
        queryResult=VK_SUCCESS;CHECK(poll_gpu_uniforms(frame) && queryReads==2 && read->snapshot->bytes.size()==8 && read->snapshot->reason.empty());
        CHECK(poll_gpu_uniforms(frame) && queryReads==2);batch->mapped=nullptr;
        // A draw list truncated to zero must not lose in-flight readback tracking.
        frame.draws.clear();frame.truncated=true;batch->submitted=false;
        submission=begin_uniform_submit(frame);CHECK(submission.size()==1 && batch->submitting);
        end_uniform_submit(submission,VK_SUCCESS);CHECK(batch->submitted);
    }
    devices.erase(device);g_devices.erase(&key);
#undef M
    std::puts("PASS: submit-time uniform bytes, offsets, map/unmap/free/reuse, aliases, failure forwarding, budgets and no-capture ordinary frames");
}
}
