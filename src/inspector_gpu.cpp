#include "surface_inspector.hpp"
#include "overlay_internal.hpp"
#include "shader_resources.hpp"
#include "depth_tracking.hpp"
#include <cmath>

namespace depth_probe {
struct InspectorGpu {
    struct ExportFrame {
        SceneColorImage color;VkFramebuffer framebuffer{};MappedUniform readback;
        VkImageView source{};VkDescriptorPool pool{};VkDescriptorSet descriptor{};VkPipeline pipeline{};
        std::shared_ptr<discovery::AssetExport> job;size_t index=0;
        uint32_t width=0,height=0;uint64_t sourceImage=0;
    };
    struct Frame {
        SceneColorImage pixel;VkFramebuffer framebuffer{};MappedUniform readback;
        VkDescriptorSet previewSet{};VkImageView previewView{};uint64_t previewImage=0,previewIdentity=0;
        uint32_t pendingTicket=0;uint64_t depthGeneration=0;ExportFrame asset;
    };
    VkRenderPass pass{};VkPipeline pickPipeline{},previewPipeline{};VkDescriptorPool previewPool{};
    VkRenderPass exportPass{};VkDescriptorSetLayout exportSetLayout{};VkPipelineLayout exportLayout{};PFN_vkCmdFillBuffer fillBuffer{};
    std::vector<Frame> frames;
};
namespace {
bool allocate_memory(const DeviceData& d,const DepthOverlayFns& f,const VkMemoryRequirements& req,VkMemoryPropertyFlags required,VkDeviceMemory& out,bool* coherent=nullptr) {
    VkPhysicalDeviceMemoryProperties props{};d.get_memory_properties(d.physical_device,&props);uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&required)==required) {
        type=i;if(!required || (props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))break;
    }
    if(type==UINT32_MAX)return false;
    if(coherent)*coherent=props.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
    return f.allocateMemory(d.device,&allocation,nullptr,&out)==VK_SUCCESS;
}
bool pipeline(const DeviceData& d,const DepthOverlayFns& f,const DepthOverlayState& st,VkRenderPass pass,uint32_t width,uint32_t height,const char* fragment,VkPipeline& out,VkPipelineLayout customLayout=VK_NULL_HANDLE,uint32_t conversion=0) {
    auto vert=read_spirv_file(depth_shader_dir()+"/depth.vert.spv");auto frag=read_spirv_file(depth_shader_dir()+"/"+fragment+".spv");if(vert.empty()||frag.empty())return false;
    VkShaderModule modules[2]{};bool ok=true;
    for(unsigned i=0;i<2;++i) {
        const auto& code=i?frag:vert;VkShaderModuleCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;info.codeSize=code.size()*4;info.pCode=code.data();
        if(f.createShaderModule(d.device,&info,nullptr,&modules[i])!=VK_SUCCESS){ok=false;break;}
    }
    if(ok) {
        VkPipelineShaderStageCreateInfo stages[2]{};for(unsigned i=0;i<2;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=modules[i];stages[i].pName="main";}
        VkSpecializationMapEntry entry{0,0,sizeof(uint32_t)};VkSpecializationInfo special{1,&entry,sizeof(conversion),&conversion};
        if(customLayout)stages[1].pSpecializationInfo=&special;
        VkPipelineVertexInputStateCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,float(width),float(height),0,1};VkRect2D scissor{{0,0},{width,height}};
        VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo rs{};rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb.attachmentCount=1;cb.pAttachments=&attachment;
        VkGraphicsPipelineCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;ci.stageCount=2;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vp;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pColorBlendState=&cb;ci.layout=customLayout?customLayout:st.pipelineLayout;ci.renderPass=pass;
        ok=f.createGraphicsPipelines(d.device,VK_NULL_HANDLE,1,&ci,nullptr,&out)==VK_SUCCESS;
    }
    for(auto m:modules)if(m)f.destroyShaderModule(d.device,m,nullptr);
    return ok;
}
void preview_descriptor(const DeviceData& data,const DepthOverlayFns& f,const DepthOverlayState& st,uint32_t i,VkImageView view,VkImageLayout layout) {
    auto& frame=st.inspectorGpu->frames[i];VkDescriptorImageInfo image{st.sampler,view,layout};VkDescriptorBufferInfo buffer{st.uniforms[i].buffer,0,sizeof(OverlayUniform)};
    VkWriteDescriptorSet writes[3]{};for(auto& w:writes){w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=frame.previewSet;w.descriptorCount=1;}
    writes[0].dstBinding=0;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[0].pImageInfo=&image;
    writes[1].dstBinding=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;writes[1].pBufferInfo=&buffer;
    writes[2]=writes[0];writes[2].dstBinding=2;
    f.updateDescriptorSets(data.device,3,writes,0,nullptr);
}

void update_asset(const DeviceData&,const DepthOverlayFns&,DepthOverlayState&,uint32_t,discovery::Device&,discovery::Inspector&);
void finish_asset(const DeviceData&,const DepthOverlayFns&,DepthOverlayState&,uint32_t,bool);
void record_asset(const DepthOverlayFns&,const DepthOverlayState&,uint32_t);

}
bool create_inspector_gpu(const DeviceData& d,const DepthOverlayFns& f,DepthOverlayState& st) {
    if(!inspector_enabled())return true;
    if(!f.cmdCopyImageToBuffer || !f.invalidateMappedMemoryRanges || !d.get_memory_properties || !d.get_format_properties)return false;
    VkFormatProperties properties{};d.get_format_properties(d.physical_device,VK_FORMAT_R32G32B32A32_UINT,&properties);
    if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || !(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))return false;
    st.inspectorGpu=std::make_shared<InspectorGpu>();auto& gpu=*st.inspectorGpu;gpu.frames.resize(st.colorImages.size());gpu.fillBuffer=reinterpret_cast<PFN_vkCmdFillBuffer>(d.next_gdpa(d.device,"vkCmdFillBuffer"));
    VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_R32G32B32A32_UINT;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
    VkSubpassDependency dependency{};dependency.srcSubpass=0;dependency.dstSubpass=VK_SUBPASS_EXTERNAL;dependency.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependency.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dependency.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;rp.attachmentCount=rp.subpassCount=rp.dependencyCount=1;rp.pAttachments=&attachment;rp.pSubpasses=&sub;rp.pDependencies=&dependency;
    if(f.createRenderPass(d.device,&rp,nullptr,&gpu.pass)!=VK_SUCCESS)return false;
    if(!pipeline(d,f,st,gpu.pass,1,1,"inspector_pick.frag",gpu.pickPipeline))return false;
    if(!pipeline(d,f,st,st.renderPass,st.width,st.height,"inspector_preview.frag",gpu.previewPipeline))return false;
    const uint32_t count=gpu.frames.size();VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,count*2},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,count}};
    VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.maxSets=count;pool.poolSizeCount=2;pool.pPoolSizes=sizes;
    if(f.createDescriptorPool(d.device,&pool,nullptr,&gpu.previewPool)!=VK_SUCCESS)return false;
    for(uint32_t i=0;i<count;++i) {
        auto& frame=gpu.frames[i];VkImageCreateInfo im{};im.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;im.imageType=VK_IMAGE_TYPE_2D;im.format=attachment.format;im.extent={1,1,1};im.mipLevels=im.arrayLayers=1;im.samples=VK_SAMPLE_COUNT_1_BIT;im.tiling=VK_IMAGE_TILING_OPTIMAL;im.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if(f.createImage(d.device,&im,nullptr,&frame.pixel.image)!=VK_SUCCESS)return false;
        VkMemoryRequirements requirements{};f.getImageMemoryRequirements(d.device,frame.pixel.image,&requirements);
        if(!allocate_memory(d,f,requirements,0,frame.pixel.memory) || f.bindImageMemory(d.device,frame.pixel.image,frame.pixel.memory,0)!=VK_SUCCESS)return false;
        VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;view.image=frame.pixel.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=im.format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(f.createImageView(d.device,&view,nullptr,&frame.pixel.view)!=VK_SUCCESS)return false;
        VkFramebufferCreateInfo fb{};fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fb.renderPass=gpu.pass;fb.attachmentCount=1;fb.pAttachments=&frame.pixel.view;fb.width=fb.height=fb.layers=1;
        if(f.createFramebuffer(d.device,&fb,nullptr,&frame.framebuffer)!=VK_SUCCESS)return false;
        VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;buffer.size=16;buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto& read=frame.readback;if(f.createBuffer(d.device,&buffer,nullptr,&read.buffer)!=VK_SUCCESS)return false;
        f.getBufferMemoryRequirements(d.device,read.buffer,&requirements);
        if(!allocate_memory(d,f,requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,read.memory,&read.coherent) || f.bindBufferMemory(d.device,read.buffer,read.memory,0)!=VK_SUCCESS || f.mapMemory(d.device,read.memory,0,VK_WHOLE_SIZE,0,&read.mapped)!=VK_SUCCESS)return false;
        VkDescriptorSetAllocateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ds.descriptorPool=gpu.previewPool;ds.descriptorSetCount=1;ds.pSetLayouts=&st.descriptorSetLayout;
        if(f.allocateDescriptorSets(d.device,&ds,&frame.previewSet)!=VK_SUCCESS)return false;
        preview_descriptor(d,f,st,i,st.depthView,VK_IMAGE_LAYOUT_GENERAL);
    }
    log_line("[INSPECTOR-GPU-READY] depthReadback=16bytes per-image fence reuse; assetExport=BMP+SPIRV preview=per-mip; no extra submits or queue idle");return true;
}
void destroy_inspector_gpu(const DeviceData& d,const DepthOverlayFns& f,DepthOverlayState& st) {
    if(!st.inspectorGpu)return;
    auto& gpu=*st.inspectorGpu;
    // Caller already completed the existing overlay lifetime wait.
    for(uint32_t i=0;i<gpu.frames.size();++i) {
        finish_asset(d,f,st,i,true);auto& frame=gpu.frames[i];
        if(frame.previewView)f.destroyImageView(d.device,frame.previewView,nullptr);
        if(frame.framebuffer)f.destroyFramebuffer(d.device,frame.framebuffer,nullptr);
        if(frame.pixel.view)f.destroyImageView(d.device,frame.pixel.view,nullptr);
        if(frame.pixel.image)f.destroyImage(d.device,frame.pixel.image,nullptr);
        if(frame.pixel.memory)f.freeMemory(d.device,frame.pixel.memory,nullptr);
        if(frame.readback.mapped)f.unmapMemory(d.device,frame.readback.memory);
        if(frame.readback.buffer)f.destroyBuffer(d.device,frame.readback.buffer,nullptr);
        if(frame.readback.memory)f.freeMemory(d.device,frame.readback.memory,nullptr);
    }
    if(gpu.exportLayout)f.destroyPipelineLayout(d.device,gpu.exportLayout,nullptr);
    if(gpu.exportSetLayout)f.destroyDescriptorSetLayout(d.device,gpu.exportSetLayout,nullptr);
    if(gpu.exportPass)f.destroyRenderPass(d.device,gpu.exportPass,nullptr);
    if(gpu.previewPool)f.destroyDescriptorPool(d.device,gpu.previewPool,nullptr);
    if(gpu.pickPipeline)f.destroyPipeline(d.device,gpu.pickPipeline,nullptr);
    if(gpu.previewPipeline)f.destroyPipeline(d.device,gpu.previewPipeline,nullptr);
    if(gpu.pass)f.destroyRenderPass(d.device,gpu.pass,nullptr);
    {
        std::lock_guard lock(discovery::mutex);auto it=discovery::devices.find(d.device);
        if(it!=discovery::devices.end() && it->second.inspector)if(auto job=it->second.inspector->assetExport) {
            std::vector<size_t> queued;
            {std::lock_guard guard(job->lock);for(size_t i=0;i<job->textures.size();++i)if(job->textures[i].status=="queued")queued.push_back(i);}
            for(auto index:queued)discovery::asset_texture_failed(job,index,"Renderer retired before export");
        }
    }
    st.inspectorGpu.reset();
}
void record_inspector_gpu(const DepthOverlayFns& f,const DepthOverlayState& st,uint32_t i) {
    if(!st.inspectorGpu)return;
    const auto& gpu=*st.inspectorGpu;const auto& frame=gpu.frames[i];auto cmd=st.commandBuffers[i];
    if(frame.previewView || frame.asset.source) {
        VkMemoryBarrier readable{};readable.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;readable.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;readable.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&readable,0,nullptr,0,nullptr);
    }
    record_asset(f,st,i);
    VkClearValue clear{};VkRenderPassBeginInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;rp.renderPass=gpu.pass;rp.framebuffer=frame.framebuffer;rp.renderArea.extent={1,1};rp.clearValueCount=1;rp.pClearValues=&clear;
    f.cmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
    f.cmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gpu.pickPipeline);f.cmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,st.pipelineLayout,0,1,&st.descriptorSets[i],0,nullptr);
    f.cmdDrawIndirect(cmd,st.uniforms[i].buffer,offsetof(OverlayFrameData,pickDraw),1,sizeof(VkDrawIndirectCommand));f.cmdEndRenderPass(cmd);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={1,1,1};
    f.cmdCopyImageToBuffer(cmd,frame.pixel.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,frame.readback.buffer,1,&copy);
    VkBufferMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=frame.readback.buffer;barrier.size=VK_WHOLE_SIZE;
    f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
}
void record_inspector_preview(const DepthOverlayFns& f,const DepthOverlayState& st,uint32_t i) {
    if(!st.inspectorGpu)return;
    const auto& gpu=*st.inspectorGpu;auto cmd=st.commandBuffers[i];
    f.cmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gpu.previewPipeline);f.cmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,st.pipelineLayout,0,1,&gpu.frames[i].previewSet,0,nullptr);
    f.cmdDrawIndirect(cmd,st.uniforms[i].buffer,offsetof(OverlayFrameData,previewDraw),1,sizeof(VkDrawIndirectCommand));
}
VkResult update_inspector_gpu(const DeviceData& data,const DepthOverlayFns& f,DepthOverlayState& st,uint32_t i,OverlayUniform& u) {
    if(!st.inspectorGpu)return VK_SUCCESS;
    using namespace discovery;auto& frame=st.inspectorGpu->frames[i];
    std::lock_guard lock(mutex);auto& state=device_state(data.device);auto& in=inspector_state(state,data.device);
    if(frame.pendingTicket) {
        if(!frame.readback.coherent) {
            VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=frame.readback.memory;range.size=VK_WHOLE_SIZE;
            auto result=f.invalidateMappedMemoryRanges(data.device,1,&range);if(result!=VK_SUCCESS)return result;
        }
        uint32_t words[4]{};std::memcpy(words,frame.readback.mapped,sizeof(words));float depth;std::memcpy(&depth,words,4);
        if(words[3]==in.ticket && words[3]==frame.pendingTicket && words[1]==in.x && words[2]==in.y && frame.depthGeneration==in.depthGeneration && std::isfinite(depth)) {
            in.depth=depth;in.depthReady=true;log_line("[SURFACE-DEPTH] ticket="+std::to_string(in.ticket)+" pixel="+std::to_string(in.x)+","+std::to_string(in.y)+" raw="+std::to_string(depth));
        }
        frame.pendingTicket=0;
    }
    frame.pendingTicket=u.pick[2];frame.depthGeneration=in.depthGeneration;
    if(u.pick[2])in.captureDepth=false;
    // Per-image descriptor changes happen only after that image's existing fence.
    update_asset(data,f,st,i,state,in);
    const ResourceUse* resource=nullptr;unsigned index=0;
    if(in.page==3 && in.preview && in.selected && u.menu[0])for(const auto& r:in.selected->resources)if(r.descriptor.view && r.descriptor.view->image) {
        if(index++==in.texture)resource=&r;
    }
    TextureSource source;if(resource)source=inspector_texture_source(state,*resource,in.queue);
    auto chosen=source.view;bool supported=bool(source);VkImageLayout layout=source.layout;
    if(supported) {
        VkFormatProperties properties{};data.get_format_properties(data.physical_device,chosen->format,&properties);
        if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)){supported=false;source.reason="Format sampling unsupported by device";}
    }
    // Rebuild this small view on explicit inspection only; retain it between frames.
    const uint64_t wanted=supported?chosen->image->handle:0;
    const uint64_t wantedIdentity=supported?texture_identity(*chosen):0;
    if(frame.previewImage!=wanted || frame.previewIdentity!=wantedIdentity) {
        st.commandRecorded[i]=false;
        preview_descriptor(data,f,st,i,st.depthView,VK_IMAGE_LAYOUT_GENERAL);
        if(frame.previewView)f.destroyImageView(data.device,frame.previewView,nullptr);
        frame.previewView={};frame.previewImage=frame.previewIdentity=0;
        if(supported) {
            VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;view.image=(VkImage)wanted;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=chosen->format;view.components=chosen->components;view.subresourceRange=chosen->range;view.subresourceRange.levelCount=1;view.subresourceRange.layerCount=1;
            VkImageViewUsageCreateInfo usage{};usage.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;usage.usage=VK_IMAGE_USAGE_SAMPLED_BIT;view.pNext=&usage;
            if(f.createImageView(data.device,&view,nullptr,&frame.previewView)==VK_SUCCESS){frame.previewImage=wanted;frame.previewIdentity=wantedIdentity;}
        }
    }
    u.pick[3]=supported && frame.previewView;
    in.previewStatus=!in.preview?"Preview not requested":u.pick[3]?"RGB preview ready - native GPU sampling":resource?(source.reason.empty()?"Private texture view creation failed":source.reason):"No texture selected";
    const auto diagnostic=std::to_string(in.ticket)+":"+std::to_string(in.texture)+":"+in.previewStatus;
    if(in.preview && resource && in.previewDiagnostic!=diagnostic) {
        in.previewDiagnostic=diagnostic;
        log_line("[INSPECTOR-PREVIEW] ticket="+std::to_string(in.ticket)+" texture="+std::to_string(in.texture)+" ready="+std::to_string(bool(u.pick[3]))+" reason="+in.previewStatus+
            " format="+std::to_string(resource->descriptor.view->format)+" flags="+std::to_string(resource->descriptor.view->image->flags)+" mip="+std::to_string(resource->descriptor.view->range.baseMipLevel));
    }
    if(u.pick[3])preview_descriptor(data,f,st,i,frame.previewView,layout);
    if(frame.previewImage || wanted)st.commandRecorded[i]=false;
    return VK_SUCCESS;
}
void release_inspector_image(VkDevice device,VkImage image) {
    if(!inspector_enabled())return;
    std::lock_guard life(g_depth_lifecycle_mutex);auto data=get_device_data(device);if(!data)return;auto f=load_depth_overlay_fns(*data);
    // The app may finish its own submission before the layer's preview. Pin the
    // image until ONLY the associated overlay fences complete; no queue-idle.
    for(auto& [key,st]:g_depth_overlay_states)if(key.device==device && st.inspectorGpu) {
        for(uint32_t i=0;i<st.inspectorGpu->frames.size();++i) {
            auto& frame=st.inspectorGpu->frames[i];
            const bool assetSource=frame.asset.sourceImage==discovery::id(image);
            if(frame.previewImage!=discovery::id(image) && !assetSource)continue;
            if(i<st.sync.size() && st.sync[i].submitted)f.waitForFences(device,1,&st.sync[i].submitFence,VK_TRUE,UINT64_MAX);
            if(assetSource)finish_asset(*data,f,st,i,true);
            if(frame.previewImage!=discovery::id(image))continue;
            preview_descriptor(*data,f,st,i,st.depthView,VK_IMAGE_LAYOUT_GENERAL);
            f.destroyImageView(device,frame.previewView,nullptr);frame.previewView={};frame.previewImage=frame.previewIdentity=0;
            // Updating a descriptor invalidates its old recording, rebuild safely.
            st.commandRecorded[i]=false;
        }
    }
}
namespace {
constexpr uint32_t assetMarker=0x534d4858;
void clear_asset(const DeviceData& d,const DepthOverlayFns& f,InspectorGpu::ExportFrame& a) {
    if(a.pipeline)f.destroyPipeline(d.device,a.pipeline,nullptr);
    if(a.pool)f.destroyDescriptorPool(d.device,a.pool,nullptr);
    if(a.source)f.destroyImageView(d.device,a.source,nullptr);
    if(a.framebuffer)f.destroyFramebuffer(d.device,a.framebuffer,nullptr);
    if(a.color.view)f.destroyImageView(d.device,a.color.view,nullptr);
    if(a.color.image)f.destroyImage(d.device,a.color.image,nullptr);
    if(a.color.memory)f.freeMemory(d.device,a.color.memory,nullptr);
    if(a.readback.mapped)f.unmapMemory(d.device,a.readback.memory);
    if(a.readback.buffer)f.destroyBuffer(d.device,a.readback.buffer,nullptr);
    if(a.readback.memory)f.freeMemory(d.device,a.readback.memory,nullptr);
    a={};
}
bool export_pass(const DeviceData& d,const DepthOverlayFns& f,InspectorGpu& gpu) {
    if(!gpu.fillBuffer)return false;
    if(!gpu.exportSetLayout) {
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;info.bindingCount=1;info.pBindings=&binding;
        if(f.createDescriptorSetLayout(d.device,&info,nullptr,&gpu.exportSetLayout)!=VK_SUCCESS)return false;
    }
    if(!gpu.exportLayout) {
        VkPipelineLayoutCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;info.setLayoutCount=1;info.pSetLayouts=&gpu.exportSetLayout;
        if(f.createPipelineLayout(d.device,&info,nullptr,&gpu.exportLayout)!=VK_SUCCESS)return false;
    }
    if(!gpu.exportPass) {
        VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_R8G8B8A8_UNORM;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        VkSubpassDependency dependency{};dependency.srcSubpass=0;dependency.dstSubpass=VK_SUBPASS_EXTERNAL;dependency.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependency.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dependency.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;info.attachmentCount=info.subpassCount=info.dependencyCount=1;info.pAttachments=&attachment;info.pSubpasses=&sub;info.pDependencies=&dependency;
        if(f.createRenderPass(d.device,&info,nullptr,&gpu.exportPass)!=VK_SUCCESS)return false;
    }
    return true;
}
bool allocate_asset(const DeviceData& d,const DepthOverlayFns& f,DepthOverlayState& st,uint32_t i,const discovery::TextureSource& source) {
    auto& gpu=*st.inspectorGpu;auto& a=gpu.frames[i].asset;
    if(!export_pass(d,f,gpu))return false;
    VkFormatProperties properties{};d.get_format_properties(d.physical_device,source.view->format,&properties);
    if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))return false;
    d.get_format_properties(d.physical_device,VK_FORMAT_R8G8B8A8_UNORM,&properties);
    if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || !(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))return false;
    a.width=source.width;a.height=source.height;a.sourceImage=source.view->image->handle;
    VkImageViewUsageCreateInfo usage{};usage.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;usage.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;view.pNext=&usage;view.image=(VkImage)a.sourceImage;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=source.view->format;view.components=source.view->components;view.subresourceRange=source.view->range;view.subresourceRange.layerCount=view.subresourceRange.levelCount=1;
    if(f.createImageView(d.device,&view,nullptr,&a.source)!=VK_SUCCESS)return false;
    VkImageCreateInfo im{};im.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;im.imageType=VK_IMAGE_TYPE_2D;im.format=VK_FORMAT_R8G8B8A8_UNORM;im.extent={a.width,a.height,1};im.mipLevels=im.arrayLayers=1;im.samples=VK_SAMPLE_COUNT_1_BIT;im.tiling=VK_IMAGE_TILING_OPTIMAL;im.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if(f.createImage(d.device,&im,nullptr,&a.color.image)!=VK_SUCCESS)return false;
    VkMemoryRequirements req{};f.getImageMemoryRequirements(d.device,a.color.image,&req);
    if(!allocate_memory(d,f,req,0,a.color.memory) || f.bindImageMemory(d.device,a.color.image,a.color.memory,0)!=VK_SUCCESS)return false;
    view.pNext=nullptr;view.image=a.color.image;view.format=im.format;view.components={};view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(f.createImageView(d.device,&view,nullptr,&a.color.view)!=VK_SUCCESS)return false;
    VkFramebufferCreateInfo fb{};fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fb.renderPass=gpu.exportPass;fb.attachmentCount=1;fb.pAttachments=&a.color.view;fb.width=a.width;fb.height=a.height;fb.layers=1;
    if(f.createFramebuffer(d.device,&fb,nullptr,&a.framebuffer)!=VK_SUCCESS)return false;
    const size_t bytes=size_t(a.width)*a.height*4;VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;buffer.size=bytes+4;buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(f.createBuffer(d.device,&buffer,nullptr,&a.readback.buffer)!=VK_SUCCESS)return false;
    f.getBufferMemoryRequirements(d.device,a.readback.buffer,&req);
    if(!allocate_memory(d,f,req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,a.readback.memory,&a.readback.coherent) || f.bindBufferMemory(d.device,a.readback.buffer,a.readback.memory,0)!=VK_SUCCESS || f.mapMemory(d.device,a.readback.memory,0,VK_WHOLE_SIZE,0,&a.readback.mapped)!=VK_SUCCESS)return false;
    std::memset(static_cast<uint8_t*>(a.readback.mapped)+bytes,0,4);
    if(!a.readback.coherent){VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=a.readback.memory;r.size=VK_WHOLE_SIZE;if(f.flushMappedMemoryRanges(d.device,1,&r)!=VK_SUCCESS)return false;}
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.maxSets=pool.poolSizeCount=1;pool.pPoolSizes=&size;
    if(f.createDescriptorPool(d.device,&pool,nullptr,&a.pool)!=VK_SUCCESS)return false;
    VkDescriptorSetAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;alloc.descriptorPool=a.pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&gpu.exportSetLayout;
    if(f.allocateDescriptorSets(d.device,&alloc,&a.descriptor)!=VK_SUCCESS)return false;
    VkDescriptorImageInfo image{st.sampler,a.source,source.layout};VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=a.descriptor;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;f.updateDescriptorSets(d.device,1,&write,0,nullptr);
    return pipeline(d,f,st,gpu.exportPass,a.width,a.height,"inspector_export.frag",a.pipeline,gpu.exportLayout,source.conversion);
}
void finish_asset(const DeviceData& d,const DepthOverlayFns& f,DepthOverlayState& st,uint32_t i,bool force) {
    using namespace discovery;auto& a=st.inspectorGpu->frames[i].asset;if(!a.job)return;
    uint32_t marker=0;const size_t bytes=size_t(a.width)*a.height*4;bool readable=a.readback.mapped;
    if(readable && !a.readback.coherent) {VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=a.readback.memory;r.size=VK_WHOLE_SIZE;readable=f.invalidateMappedMemoryRanges(d.device,1,&r)==VK_SUCCESS;}
    if(readable)std::memcpy(&marker,static_cast<const uint8_t*>(a.readback.mapped)+bytes,4);
    if(marker!=assetMarker && !force)return; // failed/not-yet-submitted recording is never exported
    if(marker==assetMarker) {
        try {const auto* ptr=static_cast<const uint8_t*>(a.readback.mapped);std::vector<uint8_t> pixels(ptr,ptr+bytes);if(!save_asset_pixels(a.job,a.index,std::move(pixels)))asset_texture_failed(a.job,a.index,"Bitmap writer busy");}
        catch(const std::exception&){asset_texture_failed(a.job,a.index,"CPU readback allocation failed");}
    } else asset_texture_failed(a.job,a.index,"Source or renderer retired before readback");
    clear_asset(d,f,a);st.commandRecorded[i]=false;
}
void update_asset(const DeviceData& d,const DepthOverlayFns& f,DepthOverlayState& st,uint32_t i,discovery::Device& state,discovery::Inspector& in) {
    using namespace discovery;auto& a=st.inspectorGpu->frames[i].asset;finish_asset(d,f,st,i,false);if(a.job)return;
    auto job=in.assetExport;if(!job)return;
    for(size_t attempt=0;attempt<maxResourceUses;++attempt) {
        ResourceUse resource;size_t index=SIZE_MAX;
        {std::lock_guard lock(job->lock);if(!job->ready || job->failed)return;
            for(const auto& t:job->textures)if(t.status=="gpu" || t.status=="writing")return;
            for(size_t n=0;n<job->textures.size();++n)if(job->textures[n].status=="queued"){index=n;resource=job->textures[n].resource;job->textures[n].status="gpu";break;}
        }
        if(index==SIZE_MAX)return;
        const auto source=inspector_texture_source(state,resource,in.queue);
        if(!source){asset_texture_failed(job,index,source.reason);continue;}
        if(source.width>8192 || source.height>8192 || uint64_t(source.width)*source.height*4>maxTextureExportBytes){asset_texture_failed(job,index,"Texture exceeds 64 MiB bitmap budget");continue;}
        a.job=job;a.index=index;
        if(!allocate_asset(d,f,st,i,source)){asset_texture_failed(job,index,"GPU export allocation/pipeline failed");clear_asset(d,f,a);continue;}
        {std::lock_guard lock(job->lock);job->textures[index].width=source.width;job->textures[index].height=source.height;}
        st.commandRecorded[i]=false;return;
    }
}
void record_asset(const DepthOverlayFns& f,const DepthOverlayState& st,uint32_t i) {
    const auto& gpu=*st.inspectorGpu;const auto& a=gpu.frames[i].asset;if(!a.job || !a.pipeline)return;auto cmd=st.commandBuffers[i];
    VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;begin.renderPass=gpu.exportPass;begin.framebuffer=a.framebuffer;begin.renderArea.extent={a.width,a.height};
    f.cmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);f.cmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,a.pipeline);f.cmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gpu.exportLayout,0,1,&a.descriptor,0,nullptr);f.cmdDraw(cmd,3,1,0,0);f.cmdEndRenderPass(cmd);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={a.width,a.height,1};f.cmdCopyImageToBuffer(cmd,a.color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,a.readback.buffer,1,&copy);
    gpu.fillBuffer(cmd,a.readback.buffer,uint64_t(a.width)*a.height*4,4,assetMarker);
    VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=a.readback.buffer;host.size=VK_WHOLE_SIZE;
    f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
}
}

}
