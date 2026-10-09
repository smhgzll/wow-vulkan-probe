#include "water_reflections.hpp"
#include "shader_resources.hpp"
#include <fstream>
#include <cstring>

namespace depth_probe::discovery {
namespace {
// Last creation failure of this thread; create_water_batch reports it so a failed private
// resource names the step and Vulkan result instead of just "creation failed".
thread_local std::string lastFailure;
void note(const std::string& text){lastFailure=text;}
template<class T>T raw(const DeviceData& d,const char* n){return reinterpret_cast<T>(d.next_gdpa(d.device,n));}
bool allocate(const DeviceData& d,const DepthOverlayFns& f,const VkMemoryRequirements& r,VkDeviceMemory& memory) {
    if(!d.get_memory_properties)return false;
    VkPhysicalDeviceMemoryProperties p{};d.get_memory_properties(d.physical_device,&p);
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<p.memoryTypeCount;++i)if(r.memoryTypeBits&(1u<<i)){type=i;if(p.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)break;}
    if(type==UINT32_MAX)return false;
    VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=type;
    return f.allocateMemory(d.device,&a,nullptr,&memory)==VK_SUCCESS;
}
bool allocate_host(const DeviceData& d,const DepthOverlayFns& f,const VkMemoryRequirements& r,VkDeviceMemory& memory) {
    if(!d.get_memory_properties)return false;
    VkPhysicalDeviceMemoryProperties p{};d.get_memory_properties(d.physical_device,&p);
    const VkMemoryPropertyFlags wanted=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<p.memoryTypeCount;++i)if((r.memoryTypeBits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&wanted)==wanted) {
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=i;
        return f.allocateMemory(d.device,&a,nullptr,&memory)==VK_SUCCESS;
    }
    return false;
}
// A water texture file ("SWT1": size, size, 3 layers, levels, then RGBA8 texels layer by layer, level by level; tools/generate-water-textures.py
// and tools/generate-modern-water-textures.py). A missing or malformed file is not fatal: that set gets a flat 1x1 fallback (no ripple detail,
// plain foam, no caustics) and the reason is logged once per batch.
struct DetailFile {uint32_t size=1,levels=1;std::vector<uint8_t> bytes;bool fromFile=false;std::string problem;};
DetailFile read_detail_file(const char* name) {
    DetailFile out;const auto path=depth_shader_dir()+"/"+name;
    auto fallback=[&](const std::string& why){
        out.problem=why;out.size=1;out.levels=1;out.fromFile=false;out.bytes={128,128,255,128,255,255,255,255,0,0,0,0};   // layer 0: flat normal; 1: full foam pattern; 2: no caustics
        return out;
    };
    std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)return fallback("missing "+path);
    const auto total=size_t(in.tellg());in.seekg(0);std::vector<uint8_t> file(total);in.read(reinterpret_cast<char*>(file.data()),std::streamsize(total));
    if(!in || total<20 || std::memcmp(file.data(),"SWT1",4)!=0)return fallback("bad header in "+path);
    uint32_t header[4];std::memcpy(header,file.data()+4,16);
    const uint32_t size=header[0],height=header[1],layers=header[2],levels=header[3];
    if(size==0 || size>2048 || (size&(size-1)) || height!=size || layers!=3 || levels==0 || levels>12 || (size>>(levels-1))==0)return fallback("unsupported dimensions in "+path);
    size_t expected=0;for(uint32_t l=0;l<levels;++l)expected+=size_t(size>>l)*(size>>l)*4;expected*=layers;
    if(total!=20+expected)return fallback("size mismatch in "+path);
    out.size=size;out.levels=levels;out.bytes.assign(file.begin()+20,file.end());out.fromFile=true;return out;
}
bool create_textures(WaterBatch& b,const char* name,WaterBatch::Textures& t) {
    auto& f=b.f;auto device=b.data.device;const auto file=read_detail_file(name);
    if(!file.problem.empty())log_line("[WATER-TEXTURE] "+file.problem+" - using a flat fallback");
    t.size=file.size;t.levels=file.levels;t.fromFile=file.fromFile;
    VkImageCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_R8G8B8A8_UNORM;
    info.extent={file.size,file.size,1};info.mipLevels=file.levels;info.arrayLayers=3;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
    info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(auto r=f.createImage(device,&info,nullptr,&t.image);r!=VK_SUCCESS){note("vkCreateImage water textures result "+std::to_string(int(r)));return false;}
    VkMemoryRequirements requirements{};f.getImageMemoryRequirements(device,t.image,&requirements);
    if(!allocate(b.data,f,requirements,t.memory)){note("water texture memory allocation failed");return false;}
    if(f.bindImageMemory(device,t.image,t.memory,0)!=VK_SUCCESS){note("bind water texture memory failed");return false;}
    VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=t.image;v.format=info.format;v.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,file.levels,0,3};
    if(f.createImageView(device,&v,nullptr,&t.view)!=VK_SUCCESS){note("water texture view failed");return false;}
    VkFormatProperties caps{};b.data.get_format_properties(b.data.physical_device,info.format,&caps);
    VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter=si.minFilter=(caps.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;si.maxLod=float(file.levels);
    if(f.createSampler(device,&si,nullptr,&t.sampler)!=VK_SUCCESS){note("water texture sampler failed");return false;}
    // Staging buffer with every level of the three layers, in the order the file stores them.
    VkBufferCreateInfo bi{};bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=file.bytes.size();bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if(f.createBuffer(device,&bi,nullptr,&t.staging)!=VK_SUCCESS){note("water texture staging buffer failed");return false;}
    VkMemoryRequirements br{};f.getBufferMemoryRequirements(device,t.staging,&br);
    if(!allocate_host(b.data,f,br,t.stagingMemory) || f.bindBufferMemory(device,t.staging,t.stagingMemory,0)!=VK_SUCCESS){note("water texture staging memory failed");return false;}
    void* mapped=nullptr;if(f.mapMemory(device,t.stagingMemory,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS || !mapped){note("water texture staging map failed");return false;}
    std::memcpy(mapped,file.bytes.data(),file.bytes.size());f.unmapMemory(device,t.stagingMemory);   // host-coherent: no flush needed
    VkDeviceSize offset=0;
    for(uint32_t layer=0;layer<3;++layer)for(uint32_t level=0;level<file.levels;++level) {
        const uint32_t extent=file.size>>level;VkBufferImageCopy c{};c.bufferOffset=offset;c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1};c.imageExtent={extent,extent,1};
        t.copies.push_back(c);offset+=VkDeviceSize(extent)*extent*4;
    }
    return true;
}
// levels > 1: a mip chain built by blits (water_color_mips); without blit/linear-filter support for the format it stays one level.
bool image(WaterBatch& b,WaterImage& out,VkFormat format,uint32_t w,uint32_t h,bool depth,bool transfer=false,uint32_t levels=1) {
    VkFormatProperties caps{};b.data.get_format_properties(b.data.physical_device,format,&caps);
    VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|(depth?VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    if((caps.optimalTilingFeatures&required)!=required){note("format "+std::to_string(int(format))+" lacks features 0x"+hex(required&~caps.optimalTilingFeatures)+(depth?" (depth)":" (color)"));return false;}
    const VkFormatFeatureFlags mipFeatures=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    {uint32_t chain=1;while((std::max(w,h)>>chain)>0)++chain;levels=std::max(1u,std::min(levels,chain));}   // a full chain ends at 1x1
    if(levels>1 && (caps.optimalTilingFeatures&mipFeatures)!=mipFeatures)levels=1;
    VkImageCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;info.imageType=VK_IMAGE_TYPE_2D;info.format=format;
    info.extent={w,h,1};info.mipLevels=levels;info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
    info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|(depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if(transfer || levels>1)info.usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(levels>1)info.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // Copy source only so a debug capture can read the pass results back; skipped where the format cannot.
    if(caps.optimalTilingFeatures&VK_FORMAT_FEATURE_TRANSFER_SRC_BIT){info.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;out.transferSrc=true;}
    if(auto r=b.f.createImage(b.data.device,&info,nullptr,&out.image);r!=VK_SUCCESS){note("vkCreateImage "+std::to_string(w)+"x"+std::to_string(h)+" format "+std::to_string(int(format))+" result "+std::to_string(int(r)));return false;}
    VkMemoryRequirements r{};b.f.getImageMemoryRequirements(b.data.device,out.image,&r);
    if(!allocate(b.data,b.f,r,out.memory)){note("image memory allocation "+std::to_string(r.size)+" bytes failed");return false;}
    if(auto bound=b.f.bindImageMemory(b.data.device,out.image,out.memory,0);bound!=VK_SUCCESS){note("bind image memory result "+std::to_string(int(bound)));return false;}
    VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=out.image;v.format=format;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
    v.subresourceRange={depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(auto r=b.f.createImageView(b.data.device,&v,nullptr,&out.view);r!=VK_SUCCESS){note("vkCreateImageView result "+std::to_string(int(r)));return false;}
    out.levels=levels;
    if(levels>1) {
        v.subresourceRange.levelCount=levels;
        if(auto r=b.f.createImageView(b.data.device,&v,nullptr,&out.mips);r!=VK_SUCCESS){note("vkCreateImageView (mips) result "+std::to_string(int(r)));return false;}
    }
    return true;
}
// keep: with load, the attachment starts and ends in this layout (used for the persistent sky memory, which is read by shaders in between).
// colorCount 2 adds a second color attachment (color2), used by the mask pass for the water shader's original color.
bool pass(WaterBatch& b,VkRenderPass& out,VkFormat color,bool depth,bool load=false,VkImageLayout keep=VK_IMAGE_LAYOUT_UNDEFINED,uint32_t colorCount=1,VkFormat color2=VK_FORMAT_UNDEFINED) {
    VkAttachmentDescription a[3]{};a[0].format=color;a[0].samples=VK_SAMPLE_COUNT_1_BIT;
    a[0].loadOp=load?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;a[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout=load?(keep?keep:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL):VK_IMAGE_LAYOUT_UNDEFINED;
    a[0].finalLayout=load?(keep?keep:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL):VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(colorCount>1){a[1]=a[0];a[1].format=color2;}
    a[colorCount]=a[0];a[colorCount].format=VK_FORMAT_D32_SFLOAT;a[colorCount].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference c[2]{{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}},z{colorCount,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=colorCount;sub.pColorAttachments=c;if(depth)sub.pDepthStencilAttachment=&z;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;deps[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;deps[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;deps[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;deps[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;info.attachmentCount=colorCount+(depth?1:0);info.pAttachments=a;info.subpassCount=1;info.pSubpasses=&sub;info.dependencyCount=2;info.pDependencies=deps;
    if(auto r=b.f.createRenderPass(b.data.device,&info,nullptr,&out);r!=VK_SUCCESS){note("vkCreateRenderPass format "+std::to_string(int(color))+" result "+std::to_string(int(r)));return false;}
    return true;
}
bool framebuffer(WaterBatch& b,VkFramebuffer& out,VkRenderPass pass,VkImageView color,VkImageView depth,uint32_t w,uint32_t h,VkImageView color2=VK_NULL_HANDLE) {
    VkImageView views[3];uint32_t count=0;views[count++]=color;if(color2)views[count++]=color2;if(depth)views[count++]=depth;
    VkFramebufferCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;i.renderPass=pass;i.attachmentCount=count;i.pAttachments=views;i.width=w;i.height=h;i.layers=1;
    if(auto r=b.f.createFramebuffer(b.data.device,&i,nullptr,&out);r!=VK_SUCCESS){note("vkCreateFramebuffer "+std::to_string(w)+"x"+std::to_string(h)+" result "+std::to_string(int(r)));return false;}
    return true;
}
VkShaderModule module(WaterBatch& b,const std::vector<uint32_t>& code) {
    if(code.empty()){if(lastFailure.empty())note("empty shader code");return {};}
    VkShaderModuleCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;i.codeSize=code.size()*4;i.pCode=code.data();VkShaderModule result{};
    if(auto r=b.f.createShaderModule(b.data.device,&i,nullptr,&result);r!=VK_SUCCESS){note("vkCreateShaderModule result "+std::to_string(int(r)));return VK_NULL_HANDLE;}
    return result;
}
VkShaderModule module(WaterBatch& b,const char* name){
    auto code=read_spirv_file(depth_shader_dir()+"/"+name+".spv");if(code.empty())note(std::string("shader file missing or unreadable: ")+depth_shader_dir()+"/"+name+".spv");
    return module(b,code);
}
VkPipeline pipeline(WaterBatch& b,VkPipelineLayout layout,VkRenderPass pass,VkShaderModule vs,VkShaderModule fs,
                    const Draw* draw,bool reflected,bool composite=false,uint32_t colorAttachments=1,bool premultiplied=false) {
    if(!vs || !fs){if(lastFailure.empty())note(std::string(!vs?"vertex":"fragment")+" shader module missing");return {};}
    VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
    VkSpecializationInfo special[2]{};
    VkPipelineVertexInputStateCreateInfo vertex{};vertex.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.lineWidth=1;
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{};clip.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
    VkPipelineDepthStencilStateCreateInfo depth{};depth.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;depth.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    std::vector<VkVertexInputBindingDescription> vertexBindings;
    if(draw) {
        const auto& p=*draw->pipeline;const auto& recipe=*p.water;auto e=effective_state(*draw);
        auto bindings=recipe.bindings;
        if(p.dynamic.count(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE))for(auto& binding:bindings)binding.stride=draw->vertices[binding.binding].stride;
        // Keep the adjusted input descriptions alive through pipeline creation.
        vertexBindings=std::move(bindings);
        vertex.vertexBindingDescriptionCount=vertexBindings.size();vertex.pVertexBindingDescriptions=vertexBindings.data();
        vertex.vertexAttributeDescriptionCount=recipe.attributes.size();vertex.pVertexAttributeDescriptions=recipe.attributes.data();
        assembly.topology=VkPrimitiveTopology(e.topology);assembly.primitiveRestartEnable=p.dynamic.count(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)?e.restart:recipe.restart;
        raster=recipe.rasterInfo;raster.pNext=nullptr;raster.cullMode=e.cull;raster.frontFace=VkFrontFace(e.frontFace^(reflected?1:0));raster.rasterizerDiscardEnable=VK_FALSE;
        raster.depthBiasEnable=e.biasEnable;raster.depthBiasConstantFactor=e.biasConstant;raster.depthBiasSlopeFactor=e.biasSlope;raster.depthBiasClamp=e.biasClamp;
        if(recipe.clipExtension){clip.depthClipEnable=recipe.depthClip;raster.pNext=&clip;}
        depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;depth.depthCompareOp=VkCompareOp(e.compare);
        stages[0].pName=p.vsEntry.c_str();stages[1].pName=p.fsEntry.c_str();
        for(unsigned n=0;n<2u;++n) {
            const auto& source=n?recipe.fs:recipe.vs;
            special[n].mapEntryCount=source.entries.size();special[n].pMapEntries=source.entries.data();special[n].dataSize=source.bytes.size();special[n].pData=source.bytes.data();
            if(!source.entries.empty())stages[n].pSpecializationInfo=&special[n];
        }
    }
    VkPipelineViewportStateCreateInfo viewport{};viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;viewport.viewportCount=viewport.scissorCount=1;
    VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ca{};ca.colorWriteMask=15;
    if(composite && premultiplied){ca.blendEnable=VK_TRUE;ca.srcColorBlendFactor=VK_BLEND_FACTOR_ONE;ca.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ca.colorBlendOp=VK_BLEND_OP_ADD;ca.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;ca.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;ca.alphaBlendOp=VK_BLEND_OP_ADD;}
    else if(composite){ca.blendEnable=VK_TRUE;ca.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;ca.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ca.colorBlendOp=VK_BLEND_OP_ADD;ca.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;ca.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;ca.alphaBlendOp=VK_BLEND_OP_ADD;}
    VkPipelineColorBlendAttachmentState attachments[2]{ca,ca};
    VkPipelineColorBlendStateCreateInfo blend{};blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;blend.attachmentCount=colorAttachments;blend.pAttachments=attachments;
    VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dyn{};dyn.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;dyn.dynamicStateCount=2;dyn.pDynamicStates=states;
    VkGraphicsPipelineCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;i.stageCount=2;i.pStages=stages;i.pVertexInputState=&vertex;i.pInputAssemblyState=&assembly;i.pViewportState=&viewport;
    i.pRasterizationState=&raster;i.pMultisampleState=&ms;i.pDepthStencilState=&depth;i.pColorBlendState=&blend;i.pDynamicState=&dyn;i.layout=layout;i.renderPass=pass;
    VkPipeline out{};auto result=b.f.createGraphicsPipelines(b.data.device,VK_NULL_HANDLE,1,&i,nullptr,&out);
    if(result!=VK_SUCCESS){note("vkCreateGraphicsPipelines result "+std::to_string(int(result)));if(out)b.f.destroyPipeline(b.data.device,out,nullptr);return {};}
    return out;
}
bool set_layout(WaterBatch& b,VkDescriptorSetLayout& out,const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    VkDescriptorSetLayoutCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;i.bindingCount=bindings.size();i.pBindings=bindings.data();
    if(auto r=b.f.createDescriptorSetLayout(b.data.device,&i,nullptr,&out);r!=VK_SUCCESS){note("vkCreateDescriptorSetLayout result "+std::to_string(int(r)));return false;}
    return true;
}
bool layout(WaterBatch& b,VkPipelineLayout& out,VkDescriptorSetLayout set) {
    VkPipelineLayoutCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;i.setLayoutCount=1;i.pSetLayouts=&set;
    if(auto r=b.f.createPipelineLayout(b.data.device,&i,nullptr,&out);r!=VK_SUCCESS){note("vkCreatePipelineLayout result "+std::to_string(int(r)));return false;}
    return true;
}
}
WaterLayout::~WaterLayout(){release();}
void WaterLayout::release() {
    if(layout)raw<PFN_vkDestroyPipelineLayout>(data,"vkDestroyPipelineLayout")(data.device,layout,nullptr);
    if(cameraSet)raw<PFN_vkDestroyDescriptorSetLayout>(data,"vkDestroyDescriptorSetLayout")(data.device,cameraSet,nullptr);
    if(emptySet)raw<PFN_vkDestroyDescriptorSetLayout>(data,"vkDestroyDescriptorSetLayout")(data.device,emptySet,nullptr);
    layout={};cameraSet=emptySet={};
}
static bool camera_descriptor_fits(const DeviceData& data,const Layout& layout,const VkPhysicalDeviceProperties& props) {
    // Adding one combined sampler must not cross either ordinary or descriptor
    // indexing limits. Application layouts already account for all other kinds.
    struct Counts {uint64_t samplers=0,images=0,resources=0;};
    Counts ordinary[3]{},all[3]{};bool indexed=false;
    for(const auto& set:layout.sets)if(set) {
        const bool update=set->flags&VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;indexed|=update;
        for(const auto& [number,binding]:set->bindings) {
            (void)number;
            const bool sampler=binding.type==VK_DESCRIPTOR_TYPE_SAMPLER || binding.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            const bool image=binding.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || binding.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || binding.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            for(unsigned stage=0;stage<3;++stage)if(stage==2 || (binding.stages&(stage?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT))) {
                auto add=[&](Counts& c){c.samplers+=sampler?binding.count:0;c.images+=image?binding.count:0;c.resources+=binding.count;};
                add(all[stage]);if(!update)add(ordinary[stage]);
            }
        }
    }
    const auto& limits=props.limits;
    if(ordinary[2].samplers+1>limits.maxDescriptorSetSamplers || ordinary[2].images+1>limits.maxDescriptorSetSampledImages)return false;
    for(unsigned stage=0;stage<2;++stage)if(ordinary[stage].samplers+1>limits.maxPerStageDescriptorSamplers || ordinary[stage].images+1>limits.maxPerStageDescriptorSampledImages || ordinary[stage].resources+1>limits.maxPerStageResources)return false;
    if(!indexed)return true;
    auto instance=instance_for_physical_device(data.physical_device);if(!instance)return false;
    auto dispatch=get_instance_data(*instance);if(!dispatch)return false;
    auto get=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(dispatch->next_gipa(*instance,"vkGetPhysicalDeviceProperties2"));
    if(!get)get=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(dispatch->next_gipa(*instance,"vkGetPhysicalDeviceProperties2KHR"));
    if(!get)return false;
    VkPhysicalDeviceDescriptorIndexingProperties indexing{};indexing.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};properties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;properties.pNext=&indexing;get(data.physical_device,&properties);
    if(all[2].samplers+1>indexing.maxDescriptorSetUpdateAfterBindSamplers || all[2].images+1>indexing.maxDescriptorSetUpdateAfterBindSampledImages)return false;
    for(unsigned stage=0;stage<2;++stage)if(all[stage].samplers+1>indexing.maxPerStageDescriptorUpdateAfterBindSamplers || all[stage].images+1>indexing.maxPerStageDescriptorUpdateAfterBindSampledImages || all[stage].resources+1>indexing.maxPerStageUpdateAfterBindResources)return false;
    return true;
}
void create_water_layout(const DeviceData& data,Layout& target,const VkPipelineLayoutCreateInfo& info) {
    if(!inspector_enabled() || info.pNext || info.setLayoutCount>3 || (info.flags&~VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) || !data.get_properties)return;
    VkPhysicalDeviceProperties props{};data.get_properties(data.physical_device,&props);if(props.limits.maxBoundDescriptorSets<4 || !camera_descriptor_fits(data,target,props))return;
    auto out=std::make_shared<WaterLayout>();out->data=data;
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo set{};set.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;set.bindingCount=1;set.pBindings=&binding;
    if(raw<PFN_vkCreateDescriptorSetLayout>(data,"vkCreateDescriptorSetLayout")(data.device,&set,nullptr,&out->cameraSet)!=VK_SUCCESS)return;
    set.bindingCount=0;set.pBindings=nullptr;
    if(raw<PFN_vkCreateDescriptorSetLayout>(data,"vkCreateDescriptorSetLayout")(data.device,&set,nullptr,&out->emptySet)!=VK_SUCCESS)return;
    VkDescriptorSetLayout sets[]{out->emptySet,out->emptySet,out->emptySet,out->cameraSet};
    for(uint32_t i=0;i<info.setLayoutCount;++i)sets[i]=info.pSetLayouts[i];
    auto ci=info;ci.setLayoutCount=4;ci.pSetLayouts=sets;
    if(raw<PFN_vkCreatePipelineLayout>(data,"vkCreatePipelineLayout")(data.device,&ci,nullptr,&out->layout)==VK_SUCCESS)target.water=std::move(out);
}
WaterBatch::~WaterBatch(){release();}
void WaterBatch::release() {
    if(!alive)return;
    alive=false;
    for(auto& [key,p]:pipelines){(void)key;if(p)f.destroyPipeline(data.device,p,nullptr);}pipelines.clear();
    for(auto p:{cameraPipeline,compositePipeline,skyPipeline})if(p)f.destroyPipeline(data.device,p,nullptr);
    for(auto p:{cameraFB,maskFB,planarFB,compositeFB,skyFB})if(p)f.destroyFramebuffer(data.device,p,nullptr);
    for(auto p:{cameraPass,maskPass,planarPass,compositePass,skyInitPass,skyPass})if(p)f.destroyRenderPass(data.device,p,nullptr);
    for(auto p:{resolveLayout,compositeLayout})if(p)f.destroyPipelineLayout(data.device,p,nullptr);
    for(auto p:{resolveSetLayout,compositeSetLayout})if(p)f.destroyDescriptorSetLayout(data.device,p,nullptr);
    for(auto p:{sampler,skySampler,tintSampler})if(p)f.destroySampler(data.device,p,nullptr);
    for(auto* t:{&detail,&modern}) {
        if(t->sampler)f.destroySampler(data.device,t->sampler,nullptr);
        if(t->view)f.destroyImageView(data.device,t->view,nullptr);
        if(t->image)f.destroyImage(data.device,t->image,nullptr);
        if(t->memory)f.freeMemory(data.device,t->memory,nullptr);
        if(t->staging)f.destroyBuffer(data.device,t->staging,nullptr);
        if(t->stagingMemory)f.freeMemory(data.device,t->stagingMemory,nullptr);
        *t={};
    }
    for(auto p:{colorView,depthView})if(p)f.destroyImageView(data.device,p,nullptr);
    for(auto* p:{&camera,&mask,&waterColor,&maskDepth,&planar,&planarDepth,&scene,&sky}) {
        for(auto v:{p->view,p->mips})if(v)f.destroyImageView(data.device,v,nullptr);
        if(p->image)f.destroyImage(data.device,p->image,nullptr);
        if(p->memory)f.freeMemory(data.device,p->memory,nullptr);
        *p={};
    }
    layouts.clear();
}
WaterSlot::~WaterSlot(){release();}
void WaterSlot::release() {
    if(!alive)return;
    alive=false;
    if(!batch)return;
    auto& f=batch->f;auto device=batch->data.device;
    if(queries)f.destroyQueryPool(device,queries,nullptr);
    if(pool)f.destroyDescriptorPool(device,pool,nullptr);
    if(params)f.destroyBuffer(device,params,nullptr);
    if(paramsMemory)f.freeMemory(device,paramsMemory,nullptr);
    queries={};pool={};params={};paramsMemory={};
}
bool water_batch_matches(const WaterBatch& b,const Draw& draw) {
    const auto& color=draw.target.colors[0];const auto& depth=draw.target.depth;
    return b.alive && color && depth && b.sourceColor==color->image->handle && b.sourceDepth==depth->image->handle &&
           b.sourceColorGeneration==color->image->generation && b.sourceDepthGeneration==depth->image->generation &&
           b.width==draw.target.width && b.height==draw.target.height && b.colorFormat==color->format;
}
std::shared_ptr<WaterBatch> create_water_batch(const DeviceData& data,const Draw& draw,uint32_t,const WaterParams&,
    const VkDescriptorBufferInfo&,const VkDescriptorBufferInfo&,const VkDescriptorBufferInfo&,std::string* why) {
    lastFailure.clear();
    // Every failure below returns through here, so the caller can report the step and result.
    auto fail=[&](const char* step){if(why)*why=std::string(step)+(lastFailure.empty()?"":": "+lastFailure);return std::shared_ptr<WaterBatch>{};};
    if(!data.get_format_properties || !data.get_memory_properties || !draw.pipeline->layout->water)return fail("missing device queries or private layout");
    auto b=std::make_shared<WaterBatch>();b->data=data;b->f=load_depth_overlay_fns(data);b->width=draw.target.width;b->height=draw.target.height;
    auto& f=b->f;auto device=data.device;if(!depth_overlay_fns_ok(f))return fail("device functions unavailable");
    auto color=draw.target.colors[0],depth=draw.target.depth;b->colorFormat=color->format;
    b->sourceColor=color->image->handle;b->sourceDepth=depth->image->handle;b->sourceColorGeneration=color->image->generation;b->sourceDepthGeneration=depth->image->generation;
    auto view=[&](VkImageView& out,const View& v,VkImageAspectFlags aspect) {
        VkImageViewCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;i.image=(VkImage)v.image->handle;i.format=v.format;i.viewType=VK_IMAGE_VIEW_TYPE_2D;i.subresourceRange={aspect,0,1,0,1};
        return f.createImageView(device,&i,nullptr,&out)==VK_SUCCESS;
    };
    if(!view(b->colorView,*color,VK_IMAGE_ASPECT_COLOR_BIT))return fail("source color view");
    if(!view(b->depthView,*depth,VK_IMAGE_ASPECT_DEPTH_BIT))return fail("source depth view");
    if(!image(*b,b->camera,VK_FORMAT_R32G32B32A32_SFLOAT,waterCameraWidth,1,false))return fail("camera image");
    if(!image(*b,b->mask,VK_FORMAT_R32G32B32A32_SFLOAT,b->width,b->height,false))return fail("mask image");
    if(!image(*b,b->waterColor,VK_FORMAT_R16G16B16A16_SFLOAT,b->width,b->height,false,false,waterColorLevels))return fail("water color image");
    if(!image(*b,b->maskDepth,VK_FORMAT_D32_SFLOAT,b->width,b->height,true))return fail("mask depth image");
    if(!image(*b,b->planar,VK_FORMAT_R16G16B16A16_SFLOAT,b->width,b->height,false))return fail("planar image");
    if(!image(*b,b->planarDepth,VK_FORMAT_D32_SFLOAT,b->width,b->height,true))return fail("planar depth image");
    if(!image(*b,b->scene,color->format,b->width,b->height,false,true))return fail("scene copy image (source color format)");
    if(!image(*b,b->sky,VK_FORMAT_R16G16B16A16_SFLOAT,waterSkyWidth,waterSkyHeight,false))return fail("sky memory image");
    if(!pass(*b,b->cameraPass,VK_FORMAT_R32G32B32A32_SFLOAT,false))return fail("camera render pass");
    if(!pass(*b,b->maskPass,VK_FORMAT_R32G32B32A32_SFLOAT,true,false,VK_IMAGE_LAYOUT_UNDEFINED,2,VK_FORMAT_R16G16B16A16_SFLOAT))return fail("mask render pass");
    if(!pass(*b,b->planarPass,VK_FORMAT_R16G16B16A16_SFLOAT,true))return fail("planar render pass");
    if(!pass(*b,b->compositePass,color->format,false,true))return fail("composite render pass (source color format)");
    if(!pass(*b,b->skyInitPass,VK_FORMAT_R16G16B16A16_SFLOAT,false,false) || !pass(*b,b->skyPass,VK_FORMAT_R16G16B16A16_SFLOAT,false,true,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))return fail("sky memory render passes");
    if(!framebuffer(*b,b->cameraFB,b->cameraPass,b->camera.view,{},waterCameraWidth,1))return fail("camera framebuffer");
    if(!framebuffer(*b,b->maskFB,b->maskPass,b->mask.view,b->maskDepth.view,b->width,b->height,b->waterColor.view))return fail("mask framebuffer");
    if(!framebuffer(*b,b->planarFB,b->planarPass,b->planar.view,b->planarDepth.view,b->width,b->height))return fail("planar framebuffer");
    if(!framebuffer(*b,b->compositeFB,b->compositePass,b->colorView,{},b->width,b->height))return fail("composite framebuffer on the game's color image");
    // One framebuffer serves both sky passes: they differ only in load op and layouts, so they are compatible.
    if(!framebuffer(*b,b->skyFB,b->skyPass,b->sky.view,{},waterSkyWidth,waterSkyHeight))return fail("sky memory framebuffer");
    VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(f.createSampler(device,&si,nullptr,&b->sampler)!=VK_SUCCESS)return fail("sampler");
    {   // The sky memory is read with bilinear filtering and wraps around in azimuth (u); elevation (v) clamps.
        VkFormatProperties skyCaps{};data.get_format_properties(data.physical_device,VK_FORMAT_R16G16B16A16_SFLOAT,&skyCaps);
        VkSamplerCreateInfo sk=si;sk.magFilter=sk.minFilter=(skyCaps.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        sk.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if(f.createSampler(device,&sk,nullptr,&b->skySampler)!=VK_SUCCESS)return fail("sky sampler");
        // The water colour: texelFetch at level 0 (exact) and trilinear reads of its mip chain (the texture-free tint).
        VkSamplerCreateInfo tint=si;tint.magFilter=tint.minFilter=b->waterColor.levels>1?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        tint.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;tint.maxLod=VK_LOD_CLAMP_NONE;
        if(f.createSampler(device,&tint,nullptr,&b->tintSampler)!=VK_SUCCESS)return fail("water colour sampler");
    }
    if(!create_textures(*b,"water_detail.bin",b->detail) || !create_textures(*b,"water_modern.bin",b->modern))return fail("water texture");
    const auto fragment=VK_SHADER_STAGE_FRAGMENT_BIT;
    if(!set_layout(*b,b->resolveSetLayout,{{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,fragment,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,fragment,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,fragment,nullptr},{3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,fragment,nullptr}}))return fail("resolve descriptor set layout");
    std::vector<VkDescriptorSetLayoutBinding> compositeBindings;
    for(uint32_t i=0;i<6;++i)compositeBindings.push_back({i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,fragment,nullptr});
    compositeBindings.push_back({6,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,fragment,nullptr});
    compositeBindings.push_back({7,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,fragment,nullptr}); // sky memory
    compositeBindings.push_back({8,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,fragment,nullptr}); // water textures (3-layer array)
    compositeBindings.push_back({9,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,fragment,nullptr}); // the water shader's own color
    compositeBindings.push_back({10,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,fragment,nullptr}); // MODERN water textures (3-layer array)
    if(!set_layout(*b,b->compositeSetLayout,compositeBindings) || !layout(*b,b->resolveLayout,b->resolveSetLayout) || !layout(*b,b->compositeLayout,b->compositeSetLayout))return fail("composite descriptor set layout or pipeline layouts");
    auto vs=module(*b,"depth.vert"),fs=module(*b,"water_camera.frag"),cs=module(*b,"water_composite.frag"),ss=module(*b,"water_sky.frag");
    b->cameraPipeline=pipeline(*b,b->resolveLayout,b->cameraPass,vs,fs,nullptr,false);
    b->compositePipeline=pipeline(*b,b->compositeLayout,b->compositePass,vs,cs,nullptr,false,true);
    b->skyPipeline=pipeline(*b,b->compositeLayout,b->skyPass,vs,ss,nullptr,false,true);
    for(auto m:{vs,fs,cs,ss})if(m)f.destroyShaderModule(device,m,nullptr);
    if(!b->cameraPipeline)return fail("camera-resolve pipeline");
    if(!b->compositePipeline)return fail("composite pipeline");
    if(!b->skyPipeline)return fail("sky memory pipeline");
    b->layouts.push_back(draw.pipeline->layout->water);return b;
}
std::shared_ptr<WaterSlot> create_water_slot(const std::shared_ptr<WaterBatch>& batch,const Draw& draw,const VkDescriptorBufferInfo& source,
    const VkDescriptorBufferInfo& vertices,const VkDescriptorBufferInfo& indices,std::string* why) {
    lastFailure.clear();
    auto slot=std::make_shared<WaterSlot>();slot->batch=batch;
    auto fail=[&](const char* step){if(why)*why=step;return std::shared_ptr<WaterSlot>{};};
    auto& b=*batch;auto& f=b.f;auto device=b.data.device;
    if(!draw.pipeline->layout->water)return fail("no private layout");
    VkBufferCreateInfo bi{};bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=sizeof(WaterParams);bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(f.createBuffer(device,&bi,nullptr,&slot->params)!=VK_SUCCESS)return fail("params buffer");
    VkMemoryRequirements requirements{};f.getBufferMemoryRequirements(device,slot->params,&requirements);
    if(!allocate(b.data,f,requirements,slot->paramsMemory) || f.bindBufferMemory(device,slot->params,slot->paramsMemory,0)!=VK_SUCCESS)return fail("params buffer memory");
    VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,11},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,3},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
    VkDescriptorPoolCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pi.maxSets=3;pi.poolSizeCount=3;pi.pPoolSizes=sizes;
    if(f.createDescriptorPool(device,&pi,nullptr,&slot->pool)!=VK_SUCCESS)return fail("descriptor pool");
    VkDescriptorSetLayout sets[]{b.resolveSetLayout,b.compositeSetLayout,draw.pipeline->layout->water->cameraSet};VkDescriptorSet descriptors[3]{};
    VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=slot->pool;ai.descriptorSetCount=3;ai.pSetLayouts=sets;
    if(f.allocateDescriptorSets(device,&ai,descriptors)!=VK_SUCCESS)return fail("descriptor sets");
    slot->resolveSet=descriptors[0];slot->compositeSet=descriptors[1];slot->cameraSet=descriptors[2];
    VkDescriptorBufferInfo buffers[]{source,vertices,indices,{slot->params,0,sizeof(WaterParams)}};
    std::vector<VkWriteDescriptorSet> writes;
    for(uint32_t i=0;i<4;++i){VkWriteDescriptorSet w{};w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=slot->resolveSet;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=(i==1 || i==2)?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.pBufferInfo=&buffers[i];writes.push_back(w);}
    VkDescriptorImageInfo images[]{{b.sampler,b.camera.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{b.sampler,b.mask.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {b.sampler,b.depthView,VK_IMAGE_LAYOUT_GENERAL},{b.sampler,b.scene.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{b.sampler,b.planar.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{b.sampler,b.planarDepth.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    for(uint32_t i=0;i<6;++i){VkWriteDescriptorSet w{};w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=slot->compositeSet;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&images[i];writes.push_back(w);}
    auto w=writes.back();w.dstBinding=6;w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.pImageInfo=nullptr;w.pBufferInfo=&buffers[3];writes.push_back(w);
    VkDescriptorImageInfo skyImage{b.skySampler,b.sky.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    {VkWriteDescriptorSet sw{};sw.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;sw.dstSet=slot->compositeSet;sw.dstBinding=7;sw.descriptorCount=1;sw.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;sw.pImageInfo=&skyImage;writes.push_back(sw);}
    VkDescriptorImageInfo detailImage{b.detail.sampler,b.detail.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},modernImage{b.modern.sampler,b.modern.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo waterColorImage{b.tintSampler,b.waterColor.mips?b.waterColor.mips:b.waterColor.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    {VkWriteDescriptorSet cw{};cw.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;cw.dstSet=slot->compositeSet;cw.dstBinding=9;cw.descriptorCount=1;cw.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;cw.pImageInfo=&waterColorImage;writes.push_back(cw);}
    {VkWriteDescriptorSet dw{};dw.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;dw.dstSet=slot->compositeSet;dw.dstBinding=8;dw.descriptorCount=1;dw.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;dw.pImageInfo=&detailImage;writes.push_back(dw);}
    {VkWriteDescriptorSet mw{};mw.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;mw.dstSet=slot->compositeSet;mw.dstBinding=10;mw.descriptorCount=1;mw.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;mw.pImageInfo=&modernImage;writes.push_back(mw);}
    w=writes[4];w.dstSet=slot->cameraSet;w.dstBinding=0;writes.push_back(w);f.updateDescriptorSets(device,writes.size(),writes.data(),0,nullptr);
    return slot;
}
VkPipeline water_draw_pipeline(WaterBatch& b,const Draw& draw,bool reflected,std::string* why) {
    auto& p=*draw.pipeline;auto e=effective_state(draw);
    std::array<uint64_t,26> key{p.serial,uint64_t(reflected),e.cull,e.frontFace,e.topology,e.biasEnable,e.compare};
    uint32_t bits[3]{};std::memcpy(bits,&e.biasConstant,sizeof(bits));for(unsigned i=0;i<3;++i)key[7+i]=bits[i];
    if(p.dynamic.count(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE))for(unsigned i=0;i<16;++i)key[10+i]=draw.vertices[i].stride;
    key[5]|=uint64_t(p.dynamic.count(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)?e.restart:p.water->restart)<<32;
    if(b.pipelines.count(key)){auto cached=b.pipelines[key];if(!cached && why)*why="earlier creation failure for this pipeline (see WATER-PIPELINE-SKIP in the log)";return cached;}
    if(b.pipelines.size()>=512 || !water_pipeline_eligible(p) || !p.layout->water || !p.layout->water->layout)return {};
    lastFailure.clear();
    std::string reason;auto words=reflected?reflect_vertex_spirv(p.vs->words,p.vsEntry,reason):p.vs->words;
    auto fragment=reflected?p.fs->words:water_fragment_spirv(p.fs->words,p.fsEntry,reason);
    auto vs=module(b,words),fs=module(b,fragment);
    auto out=pipeline(b,p.layout->water->layout,reflected?b.planarPass:b.maskPass,vs,fs,&draw,reflected,false,reflected?1u:2u);   // mask pass: mask + original color
    for(auto m:{vs,fs})if(m)b.f.destroyShaderModule(b.data.device,m,nullptr);
    if(!out) {
        const std::string text=!reason.empty()?reason:lastFailure.empty()?std::string("private pipeline creation failed"):lastFailure;
        if(why)*why=text;
        if(b.pipelines.size()<8)log_line("[WATER-PIPELINE-SKIP] pipeline="+std::to_string(p.serial)+" reflected="+std::to_string(reflected)+" reason="+text);
    }
    b.pipelines[key]=out;b.layouts.push_back(p.layout->water);return out;
}
// The first executions of a batch each upload the (constant) water textures: any of them may be the first to run on the GPU, and every one is
// self-contained. Later executions find the image already in shader-read layout.
void water_upload_detail(WaterBatch& b,VkCommandBuffer cmd) {
    for(auto* t:{&b.detail,&b.modern}) {
        if(!t->image || !t->staging || t->copies.empty() || t->uploads>=waterDetailUploadExecutions)continue;
        VkImageMemoryBarrier v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;v.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;v.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;v.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        v.srcQueueFamilyIndex=v.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;v.image=t->image;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,t->levels,0,3};
        b.f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&v);
        raw<PFN_vkCmdCopyBufferToImage>(b.data,"vkCmdCopyBufferToImage")(cmd,t->staging,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(t->copies.size()),t->copies.data());
        v.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;v.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;v.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;v.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&v);
        ++t->uploads;
    }
}
}
