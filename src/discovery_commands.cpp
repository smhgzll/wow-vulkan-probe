#include "game_lights_capture.hpp"
#include "game_fog_capture.hpp"
#include "game_soft_shadows.hpp"
#include "water_reflections.hpp"
#include "surface_inspector.hpp"
#include "inspector_uniform_gpu.hpp"
#include "volumetric_fog.hpp"

namespace depth_probe::discovery {
namespace {
std::optional<DeviceData> command_data(VkCommandBuffer c){return get_device_data(reinterpret_cast<VkDevice>(c));}
Command* command(Device& s,VkCommandBuffer cmd) {
    if(g_perf_tracking_off.load(std::memory_order_relaxed))return nullptr;   // performance test phase: hooks forward without bookkeeping
    auto it=s.commands.find(id(cmd));return it==s.commands.end()?nullptr:&it->second;
}
void clear_command(Device& s,Command& c) {
    if(c.pick)c.pick->poll();
    auto pool=c.pool;auto level=c.level;auto water=std::move(c.waterSlots);
    for(auto& b:water)if(b)b->recorded=false;
    s.eventCount-=c.events.size();c={};c.waterSlots=std::move(water);c.pool=pool;c.level=level;c.generation=++s.serial;
}
void append(Device&,Command&,Event&&);
void layout_event(Device& s,Command& c,std::shared_ptr<Image> image,const VkImageSubresourceRange& range,VkImageLayout layout,bool known) {
    if(image)fog_layout_event(s,*image,range,layout,known && image->sharing==VK_SHARING_MODE_EXCLUSIVE);
    if(!inspector_enabled() || !image)return;
    known &= image->sharing==VK_SHARING_MODE_EXCLUSIVE;
    Event event;event.layout={std::move(image),layout,known,range};append(s,c,std::move(event));
}
void invalidate_coverage(Device& s,Command& c,const Target& target,const VkRect2D* rect=nullptr) {
    if(!inspector_enabled() || !s.inspector || !s.inspector->armed)return;
    auto d=std::make_shared<Draw>();d->target=target;d->kind=6;d->unsupported=true;d->pickTicket=s.inspector->ticket;
    if(rect){d->dynamic.scissor=*rect;d->dynamic.valid|=Scissor;}
    append(s,c,{std::move(d),0,0,{}});
}
// Held-back draws (transparent-aware fog) must be issued before the application changes their attachments' layouts.
void flush_before_barrier(const DeviceData& data,VkCommandBuffer cmd,uint32_t count,const VkImage* images,size_t stride) {
    std::lock_guard lock(mutex);auto& s=device_state(data.device);auto* c=command(s,cmd);if(!c || c->fogDeferred.empty())return;
    for(uint32_t i=0;i<count;++i)if(fog_deferred_target(*c,id(*reinterpret_cast<const VkImage*>(reinterpret_cast<const char*>(images)+i*stride)))){
        fog_flush_deferred(data,s,*c,cmd,"barrier");return;}
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t nm,const VkMemoryBarrier* m,uint32_t nb,const VkBufferMemoryBarrier* b,uint32_t ni,const VkImageMemoryBarrier* images) {
    auto data=command_data(cmd);if(!data)return;
    if(ni)flush_before_barrier(*data,cmd,ni,&images[0].image,sizeof(VkImageMemoryBarrier));
    next<PFN_vkCmdPipelineBarrier>(*data,"vkCmdPipelineBarrier")(cmd,src,dst,flags,nm,m,nb,b,ni,images);
    if(!inspector_enabled())return;
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))for(uint32_t i=0;i<ni;++i) {
        const auto& v=images[i];layout_event(s,*c,find_ptr(s.images,id(v.image)),v.subresourceRange,v.newLayout,!v.pNext && v.srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED && v.dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);
    }
}
void barrier2Impl(VkCommandBuffer cmd,const VkDependencyInfo* info,const char* name) {
    auto data=command_data(cmd);if(!data)return;
    if(info->imageMemoryBarrierCount)flush_before_barrier(*data,cmd,info->imageMemoryBarrierCount,&info->pImageMemoryBarriers[0].image,sizeof(VkImageMemoryBarrier2));
    next<PFN_vkCmdPipelineBarrier2>(*data,name)(cmd,info);
    if(!inspector_enabled())return;
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))for(uint32_t i=0;i<info->imageMemoryBarrierCount;++i) {
        const auto& v=info->pImageMemoryBarriers[i];layout_event(s,*c,find_ptr(s.images,id(v.image)),v.subresourceRange,v.newLayout,!v.pNext && !info->pNext && v.srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED && v.dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);
    }
}
VKAPI_ATTR void VKAPI_CALL barrier2(VkCommandBuffer c,const VkDependencyInfo* i){barrier2Impl(c,i,"vkCmdPipelineBarrier2");}
VKAPI_ATTR void VKAPI_CALL barrier2KHR(VkCommandBuffer c,const VkDependencyInfo* i){barrier2Impl(c,i,"vkCmdPipelineBarrier2KHR");}
VKAPI_ATTR void VKAPI_CALL clearAttachments(VkCommandBuffer cmd,uint32_t count,const VkClearAttachment* attachments,uint32_t rectCount,const VkClearRect* rects) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdClearAttachments>(*data,"vkCmdClearAttachments")(cmd,count,attachments,rectCount,rects);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))for(uint32_t i=0;i<rectCount;++i)invalidate_coverage(s,*c,c->target,&rects[i].rect);
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice d,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkAllocateCommandBuffers>(*data,"vkAllocateCommandBuffers")(d,info,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);
        for(uint32_t i=0;i<info->commandBufferCount;++i)if(s.commands.size()<maxCommands) {
            auto& c=s.commands[id(out[i])];c.pool=id(info->commandPool);c.level=info->level;c.generation=++s.serial;
        } else s.limits=true;
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice d,const VkCommandPoolCreateInfo* info,const VkAllocationCallbacks* a,VkCommandPool* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreateCommandPool>(*data,"vkCreateCommandPool")(d,info,a,out);
    if(r==VK_SUCCESS && !info->pNext){std::lock_guard lock(mutex);auto& s=device_state(d);if(s.commandPools.size()<maxObjects)s.commandPools[id(*out)]=*info;}
    return r;
}
VKAPI_ATTR void VKAPI_CALL freeCommands(VkDevice d,VkCommandPool pool,uint32_t count,const VkCommandBuffer* values) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);auto& s=device_state(d);for(uint32_t i=0;i<count;++i)if(auto* c=command(s,values[i])){s.eventCount-=c->events.size();s.commands.erase(id(values[i]));}}
    next<PFN_vkFreeCommandBuffers>(*data,"vkFreeCommandBuffers")(d,pool,count,values);
}
void begin_target(Device& s,Command& c,VkRenderPass pass,VkFramebuffer framebuffer,uint32_t subpass) {
    c.target={};c.target.renderPass=id(pass);c.target.framebuffer=id(framebuffer);c.pass=find_ptr(s.passes,id(pass));c.attachments.clear();
    auto found=s.framebuffers.find(id(framebuffer));
    if(found!=s.framebuffers.end()){c.attachments=found->second.views;c.target.width=found->second.width;c.target.height=found->second.height;}
    set_target(c,subpass);
}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer cmd,const VkCommandBufferBeginInfo* info) {
    auto data=command_data(cmd);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkBeginCommandBuffer>(*data,"vkBeginCommandBuffer")(cmd,info);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(data->device);auto* c=command(s,cmd);if(!c)return r;
        clear_command(s,*c);c->recording=true;c->usage=info->flags;
        inspector_begin_command(*data,s,*c,cmd);
        if(c->level==VK_COMMAND_BUFFER_LEVEL_SECONDARY && (info->flags&VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
            if(auto* inheritance=info->pInheritanceInfo) {
                begin_target(s,*c,inheritance->renderPass,inheritance->framebuffer,inheritance->subpass);
                c->target.inherited=true;
            } else mark_unsupported(*c,"missing-secondary-inheritance");
        }
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL end(VkCommandBuffer cmd) {
    auto data=command_data(cmd);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    {std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))fog_flush_deferred(*data,s,*c,cmd,"end of command buffer",true);}
    auto r=next<PFN_vkEndCommandBuffer>(*data,"vkEndCommandBuffer")(cmd);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd)){c->recording=false;c->executable=r==VK_SUCCESS;}return r;
}
VKAPI_ATTR VkResult VKAPI_CALL reset(VkCommandBuffer cmd,VkCommandBufferResetFlags flags) {
    auto data=command_data(cmd);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkResetCommandBuffer>(*data,"vkResetCommandBuffer")(cmd,flags);
    if(r==VK_SUCCESS){std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))clear_command(s,*c);}return r;
}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice d,VkCommandPool pool,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);auto& s=device_state(d);std::erase_if(s.commands,[&](auto& item){if(item.second.pool!=id(pool))return false;s.eventCount-=item.second.events.size();return true;});s.commandPools.erase(id(pool));}
    next<PFN_vkDestroyCommandPool>(*data,"vkDestroyCommandPool")(d,pool,a);
}
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice d,VkCommandPool pool,VkCommandPoolResetFlags flags) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkResetCommandPool>(*data,"vkResetCommandPool")(d,pool,flags);
    if(r==VK_SUCCESS){std::lock_guard lock(mutex);auto& s=device_state(d);for(auto& [h,c]:s.commands){(void)h;if(c.pool==id(pool))clear_command(s,c);}}return r;
}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipeline pipeline) {
    auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdBindPipeline>(*data,"vkCmdBindPipeline")(cmd,point,pipeline);
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS){std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))c->pipeline=find_ptr(s.pipelines,id(pipeline));}
}
void track_push(VkDevice device,VkCommandBuffer cmd,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void* values,bool unknown=false) {
    std::lock_guard lock(mutex);auto& s=device_state(device);auto* c=command(s,cmd);if(!c)return;
    const auto l=find_ptr(s.layouts,id(layout));
    for(size_t stage=0;stage<2;++stage)if(stages&(stage?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT)) {
        auto& push=c->push[stage];
        if(unknown || !l || !values || offset%4 || size%4 || offset>maxPushWords*4 || size>maxPushWords*4-offset){push={};continue;}
        if(push.signature!=l->pushSignature){push={};push.signature=l->pushSignature;}
        std::memcpy(reinterpret_cast<char*>(push.words.data())+offset,values,size);
        for(uint32_t i=offset/4;i<(offset+size)/4;++i)push.known|=uint64_t(1)<<i;
    }
}
VKAPI_ATTR void VKAPI_CALL pushConstants(VkCommandBuffer cmd,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void* values) {
    auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdPushConstants>(*data,"vkCmdPushConstants")(cmd,layout,stages,offset,size,values);
    track_push(data->device,cmd,layout,stages,offset,size,values);
}
#ifdef VK_KHR_maintenance6
void pushConstants2Impl(VkCommandBuffer cmd,const VkPushConstantsInfoKHR* info,const char* name) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdPushConstants2KHR>(*data,name)(cmd,info);
    track_push(data->device,cmd,info->layout,info->stageFlags,info->offset,info->size,info->pValues,info->pNext!=nullptr);
}
VKAPI_ATTR void VKAPI_CALL pushConstants2(VkCommandBuffer cmd,const VkPushConstantsInfoKHR* info){pushConstants2Impl(cmd,info,"vkCmdPushConstants2");}
VKAPI_ATTR void VKAPI_CALL pushConstants2KHR(VkCommandBuffer cmd,const VkPushConstantsInfoKHR* info){pushConstants2Impl(cmd,info,"vkCmdPushConstants2KHR");}
#endif
void bind_sets_impl(Device& s,Command* c,VkPipelineLayout layout,uint32_t first,uint32_t count,
    const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* offsets,const char* api="vkCmdBindDescriptorSets") {
    if(!c)return;
    c->descriptorApi=api;inspector_diagnostic(s,std::string("descriptor-api:")+api,"observed=1");
    auto l=find_ptr(s.layouts,id(layout));
    // Conservative disturbance: full layout signature mismatch invalidates the
    // old bindings. We never claim a stale binding survived an incompatible layout.
    for(auto& b:c->sets)if(!l || !b.layout || b.layout->signature!=l->signature)b={};
    uint32_t dynamicIndex=0;
    for(uint32_t i=0;i<count && first+i<maxSets;++i) {
        auto set=find_ptr(s.sets,id(sets[i]));c->sets[first+i]={id(sets[i]),set?set->generation:0,l,{}};
        auto& bound=c->sets[first+i];
        uint32_t needed=0;if(set && set->layout)for(auto& [b,v]:set->layout->bindings){(void)b;if(v.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || v.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)needed+=v.count;}
        if(needed<=64 && dynamicIndex<=dynamicCount && needed<=dynamicCount-dynamicIndex && (!needed || offsets)) {
            if(needed)bound.offsets.assign(offsets+dynamicIndex,offsets+dynamicIndex+needed);
        } else mark_unsupported(*c,"dynamic-offset-range");
        dynamicIndex+=needed;
    }
    if(dynamicIndex!=dynamicCount)mark_unsupported(*c,"dynamic-offset-count");
    c->dynamicOffsets.clear();if(dynamicCount<=64 && dynamicCount)c->dynamicOffsets.assign(offsets,offsets+dynamicCount);
    if(first+count>maxSets || dynamicCount>64)mark_unsupported(*c,"descriptor-bind-budget");
}
VKAPI_ATTR void VKAPI_CALL bindSets(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,
    const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* offsets) {
    auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdBindDescriptorSets>(*data,"vkCmdBindDescriptorSets")(cmd,point,layout,first,count,sets,dynamicCount,offsets);
    if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
    std::lock_guard lock(mutex);auto& s=device_state(data->device);bind_sets_impl(s,command(s,cmd),layout,first,count,sets,dynamicCount,offsets);
}
void push_impl(Device& s,Command& c,VkPipelineLayout layout,uint32_t set,uint32_t count,const VkWriteDescriptorSet* writes,const char* api="vkCmdPushDescriptorSetKHR") {
    c.descriptorApi=api;inspector_diagnostic(s,std::string("descriptor-api:")+api,"observed=1");
    if(set>=maxSets){mark_unsupported(c,"push-set-budget");return;}
    auto l=find_ptr(s.layouts,id(layout));
    for(auto& b:c.sets)if(!l || !b.layout || b.layout->signature!=l->signature)b={};
    auto copy=c.sets[set].pushed?std::make_shared<Set>(*c.sets[set].pushed):std::make_shared<Set>();
    copy->layout=l && set<l->sets.size()?l->sets[set]:nullptr;write_descriptors(s,*copy,count,writes);
    c.sets[set]={0,0,l,std::move(copy)};
}
VKAPI_ATTR void VKAPI_CALL pushDescriptors(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t set,uint32_t count,const VkWriteDescriptorSet* writes) {
    auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdPushDescriptorSetKHR>(*data,"vkCmdPushDescriptorSetKHR")(cmd,point,layout,set,count,writes);
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS){std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))push_impl(s,*c,layout,set,count,writes);}
}
void track_push_template(VkDevice d,VkCommandBuffer cmd,VkDescriptorUpdateTemplate t,VkPipelineLayout layout,uint32_t set,const void* values,const char* api="vkCmdPushDescriptorSetWithTemplateKHR") {
    std::lock_guard lock(mutex);auto& s=device_state(d);auto* c=command(s,cmd);if(!c)return;
    auto it=s.templates.find(id(t));
    if(it!=s.templates.end() && it->second.point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
    c->descriptorApi=api;inspector_diagnostic(s,std::string("descriptor-api:")+api,"observed=1");
    if(set>=maxSets){mark_unsupported(*c,"push-template-set-budget");return;}
    auto l=find_ptr(s.layouts,id(layout));auto copy=c->sets[set].pushed?std::make_shared<Set>(*c->sets[set].pushed):std::make_shared<Set>();
    for(auto& b:c->sets)if(!l || !b.layout || b.layout->signature!=l->signature)b={};
    if(!c->sets[set].pushed)copy=std::make_shared<Set>();
    copy->layout=l && set<l->sets.size()?l->sets[set]:nullptr;
    if(it==s.templates.end())copy->incomplete=true;else apply_template(s,*copy,it->second,values);
    c->sets[set]={0,0,l,std::move(copy)};
}
VKAPI_ATTR void VKAPI_CALL pushTemplate(VkCommandBuffer cmd,VkDescriptorUpdateTemplate t,VkPipelineLayout layout,uint32_t set,const void* values) {
    auto data=command_data(cmd);if(!data)return;
    next<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(*data,"vkCmdPushDescriptorSetWithTemplateKHR")(cmd,t,layout,set,values);
    track_push_template(data->device,cmd,t,layout,set,values);
}
void begin_pass(Device& s,Command& c,const VkRenderPassBeginInfo& info) {
    c.waterPassStart=c.events.size();c.passDraws=0;c.waterSeenInPass=false;++c.passOrdinal;fog_pass_begin(c);
    begin_target(s,c,info.renderPass,info.framebuffer,0);
    if(auto* v=chain<VkRenderPassAttachmentBeginInfo>(info.pNext,VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)) {
        c.attachments.clear();
        if(v->attachmentCount<=32)for(uint32_t i=0;i<v->attachmentCount;++i)c.attachments.push_back(find_ptr(s.views,id(v->pAttachments[i])));
        set_target(c,0);
    }
    if(c.pass && !c.pass->subpasses.empty()) {
        const auto i=c.pass->subpasses[0].depth;
        if(i<c.pass->attachments.size() && i<info.clearValueCount && c.pass->attachments[i].loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR) {
            c.target.clearKnown=true;c.target.clearDepth=info.pClearValues[i].depthStencil.depth;
        }
    }
    if(c.pass) {
        bool discarded=false;for(const auto& a:c.pass->attachments)discarded|=a.loadOp!=VK_ATTACHMENT_LOAD_OP_LOAD;
        if(discarded)invalidate_coverage(s,c,c.target,&info.renderArea);
    }

}
// The images a render pass instance is about to render into (first color, depth), for the fog's "is this still the world?" test.
void pass_images(Device& s,const VkRenderPassBeginInfo& info,const Image*& color,const Image*& depth) {
    color=depth=nullptr;auto pass=find_ptr(s.passes,id(info.renderPass));if(!pass || pass->subpasses.empty())return;
    std::vector<std::shared_ptr<View>> views;
    if(auto* v=chain<VkRenderPassAttachmentBeginInfo>(info.pNext,VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)){for(uint32_t i=0;i<v->attachmentCount && i<32;++i)views.push_back(find_ptr(s.views,id(v->pAttachments[i])));}
    else if(auto fb=s.framebuffers.find(id(info.framebuffer));fb!=s.framebuffers.end())views=fb->second.views;
    const auto& sub=pass->subpasses[0];
    if(!sub.colors.empty() && sub.colors[0]<views.size() && views[sub.colors[0]])color=views[sub.colors[0]]->image.get();
    if(sub.depth<views.size() && views[sub.depth])depth=views[sub.depth]->image.get();
}
void fog_before_render_pass(const DeviceData& data,VkCommandBuffer cmd,const VkRenderPassBeginInfo* info) {
    std::lock_guard lock(mutex);auto& s=device_state(data.device);if(!s.fog)return;auto* c=command(s,cmd);if(!c)return;
    if(s.fog->pending.active){const Image *color,*depth;pass_images(s,*info,color,depth);fog_before_pass_begin(data,s,*c,cmd,color,depth);}
    fog_flush_deferred(data,s,*c,cmd,"next pass");   // held draws never cross into another pass
}
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer cmd,const VkRenderPassBeginInfo* info,VkSubpassContents contents) {
    auto data=command_data(cmd);if(!data)return;
    fog_before_render_pass(*data,cmd,info);
    next<PFN_vkCmdBeginRenderPass>(*data,"vkCmdBeginRenderPass")(cmd,info,contents);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))begin_pass(s,*c,*info);
}
void beginPass2Impl(VkCommandBuffer cmd,const VkRenderPassBeginInfo* info,const VkSubpassBeginInfo* sub,const char* name) {
    auto data=command_data(cmd);if(!data)return;fog_before_render_pass(*data,cmd,info);next<PFN_vkCmdBeginRenderPass2>(*data,name)(cmd,info,sub);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))begin_pass(s,*c,*info);
}
VKAPI_ATTR void VKAPI_CALL beginPass2(VkCommandBuffer c,const VkRenderPassBeginInfo* i,const VkSubpassBeginInfo* s){beginPass2Impl(c,i,s,"vkCmdBeginRenderPass2");}
VKAPI_ATTR void VKAPI_CALL beginPass2KHR(VkCommandBuffer c,const VkRenderPassBeginInfo* i,const VkSubpassBeginInfo* s){beginPass2Impl(c,i,s,"vkCmdBeginRenderPass2KHR");}
VKAPI_ATTR void VKAPI_CALL nextPass(VkCommandBuffer cmd,VkSubpassContents contents) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdNextSubpass>(*data,"vkCmdNextSubpass")(cmd,contents);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))set_target(*c,c->target.subpass+1);
}
void nextPass2Impl(VkCommandBuffer cmd,const VkSubpassBeginInfo* b,const VkSubpassEndInfo* e,const char* name) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdNextSubpass2>(*data,name)(cmd,b,e);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd))set_target(*c,c->target.subpass+1);
}
VKAPI_ATTR void VKAPI_CALL nextPass2(VkCommandBuffer c,const VkSubpassBeginInfo* b,const VkSubpassEndInfo* e){nextPass2Impl(c,b,e,"vkCmdNextSubpass2");}
VKAPI_ATTR void VKAPI_CALL nextPass2KHR(VkCommandBuffer c,const VkSubpassBeginInfo* b,const VkSubpassEndInfo* e){nextPass2Impl(c,b,e,"vkCmdNextSubpass2KHR");}
void clear_target(const DeviceData& data,VkCommandBuffer cmd){std::lock_guard lock(mutex);auto& s=device_state(data.device);if(auto* c=command(s,cmd)){
    record_gpu_uniforms(data,s,*c,cmd);
    if(c->pass)for(size_t i=0;i<c->attachments.size() && i<c->pass->attachments.size();++i)if(auto v=c->attachments[i])layout_event(s,*c,v->image,v->range,c->pass->attachments[i].finalLayout,c->pass->supported);
    record_water_reflections(data,s,*c,cmd);
    // Blended effects held back over the water (fog_defer_candidate) go on top of the water composite now - unless the fog issues them after
    // its own composite (transparent_fog).
    if(!c->fogDeferred.empty() && !(s.fog && s.fog->settings.transparentFog && fog_enabled())) {
        if(s.water)s.water->perf.heldOverWater+=c->fogDeferred.size();
        fog_flush_deferred(data,s,*c,cmd,"after water");
    }
    fog_pass_end(data,s,*c,cmd);
    c->target={};c->pass.reset();c->attachments.clear();}}
VKAPI_ATTR void VKAPI_CALL endPass(VkCommandBuffer cmd){auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdEndRenderPass>(*d,"vkCmdEndRenderPass")(cmd);clear_target(*d,cmd);}
void endPass2Impl(VkCommandBuffer cmd,const VkSubpassEndInfo* end,const char* name){auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdEndRenderPass2>(*d,name)(cmd,end);clear_target(*d,cmd);}
VKAPI_ATTR void VKAPI_CALL endPass2(VkCommandBuffer c,const VkSubpassEndInfo* e){endPass2Impl(c,e,"vkCmdEndRenderPass2");}
VKAPI_ATTR void VKAPI_CALL endPass2KHR(VkCommandBuffer c,const VkSubpassEndInfo* e){endPass2Impl(c,e,"vkCmdEndRenderPass2KHR");}
void beginRenderingImpl(VkCommandBuffer cmd,const VkRenderingInfo* info,const char* name) {
    auto data=command_data(cmd);if(!data)return;
    {   // Before the new rendering instance starts: the fog may have to composite over the world it follows.
        std::lock_guard lock(mutex);auto& s=device_state(data->device);
        if(s.fog && s.fog->pending.active && !(info->flags&VK_RENDERING_RESUMING_BIT))if(auto* c=command(s,cmd)) {
            const Image* color=nullptr;const Image* depth=nullptr;
            if(info->colorAttachmentCount && info->pColorAttachments[0].imageView)if(auto v=find_ptr(s.views,id(info->pColorAttachments[0].imageView)))color=v->image.get();
            if(info->pDepthAttachment && info->pDepthAttachment->imageView)if(auto v=find_ptr(s.views,id(info->pDepthAttachment->imageView)))depth=v->image.get();
            fog_before_pass_begin(*data,s,*c,cmd,color,depth);
        }
        if(auto* c=command(s,cmd))fog_flush_deferred(*data,s,*c,cmd,"next pass");   // held draws never cross into another pass
    }
    next<PFN_vkCmdBeginRendering>(*data,name)(cmd,info);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);auto* c=command(s,cmd);if(!c)return;
    c->waterPassStart=c->events.size();c->passDraws=0;c->waterSeenInPass=false;++c->passOrdinal;fog_pass_begin(*c);c->target={};c->pass.reset();c->attachments.clear();c->target.width=info->renderArea.extent.width;c->target.height=info->renderArea.extent.height;
    c->target.known=info->colorAttachmentCount<=8 && !info->viewMask && !info->pNext && !(info->flags&(VK_RENDERING_RESUMING_BIT|VK_RENDERING_SUSPENDING_BIT));
    VkImageLayout firstColorLayout=VK_IMAGE_LAYOUT_UNDEFINED;bool firstColorPlain=false;
    for(uint32_t i=0;i<std::min(info->colorAttachmentCount,8u);++i) {
        const auto& a=info->pColorAttachments[i];if(!a.imageView)continue;
        auto view=find_ptr(s.views,id(a.imageView));c->target.colors.push_back(view);
        if(c->target.colors.size()==1){firstColorLayout=a.imageLayout;firstColorPlain=true;}
        // Structs chained onto an attachment (DXVK does) do not change imageLayout; the first observed types are logged once.
        if(a.pNext)inspector_diagnostic(s,"rendering-attachment-pnext","color types="+pnext_types(a.pNext));
        if(view)layout_event(s,*c,view->image,view->range,a.imageLayout,!a.pNext && !info->pNext);
        if(!view || a.resolveMode)c->target.known=false;
    }
    if(auto* a=info->pDepthAttachment;a && a->imageView) {
        c->target.depth=find_ptr(s.views,id(a->imageView));if(!c->target.depth || a->resolveMode)c->target.known=false;
        if(c->target.depth)layout_event(s,*c,c->target.depth->image,c->target.depth->range,a->imageLayout,!a->pNext && !info->pNext);
        c->target.clearKnown=a->loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR;c->target.clearDepth=a->clearValue.depthStencil.depth;
    }
    if(info->pDepthAttachment && info->pDepthAttachment->pNext)inspector_diagnostic(s,"rendering-attachment-pnext","depth types="+pnext_types(info->pDepthAttachment->pNext));
    if(c->target.known && c->target.colors.size()==1 && c->target.depth && info->pDepthAttachment && firstColorPlain) {
        c->target.renderingColorLayout=firstColorLayout;c->target.renderingDepthLayout=info->pDepthAttachment->imageLayout;c->target.renderingLayoutsKnown=true;
    }
    c->target.renderingArea=info->renderArea;c->target.renderingLayers=info->layerCount;
    if(auto* a=info->pStencilAttachment;a && a->imageView){c->target.renderingStencil=true;c->target.renderingStencilLayout=a->imageLayout;
        if(!c->target.depth || find_ptr(s.views,id(a->imageView))!=c->target.depth)c->target.renderingLayoutsKnown=false;}   // a separate stencil view: not re-creatable
    bool discarded=c->target.clearKnown;for(uint32_t i=0;i<info->colorAttachmentCount;++i)discarded|=info->pColorAttachments[i].loadOp!=VK_ATTACHMENT_LOAD_OP_LOAD;
    if(discarded)invalidate_coverage(s,*c,c->target,&info->renderArea);

}
VKAPI_ATTR void VKAPI_CALL beginRendering(VkCommandBuffer c,const VkRenderingInfo* i){beginRenderingImpl(c,i,"vkCmdBeginRendering");}
VKAPI_ATTR void VKAPI_CALL beginRenderingKHR(VkCommandBuffer c,const VkRenderingInfo* i){beginRenderingImpl(c,i,"vkCmdBeginRenderingKHR");}
void endRenderingImpl(VkCommandBuffer c,const char* name){auto d=command_data(c);if(!d)return;next<PFN_vkCmdEndRendering>(*d,name)(c);clear_target(*d,c);}
VKAPI_ATTR void VKAPI_CALL endRendering(VkCommandBuffer c){endRenderingImpl(c,"vkCmdEndRendering");}
VKAPI_ATTR void VKAPI_CALL endRenderingKHR(VkCommandBuffer c){endRenderingImpl(c,"vkCmdEndRenderingKHR");}
VKAPI_ATTR void VKAPI_CALL viewport(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkViewport* v) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdSetViewport>(*data,"vkCmdSetViewport")(cmd,first,count,v);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd)){if(first==0 && count){c->dynamic.viewport=v[0];c->dynamic.valid|=Viewport;c->waterRestore.viewport=v[0];c->waterRestore.set|=WaterRestore::Viewport;}if(first+count>1)mark_unsupported(*c,"multiple-viewports");}
}
VKAPI_ATTR void VKAPI_CALL scissor(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkRect2D* v) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdSetScissor>(*data,"vkCmdSetScissor")(cmd,first,count,v);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd)){if(first==0 && count){c->dynamic.scissor=v[0];c->dynamic.valid|=Scissor;c->waterRestore.scissor=v[0];c->waterRestore.set|=WaterRestore::Scissor;}if(first+count>1)mark_unsupported(*c,"multiple-scissors");}
}
// index = position in waterSimpleStates (water_reflections.cpp); isExt marks the EXT spelling of the entry point.
#define DYNAMIC(api,type,field,bit,index,isExt) \
VKAPI_ATTR void VKAPI_CALL api(VkCommandBuffer cmd,type value) { \
    auto data=command_data(cmd);if(!data)return;next<PFN_##api>(*data,#api)(cmd,value); \
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd)){c->dynamic.field=value;c->dynamic.valid|=bit; \
        auto& r=c->waterRestore;r.simple[index]=uint32_t(value);r.simpleSet|=1u<<(index);if(isExt)r.ext|=1u<<(16+index);else r.ext&=~(1u<<(16+index));} }
DYNAMIC(vkCmdSetDepthTestEnable,VkBool32,depthTest,DepthTest,0,false)
DYNAMIC(vkCmdSetDepthTestEnableEXT,VkBool32,depthTest,DepthTest,0,true)
DYNAMIC(vkCmdSetPrimitiveRestartEnable,VkBool32,restart,Restart,1,false)
DYNAMIC(vkCmdSetPrimitiveRestartEnableEXT,VkBool32,restart,Restart,1,true)
DYNAMIC(vkCmdSetDepthBoundsTestEnable,VkBool32,depthBounds,DepthBoundsEnable,2,false)
DYNAMIC(vkCmdSetDepthBoundsTestEnableEXT,VkBool32,depthBounds,DepthBoundsEnable,2,true)
DYNAMIC(vkCmdSetStencilTestEnable,VkBool32,stencil,Stencil,3,false)
DYNAMIC(vkCmdSetStencilTestEnableEXT,VkBool32,stencil,Stencil,3,true)
DYNAMIC(vkCmdSetDepthWriteEnable,VkBool32,depthWrite,DepthWrite,4,false)
DYNAMIC(vkCmdSetDepthWriteEnableEXT,VkBool32,depthWrite,DepthWrite,4,true)
DYNAMIC(vkCmdSetDepthCompareOp,VkCompareOp,compare,Compare,5,false)
DYNAMIC(vkCmdSetDepthCompareOpEXT,VkCompareOp,compare,Compare,5,true)
DYNAMIC(vkCmdSetCullMode,VkCullModeFlags,cull,Cull,6,false)
DYNAMIC(vkCmdSetCullModeEXT,VkCullModeFlags,cull,Cull,6,true)
DYNAMIC(vkCmdSetFrontFace,VkFrontFace,frontFace,FrontFace,7,false)
DYNAMIC(vkCmdSetFrontFaceEXT,VkFrontFace,frontFace,FrontFace,7,true)
DYNAMIC(vkCmdSetDepthBiasEnable,VkBool32,biasEnable,BiasEnable,8,false)
DYNAMIC(vkCmdSetDepthBiasEnableEXT,VkBool32,biasEnable,BiasEnable,8,true)
DYNAMIC(vkCmdSetPrimitiveTopology,VkPrimitiveTopology,topology,Topology,9,false)
DYNAMIC(vkCmdSetPrimitiveTopologyEXT,VkPrimitiveTopology,topology,Topology,9,true)
DYNAMIC(vkCmdSetRasterizerDiscardEnable,VkBool32,rasterDiscard,Discard,10,false)
DYNAMIC(vkCmdSetRasterizerDiscardEnableEXT,VkBool32,rasterDiscard,Discard,10,true)
#undef DYNAMIC
VKAPI_ATTR void VKAPI_CALL blendEnable(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkBool32* values) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdSetColorBlendEnableEXT>(*d,"vkCmdSetColorBlendEnableEXT")(cmd,first,count,values);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd)) {
        if(first==0 && count==1){c->dynamic.blend=values[0];c->dynamic.valid|=Blend;c->waterRestore.blendEnable=values[0];c->waterRestore.set|=WaterRestore::BlendEnable;}
        else{c->dynamic.valid&=~Blend;mark_unsupported(*c,"multiple-blend-attachments");}
    }
}
// The blend equation (which factors the water was blended with) and the write mask: recorded so the water replay can undo the blend exactly and put both back.
VKAPI_ATTR void VKAPI_CALL blendEquation(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkColorBlendEquationEXT* values) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdSetColorBlendEquationEXT>(*d,"vkCmdSetColorBlendEquationEXT")(cmd,first,count,values);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))if(first==0 && count>=1) {
        c->dynamic.blendSrc=values[0].srcColorBlendFactor;c->dynamic.blendDst=values[0].dstColorBlendFactor;c->dynamic.valid|=BlendEquation;
        c->waterRestore.blendEquation=values[0];c->waterRestore.set|=WaterRestore::BlendEquation;
    }
}
VKAPI_ATTR void VKAPI_CALL colorWriteMask(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkColorComponentFlags* values) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdSetColorWriteMaskEXT>(*d,"vkCmdSetColorWriteMaskEXT")(cmd,first,count,values);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))if(first==0 && count>=1){c->waterRestore.colorWriteMask=values[0];c->waterRestore.set|=WaterRestore::ColorWriteMask;
        c->dynamic.colorMask=values[0];c->dynamic.valid|=ColorMask;}
}
VKAPI_ATTR void VKAPI_CALL bias(VkCommandBuffer cmd,float constant,float clamp,float slope) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdSetDepthBias>(*d,"vkCmdSetDepthBias")(cmd,constant,clamp,slope);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd)){c->dynamic.biasConstant=constant;c->dynamic.biasClamp=clamp;c->dynamic.biasSlope=slope;c->dynamic.valid|=Bias;
        auto& r=c->waterRestore;r.biasConstant=constant;r.biasClamp=clamp;r.biasSlope=slope;r.set|=WaterRestore::Bias;}
}
VKAPI_ATTR void VKAPI_CALL waterBlendConstants(VkCommandBuffer cmd,const float* values) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdSetBlendConstants>(*data,"vkCmdSetBlendConstants")(cmd,values);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);
    if(auto* c=command(s,cmd)){std::copy_n(values,4,c->waterRestore.blendConstants);c->waterRestore.set|=WaterRestore::BlendConstants;}
}
VKAPI_ATTR void VKAPI_CALL waterLineWidth(VkCommandBuffer cmd,float value) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdSetLineWidth>(*data,"vkCmdSetLineWidth")(cmd,value);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd)){c->waterRestore.lineWidth=value;c->waterRestore.set|=WaterRestore::LineWidth;}
}
VKAPI_ATTR void VKAPI_CALL waterDepthBounds(VkCommandBuffer cmd,float low,float high) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdSetDepthBounds>(*data,"vkCmdSetDepthBounds")(cmd,low,high);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd)){c->waterRestore.depthBoundsLow=low;c->waterRestore.depthBoundsHigh=high;c->waterRestore.set|=WaterRestore::DepthBounds;}
}
VKAPI_ATTR void VKAPI_CALL water_vkCmdSetLogicOpEXT(VkCommandBuffer cmd,VkLogicOp value) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdSetLogicOpEXT>(*data,"vkCmdSetLogicOpEXT")(cmd,value);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd)){c->waterRestore.logicOp=value;c->waterRestore.set|=WaterRestore::LogicOp;}
}
// Front (1) and back (2) faces are kept separately; `kind` indexes stencilFaces (0 compare, 1 write, 2 reference).
#define WATER_STENCIL(api,kind,array,bit) \
VKAPI_ATTR void VKAPI_CALL water_##api(VkCommandBuffer cmd,VkStencilFaceFlags faces,uint32_t value) { \
    auto data=command_data(cmd);if(!data)return;next<PFN_##api>(*data,#api)(cmd,faces,value); \
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd)){auto& r=c->waterRestore; \
        for(unsigned f=0;f<2;++f)if(faces&(1u<<f)){r.array[f]=value;r.stencilFaces[kind]|=uint8_t(1u<<f);}r.set|=WaterRestore::bit;} }
WATER_STENCIL(vkCmdSetStencilCompareMask,0,stencilCompare,StencilCompare)
WATER_STENCIL(vkCmdSetStencilWriteMask,1,stencilWrite,StencilWrite)
WATER_STENCIL(vkCmdSetStencilReference,2,stencilReference,StencilReference)
#undef WATER_STENCIL
void waterStencilOpImpl(VkCommandBuffer cmd,VkStencilFaceFlags faces,VkStencilOp fail,VkStencilOp pass,VkStencilOp depthFail,VkCompareOp compare,const char* name,bool ext) {
    auto data=command_data(cmd);if(!data)return;next<PFN_vkCmdSetStencilOp>(*data,name)(cmd,faces,fail,pass,depthFail,compare);
    std::lock_guard lock(mutex);auto& s=device_state(data->device);if(auto* c=command(s,cmd)){auto& r=c->waterRestore;
        for(unsigned f=0;f<2;++f)if(faces&(1u<<f)){r.stencilOp[f]={fail,pass,depthFail,compare};r.stencilFaces[3]|=uint8_t(1u<<f);}
        r.set|=WaterRestore::StencilOp;if(ext)r.ext|=WaterRestore::StencilOp;else r.ext&=~uint32_t(WaterRestore::StencilOp);}
}
VKAPI_ATTR void VKAPI_CALL waterStencilOp(VkCommandBuffer c,VkStencilFaceFlags f,VkStencilOp a,VkStencilOp b,VkStencilOp d,VkCompareOp e){waterStencilOpImpl(c,f,a,b,d,e,"vkCmdSetStencilOp",false);}
VKAPI_ATTR void VKAPI_CALL waterStencilOpEXT(VkCommandBuffer c,VkStencilFaceFlags f,VkStencilOp a,VkStencilOp b,VkStencilOp d,VkCompareOp e){waterStencilOpImpl(c,f,a,b,d,e,"vkCmdSetStencilOpEXT",true);}
template<class Value> void withCount(VkCommandBuffer cmd,uint32_t count,const Value* values,const char* name,bool ext) {
    auto d=command_data(cmd);if(!d)return;
    using Function=void(VKAPI_PTR*)(VkCommandBuffer,uint32_t,const Value*);next<Function>(*d,name)(cmd,count,values);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd)) {
        if(count==1){auto& r=c->waterRestore;
            if constexpr(std::is_same_v<Value,VkViewport>){r.viewport=values[0];r.set|=WaterRestore::Viewport|WaterRestore::ViewportCount;if(ext)r.ext|=WaterRestore::ViewportCount;else r.ext&=~uint32_t(WaterRestore::ViewportCount);
                c->dynamic.viewport=values[0];c->dynamic.valid|=Viewport;}
            else{r.scissor=values[0];r.set|=WaterRestore::Scissor|WaterRestore::ScissorCount;if(ext)r.ext|=WaterRestore::ScissorCount;else r.ext&=~uint32_t(WaterRestore::ScissorCount);
                c->dynamic.scissor=values[0];c->dynamic.valid|=Scissor;}}
        else mark_unsupported(*c,"viewport-or-scissor-count");
    }
}
VKAPI_ATTR void VKAPI_CALL viewportCount(VkCommandBuffer c,uint32_t n,const VkViewport* v){withCount(c,n,v,"vkCmdSetViewportWithCount",false);}
VKAPI_ATTR void VKAPI_CALL viewportCountEXT(VkCommandBuffer c,uint32_t n,const VkViewport* v){withCount(c,n,v,"vkCmdSetViewportWithCountEXT",true);}
VKAPI_ATTR void VKAPI_CALL scissorCount(VkCommandBuffer c,uint32_t n,const VkRect2D* v){withCount(c,n,v,"vkCmdSetScissorWithCount",false);}
VKAPI_ATTR void VKAPI_CALL scissorCountEXT(VkCommandBuffer c,uint32_t n,const VkRect2D* v){withCount(c,n,v,"vkCmdSetScissorWithCountEXT",true);}
void append(Device& s,Command& c,Event&& event) {
    if(s.eventCount<maxEvents && c.events.size()<maxCommandEvents){c.events.push_back(std::move(event));++s.eventCount;}
    else{c.truncated=true;s.limits=true;}
}
VKAPI_ATTR void VKAPI_CALL execute(VkCommandBuffer cmd,uint32_t count,const VkCommandBuffer* secondaries) {
    auto d=command_data(cmd);if(!d)return;
    {std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))fog_flush_deferred(*d,s,*c,cmd,"execute commands");}
    next<PFN_vkCmdExecuteCommands>(*d,"vkCmdExecuteCommands")(cmd,count,secondaries);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);auto* c=command(s,cmd);if(!c)return;
    for(uint32_t i=0;i<count;++i) {
        auto* secondary=command(s,secondaries[i]);Event e;e.secondary=id(secondaries[i]);e.generation=secondary?secondary->generation:0;
        // An empty Draw carries the primary's attachment context at this exact
        // execution point. Secondary pipeline/descriptor state stays separate.
        e.draw=std::make_shared<Draw>();e.draw->target=c->target;append(s,*c,std::move(e));
    }
    c->pipeline.reset();c->sets={};c->dynamic={};c->dynamicOffsets.clear();
}
BufferBinding buffer_binding(Device& s,VkBuffer handle,VkDeviceSize offset,VkDeviceSize size=0,uint32_t stride=0) {
    auto b=find_ptr(s.buffers,id(handle));return {id(handle),b?b->generation:0,offset,size,stride,true};
}
VKAPI_ATTR void VKAPI_CALL vertexBuffers(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBindVertexBuffers>(*d,"vkCmdBindVertexBuffers")(cmd,first,count,buffers,offsets);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))for(uint32_t i=0;i<count && first+i<c->vertices.size();++i)c->vertices[first+i]=buffer_binding(s,buffers[i],offsets[i]);
}
void vertexBuffers2Impl(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets,const VkDeviceSize* sizes,const VkDeviceSize* strides,const char* name) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBindVertexBuffers2>(*d,name)(cmd,first,count,buffers,offsets,sizes,strides);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))for(uint32_t i=0;i<count && first+i<c->vertices.size();++i)c->vertices[first+i]=buffer_binding(s,buffers[i],offsets[i],sizes?sizes[i]:0,strides?uint32_t(strides[i]):c->vertices[first+i].stride);
}
VKAPI_ATTR void VKAPI_CALL vertexBuffers2(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer* b,const VkDeviceSize* o,const VkDeviceSize* z,const VkDeviceSize* t){vertexBuffers2Impl(c,f,n,b,o,z,t,"vkCmdBindVertexBuffers2");}
VKAPI_ATTR void VKAPI_CALL vertexBuffers2EXT(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer* b,const VkDeviceSize* o,const VkDeviceSize* z,const VkDeviceSize* t){vertexBuffers2Impl(c,f,n,b,o,z,t,"vkCmdBindVertexBuffers2EXT");}
VKAPI_ATTR void VKAPI_CALL indexBuffer(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset,VkIndexType type) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBindIndexBuffer>(*d,"vkCmdBindIndexBuffer")(cmd,buffer,offset,type);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd)){c->index=buffer_binding(s,buffer,offset);c->indexType=type;}
}
void indexBuffer2Impl(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize size,VkIndexType type,const char* name) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBindIndexBuffer2KHR>(*d,name)(cmd,buffer,offset,size,type);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd)){c->index=buffer_binding(s,buffer,offset,size);c->indexType=type;}
}
VKAPI_ATTR void VKAPI_CALL indexBuffer2(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize z,VkIndexType t){indexBuffer2Impl(c,b,o,z,t,"vkCmdBindIndexBuffer2");}
VKAPI_ATTR void VKAPI_CALL indexBuffer2KHR(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize z,VkIndexType t){indexBuffer2Impl(c,b,o,z,t,"vkCmdBindIndexBuffer2KHR");}
bool isolate(Device& s,const Command& c,const Draw& draw) {
    if(!s.isolation || std::chrono::steady_clock::now()>s.isolationDeadline){s.isolation=0;return false;}
    // Cached recordings must never get a persistent destructive filter. The
    // selected debug mode affects only ONE_TIME primary direct draws, within
    // the selected color attachment identity, outside queries/conditional/TF.
    if(c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY || !(c.usage&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) ||
       c.activeQueries || c.conditional || c.transformFeedback || draw.unsupported || draw.kind>1 ||
       !draw.pipeline || !draw.pipeline->isolationSafe || !draw.target.known || draw.target.colors.size()!=1)return false;
    const auto& color=draw.target.colors[0];
    if(!color || !color->image || !s.isolationTargets.count(color->image->generation))return false;
    for(auto state:draw.pipeline->dynamic) {
        if(state!=VK_DYNAMIC_STATE_VIEWPORT && state!=VK_DYNAMIC_STATE_SCISSOR && state!=VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT && state!=VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT &&
           state!=VK_DYNAMIC_STATE_DEPTH_BIAS && state!=VK_DYNAMIC_STATE_BLEND_CONSTANTS && state!=VK_DYNAMIC_STATE_DEPTH_BOUNDS && state!=VK_DYNAMIC_STATE_STENCIL_REFERENCE &&
           state!=VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE && state!=VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE && state!=VK_DYNAMIC_STATE_DEPTH_COMPARE_OP &&
           state!=VK_DYNAMIC_STATE_CULL_MODE && state!=VK_DYNAMIC_STATE_FRONT_FACE && state!=VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY && state!=VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE)return false;
    }
    return s.isolation==1 ? draw.pipeline->serial==s.selected : draw.pipeline->serial!=s.selected;
}
}   // namespace (these two are declared in discovery_internal.hpp)
bool draw_records_wanted(const Device& s) {
    static const bool explorer=[]{const char* e=std::getenv("DEPTH_PROBE_PIPELINE_EXPLORER");return e && !std::strcmp(e,"1");}();
    return explorer || s.capture || s.requestedFrames || s.isolation || s.frameSerial<s.explorerUntil ||
           (s.inspector && (s.inspector->armed || s.inspector->highlight)) ||
           scene_debugger_full_capture(s);   // same opt-in cost class as Inspector armed/highlight above
}
bool water_wants_draw(const Device& s,const Pipeline* p) {
    if(!s.water || !s.water->mode || !s.water->selectedVS || !p || !p->vs || !p->fs)return false;
    return s.water->mode==2 || water_draw_shaders(*s.water,*p);   // planar re-draws the opaque scene; SSR only needs the water tiles (and their VS variants)
}
namespace {
// recorded: false when the draw was not snapshotted (nothing wants it), so the highlight tint must not look at the previous draw.
bool observe(const DeviceData& data,VkCommandBuffer cmd,uint32_t kind,uint32_t count,uint32_t instances,uint32_t first,int32_t offset,uint32_t firstInstance,bool* recorded=nullptr) {
    if(recorded)*recorded=false;
    std::lock_guard lock(mutex);auto& s=device_state(data.device);auto* c=command(s,cmd);if(!c || !c->recording)return false;
    ++c->passDraws;fog_observe_draw(s,*c);game_lights_observe_draw(s,*c);game_fog_observe_draw(s,*c);soft_shadows_observe_draw(s,*c);
    DrawFacts facts;const DrawSemantic semantic=draw_classes_enabled()?classify_recorded(s,*c,&facts):DrawSemantic{};
    if(!c->waterSeenInPass && s.water && s.water->mode && c->pipeline && water_draw_shaders(*s.water,*c->pipeline))c->waterSeenInPass=true;
    // Transparent-aware fog: a particle / glow may be held back and issued after the fog composite (volumetric_fog.cpp).
    const bool deferCandidate=draw_classes_enabled() && fog_defer_candidate(s,*c,semantic,facts,kind,count,first,offset);
    c->lastGeometry={kind==1?c->index.buffer:0,c->vertices[0].buffer,c->vertices[0].offset,kind,first,count,offset,true};
    if(!deferCandidate && !draw_records_wanted(s) && !water_wants_draw(s,c->pipeline.get()) && !scene_debugger_peek_draw(s,*c)){++c->ordinal;return false;}
    if(s.eventCount>=maxEvents || c->events.size()>=maxCommandEvents){c->truncated=true;s.limits=true;return false;}
    if(recorded)*recorded=true;
    auto draw=std::make_shared<Draw>();draw->pipeline=c->pipeline;draw->target=c->target;draw->dynamic=c->dynamic;draw->sets=c->sets;
    draw->vertices=c->vertices;draw->index=c->index;draw->indexType=c->indexType;draw->commandGeneration=c->generation;
    draw->push=c->push;draw->dynamicOffsets=c->dynamicOffsets;draw->command=id(cmd);draw->ordinal=++c->ordinal;draw->kind=kind;draw->count=count;
    draw->instances=instances;draw->first=first;draw->vertexOffset=offset;draw->firstInstance=firstInstance;draw->unsupported=c->unsupported;draw->unsupportedReason=c->unsupportedReason;draw->descriptorApi=c->descriptorApi;
    draw->waterReplayUnsafe=c->activeQueries || c->conditional || c->transformFeedback;draw->semantic=semantic;draw->passOrdinal=c->passOrdinal;
    if(deferCandidate && fog_defer_draw(s,*c,draw)){append(s,*c,{std::move(draw),0,0,{}});return true;}   // held: not forwarded now
    draw->skipped=isolate(s,*c,*draw);inspector_observe_draw(data,s,*c,*draw,cmd);scene_debugger_observe_draw(data,s,*c,*draw,cmd);bool skip=draw->skipped;append(s,*c,{std::move(draw),0,0,{}});return skip;
}
void begin_tint(const DeviceData& data,VkCommandBuffer cmd,VkPipeline& original,VkPipeline& tint) {
    if(!inspector_enabled())return;
    std::lock_guard lock(mutex);auto& s=device_state(data.device);auto* c=command(s,cmd);
    if(!c || c->events.empty() || c->level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY || !(c->usage&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) || c->activeQueries || c->conditional || c->transformFeedback)return;
    auto d=c->events.back().draw;if(!d || !d->pipeline)return;
    tint=inspector_tint(data,s,*d);original=(VkPipeline)d->pipeline->handle;
    if(tint)next<PFN_vkCmdBindPipeline>(data,"vkCmdBindPipeline")(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,tint);
}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer cmd,uint32_t count,uint32_t instances,uint32_t first,uint32_t firstInstance) {
    g_perf.draws.fetch_add(1,std::memory_order_relaxed);
    auto d=command_data(cmd);if(!d)return;
    bool recorded=false;
    if(!observe(*d,cmd,0,count,instances,first,0,firstInstance,&recorded)) {
        VkPipeline original{},tint{};if(recorded)begin_tint(*d,cmd,original,tint);
        next<PFN_vkCmdDraw>(*d,"vkCmdDraw")(cmd,count,instances,first,firstInstance);
        if(tint)next<PFN_vkCmdBindPipeline>(*d,"vkCmdBindPipeline")(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);
    }
}
VKAPI_ATTR void VKAPI_CALL indexed(VkCommandBuffer cmd,uint32_t count,uint32_t instances,uint32_t first,int32_t offset,uint32_t firstInstance) {
    g_perf.draws.fetch_add(1,std::memory_order_relaxed);
    auto d=command_data(cmd);if(!d)return;
    bool recorded=false;
    if(!observe(*d,cmd,1,count,instances,first,offset,firstInstance,&recorded)) {
        VkPipeline original{},tint{};if(recorded)begin_tint(*d,cmd,original,tint);
        next<PFN_vkCmdDrawIndexed>(*d,"vkCmdDrawIndexed")(cmd,count,instances,first,offset,firstInstance);
        if(tint)next<PFN_vkCmdBindPipeline>(*d,"vkCmdBindPipeline")(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);
    }
}
VKAPI_ATTR void VKAPI_CALL indirect(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset,uint32_t count,uint32_t stride) {
    auto d=command_data(cmd);if(!d)return;observe(*d,cmd,2,count,0,0,0,0);next<PFN_vkCmdDrawIndirect>(*d,"vkCmdDrawIndirect")(cmd,buffer,offset,count,stride);
}
VKAPI_ATTR void VKAPI_CALL indexedIndirect(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset,uint32_t count,uint32_t stride) {
    auto d=command_data(cmd);if(!d)return;observe(*d,cmd,3,count,0,0,0,0);next<PFN_vkCmdDrawIndexedIndirect>(*d,"vkCmdDrawIndexedIndirect")(cmd,buffer,offset,count,stride);
}
void indirectCountImpl(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize offset,VkBuffer countBuffer,VkDeviceSize countOffset,uint32_t max,uint32_t stride,const char* name,bool indexed) {
    auto d=command_data(cmd);if(!d)return;observe(*d,cmd,indexed?5:4,max,0,0,0,0);
    next<PFN_vkCmdDrawIndirectCount>(*d,name)(cmd,buffer,offset,countBuffer,countOffset,max,stride);
}
#define INDIRECT_COUNT(api,indexed) VKAPI_ATTR void VKAPI_CALL api(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkBuffer cb,VkDeviceSize co,uint32_t n,uint32_t s){indirectCountImpl(c,b,o,cb,co,n,s,#api,indexed);}
INDIRECT_COUNT(vkCmdDrawIndirectCount,false)
INDIRECT_COUNT(vkCmdDrawIndirectCountKHR,false)
INDIRECT_COUNT(vkCmdDrawIndexedIndirectCount,true)
INDIRECT_COUNT(vkCmdDrawIndexedIndirectCountKHR,true)
#undef INDIRECT_COUNT
VKAPI_ATTR void VKAPI_CALL beginQuery(VkCommandBuffer cmd,VkQueryPool pool,uint32_t index,VkQueryControlFlags flags) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBeginQuery>(*d,"vkCmdBeginQuery")(cmd,pool,index,flags);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))++c->activeQueries;
}
VKAPI_ATTR void VKAPI_CALL endQuery(VkCommandBuffer cmd,VkQueryPool pool,uint32_t index) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdEndQuery>(*d,"vkCmdEndQuery")(cmd,pool,index);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd);c && c->activeQueries)--c->activeQueries;
}
VKAPI_ATTR void VKAPI_CALL beginQueryIndexed(VkCommandBuffer cmd,VkQueryPool pool,uint32_t index,VkQueryControlFlags flags,uint32_t stream) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBeginQueryIndexedEXT>(*d,"vkCmdBeginQueryIndexedEXT")(cmd,pool,index,flags,stream);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))++c->activeQueries;
}
VKAPI_ATTR void VKAPI_CALL endQueryIndexed(VkCommandBuffer cmd,VkQueryPool pool,uint32_t index,uint32_t stream) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdEndQueryIndexedEXT>(*d,"vkCmdEndQueryIndexedEXT")(cmd,pool,index,stream);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd);c && c->activeQueries)--c->activeQueries;
}
VKAPI_ATTR void VKAPI_CALL conditional(VkCommandBuffer cmd,const VkConditionalRenderingBeginInfoEXT* info) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBeginConditionalRenderingEXT>(*d,"vkCmdBeginConditionalRenderingEXT")(cmd,info);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))c->conditional=true;
}
VKAPI_ATTR void VKAPI_CALL endConditional(VkCommandBuffer cmd) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdEndConditionalRenderingEXT>(*d,"vkCmdEndConditionalRenderingEXT")(cmd);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))c->conditional=false;
}
void transformFeedbackImpl(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets,const char* name,bool active) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBeginTransformFeedbackEXT>(*d,name)(cmd,first,count,buffers,offsets);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))c->transformFeedback=active;
}
VKAPI_ATTR void VKAPI_CALL beginTF(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer* b,const VkDeviceSize* o){transformFeedbackImpl(c,f,n,b,o,"vkCmdBeginTransformFeedbackEXT",true);}
VKAPI_ATTR void VKAPI_CALL endTF(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer* b,const VkDeviceSize* o){transformFeedbackImpl(c,f,n,b,o,"vkCmdEndTransformFeedbackEXT",false);}
VKAPI_ATTR void VKAPI_CALL descriptorBuffers(VkCommandBuffer cmd,uint32_t count,const VkDescriptorBufferBindingInfoEXT* info) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBindDescriptorBuffersEXT>(*d,"vkCmdBindDescriptorBuffersEXT")(cmd,count,info);
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd)){mark_unsupported(*c,"descriptor-buffer");c->sets={};c->descriptorApi="vkCmdBindDescriptorBuffersEXT";inspector_diagnostic(s,"descriptor-api:vkCmdBindDescriptorBuffersEXT","unsupported=descriptor-buffer");}
}
VKAPI_ATTR void VKAPI_CALL bindShaders(VkCommandBuffer cmd,uint32_t count,const VkShaderStageFlagBits* stages,const VkShaderEXT* shaders) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBindShadersEXT>(*d,"vkCmdBindShadersEXT")(cmd,count,stages,shaders);
    for(uint32_t i=0;i<count;++i)if(stages[i]&VK_SHADER_STAGE_ALL_GRAPHICS) {
        std::lock_guard lock(mutex);auto& s=device_state(d->device);
        if(auto* c=command(s,cmd)){c->pipeline.reset();mark_unsupported(*c,"shader-object");}
        break;
    }
}
// New binding APIs share the known-layout tracker. Unsupported dynamic layout
// chains discard the previous snapshot; stale textures are never attributed.
void unknownDescriptors(VkDevice device,VkCommandBuffer cmd,const char* api,const void* pNext,VkPipelineLayout layout) {
    std::lock_guard lock(mutex);auto& s=device_state(device);
    if(auto* c=command(s,cmd)){c->sets={};c->dynamicOffsets.clear();c->descriptorApi=api;mark_unsupported(*c,"descriptor-dynamic-layout");}
    inspector_diagnostic(s,std::string("descriptor-api:")+api+":unknown","pNext="+pnext_types(pNext)+" layout="+hex(id(layout)));
}
VKAPI_ATTR void VKAPI_CALL pushCore(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t set,uint32_t count,const VkWriteDescriptorSet* writes) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdPushDescriptorSetKHR>(*d,"vkCmdPushDescriptorSet")(cmd,point,layout,set,count,writes);
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS){std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))push_impl(s,*c,layout,set,count,writes,"vkCmdPushDescriptorSet");}
}
VKAPI_ATTR void VKAPI_CALL pushTemplateCore(VkCommandBuffer cmd,VkDescriptorUpdateTemplate t,VkPipelineLayout layout,uint32_t set,const void* data) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(*d,"vkCmdPushDescriptorSetWithTemplate")(cmd,t,layout,set,data);
    track_push_template(d->device,cmd,t,layout,set,data,"vkCmdPushDescriptorSetWithTemplate");
}
#ifdef VK_KHR_maintenance6
void bind2Impl(VkCommandBuffer cmd,const VkBindDescriptorSetsInfoKHR* info,const char* name) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdBindDescriptorSets2KHR>(*d,name)(cmd,info);
    if(!(info->stageFlags&VK_SHADER_STAGE_ALL_GRAPHICS))return;
    if(info->pNext || !info->layout){unknownDescriptors(d->device,cmd,name,info->pNext,info->layout);return;}
    std::lock_guard lock(mutex);auto& s=device_state(d->device);
    bind_sets_impl(s,command(s,cmd),info->layout,info->firstSet,info->descriptorSetCount,info->pDescriptorSets,info->dynamicOffsetCount,info->pDynamicOffsets,name);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2(VkCommandBuffer cmd,const VkBindDescriptorSetsInfoKHR* info){bind2Impl(cmd,info,"vkCmdBindDescriptorSets2");}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2KHR(VkCommandBuffer cmd,const VkBindDescriptorSetsInfoKHR* info){bind2Impl(cmd,info,"vkCmdBindDescriptorSets2KHR");}
void push2Impl(VkCommandBuffer cmd,const VkPushDescriptorSetInfoKHR* info,const char* name) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdPushDescriptorSet2KHR>(*d,name)(cmd,info);
    if(!(info->stageFlags&VK_SHADER_STAGE_ALL_GRAPHICS))return;
    if(info->pNext || !info->layout){unknownDescriptors(d->device,cmd,name,info->pNext,info->layout);return;}
    std::lock_guard lock(mutex);auto& s=device_state(d->device);if(auto* c=command(s,cmd))push_impl(s,*c,info->layout,info->set,info->descriptorWriteCount,info->pDescriptorWrites,name);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet2(VkCommandBuffer cmd,const VkPushDescriptorSetInfoKHR* info){push2Impl(cmd,info,"vkCmdPushDescriptorSet2");}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet2KHR(VkCommandBuffer cmd,const VkPushDescriptorSetInfoKHR* info){push2Impl(cmd,info,"vkCmdPushDescriptorSet2KHR");}
void pushTemplate2Impl(VkCommandBuffer cmd,const VkPushDescriptorSetWithTemplateInfoKHR* info,const char* name) {
    auto d=command_data(cmd);if(!d)return;next<PFN_vkCmdPushDescriptorSetWithTemplate2KHR>(*d,name)(cmd,info);
    if(info->pNext || !info->layout){unknownDescriptors(d->device,cmd,name,info->pNext,info->layout);return;}
    track_push_template(d->device,cmd,info->descriptorUpdateTemplate,info->layout,info->set,info->pData,name);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplate2(VkCommandBuffer c,const VkPushDescriptorSetWithTemplateInfoKHR* i){pushTemplate2Impl(c,i,"vkCmdPushDescriptorSetWithTemplate2");}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplate2KHR(VkCommandBuffer c,const VkPushDescriptorSetWithTemplateInfoKHR* i){pushTemplate2Impl(c,i,"vkCmdPushDescriptorSetWithTemplate2KHR");}

#endif
void unobserved_command(VkDevice device,VkCommandBuffer cmd,bool transfer,bool changesState=false,const char* api="unobserved-command") {
    if(!inspector_enabled())return;
    std::lock_guard lock(mutex);auto& s=device_state(device);auto* c=command(s,cmd);if(!c)return;
    if(transfer) {
        if(s.inspector && s.inspector->armed){auto draw=std::make_shared<Draw>();draw->kind=7;draw->unsupported=true;append(s,*c,{std::move(draw),0,0,{}});}
    } else invalidate_coverage(s,*c,c->target);
    if(changesState){c->pipeline.reset();c->sets={};c->dynamic={};c->push={};mark_unsupported(*c,api);inspector_diagnostic(s,std::string("unobserved-state:")+api,"unsupported=1");}
}
// Before a transfer touches the world's color/depth targets, the fog composites over them (e.g. WoW's glow copy).
void fog_transfer(const DeviceData& data,VkCommandBuffer cmd,VkImage a,VkImage b,const char* what) {
    std::lock_guard lock(mutex);auto& s=device_state(data.device);if(!s.fog)return;auto* c=command(s,cmd);if(!c)return;
    if(s.fog->pending.active)fog_before_transfer(data,s,*c,cmd,a,b,what);
    if(fog_deferred_target(*c,id(a)) || fog_deferred_target(*c,id(b)))fog_flush_deferred(data,s,*c,cmd,what);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy* pRegions) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,srcImage,dstImage,"vkCmdCopyImage");next<PFN_vkCmdCopyImage>(*data,"vkCmdCopyImage")(commandBuffer,srcImage,srcImageLayout,dstImage,dstImageLayout,regionCount,pRegions);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy* pRegions) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,VkImage{},dstImage,"vkCmdCopyBufferToImage");next<PFN_vkCmdCopyBufferToImage>(*data,"vkCmdCopyBufferToImage")(commandBuffer,srcBuffer,dstImage,dstImageLayout,regionCount,pRegions);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearColorValue* pColor, uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,image,VkImage{},"vkCmdClearColorImage");next<PFN_vkCmdClearColorImage>(*data,"vkCmdClearColorImage")(commandBuffer,image,imageLayout,pColor,rangeCount,pRanges);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,srcImage,dstImage,"vkCmdBlitImage");next<PFN_vkCmdBlitImage>(*data,"vkCmdBlitImage")(commandBuffer,srcImage,srcImageLayout,dstImage,dstImageLayout,regionCount,pRegions,filter);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearDepthStencilValue* pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,image,VkImage{},"vkCmdClearDepthStencilImage");next<PFN_vkCmdClearDepthStencilImage>(*data,"vkCmdClearDepthStencilImage")(commandBuffer,image,imageLayout,pDepthStencil,rangeCount,pRanges);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageResolve* pRegions) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,srcImage,dstImage,"vkCmdResolveImage");next<PFN_vkCmdResolveImage>(*data,"vkCmdResolveImage")(commandBuffer,srcImage,srcImageLayout,dstImage,dstImageLayout,regionCount,pRegions);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,pCopyImageInfo->srcImage,pCopyImageInfo->dstImage,"vkCmdCopyImage2");next<PFN_vkCmdCopyImage2>(*data,"vkCmdCopyImage2")(commandBuffer,pCopyImageInfo);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdCopyBufferToImage2(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,VkImage{},pCopyBufferToImageInfo->dstImage,"vkCmdCopyBufferToImage2");next<PFN_vkCmdCopyBufferToImage2>(*data,"vkCmdCopyBufferToImage2")(commandBuffer,pCopyBufferToImageInfo);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,pBlitImageInfo->srcImage,pBlitImageInfo->dstImage,"vkCmdBlitImage2");next<PFN_vkCmdBlitImage2>(*data,"vkCmdBlitImage2")(commandBuffer,pBlitImageInfo);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdResolveImage2(VkCommandBuffer commandBuffer, const VkResolveImageInfo2* pResolveImageInfo) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,pResolveImageInfo->srcImage,pResolveImageInfo->dstImage,"vkCmdResolveImage2");next<PFN_vkCmdResolveImage2>(*data,"vkCmdResolveImage2")(commandBuffer,pResolveImageInfo);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdCopyImage2KHR(VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,pCopyImageInfo->srcImage,pCopyImageInfo->dstImage,"vkCmdCopyImage2KHR");next<PFN_vkCmdCopyImage2KHR>(*data,"vkCmdCopyImage2KHR")(commandBuffer,pCopyImageInfo);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdCopyBufferToImage2KHR(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,VkImage{},pCopyBufferToImageInfo->dstImage,"vkCmdCopyBufferToImage2KHR");next<PFN_vkCmdCopyBufferToImage2KHR>(*data,"vkCmdCopyBufferToImage2KHR")(commandBuffer,pCopyBufferToImageInfo);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdBlitImage2KHR(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,pBlitImageInfo->srcImage,pBlitImageInfo->dstImage,"vkCmdBlitImage2KHR");next<PFN_vkCmdBlitImage2KHR>(*data,"vkCmdBlitImage2KHR")(commandBuffer,pBlitImageInfo);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdResolveImage2KHR(VkCommandBuffer commandBuffer, const VkResolveImageInfo2* pResolveImageInfo) {
    auto data=command_data(commandBuffer);if(!data)return;fog_transfer(*data,commandBuffer,pResolveImageInfo->srcImage,pResolveImageInfo->dstImage,"vkCmdResolveImage2KHR");next<PFN_vkCmdResolveImage2KHR>(*data,"vkCmdResolveImage2KHR")(commandBuffer,pResolveImageInfo);
    unobserved_command(data->device,commandBuffer,true);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer, uint32_t instanceCount, uint32_t firstInstance, VkBuffer counterBuffer, VkDeviceSize counterBufferOffset, uint32_t counterOffset, uint32_t vertexStride) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawIndirectByteCountEXT>(*data,"vkCmdDrawIndirectByteCountEXT")(commandBuffer,instanceCount,firstInstance,counterBuffer,counterBufferOffset,counterOffset,vertexStride);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawIndirectCountAMD(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawIndirectCountAMD>(*data,"vkCmdDrawIndirectCountAMD")(commandBuffer,buffer,offset,countBuffer,countBufferOffset,maxDrawCount,stride);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawIndexedIndirectCountAMD(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawIndexedIndirectCountAMD>(*data,"vkCmdDrawIndexedIndirectCountAMD")(commandBuffer,buffer,offset,countBuffer,countBufferOffset,maxDrawCount,stride);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawMeshTasksNV(VkCommandBuffer commandBuffer, uint32_t taskCount, uint32_t firstTask) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawMeshTasksNV>(*data,"vkCmdDrawMeshTasksNV")(commandBuffer,taskCount,firstTask);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawMeshTasksIndirectNV(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawMeshTasksIndirectNV>(*data,"vkCmdDrawMeshTasksIndirectNV")(commandBuffer,buffer,offset,drawCount,stride);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawMeshTasksIndirectCountNV(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawMeshTasksIndirectCountNV>(*data,"vkCmdDrawMeshTasksIndirectCountNV")(commandBuffer,buffer,offset,countBuffer,countBufferOffset,maxDrawCount,stride);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdExecuteGeneratedCommandsNV(VkCommandBuffer commandBuffer, VkBool32 isPreprocessed, const VkGeneratedCommandsInfoNV* pGeneratedCommandsInfo) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdExecuteGeneratedCommandsNV>(*data,"vkCmdExecuteGeneratedCommandsNV")(commandBuffer,isPreprocessed,pGeneratedCommandsInfo);
    unobserved_command(data->device,commandBuffer,false,true,"vkCmdExecuteGeneratedCommandsNV");
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawMultiEXT(VkCommandBuffer commandBuffer, uint32_t drawCount, const VkMultiDrawInfoEXT* pVertexInfo, uint32_t instanceCount, uint32_t firstInstance, uint32_t stride) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawMultiEXT>(*data,"vkCmdDrawMultiEXT")(commandBuffer,drawCount,pVertexInfo,instanceCount,firstInstance,stride);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawMultiIndexedEXT(VkCommandBuffer commandBuffer, uint32_t drawCount, const VkMultiDrawIndexedInfoEXT* pIndexInfo, uint32_t instanceCount, uint32_t firstInstance, uint32_t stride, const int32_t* pVertexOffset) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawMultiIndexedEXT>(*data,"vkCmdDrawMultiIndexedEXT")(commandBuffer,drawCount,pIndexInfo,instanceCount,firstInstance,stride,pVertexOffset);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawClusterHUAWEI(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawClusterHUAWEI>(*data,"vkCmdDrawClusterHUAWEI")(commandBuffer,groupCountX,groupCountY,groupCountZ);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawClusterIndirectHUAWEI(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawClusterIndirectHUAWEI>(*data,"vkCmdDrawClusterIndirectHUAWEI")(commandBuffer,buffer,offset);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdExecuteGeneratedCommandsEXT(VkCommandBuffer commandBuffer, VkBool32 isPreprocessed, const VkGeneratedCommandsInfoEXT* pGeneratedCommandsInfo) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdExecuteGeneratedCommandsEXT>(*data,"vkCmdExecuteGeneratedCommandsEXT")(commandBuffer,isPreprocessed,pGeneratedCommandsInfo);
    unobserved_command(data->device,commandBuffer,false,true,"vkCmdExecuteGeneratedCommandsEXT");
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawMeshTasksEXT(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawMeshTasksEXT>(*data,"vkCmdDrawMeshTasksEXT")(commandBuffer,groupCountX,groupCountY,groupCountZ);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawMeshTasksIndirectEXT(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawMeshTasksIndirectEXT>(*data,"vkCmdDrawMeshTasksIndirectEXT")(commandBuffer,buffer,offset,drawCount,stride);
    unobserved_command(data->device,commandBuffer,false);
}
VKAPI_ATTR void VKAPI_CALL unsupported_vkCmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    auto data=command_data(commandBuffer);if(!data)return;next<PFN_vkCmdDrawMeshTasksIndirectCountEXT>(*data,"vkCmdDrawMeshTasksIndirectCountEXT")(commandBuffer,buffer,offset,countBuffer,countBufferOffset,maxDrawCount,stride);
    unobserved_command(data->device,commandBuffer,false);
}

}
PFN_vkVoidFunction command_hook(const char* name) {
#define HOOK(api,fn) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(fn)
    HOOK(vkCreateCommandPool,createPool);
    HOOK(vkAllocateCommandBuffers,allocate);HOOK(vkFreeCommandBuffers,freeCommands);HOOK(vkBeginCommandBuffer,begin);HOOK(vkEndCommandBuffer,end);HOOK(vkResetCommandBuffer,reset);
    HOOK(vkDestroyCommandPool,destroyPool);HOOK(vkResetCommandPool,resetPool);HOOK(vkCmdBindPipeline,bindPipeline);HOOK(vkCmdPushConstants,pushConstants);
#ifdef VK_KHR_maintenance6
    HOOK(vkCmdPushConstants2,pushConstants2);HOOK(vkCmdPushConstants2KHR,pushConstants2KHR);
#endif
HOOK(vkCmdBindDescriptorSets,bindSets);
    HOOK(vkCmdPushDescriptorSetKHR,pushDescriptors);HOOK(vkCmdPushDescriptorSetWithTemplateKHR,pushTemplate);
    HOOK(vkCmdPushDescriptorSet,pushCore);HOOK(vkCmdPushDescriptorSetWithTemplate,pushTemplateCore);
#ifdef VK_KHR_maintenance6
    HOOK(vkCmdBindDescriptorSets2,vkCmdBindDescriptorSets2);HOOK(vkCmdBindDescriptorSets2KHR,vkCmdBindDescriptorSets2KHR);
    HOOK(vkCmdPushDescriptorSet2,vkCmdPushDescriptorSet2);HOOK(vkCmdPushDescriptorSet2KHR,vkCmdPushDescriptorSet2KHR);
    HOOK(vkCmdPushDescriptorSetWithTemplate2,vkCmdPushDescriptorSetWithTemplate2);HOOK(vkCmdPushDescriptorSetWithTemplate2KHR,vkCmdPushDescriptorSetWithTemplate2KHR);
#endif
    HOOK(vkCmdBeginRenderPass,beginPass);HOOK(vkCmdBeginRenderPass2,beginPass2);HOOK(vkCmdBeginRenderPass2KHR,beginPass2KHR);
    HOOK(vkCmdNextSubpass,nextPass);HOOK(vkCmdNextSubpass2,nextPass2);HOOK(vkCmdNextSubpass2KHR,nextPass2KHR);
    HOOK(vkCmdEndRenderPass,endPass);HOOK(vkCmdEndRenderPass2,endPass2);HOOK(vkCmdEndRenderPass2KHR,endPass2KHR);
    HOOK(vkCmdBeginRendering,beginRendering);HOOK(vkCmdBeginRenderingKHR,beginRenderingKHR);HOOK(vkCmdEndRendering,endRendering);HOOK(vkCmdEndRenderingKHR,endRenderingKHR);
    HOOK(vkCmdSetPrimitiveRestartEnable,vkCmdSetPrimitiveRestartEnable);
    HOOK(vkCmdSetPrimitiveRestartEnableEXT,vkCmdSetPrimitiveRestartEnableEXT);
    HOOK(vkCmdSetDepthBoundsTestEnable,vkCmdSetDepthBoundsTestEnable);
    HOOK(vkCmdSetDepthBoundsTestEnableEXT,vkCmdSetDepthBoundsTestEnableEXT);
    HOOK(vkCmdSetLogicOpEXT,water_vkCmdSetLogicOpEXT);
    HOOK(vkCmdSetStencilCompareMask,water_vkCmdSetStencilCompareMask);
    HOOK(vkCmdSetStencilWriteMask,water_vkCmdSetStencilWriteMask);
    HOOK(vkCmdSetStencilReference,water_vkCmdSetStencilReference);
    HOOK(vkCmdSetStencilOp,waterStencilOp);HOOK(vkCmdSetStencilOpEXT,waterStencilOpEXT);
    HOOK(vkCmdSetBlendConstants,waterBlendConstants);HOOK(vkCmdSetLineWidth,waterLineWidth);HOOK(vkCmdSetDepthBounds,waterDepthBounds);
    HOOK(vkCmdSetViewport,viewport);HOOK(vkCmdSetScissor,scissor);HOOK(vkCmdSetDepthBias,bias);
    HOOK(vkCmdSetViewportWithCount,viewportCount);HOOK(vkCmdSetViewportWithCountEXT,viewportCountEXT);HOOK(vkCmdSetScissorWithCount,scissorCount);HOOK(vkCmdSetScissorWithCountEXT,scissorCountEXT);
    HOOK(vkCmdSetDepthTestEnable,vkCmdSetDepthTestEnable);HOOK(vkCmdSetDepthTestEnableEXT,vkCmdSetDepthTestEnableEXT);
    HOOK(vkCmdSetStencilTestEnable,vkCmdSetStencilTestEnable);HOOK(vkCmdSetStencilTestEnableEXT,vkCmdSetStencilTestEnableEXT);
    HOOK(vkCmdSetDepthWriteEnable,vkCmdSetDepthWriteEnable);HOOK(vkCmdSetDepthWriteEnableEXT,vkCmdSetDepthWriteEnableEXT);
    HOOK(vkCmdSetDepthCompareOp,vkCmdSetDepthCompareOp);HOOK(vkCmdSetDepthCompareOpEXT,vkCmdSetDepthCompareOpEXT);
    HOOK(vkCmdSetCullMode,vkCmdSetCullMode);HOOK(vkCmdSetCullModeEXT,vkCmdSetCullModeEXT);
    HOOK(vkCmdSetFrontFace,vkCmdSetFrontFace);HOOK(vkCmdSetFrontFaceEXT,vkCmdSetFrontFaceEXT);
    HOOK(vkCmdSetDepthBiasEnable,vkCmdSetDepthBiasEnable);HOOK(vkCmdSetDepthBiasEnableEXT,vkCmdSetDepthBiasEnableEXT);HOOK(vkCmdSetColorBlendEnableEXT,blendEnable);HOOK(vkCmdSetColorBlendEquationEXT,blendEquation);HOOK(vkCmdSetColorWriteMaskEXT,colorWriteMask);
    HOOK(vkCmdSetPrimitiveTopology,vkCmdSetPrimitiveTopology);HOOK(vkCmdSetPrimitiveTopologyEXT,vkCmdSetPrimitiveTopologyEXT);
    HOOK(vkCmdSetRasterizerDiscardEnable,vkCmdSetRasterizerDiscardEnable);HOOK(vkCmdSetRasterizerDiscardEnableEXT,vkCmdSetRasterizerDiscardEnableEXT);
    HOOK(vkCmdExecuteCommands,execute);HOOK(vkCmdDraw,draw);HOOK(vkCmdDrawIndexed,indexed);HOOK(vkCmdDrawIndirect,indirect);HOOK(vkCmdDrawIndexedIndirect,indexedIndirect);
    HOOK(vkCmdDrawIndirectCount,vkCmdDrawIndirectCount);HOOK(vkCmdDrawIndirectCountKHR,vkCmdDrawIndirectCountKHR);
    HOOK(vkCmdDrawIndexedIndirectCount,vkCmdDrawIndexedIndirectCount);HOOK(vkCmdDrawIndexedIndirectCountKHR,vkCmdDrawIndexedIndirectCountKHR);
    HOOK(vkCmdBeginQuery,beginQuery);HOOK(vkCmdEndQuery,endQuery);HOOK(vkCmdBeginQueryIndexedEXT,beginQueryIndexed);HOOK(vkCmdEndQueryIndexedEXT,endQueryIndexed);
    HOOK(vkCmdBeginConditionalRenderingEXT,conditional);HOOK(vkCmdEndConditionalRenderingEXT,endConditional);
    HOOK(vkCmdBeginTransformFeedbackEXT,beginTF);HOOK(vkCmdEndTransformFeedbackEXT,endTF);HOOK(vkCmdBindDescriptorBuffersEXT,descriptorBuffers);
    HOOK(vkCmdBindShadersEXT,bindShaders);
    HOOK(vkCmdPipelineBarrier,barrier);HOOK(vkCmdPipelineBarrier2,barrier2);HOOK(vkCmdPipelineBarrier2KHR,barrier2KHR);HOOK(vkCmdClearAttachments,clearAttachments);
    HOOK(vkCmdBindVertexBuffers,vertexBuffers);HOOK(vkCmdBindVertexBuffers2,vertexBuffers2);HOOK(vkCmdBindVertexBuffers2EXT,vertexBuffers2EXT);
    HOOK(vkCmdBindIndexBuffer,indexBuffer);HOOK(vkCmdBindIndexBuffer2,indexBuffer2);HOOK(vkCmdBindIndexBuffer2KHR,indexBuffer2KHR);
    HOOK(vkCmdCopyImage,unsupported_vkCmdCopyImage);
    HOOK(vkCmdCopyBufferToImage,unsupported_vkCmdCopyBufferToImage);
    HOOK(vkCmdClearColorImage,unsupported_vkCmdClearColorImage);
    HOOK(vkCmdBlitImage,unsupported_vkCmdBlitImage);
    HOOK(vkCmdClearDepthStencilImage,unsupported_vkCmdClearDepthStencilImage);
    HOOK(vkCmdResolveImage,unsupported_vkCmdResolveImage);
    HOOK(vkCmdCopyImage2,unsupported_vkCmdCopyImage2);
    HOOK(vkCmdCopyBufferToImage2,unsupported_vkCmdCopyBufferToImage2);
    HOOK(vkCmdBlitImage2,unsupported_vkCmdBlitImage2);
    HOOK(vkCmdResolveImage2,unsupported_vkCmdResolveImage2);
    HOOK(vkCmdCopyImage2KHR,unsupported_vkCmdCopyImage2KHR);
    HOOK(vkCmdCopyBufferToImage2KHR,unsupported_vkCmdCopyBufferToImage2KHR);
    HOOK(vkCmdBlitImage2KHR,unsupported_vkCmdBlitImage2KHR);
    HOOK(vkCmdResolveImage2KHR,unsupported_vkCmdResolveImage2KHR);
    HOOK(vkCmdDrawIndirectByteCountEXT,unsupported_vkCmdDrawIndirectByteCountEXT);
    HOOK(vkCmdDrawIndirectCountAMD,unsupported_vkCmdDrawIndirectCountAMD);
    HOOK(vkCmdDrawIndexedIndirectCountAMD,unsupported_vkCmdDrawIndexedIndirectCountAMD);
    HOOK(vkCmdDrawMeshTasksNV,unsupported_vkCmdDrawMeshTasksNV);
    HOOK(vkCmdDrawMeshTasksIndirectNV,unsupported_vkCmdDrawMeshTasksIndirectNV);
    HOOK(vkCmdDrawMeshTasksIndirectCountNV,unsupported_vkCmdDrawMeshTasksIndirectCountNV);
    HOOK(vkCmdExecuteGeneratedCommandsNV,unsupported_vkCmdExecuteGeneratedCommandsNV);
    HOOK(vkCmdDrawMultiEXT,unsupported_vkCmdDrawMultiEXT);
    HOOK(vkCmdDrawMultiIndexedEXT,unsupported_vkCmdDrawMultiIndexedEXT);
    HOOK(vkCmdDrawClusterHUAWEI,unsupported_vkCmdDrawClusterHUAWEI);
    HOOK(vkCmdDrawClusterIndirectHUAWEI,unsupported_vkCmdDrawClusterIndirectHUAWEI);
    HOOK(vkCmdExecuteGeneratedCommandsEXT,unsupported_vkCmdExecuteGeneratedCommandsEXT);
    HOOK(vkCmdDrawMeshTasksEXT,unsupported_vkCmdDrawMeshTasksEXT);
    HOOK(vkCmdDrawMeshTasksIndirectEXT,unsupported_vkCmdDrawMeshTasksIndirectEXT);
    HOOK(vkCmdDrawMeshTasksIndirectCountEXT,unsupported_vkCmdDrawMeshTasksIndirectCountEXT);
#undef HOOK
    return nullptr;
}
}
