#include "discovery_internal.hpp"
#include "water_reflections.hpp"
#include "overlay_menu.hpp"
#include "overlay_uniform.hpp"
#include "shader_resources.hpp"
#include "sync_tracking.hpp"
#include <cstdio>
#include <filesystem>
using namespace depth_probe;
namespace disc=depth_probe::discovery;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
#define H(api) reinterpret_cast<PFN_##api>(discovery_hook(#api))
template<class T> T handle(uint64_t value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(uintptr_t(value));else return value;}
static uint64_t nextHandle=100;
struct Dispatchable {void* dispatch;};
static int key;
static Dispatchable dev{&key},queueObject{&key},buffers[8];
static unsigned allocated=0;
static VkDevice device=reinterpret_cast<VkDevice>(&dev);
static VkQueue queue=reinterpret_cast<VkQueue>(&queueObject);
static unsigned draws=0,indexedDraws=0,submitCalls=0,presentCalls=0;
static VkResult submitResult=VK_SUCCESS;
static const VkSubmitInfo* expectedSubmit=nullptr;
static const VkPresentInfoKHR* expectedPresent=nullptr;
static VkFence expectedFence{};
template<class Info,class Out> VkResult create(VkDevice,const Info*,const VkAllocationCallbacks*,Out* out){*out=handle<Out>(nextHandle++);return VK_SUCCESS;}
template<class T> void destroy(VkDevice,T,const VkAllocationCallbacks*){}
static VkResult pipelines(VkDevice,VkPipelineCache,uint32_t count,const VkGraphicsPipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline* out){for(uint32_t i=0;i<count;++i)out[i]=handle<VkPipeline>(nextHandle++);return VK_SUCCESS;}
static VkResult allocate(VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out){CHECK(allocated+info->commandBufferCount<=8);for(uint32_t i=0;i<info->commandBufferCount;++i){buffers[allocated].dispatch=&key;out[i]=reinterpret_cast<VkCommandBuffer>(&buffers[allocated++]);}return VK_SUCCESS;}
static void freeCommands(VkDevice,VkCommandPool,uint32_t,const VkCommandBuffer*){}
static VkResult begin(VkCommandBuffer,const VkCommandBufferBeginInfo*){return VK_SUCCESS;}
static VkResult end(VkCommandBuffer){return VK_SUCCESS;}
static VkResult reset(VkCommandBuffer,VkCommandBufferResetFlags){return VK_SUCCESS;}
static VkResult resetPool(VkDevice,VkCommandPool,VkCommandPoolResetFlags){return VK_SUCCESS;}
static VkResult allocateSets(VkDevice,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* out){for(uint32_t i=0;i<info->descriptorSetCount;++i)out[i]=handle<VkDescriptorSet>(nextHandle++);return VK_SUCCESS;}
static VkResult freeSets(VkDevice,VkDescriptorPool,uint32_t,const VkDescriptorSet*){return VK_SUCCESS;}
static VkResult resetDescriptors(VkDevice,VkDescriptorPool,VkDescriptorPoolResetFlags){return VK_SUCCESS;}
static void update(VkDevice,uint32_t,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*){}
static void updateTemplate(VkDevice,VkDescriptorSet,VkDescriptorUpdateTemplate,const void*){}
static void bind(VkCommandBuffer,VkPipelineBindPoint,VkPipeline){}
static void bindSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*){}
static void bindSets2(VkCommandBuffer,const VkBindDescriptorSetsInfo*){}
static void pushConstants(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,uint32_t,uint32_t,const void*){}
static void pushConstants2(VkCommandBuffer,const VkPushConstantsInfoKHR*){}
static void bindShaders(VkCommandBuffer,uint32_t,const VkShaderStageFlagBits*,const VkShaderEXT*){}
static void pushTemplate(VkCommandBuffer,VkDescriptorUpdateTemplate,VkPipelineLayout,uint32_t,const void*){}
static void identifier(VkDevice,const VkShaderModuleCreateInfo* info,VkShaderModuleIdentifierEXT* out){auto h=disc::hash_bytes(info->pCode,info->codeSize);out->identifierSize=8;std::memcpy(out->identifier,&h,8);}
static void rendering(VkCommandBuffer,const VkRenderingInfo*){}
static void endRendering(VkCommandBuffer){}
static void viewport(VkCommandBuffer,uint32_t,uint32_t,const VkViewport*){}
static void scissor(VkCommandBuffer,uint32_t,uint32_t,const VkRect2D*){}
static void depthWrite(VkCommandBuffer,VkBool32){}
static void cullMode(VkCommandBuffer,VkCullModeFlags){}
static void stencilValue(VkCommandBuffer,VkStencilFaceFlags,uint32_t){}
static void stencilOp(VkCommandBuffer,VkStencilFaceFlags,VkStencilOp,VkStencilOp,VkStencilOp,VkCompareOp){}
static void floats4(VkCommandBuffer,const float*){}
static void oneFloat(VkCommandBuffer,float){}
static void twoFloats(VkCommandBuffer,float,float){}
static void biasValues(VkCommandBuffer,float,float,float){}
static void logicOp(VkCommandBuffer,VkLogicOp){}
static void viewportCount(VkCommandBuffer,uint32_t,const VkViewport*){}
static void blendEquationStub(VkCommandBuffer,uint32_t,uint32_t,const VkColorBlendEquationEXT*){}
static void writeMaskStub(VkCommandBuffer,uint32_t,uint32_t,const VkColorComponentFlags*){}
static void blendEnableStub(VkCommandBuffer,uint32_t,uint32_t,const VkBool32*){}
static void draw(VkCommandBuffer,uint32_t count,uint32_t instances,uint32_t first,uint32_t firstInstance){CHECK(count==3 && instances==1 && first==0 && firstInstance==0);++draws;}
static void indexed(VkCommandBuffer,uint32_t count,uint32_t instances,uint32_t first,int32_t offset,uint32_t firstInstance){CHECK(count==9 && instances==2 && first==4 && offset==-2 && firstInstance==1);++indexedDraws;}
static void indirect(VkCommandBuffer,VkBuffer,VkDeviceSize,uint32_t,uint32_t){++draws;}
static void queryBegin(VkCommandBuffer,VkQueryPool,uint32_t,VkQueryControlFlags){}
static void queryEnd(VkCommandBuffer,VkQueryPool,uint32_t){}
static void execute(VkCommandBuffer,uint32_t,const VkCommandBuffer*){}
static void beginPass(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents){}
static VkResult submit(VkQueue,uint32_t count,const VkSubmitInfo* info,VkFence fence){CHECK(count==1 && info==expectedSubmit && fence==expectedFence);++submitCalls;return submitResult;}
static VkResult present(VkQueue,const VkPresentInfoKHR* info){CHECK(info==expectedPresent);++presentCalls;return VK_SUCCESS;}
static PFN_vkVoidFunction VKAPI_CALL dispatch(VkDevice,const char* name) {
#define FN(api,fn) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(fn)
    FN(vkCreateImage,(create<VkImageCreateInfo,VkImage>));FN(vkDestroyImage,(destroy<VkImage>));
    FN(vkCreateImageView,(create<VkImageViewCreateInfo,VkImageView>));FN(vkDestroyImageView,(destroy<VkImageView>));
    FN(vkCreateSampler,(create<VkSamplerCreateInfo,VkSampler>));FN(vkDestroySampler,(destroy<VkSampler>));
    FN(vkCreateDescriptorSetLayout,(create<VkDescriptorSetLayoutCreateInfo,VkDescriptorSetLayout>));FN(vkDestroyDescriptorSetLayout,(destroy<VkDescriptorSetLayout>));
    FN(vkCreatePipelineLayout,(create<VkPipelineLayoutCreateInfo,VkPipelineLayout>));FN(vkDestroyPipelineLayout,(destroy<VkPipelineLayout>));
    FN(vkAllocateDescriptorSets,allocateSets);FN(vkFreeDescriptorSets,freeSets);FN(vkUpdateDescriptorSets,update);
    FN(vkResetDescriptorPool,resetDescriptors);FN(vkDestroyDescriptorPool,(destroy<VkDescriptorPool>));
    FN(vkCreateDescriptorUpdateTemplate,(create<VkDescriptorUpdateTemplateCreateInfo,VkDescriptorUpdateTemplate>));FN(vkDestroyDescriptorUpdateTemplate,(destroy<VkDescriptorUpdateTemplate>));FN(vkUpdateDescriptorSetWithTemplate,updateTemplate);
    FN(vkCreateShaderModule,(create<VkShaderModuleCreateInfo,VkShaderModule>));FN(vkDestroyShaderModule,(destroy<VkShaderModule>));
    FN(vkGetShaderModuleCreateInfoIdentifierEXT,identifier);
    FN(vkCreateGraphicsPipelines,pipelines);FN(vkDestroyPipeline,(destroy<VkPipeline>));
    FN(vkAllocateCommandBuffers,allocate);FN(vkFreeCommandBuffers,freeCommands);FN(vkBeginCommandBuffer,begin);FN(vkEndCommandBuffer,end);FN(vkResetCommandBuffer,reset);FN(vkResetCommandPool,resetPool);FN(vkDestroyCommandPool,(destroy<VkCommandPool>));
    FN(vkCmdBindPipeline,bind);FN(vkCmdBindDescriptorSets,bindSets);FN(vkCmdBeginRendering,rendering);FN(vkCmdEndRendering,endRendering);
    FN(vkCmdBindDescriptorSets2,bindSets2);FN(vkCmdBindShadersEXT,bindShaders);FN(vkCmdPushDescriptorSetWithTemplateKHR,pushTemplate);
    FN(vkCmdPushConstants,pushConstants);FN(vkCmdPushConstants2,pushConstants2);FN(vkCmdPushConstants2KHR,pushConstants2);
    FN(vkCmdSetViewport,viewport);FN(vkCmdSetScissor,scissor);FN(vkCmdSetDepthWriteEnable,depthWrite);
    FN(vkCmdSetCullModeEXT,cullMode);FN(vkCmdSetStencilCompareMask,stencilValue);FN(vkCmdSetStencilWriteMask,stencilValue);FN(vkCmdSetStencilReference,stencilValue);
    FN(vkCmdSetStencilOp,stencilOp);FN(vkCmdSetBlendConstants,floats4);FN(vkCmdSetLineWidth,oneFloat);FN(vkCmdSetDepthBounds,twoFloats);FN(vkCmdSetDepthBias,biasValues);
    FN(vkCmdSetLogicOpEXT,logicOp);FN(vkCmdSetColorBlendEquationEXT,blendEquationStub);FN(vkCmdSetColorWriteMaskEXT,writeMaskStub);FN(vkCmdSetColorBlendEnableEXT,blendEnableStub);FN(vkCmdSetViewportWithCount,viewportCount);
    FN(vkCmdDraw,draw);FN(vkCmdDrawIndexed,indexed);FN(vkCmdDrawIndirect,indirect);FN(vkCmdDrawIndexedIndirect,indirect);
    FN(vkCmdBeginQuery,queryBegin);FN(vkCmdEndQuery,queryEnd);FN(vkCmdExecuteCommands,execute);
    FN(vkCreateRenderPass,(create<VkRenderPassCreateInfo,VkRenderPass>));FN(vkDestroyRenderPass,(destroy<VkRenderPass>));
    FN(vkCreateFramebuffer,(create<VkFramebufferCreateInfo,VkFramebuffer>));FN(vkDestroyFramebuffer,(destroy<VkFramebuffer>));FN(vkCmdBeginRenderPass,beginPass);FN(vkCmdEndRenderPass,endRendering);
    FN(vkQueueSubmit,submit);FN(vkQueuePresentKHR,present);
#undef FN
    std::fprintf(stderr,"missing downstream: %s\n",name);return nullptr;
}
#include "surface_inspector.hpp"
#include "sampler_tracking_cases.hpp"
int main() {
    setenv("DEPTH_PROBE_PIPELINE_EXPLORER","1",1);setenv("DEPTH_PROBE_UI_INPUT","0",1);setenv("DEPTH_PROBE_DEPTH_PRESENT","0",1);
    setenv("DEPTH_PROBE_LAYER_LOG","/tmp/depth-probe-discovery-test.log",1);
    auto directory="/tmp/depth-probe-discovery-test-"+std::to_string(getpid());setenv("DEPTH_PROBE_DISCOVERY_DIR",directory.c_str(),1);
    std::filesystem::remove("/tmp/depth-probe-discovery-test.log");
    DeviceData data;data.device=device;data.next_gdpa=dispatch;data.next_create_image=create<VkImageCreateInfo,VkImage>;data.next_destroy_image=destroy<VkImage>;
    data.next_create_image_view=create<VkImageViewCreateInfo,VkImageView>;data.next_destroy_image_view=destroy<VkImageView>;data.next_queue_present=present;
    data.sync=std::make_shared<SyncDeviceState>();g_devices[&key]=data;
    auto makeImage=[&](VkFormat format,uint32_t width,VkImageAspectFlags aspect) {
        VkImageCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;info.imageType=VK_IMAGE_TYPE_2D;info.format=format;info.extent={width,width,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|(aspect==VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        VkImage image;CHECK(H(vkCreateImage)(device,&info,nullptr,&image)==VK_SUCCESS);
        VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={aspect,0,1,0,1};
        VkImageView view;CHECK(H(vkCreateImageView)(device,&vi,nullptr,&view)==VK_SUCCESS);return std::pair{image,view};
    };
    auto [depth,depthView]=makeImage(VK_FORMAT_D32_SFLOAT,2048,VK_IMAGE_ASPECT_DEPTH_BIT);
    auto [otherDepth,otherView]=makeImage(VK_FORMAT_D32_SFLOAT,1024,VK_IMAGE_ASPECT_DEPTH_BIT);
    auto [color,colorView]=makeImage(VK_FORMAT_R8G8B8A8_UNORM,64,VK_IMAGE_ASPECT_COLOR_BIT);
    VkSamplerCreateInfo samplerInfo{};samplerInfo.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;samplerInfo.compareEnable=1;samplerInfo.compareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    VkSampler sampler;CHECK(H(vkCreateSampler)(device,&samplerInfo,nullptr,&sampler)==VK_SUCCESS);
    VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;binding.pImmutableSamplers=&sampler;
    VkDescriptorSetLayoutCreateInfo sl{};sl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;sl.bindingCount=1;sl.pBindings=&binding;
    VkDescriptorSetLayout setLayout;CHECK(H(vkCreateDescriptorSetLayout)(device,&sl,nullptr,&setLayout)==VK_SUCCESS);
    VkPipelineLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.setLayoutCount=1;li.pSetLayouts=&setLayout;
    VkPipelineLayout layout;CHECK(H(vkCreatePipelineLayout)(device,&li,nullptr,&layout)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=handle<VkDescriptorPool>(42);ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
    VkDescriptorSet set,copySet;CHECK(H(vkAllocateDescriptorSets)(device,&ai,&set)==VK_SUCCESS);CHECK(H(vkAllocateDescriptorSets)(device,&ai,&copySet)==VK_SUCCESS);
    auto updateImage=[&](VkImageView view) {
        VkDescriptorImageInfo image{};image.imageView=view;image.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.descriptorCount=1;w.descriptorType=binding.descriptorType;w.pImageInfo=&image;H(vkUpdateDescriptorSets)(device,1,&w,0,nullptr);
    };updateImage(depthView);
    auto shader=[&](const char* file) {
        auto words=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/"+file+".spv");CHECK(!words.empty());
        auto analysis=disc::inspect_spirv(words.data(),words.size()*4);CHECK(analysis.valid && analysis.isolationSafe);
        if(std::string(file)=="discovery_depth.frag"){CHECK(analysis.drefs==1 && analysis.samples==1 && analysis.unresolvedSamples==0);CHECK(analysis.bindings.size()==1 && analysis.bindings[0].dref);}
        VkShaderModuleCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;info.codeSize=words.size()*4;info.pCode=words.data();VkShaderModule module;CHECK(H(vkCreateShaderModule)(device,&info,nullptr,&module)==VK_SUCCESS);return module;
    };
    const auto vs=shader("discovery_scene.vert"),fs=shader("discovery_depth.frag");
    auto makePipeline=[&](bool write,bool library=false,unsigned source=0) {
        VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
        std::vector<uint32_t> codes[2]={read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/discovery_scene.vert.spv"),read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/discovery_depth.frag.spv")};
        VkShaderModuleCreateInfo inlineCode[2]{};VkShaderModuleIdentifierEXT identifiers[2]{};VkPipelineShaderStageModuleIdentifierCreateInfoEXT references[2]{};
        if(source)for(unsigned i=0;i<2;++i) {
            inlineCode[i].sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;inlineCode[i].codeSize=codes[i].size()*4;inlineCode[i].pCode=codes[i].data();
            stages[i].module={};stages[i].pNext=&inlineCode[i];
            if(source>=2) {
                identifiers[i].sType=VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT;
                H(vkGetShaderModuleCreateInfoIdentifierEXT)(device,&inlineCode[i],&identifiers[i]);
                if(source==3)identifiers[i].identifier[0]^=0x55;
                references[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT;references[i].identifierSize=identifiers[i].identifierSize;references[i].pIdentifier=identifiers[i].identifier;
                stages[i].pNext=&references[i];
            }
        }
        VkPipelineDepthStencilStateCreateInfo depth{};depth.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;depth.depthTestEnable=1;depth.depthWriteEnable=write;depth.depthCompareOp=VK_COMPARE_OP_LESS;
        VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.lineWidth=1;
        VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blend{};blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;blend.attachmentCount=1;blend.pAttachments=&attachment;
        VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
        VkPipelineDynamicStateCreateInfo dynamic{};dynamic.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;dynamic.dynamicStateCount=3;dynamic.pDynamicStates=states;
        VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;info.stageCount=2;info.pStages=stages;info.pRasterizationState=&raster;info.pDepthStencilState=&depth;info.pInputAssemblyState=&assembly;info.pColorBlendState=&blend;info.pDynamicState=&dynamic;info.layout=layout;
        VkGraphicsPipelineLibraryCreateInfoEXT parts{};parts.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;parts.flags=15;
        if(library){info.flags=VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;info.pNext=&parts;}
        VkPipeline pipeline;CHECK(H(vkCreateGraphicsPipelines)(device,{},1,&info,nullptr,&pipeline)==VK_SUCCESS);return pipeline;
    };
    auto producer=makePipeline(true),consumer=makePipeline(false);
    auto library=makePipeline(false,true);
    // Pipeline-library linking retains shader identities from destroyed modules.
    VkPipelineLibraryCreateInfoKHR links{};links.sType=VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;links.libraryCount=1;links.pLibraries=&library;
    VkGraphicsPipelineCreateInfo linkedInfo{};linkedInfo.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;linkedInfo.pNext=&links;linkedInfo.layout=layout;
    VkPipeline linked;CHECK(H(vkCreateGraphicsPipelines)(device,{},1,&linkedInfo,nullptr,&linked)==VK_SUCCESS);
    {std::lock_guard lock(disc::mutex);auto p=disc::devices.at(device).pipelines.at(disc::id(linked));CHECK(p->vs && p->fs && p->parts==15 && p->isolationSafe);}
    H(vkDestroyShaderModule)(device,vs,nullptr);H(vkDestroyShaderModule)(device,fs,nullptr);
    auto inlinePipeline=makePipeline(false,true,1),identifierPipeline=makePipeline(false,false,2),unknownIdentifier=makePipeline(false,false,3);
    links.pLibraries=&inlinePipeline;VkPipeline inlineLinked;CHECK(H(vkCreateGraphicsPipelines)(device,{},1,&linkedInfo,nullptr,&inlineLinked)==VK_SUCCESS);
    H(vkDestroyPipeline)(device,inlinePipeline,nullptr);
    {std::lock_guard lock(disc::mutex);const auto& state=disc::devices.at(device);auto p=state.pipelines.at(disc::id(inlineLinked)),q=state.pipelines.at(disc::id(identifierPipeline));
        CHECK(p->vs && p->fs && q->vs && q->fs && p->vs->hash==q->vs->hash && p->fs->hash==q->fs->hash);
        CHECK(p->vsSource=="inline-spirv" && q->fsSource=="identifier-snapshot" && !p->vs->words.empty());
        CHECK(!state.pipelines.at(disc::id(unknownIdentifier))->vs && state.pipelines.at(disc::id(unknownIdentifier))->fsSource=="identifier-bytes-unobserved");
        CHECK(state.shaderSnapshots.size()==2);}
    // DXVK's D3D9 alpha test (draw_semantics: opaque vs alpha-tested): tests/dxvk_alpha_test.frag has the structure of every
    // WoW fragment shader under DXVK. Its compare op comes from the fragment stage's specialization (SpecId 20 = use spec
    // constants, SpecId 1 bits 21..23 = VkCompareOp; ALWAYS = off); without SpecId 20 it is read from DXVK's UBO: unknown.
    {
        auto words=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/dxvk_alpha_test.frag.spv");CHECK(!words.empty());
        const auto analysis=disc::inspect_spirv(words.data(),words.size()*4);
        CHECK(analysis.valid && analysis.discards && analysis.dxvkAlphaTest.found);
        CHECK(analysis.dxvkAlphaTest.useSpecId==20 && analysis.dxvkAlphaTest.wordSpecId==1 && analysis.dxvkAlphaTest.offset==21 && analysis.dxvkAlphaTest.bits==3);
        // The fast-link side (roadmap M4): spec_state.dwords[1] at set 2 binding 5, byte 16 (std140: a uint array has a 16-byte stride).
        CHECK(analysis.dxvkAlphaTest.uboKnown && analysis.dxvkAlphaTest.uboSet==2 && analysis.dxvkAlphaTest.uboBinding==5 && analysis.dxvkAlphaTest.uboOffset==16);
        auto plain=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/discovery_depth.frag.spv");
        CHECK(!disc::inspect_spirv(plain.data(),plain.size()*4).dxvkAlphaTest.found);
        VkShaderModuleCreateInfo mi{};mi.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;mi.codeSize=words.size()*4;mi.pCode=words.data();
        VkShaderModule alphaFs;CHECK(H(vkCreateShaderModule)(device,&mi,nullptr,&alphaFs)==VK_SUCCESS);
        auto build=[&](const uint32_t* values,uint32_t count,bool fragmentLibrary) {
            VkSpecializationMapEntry entries[2]{{20,0,4},{1,4,4}};
            VkSpecializationInfo spec{count,entries,count*4u,values};
            VkPipelineShaderStageCreateInfo stage{};stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.stage=VK_SHADER_STAGE_FRAGMENT_BIT;
            stage.module=alphaFs;stage.pName="main";stage.pSpecializationInfo=count?&spec:nullptr;
            VkPipelineDepthStencilStateCreateInfo depth{};depth.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;depth.depthTestEnable=1;depth.depthWriteEnable=1;
            VkGraphicsPipelineLibraryCreateInfoEXT parts{};parts.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;parts.flags=VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
            VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;info.stageCount=1;info.pStages=&stage;info.pDepthStencilState=&depth;info.layout=layout;
            if(fragmentLibrary){info.flags=VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;info.pNext=&parts;}
            VkPipeline pipeline;CHECK(H(vkCreateGraphicsPipelines)(device,{},1,&info,nullptr,&pipeline)==VK_SUCCESS);return pipeline;
        };
        const uint32_t off[2]{1,uint32_t(VK_COMPARE_OP_ALWAYS)<<21 | 0x5},greater[2]{1,uint32_t(VK_COMPARE_OP_GREATER)<<21},fastLink[2]{0,uint32_t(VK_COMPARE_OP_GREATER)<<21};
        const auto alwaysPipeline=build(off,2,false),greaterPipeline=build(greater,2,false),fastPipeline=build(fastLink,2,false),bare=build(nullptr,0,false);
        const auto fragmentPart=build(greater,2,true);
        VkPipelineLibraryCreateInfoKHR fragmentLinks{};fragmentLinks.sType=VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;fragmentLinks.libraryCount=1;fragmentLinks.pLibraries=&fragmentPart;
        VkGraphicsPipelineCreateInfo li2{};li2.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;li2.pNext=&fragmentLinks;li2.layout=layout;
        VkPipeline fragmentLinked;CHECK(H(vkCreateGraphicsPipelines)(device,{},1,&li2,nullptr,&fragmentLinked)==VK_SUCCESS);
        std::lock_guard lock(disc::mutex);const auto& state=disc::devices.at(device);
        auto alpha=[&](VkPipeline p){return state.pipelines.at(disc::id(p))->alphaTest;};
        CHECK(alpha(alwaysPipeline).state==0 && alpha(alwaysPipeline).compare==VK_COMPARE_OP_ALWAYS && std::string(alpha(alwaysPipeline).source)=="specialization");
        CHECK(alpha(greaterPipeline).state==1 && alpha(greaterPipeline).compare==VK_COMPARE_OP_GREATER);
        CHECK(alpha(fastPipeline).state==-1 && std::string(alpha(fastPipeline).source)=="fast-link (DXVK spec UBO)");
        CHECK(alpha(bare).state==-1);                                                          // module defaults: SpecId 20 = 0
        CHECK(alpha(fragmentLinked).state==1 && alpha(fragmentLinked).compare==VK_COMPARE_OP_GREATER);   // carried over from the fragment library
        CHECK(state.pipelines.at(disc::id(producer))->alphaTest.state==-1 && std::string(state.pipelines.at(disc::id(producer))->alphaTest.source)=="no DXVK alpha-test pattern");
    }
    VkCommandBufferAllocateInfo ca{};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ca.commandPool=handle<VkCommandPool>(44);ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=2;
    VkCommandBuffer cmds[2];CHECK(H(vkAllocateCommandBuffers)(device,&ca,cmds)==VK_SUCCESS);auto cmd=cmds[0];
    sampler_tracking_cases(cmd);
    VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    auto bindState=[&](VkPipeline pipeline,bool write) {
        H(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);H(vkCmdSetDepthWriteEnable)(cmd,write);
        VkViewport viewport{0,0,64,64,0,1};VkRect2D scissor{{0,0},{64,64}};H(vkCmdSetViewport)(cmd,0,1,&viewport);H(vkCmdSetScissor)(cmd,0,1,&scissor);
    };
    VkRenderingAttachmentInfo da{};da.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;da.imageView=depthView;da.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;da.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;da.clearValue.depthStencil.depth=1;
    VkRenderingAttachmentInfo col{};col.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;col.imageView=colorView;col.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkRenderingInfo ri{};ri.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;ri.renderArea.extent={64,64};ri.layerCount=1;ri.pDepthAttachment=&da;
    {
        // DXVK chains structs onto rendering attachments. That does not change imageLayout, so the layouts the instance
        // began with (which are also its end layouts) must still be recorded for the water replay.
        VkCommandBufferAllocateInfo one=ca;one.commandBufferCount=1;VkCommandBuffer extra{};CHECK(H(vkAllocateCommandBuffers)(device,&one,&extra)==VK_SUCCESS);
        VkBaseInStructure chained{VK_STRUCTURE_TYPE_MAX_ENUM,nullptr};VkRenderingAttachmentInfo colorChained=col;colorChained.pNext=&chained;
        VkRenderingInfo info{};info.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;info.renderArea.extent={64,64};info.layerCount=1;info.colorAttachmentCount=1;info.pColorAttachments=&colorChained;info.pDepthAttachment=&da;
        CHECK(H(vkBeginCommandBuffer)(extra,&bi)==VK_SUCCESS);H(vkCmdBeginRendering)(extra,&info);
        {std::lock_guard lock(disc::mutex);const auto& target=disc::devices.at(device).commands.at(disc::id(extra)).target;
         CHECK(target.known && target.renderingLayoutsKnown && target.renderingColorLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && target.renderingDepthLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);}
        H(vkCmdEndRendering)(extra);
        // A resolve attachment makes the target shape unknown, so no layout may be claimed.
        colorChained.pNext=nullptr;colorChained.resolveMode=VK_RESOLVE_MODE_AVERAGE_BIT;colorChained.resolveImageView=colorView;
        H(vkCmdBeginRendering)(extra,&info);
        {std::lock_guard lock(disc::mutex);const auto& target=disc::devices.at(device).commands.at(disc::id(extra)).target;CHECK(!target.known && !target.renderingLayoutsKnown);}
        H(vkCmdEndRendering)(extra);CHECK(H(vkEndCommandBuffer)(extra)==VK_SUCCESS);
    }
    CHECK(H(vkBeginCommandBuffer)(cmd,&bi)==VK_SUCCESS);H(vkCmdBeginRendering)(cmd,&ri);bindState(producer,true);H(vkCmdDraw)(cmd,3,1,0,0);H(vkCmdEndRendering)(cmd);
    ri.colorAttachmentCount=1;ri.pColorAttachments=&col;ri.pDepthAttachment=nullptr;
    H(vkCmdBeginRendering)(cmd,&ri);bindState(consumer,false);H(vkCmdBindDescriptorSets)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
    H(vkCmdDrawIndexed)(cmd,9,2,4,-2,1);H(vkCmdDraw)(cmd,3,1,0,0);H(vkCmdEndRendering)(cmd);CHECK(H(vkEndCommandBuffer)(cmd)==VK_SUCCESS);
    CHECK(draws==2 && indexedDraws==1);
    // Descriptor mutation after command recording is resolved at submit, not at bind.
    updateImage(otherView);
    VkSemaphore wait=handle<VkSemaphore>(51),signal=handle<VkSemaphore>(52);VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.commandBufferCount=1;si.pCommandBuffers=&cmd;si.waitSemaphoreCount=1;si.pWaitSemaphores=&wait;si.pWaitDstStageMask=&waitStage;si.signalSemaphoreCount=1;si.pSignalSemaphores=&signal;
    expectedSubmit=&si;expectedFence=handle<VkFence>(53);
    CHECK(H(vkQueueSubmit)(queue,1,&si,expectedFence)==VK_SUCCESS);
    CHECK(data.sync->serial==1 && data.sync->signals.at(signal).queue==queue);
    {
        std::lock_guard lock(disc::mutex);const auto& pending=disc::devices.at(device).pending.at(disc::id(queue));CHECK(pending.draws.size()==3);
        CHECK(pending.draws[1].resources.size()==1);const auto& r=pending.draws[1].resources[0];CHECK(r.descriptor.view->image->handle==disc::id(otherDepth));CHECK(r.staticDref && r.descriptor.sampler->compare);
        CHECK(pending.draws[0].draw.dynamic.depthWrite==1 && pending.draws[1].draw.dynamic.depthWrite==0);
    }
    VkPresentInfoKHR pi{};pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;expectedPresent=&pi;CHECK(H(vkQueuePresentKHR)(queue,&pi)==VK_SUCCESS);
    updateImage(depthView);request_discovery_capture(device,1);CHECK(H(vkQueueSubmit)(queue,1,&si,expectedFence)==VK_SUCCESS);CHECK(H(vkQueuePresentKHR)(queue,&pi)==VK_SUCCESS);
    flush_discovery_exports();CHECK(submitCalls==2 && presentCalls==2);
    std::filesystem::path capturePath;for(auto& path:std::filesystem::directory_iterator(directory))if(path.path().extension()==".json")capturePath=path.path();CHECK(!capturePath.empty());
    std::ifstream capture(capturePath);std::string json((std::istreambuf_iterator<char>(capture)),{});
    CHECK(json.find("\"category\":\"unknown\"")!=std::string::npos && json.find("\"staticDref\":1")!=std::string::npos);
    CHECK(json.find("\"count\":9,\"instances\":2,\"first\":4,\"vertexOffset\":-2")!=std::string::npos);
    CHECK(json.find("\"semantic\":{\"class\":")!=std::string::npos && json.find("\"alphaTestSource\":")!=std::string::npos && json.find("\"dxvkAlphaTest\":")!=std::string::npos);
    // Frame scan (tools/frame-scan.py): a request file in DEPTH_PROBE_EXPORT_DIR, noticed within 30 presents, captures the asked
    // number of frames into its own folder, finished by a DONE file naming the capture; the request is consumed.
    {
        const auto exportDir=std::filesystem::path("/tmp/depth-probe-frame-scan-test-"+std::to_string(getpid()));
        std::filesystem::remove_all(exportDir);std::filesystem::create_directories(exportDir);setenv("DEPTH_PROBE_EXPORT_DIR",exportDir.c_str(),1);
        {std::ofstream request(exportDir/"frame-scan-request");request<<"1 uniforms\n";}   // "uniforms": shadow-sampling draws' constants too
        for(int i=0;i<30 && std::filesystem::exists(exportDir/"frame-scan-request");++i)CHECK(H(vkQueuePresentKHR)(queue,&pi)==VK_SUCCESS);
        CHECK(!std::filesystem::exists(exportDir/"frame-scan-request"));
        CHECK(H(vkQueueSubmit)(queue,1,&si,expectedFence)==VK_SUCCESS);CHECK(H(vkQueuePresentKHR)(queue,&pi)==VK_SUCCESS);
        flush_discovery_exports();
        std::filesystem::path scan;for(auto& e:std::filesystem::directory_iterator(exportDir))if(e.path().filename().string().rfind("frame-scan-",0)==0)scan=e.path();
        CHECK(!scan.empty() && std::filesystem::exists(scan/"DONE") && std::filesystem::exists(scan/"shaders"));
        std::ifstream done(scan/"DONE");std::string name;done>>name;CHECK(std::filesystem::exists(scan/name));
        std::ifstream scanFile(scan/name);std::string scanJson((std::istreambuf_iterator<char>(scanFile)),{});
        CHECK(scanJson.find("\"sceneDepthImage\":")!=std::string::npos && scanJson.find("\"draws\":[{")!=std::string::npos);
        CHECK(scanJson.find("\"context\":null")!=std::string::npos && std::filesystem::exists(scan/"uniforms"));   // no fog here: no camera context
        unsetenv("DEPTH_PROBE_EXPORT_DIR");
        if(std::getenv("DEPTH_PROBE_KEEP_FRAME_SCAN"))std::fprintf(stderr,"kept %s\n",scan.c_str());else std::filesystem::remove_all(exportDir);
        submitCalls=presentCalls=2;   // the counts the checks below expect
    }
    // Frame scan "lights" (tools/light-constants.py): the same capture, with the constants of the draws on the scene's depth.
    {
        const auto exportDir=std::filesystem::path("/tmp/depth-probe-frame-scan-lights-test-"+std::to_string(getpid()));
        std::filesystem::remove_all(exportDir);std::filesystem::create_directories(exportDir);setenv("DEPTH_PROBE_EXPORT_DIR",exportDir.c_str(),1);
        {std::ofstream request(exportDir/"frame-scan-request");request<<"1 lights\n";}
        for(int i=0;i<30 && std::filesystem::exists(exportDir/"frame-scan-request");++i)CHECK(H(vkQueuePresentKHR)(queue,&pi)==VK_SUCCESS);
        CHECK(!std::filesystem::exists(exportDir/"frame-scan-request"));
        CHECK(H(vkQueueSubmit)(queue,1,&si,expectedFence)==VK_SUCCESS);CHECK(H(vkQueuePresentKHR)(queue,&pi)==VK_SUCCESS);
        flush_discovery_exports();
        std::filesystem::path scan;for(auto& e:std::filesystem::directory_iterator(exportDir))if(e.path().filename().string().rfind("frame-scan-",0)==0)scan=e.path();
        CHECK(!scan.empty() && std::filesystem::exists(scan/"DONE") && std::filesystem::exists(scan/"uniforms"));
        std::ifstream done(scan/"DONE");std::string name;done>>name;std::ifstream scanFile(scan/name);std::string scanJson((std::istreambuf_iterator<char>(scanFile)),{});
        CHECK(scanJson.find("\"context\":null")!=std::string::npos);   // the camera context is asked for (none without the fog)
        unsetenv("DEPTH_PROBE_EXPORT_DIR");std::filesystem::remove_all(exportDir);
        submitCalls=presentCalls=2;
    }
    // Failed submits do not become frame activity.
    submitResult=VK_ERROR_OUT_OF_HOST_MEMORY;CHECK(H(vkQueueSubmit)(queue,1,&si,expectedFence)==submitResult);submitResult=VK_SUCCESS;
    {std::lock_guard lock(disc::mutex);CHECK(disc::devices.at(device).pending.empty());}
    // Template update and descriptor copy preserve immutable compare samplers.
    VkDescriptorUpdateTemplateEntry entry{0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,0,sizeof(VkDescriptorImageInfo)};
    VkDescriptorUpdateTemplateCreateInfo ti{};ti.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;ti.descriptorUpdateEntryCount=1;ti.pDescriptorUpdateEntries=&entry;ti.descriptorSetLayout=setLayout;
    VkDescriptorUpdateTemplate templ;CHECK(H(vkCreateDescriptorUpdateTemplate)(device,&ti,nullptr,&templ)==VK_SUCCESS);
    VkDescriptorImageInfo di{};di.imageView=otherView;di.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;H(vkUpdateDescriptorSetWithTemplate)(device,set,templ,&di);
    VkCopyDescriptorSet copy{};copy.sType=VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;copy.srcSet=set;copy.dstSet=copySet;copy.descriptorCount=1;H(vkUpdateDescriptorSets)(device,0,nullptr,1,&copy);
    {std::lock_guard lock(disc::mutex);auto value=disc::devices.at(device).sets.at(disc::id(copySet))->descriptors.at({0,0});CHECK(value.view->image->handle==disc::id(otherDepth) && value.sampler->compare);}
    // UI isolation is not a persistent setting. The selected target is exact,
    // depth-only draws / reusable buffers / query scopes always pass through.
    OverlayUniform uniform;uniform.panel[0]=discovery_first_page();populate_discovery_menu(device,uniform);
    uint64_t consumerId;
    {std::lock_guard lock(disc::mutex);consumerId=disc::devices.at(device).pipelines.at(disc::id(consumer))->serial;disc::devices.at(device).selected=consumerId;}
    CHECK(discovery_menu_input(discovery_first_page(),9,0,true));
    auto runIsolated=[&](VkCommandBufferUsageFlags flags,bool depthOnly,bool query) {
        bi.flags=flags;CHECK(H(vkBeginCommandBuffer)(cmd,&bi)==VK_SUCCESS);
        ri.colorAttachmentCount=depthOnly?0:1;ri.pDepthAttachment=depthOnly?&da:nullptr;
        H(vkCmdBeginRendering)(cmd,&ri);bindState(consumer,false);
        if(query)H(vkCmdBeginQuery)(cmd,handle<VkQueryPool>(77),0,0);
        H(vkCmdDraw)(cmd,3,1,0,0);
        if(query)H(vkCmdEndQuery)(cmd,handle<VkQueryPool>(77),0);
        H(vkCmdEndRendering)(cmd);CHECK(H(vkEndCommandBuffer)(cmd)==VK_SUCCESS);
    };
    auto before=draws;runIsolated(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,false,false);CHECK(draws==before);
    runIsolated(0,false,false);CHECK(draws==before+1);
    runIsolated(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,true,false);CHECK(draws==before+2);
    runIsolated(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,false,true);CHECK(draws==before+3);
    stop_discovery_isolation();runIsolated(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,false,false);CHECK(draws==before+4);
    // A secondary's draws belong at ExecuteCommands in the primary target,
    // never at the time the secondary was originally recorded.
    ca.commandBufferCount=1;ca.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;VkCommandBuffer secondary;
    CHECK(H(vkAllocateCommandBuffers)(device,&ca,&secondary)==VK_SUCCESS);
    VkCommandBufferInheritanceInfo inheritance{};inheritance.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    VkCommandBufferBeginInfo secondaryBegin{};secondaryBegin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;secondaryBegin.flags=VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;secondaryBegin.pInheritanceInfo=&inheritance;
    auto primary=cmd;cmd=secondary;CHECK(H(vkBeginCommandBuffer)(cmd,&secondaryBegin)==VK_SUCCESS);bindState(consumer,false);
    H(vkCmdBindDescriptorSets)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);H(vkCmdDraw)(cmd,3,1,0,0);CHECK(H(vkEndCommandBuffer)(cmd)==VK_SUCCESS);
    cmd=primary;bi.flags=0;CHECK(H(vkBeginCommandBuffer)(cmd,&bi)==VK_SUCCESS);H(vkCmdBeginRendering)(cmd,&ri);H(vkCmdExecuteCommands)(cmd,1,&secondary);H(vkCmdEndRendering)(cmd);CHECK(H(vkEndCommandBuffer)(cmd)==VK_SUCCESS);
    auto expanded=disc::submitted_frame(device,queue,1,&cmd);CHECK(expanded.draws.size()==1 && expanded.draws[0].draw.target.colors[0]->image->handle==disc::id(color));
    CHECK(H(vkResetCommandBuffer)(secondary,0)==VK_SUCCESS);expanded=disc::submitted_frame(device,queue,1,&cmd);CHECK(expanded.draws.empty() && expanded.missingCommands==1);
    // Saturating observation never suppresses an application's draw.
    CHECK(H(vkBeginCommandBuffer)(cmd,&bi)==VK_SUCCESS);H(vkCmdBeginRendering)(cmd,&ri);bindState(consumer,false);before=draws;
    for(size_t i=0;i<disc::maxCommandEvents+2;++i)H(vkCmdDraw)(cmd,3,1,0,0);
    H(vkCmdEndRendering)(cmd);CHECK(H(vkEndCommandBuffer)(cmd)==VK_SUCCESS);CHECK(draws==before+disc::maxCommandEvents+2);
    expanded=disc::submitted_frame(device,queue,1,&cmd);CHECK(expanded.truncated && expanded.draws.size()==disc::maxCommandEvents);
    // Maintenance6 descriptor binds resolve graphics resources. Unknown dynamic
    // layouts still invalidate rather than retain the previous texture binding.
    CHECK(H(vkBeginCommandBuffer)(cmd,&bi)==VK_SUCCESS);bindState(consumer,false);
    H(vkCmdBindDescriptorSets)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
    VkBindDescriptorSetsInfo bind2{};bind2.sType=VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO;bind2.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;bind2.layout=layout;bind2.descriptorSetCount=1;bind2.pDescriptorSets=&copySet;
    H(vkCmdBindDescriptorSets2)(cmd,&bind2);
    {std::lock_guard lock(disc::mutex);const auto& c=disc::devices.at(device).commands.at(disc::id(cmd));CHECK(!c.unsupported && c.sets[0].handle==disc::id(copySet));CHECK(std::string(c.descriptorApi)=="vkCmdBindDescriptorSets2");}
    bind2.layout={};H(vkCmdBindDescriptorSets2)(cmd,&bind2);
    {std::lock_guard lock(disc::mutex);const auto& c=disc::devices.at(device).commands.at(disc::id(cmd));CHECK(c.unsupported && !c.sets[0].handle);CHECK(std::string(c.unsupportedReason)=="descriptor-dynamic-layout");}
    {
        // Dynamic state the application sets is remembered as plain values so a private water pass can put it back.
        H(vkCmdSetDepthWriteEnable)(cmd,VK_TRUE);H(vkCmdSetCullModeEXT)(cmd,VK_CULL_MODE_BACK_BIT);
        H(vkCmdSetStencilReference)(cmd,VK_STENCIL_FACE_FRONT_BIT,7);H(vkCmdSetStencilReference)(cmd,VK_STENCIL_FACE_FRONT_AND_BACK,9);H(vkCmdSetStencilWriteMask)(cmd,VK_STENCIL_FACE_BACK_BIT,0xf0);
        H(vkCmdSetStencilOp)(cmd,VK_STENCIL_FACE_FRONT_BIT,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_ZERO,VK_COMPARE_OP_ALWAYS);
        const float blend[4]{.1f,.2f,.3f,.4f};H(vkCmdSetBlendConstants)(cmd,blend);H(vkCmdSetLineWidth)(cmd,2.f);H(vkCmdSetDepthBounds)(cmd,.25f,.75f);H(vkCmdSetDepthBias)(cmd,1.f,2.f,3.f);
        VkViewport counted{1,2,30,40,.1f,.9f};H(vkCmdSetViewportWithCount)(cmd,1,&counted);
        VkColorBlendEquationEXT equation{VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_OP_ADD,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ZERO,VK_BLEND_OP_ADD};
        H(vkCmdSetColorBlendEquationEXT)(cmd,0,1,&equation);VkColorComponentFlags mask=7;H(vkCmdSetColorWriteMaskEXT)(cmd,0,1,&mask);VkBool32 blendOn=VK_TRUE;H(vkCmdSetColorBlendEnableEXT)(cmd,0,1,&blendOn);
        std::lock_guard lock(disc::mutex);const auto& r=disc::devices.at(device).commands.at(disc::id(cmd)).waterRestore;
        CHECK(r.simpleSet==((1u<<4)|(1u<<6)) && r.simple[4]==VK_TRUE && r.simple[6]==VK_CULL_MODE_BACK_BIT && (r.ext&(1u<<(16+6))) && !(r.ext&(1u<<(16+4))));
        CHECK(r.stencilFaces[2]==3 && r.stencilReference[0]==9 && r.stencilReference[1]==9 && r.stencilFaces[1]==2 && r.stencilWrite[1]==0xf0 && r.stencilFaces[0]==0);
        CHECK(r.stencilFaces[3]==1 && r.stencilOp[0].pass==VK_STENCIL_OP_REPLACE && !(r.ext&disc::WaterRestore::StencilOp));
        CHECK(r.blendConstants[2]==.3f && r.lineWidth==2.f && r.depthBoundsLow==.25f && r.depthBoundsHigh==.75f && r.biasSlope==3.f);
        {const auto& d=disc::devices.at(device).commands.at(disc::id(cmd)).dynamic;
         CHECK((d.valid&disc::BlendEquation) && d.blendSrc==VK_BLEND_FACTOR_ONE && d.blendDst==VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);}
        CHECK((r.set&disc::WaterRestore::BlendEquation) && r.blendEquation.srcColorBlendFactor==VK_BLEND_FACTOR_ONE && (r.set&disc::WaterRestore::ColorWriteMask) && r.colorWriteMask==7 && (r.set&disc::WaterRestore::BlendEnable) && r.blendEnable==VK_TRUE);
        CHECK((r.set&disc::WaterRestore::ViewportCount) && (r.set&disc::WaterRestore::Viewport) && r.viewport.width==30 && !(r.ext&disc::WaterRestore::ViewportCount));
    }
    VkShaderStageFlagBits stage=VK_SHADER_STAGE_FRAGMENT_BIT;VkShaderEXT object=handle<VkShaderEXT>(901);H(vkCmdBindShadersEXT)(cmd,1,&stage,&object);
    {std::lock_guard lock(disc::mutex);CHECK(!disc::devices.at(device).commands.at(disc::id(cmd)).pipeline);}
    CHECK(H(vkEndCommandBuffer)(cmd)==VK_SUCCESS);
    // A compute push template must not replace the graphics descriptor state.
    ti.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;ti.pipelineBindPoint=VK_PIPELINE_BIND_POINT_COMPUTE;ti.pipelineLayout=layout;
    VkDescriptorUpdateTemplate computeTemplate;CHECK(H(vkCreateDescriptorUpdateTemplate)(device,&ti,nullptr,&computeTemplate)==VK_SUCCESS);
    CHECK(H(vkBeginCommandBuffer)(cmd,&bi)==VK_SUCCESS);H(vkCmdBindDescriptorSets)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
    H(vkCmdPushDescriptorSetWithTemplateKHR)(cmd,computeTemplate,layout,0,&di);
    {std::lock_guard lock(disc::mutex);const auto& c=disc::devices.at(device).commands.at(disc::id(cmd));CHECK(c.sets[0].handle==disc::id(set) && !c.sets[0].pushed);}
    CHECK(H(vkEndCommandBuffer)(cmd)==VK_SUCCESS);
    // Resource failure reasons preserve the same conservative rejection policy.
    {
        auto& s=disc::devices.at(device);disc::SubmittedDraw base;base.draw.pipeline=s.pipelines.at(disc::id(consumer));
        base.draw.sets[0]={disc::id(set),s.sets.at(disc::id(set))->generation,s.layouts.at(disc::id(layout)),{}};
        auto check=[&](disc::SubmittedDraw copy,uint32_t flag) {disc::resolve_resources(s,copy);CHECK(copy.descriptorsIncomplete && (copy.descriptorIssues&flag));};
        auto missing=base;missing.draw.sets[0].handle=999999;check(missing,disc::MissingSet);
        auto stale=base;++stale.draw.sets[0].generation;check(stale,disc::StaleSet);
        auto noLayout=base;noLayout.draw.sets[0].layout.reset();check(noLayout,disc::MissingBoundLayout);
        auto mismatch=base;auto different=std::make_shared<disc::Layout>(*base.draw.sets[0].layout);++different->signature;mismatch.draw.sets[0].layout=different;check(mismatch,disc::LayoutMismatch);
        disc::resolve_resources(s,base);CHECK(!base.descriptorsIncomplete && !base.descriptorIssues && !base.resources.empty());
    }
    // Handle reuse gets fresh image identity, while captured views retain the original generation.
    uint64_t oldGeneration;
    {std::lock_guard lock(disc::mutex);oldGeneration=disc::devices.at(device).images.at(disc::id(depth))->generation;}
    H(vkDestroyImageView)(device,depthView,nullptr);H(vkDestroyImage)(device,depth,nullptr);nextHandle=disc::id(depth);auto reused=makeImage(VK_FORMAT_D32_SFLOAT,512,VK_IMAGE_ASPECT_DEPTH_BIT);
    {std::lock_guard lock(disc::mutex);CHECK(disc::devices.at(device).images.at(disc::id(reused.first))->generation!=oldGeneration);}
    H(vkResetCommandPool)(device,ca.commandPool,0);
    {std::lock_guard lock(disc::mutex);CHECK(disc::devices.at(device).eventCount==0);}
    H(vkResetDescriptorPool)(device,ai.descriptorPool,0);
    {std::lock_guard lock(disc::mutex);CHECK(disc::devices.at(device).sets.empty());}
    {
        // The water gate must say why it refuses a pipeline. DXVK's library-linked
        // pipelines carry sample-locations / depth-clip dynamic states and layouts
        // with an untracked update-after-bind set; both have to be nameable.
        auto has=[](const std::string& text,const char* part){return text.find(part)!=std::string::npos;};
        disc::Pipeline p;std::string why;
        CHECK(!disc::water_pipeline_check(p,&why) && !disc::water_pipeline_check(p,nullptr) && !disc::water_pipeline_eligible(p));
        CHECK(has(why,"no-recipe") && has(why,"no-layout") && has(why,"missing-vs-or-fs"));
        p.water=std::make_shared<disc::WaterRecipe>();p.water->vertexInput=p.water->raster=p.water->fragment=p.water->output=true;
        p.layout=std::make_shared<disc::Layout>();p.layout->water=std::make_shared<disc::WaterLayout>();
        p.vs=std::make_shared<disc::Shader>();p.fs=std::make_shared<disc::Shader>();p.vs->replaySafe=p.fs->replaySafe=true;
        why.clear();CHECK(disc::water_pipeline_check(p,&why) && why.empty() && disc::water_pipeline_check(p,nullptr) && disc::water_pipeline_eligible(p));
        p.dynamic={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,VkDynamicState(1000455016),VkDynamicState(1000143000),VkDynamicState(1000455017)};
        p.replaySubset=false;p.queryStatus="rejected;dynamic-state:1000455016";
        auto set=std::make_shared<disc::SetLayout>();set->supported=false;set->diagnostic="flags=0000000000000002 pNext=[1000161000]";
        p.layout->supported=false;p.layout->diagnostic="flags=0000000000000002 pNext=[]";p.layout->sets={set,std::make_shared<disc::SetLayout>()};
        p.samples=4;
        why.clear();CHECK(!disc::water_pipeline_check(p,&why) && !disc::water_pipeline_check(p,nullptr) && !disc::water_pipeline_eligible(p));
        for(const char* part:{"dynamic-state:1000455016","dynamic-state:1000143000","dynamic-state:1000455017","replay-subset-rejected{rejected;dynamic-state:1000455016}",
                              "layout-untracked{flags=0000000000000002 pNext=[]}","set0{flags=0000000000000002 pNext=[1000161000]}","samples:4"})
            CHECK(has(why,part));
        CHECK(!has(why,"set1{") && !has(why,"dynamic-state:0,") && !has(why,"dynamic-state:1,"));
    }
    destroy_device_discovery(device);CHECK(disc::devices.empty());g_devices.clear();
    std::printf("PASS: discovery shader/resource/template/submit/present tracking, immutable samplers, dynamic state, guarded isolation, handle generations and cleanup; capture=%s\n",capturePath.c_str());
}
