// Deterministic GPU exercise of the production render-pass-end injection.
#include "water_reflections.hpp"
#include "water_shading.hpp"
#include "depth_tracking.hpp"
#include "shader_resources.hpp"
#include "surface_inspector.hpp"
#include "volumetric_fog.hpp"
#include <dlfcn.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <set>
#include <algorithm>
#include <filesystem>
using namespace depth_probe;
using namespace depth_probe::discovery;
#define CHECK(x) do{if(!(x)){std::fflush(stdout);std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
static float half_to_float(uint16_t h) {
    const uint32_t sign=(h>>15)&1,exponent=(h>>10)&31,mantissa=h&1023;float f;
    if(exponent==0)f=std::ldexp(float(mantissa),-24);else if(exponent==31)f=mantissa?NAN:INFINITY;else f=std::ldexp(float(mantissa+1024),int(exponent)-25);
    return sign?-f:f;
}
static float skyClear[3]{0,0,0}; // the scene's cleared color: what the "sky" (pixels with nothing drawn) looks like
static VkDevice device{};static VkInstance instance{};
static PFN_vkGetDeviceProcAddr gdpa;static PFN_vkGetInstanceProcAddr gipa;
#define D(api) reinterpret_cast<PFN_##api>(gdpa(device,#api))
#define I(api) reinterpret_cast<PFN_##api>(gipa(instance,#api))
#define A(api) reinterpret_cast<PFN_##api>(discovery_hook(#api)?discovery_hook(#api):gdpa(device,#api))
static void ok(VkResult r){CHECK(r==VK_SUCCESS);}
static std::atomic<unsigned> errors{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* p,void*){++errors;std::fprintf(stderr,"VALIDATION: %s\n",p->pMessage);return VK_FALSE;}
int main() {
    const auto captureRoot=std::filesystem::path("/tmp")/("depth-probe-water-capture-test-"+std::to_string(getpid()));std::filesystem::remove_all(captureRoot);setenv("DEPTH_PROBE_EXPORT_DIR",captureRoot.c_str(),1);
    setenv("DEPTH_PROBE_WATER_FREEZE","1",1); // waves are animated; frame-to-frame pixel comparisons need a fixed time
    setenv("DEPTH_PROBE_WATER_SETTINGS","/dev/null/depth-probe-water.ini",1); /* tests never read or write the user's water settings */
    setenv("DEPTH_PROBE_SURFACE_INSPECTOR","1",1);setenv("DEPTH_PROBE_DEPTH_PRESENT","0",1);setenv("DEPTH_PROBE_UI_INPUT","0",1);
    setenv("DEPTH_PROBE_SHADER_DIR",DEPTH_PROBE_TEST_ROOT_DIR "/shaders",1);setenv("DEPTH_PROBE_LAYER_LOG","/tmp/depth-probe-water-gpu.log",1);
    auto loader=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);CHECK(loader);gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));
    const bool validate=std::getenv("DEPTH_PROBE_SMOKE_VALIDATION");const char* layer="VK_LAYER_KHRONOS_validation";const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug{};debug.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;debug.pfnUserCallback=validation;
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ii.pApplicationInfo=&app;
    if(validate){ii.pNext=&debug;ii.enabledLayerCount=1;ii.ppEnabledLayerNames=&layer;ii.enabledExtensionCount=1;ii.ppEnabledExtensionNames=&extension;}
    ok(I(vkCreateInstance)(&ii,nullptr,&instance));VkDebugUtilsMessengerEXT messenger{};if(validate)ok(I(vkCreateDebugUtilsMessengerEXT)(instance,&debug,nullptr,&messenger));
    uint32_t count=0;ok(I(vkEnumeratePhysicalDevices)(instance,&count,nullptr));CHECK(count);std::vector<VkPhysicalDevice> physical(count);ok(I(vkEnumeratePhysicalDevices)(instance,&count,physical.data()));auto gpu=physical[0];
    I(vkGetPhysicalDeviceQueueFamilyProperties)(gpu,&count,nullptr);std::vector<VkQueueFamilyProperties> queues(count);I(vkGetPhysicalDeviceQueueFamilyProperties)(gpu,&count,queues.data());
    uint32_t family=0;while(family<count && !(queues[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;CHECK(family<count);
    float priority=1;VkDeviceQueueCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkPhysicalDeviceFeatures features{};features.shaderClipDistance=VK_TRUE;features.depthClamp=VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{};f13.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;f13.dynamicRendering=VK_TRUE;
    VkDeviceCreateInfo di{};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;di.pNext=&f13;di.pEnabledFeatures=&features;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    ok(I(vkCreateDevice)(gpu,&di,nullptr,&device));gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance,"vkGetDeviceProcAddr"));VkQueue queue{};D(vkGetDeviceQueue)(device,family,0,&queue);
    DeviceData data;data.device=device;data.physical_device=gpu;data.next_gdpa=gdpa;
    data.next_create_image=D(vkCreateImage);data.next_destroy_image=D(vkDestroyImage);data.next_create_image_view=D(vkCreateImageView);data.next_destroy_image_view=D(vkDestroyImageView);
    data.get_properties=I(vkGetPhysicalDeviceProperties);data.get_memory_properties=I(vkGetPhysicalDeviceMemoryProperties);data.get_format_properties=I(vkGetPhysicalDeviceFormatProperties);data.get_queue_properties=I(vkGetPhysicalDeviceQueueFamilyProperties);
    data.get_image_format_properties=I(vkGetPhysicalDeviceImageFormatProperties);data.get_image_format_properties2=I(vkGetPhysicalDeviceImageFormatProperties2);
    data.next_cmd_pipeline_barrier=D(vkCmdPipelineBarrier); // the layer forwards barriers through this in production
    g_devices[dispatch_key(device)]=data;
    std::vector<VkDeviceMemory> allocations;
    auto allocate=[&](VkMemoryRequirements r,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties memory{};data.get_memory_properties(gpu,&memory);uint32_t index=0;
        while(index<memory.memoryTypeCount && (!(r.memoryTypeBits&(1u<<index)) || (memory.memoryTypes[index].propertyFlags&flags)!=flags))++index;
        CHECK(index<memory.memoryTypeCount);
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=index;VkDeviceMemory out{};ok(A(vkAllocateMemory)(device,&a,nullptr,&out));allocations.push_back(out);return out;
    };
    const unsigned width=128,height=128;
    VkImage color{},depth{};VkImageView colorView{},depthView{};
    auto image=[&](VkFormat format,VkImageUsageFlags usage,VkImageAspectFlags aspect,VkImage& out,VkImageView& view) {
        VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;i.imageType=VK_IMAGE_TYPE_2D;i.format=format;i.extent={width,height,1};i.mipLevels=i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=usage;
        ok(A(vkCreateImage)(device,&i,nullptr,&out));VkMemoryRequirements r{};D(vkGetImageMemoryRequirements)(device,out,&r);auto mem=allocate(r,0);ok(D(vkBindImageMemory)(device,out,mem,0));
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=out;v.format=format;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.subresourceRange={aspect,0,1,0,1};ok(A(vkCreateImageView)(device,&v,nullptr,&view));
    };
    image(VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT,color,colorView);
    image(VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,depth,depthView);
    auto buffer=[&](VkDeviceSize size,VkBufferUsageFlags usage,void** mapped) {
        VkBufferCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;i.size=size;i.usage=usage;VkBuffer out{};ok(A(vkCreateBuffer)(device,&i,nullptr,&out));
        VkMemoryRequirements r{};D(vkGetBufferMemoryRequirements)(device,out,&r);auto mem=allocate(r,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);ok(A(vkBindBufferMemory)(device,out,mem,0));if(mapped)ok(A(vkMapMemory)(device,mem,0,VK_WHOLE_SIZE,0,mapped));return out;
    };
    void* cameraMap{};auto camera=buffer(512,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&cameraMap);std::memset(cameraMap,0,512);
    float* c=static_cast<float*>(cameraMap)+64;
    const float matrices[36]={1.2,0,0,0, 0,1.2,0,0, 0,0,100.f/99.8f,1, 0,0,-20.f/99.8f,0, 0,0,0,0, 1,0,0,0, 0,.6,.8,0, 0,.8,-.6,0, 0,.8,4.4,1};
    std::copy_n(matrices,36,c);
    // Floor, an above-water red wall, a submerged green wall, and a flat water quad.
    std::vector<float> vertices;
    auto quad=[&](std::array<float,3>a,std::array<float,3>b,std::array<float,3>cc,std::array<float,3>d){for(auto v:{a,b,cc,a,cc,d})vertices.insert(vertices.end(),v.begin(),v.end());};
    quad({-8,-3,-1},{8,-3,-1},{8,10,-1},{-8,10,-1});
    quad({-1,2,0},{1,2,0},{1,2,2},{-1,2,2});
    quad({2,2,-2},{3,2,-2},{3,2,-.1f},{2,2,-.1f});
    quad({-5,-2,0},{5,-2,0},{5,6,0},{-5,6,0});
    // A second pond at another (horizontal) level and a sloped water patch, drawn only by the multi-level scenario.
    quad({-3,-1,.4f},{-1,-1,.4f},{-1,1,.4f},{-3,1,.4f});
    quad({1.5f,-1,.2f},{3.5f,-1,.2f},{3.5f,1,.9f},{1.5f,1,.9f});
    // A pool ABOVE the camera (camera z 2), drawn first only by the high-first scenario (vertices 36..41).
    quad({-6,8,3},{-4,8,3},{-4,9,3},{-6,9,3});
    // A rock slab .5 mm above the water (vertices 42..47), drawn only by the waterline scenario: in front of the water plane by less than the
    // composite's old depth slack (2e-5 raw: ~.002-.004 yd along the view ray here) - what the terrain is in a band along every shore seen from
    // ~35 yd in game (48451-2). The game's water fails its depth test there; ours must too (slack 2e-7).
    quad({-4,1,.0005f},{-2,1,.0005f},{-2,4,.0005f},{-4,4,.0005f});
    void* vertexMap{};auto vertex=buffer(vertices.size()*4,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&vertexMap);std::memcpy(vertexMap,vertices.data(),vertices.size()*4);
    void* indexMap{};auto index=buffer(32,VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&indexMap);
    for(unsigned i=0;i<8;++i)static_cast<uint16_t*>(indexMap)[i]=uint16_t(18+i%6);
    void* readMap{};auto readback=buffer(width*height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&readMap);
    VkDescriptorSetLayout empty{},sourceSet{};VkDescriptorSetLayoutCreateInfo sli{};sli.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;ok(A(vkCreateDescriptorSetLayout)(device,&sli,nullptr,&empty));
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};sli.bindingCount=1;sli.pBindings=&binding;ok(A(vkCreateDescriptorSetLayout)(device,&sli,nullptr,&sourceSet));
    VkDescriptorSetLayout sets[]{empty,empty,sourceSet};VkPipelineLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.setLayoutCount=3;li.pSetLayouts=sets;VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,96};li.pushConstantRangeCount=1;li.pPushConstantRanges=&pushRange;VkPipelineLayout layout{};ok(A(vkCreatePipelineLayout)(device,&li,nullptr,&layout));
    CHECK(devices.at(device).layouts.at(id(layout))->water);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1};VkDescriptorPoolCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&ps;VkDescriptorPool descriptors{};ok(A(vkCreateDescriptorPool)(device,&pi,nullptr,&descriptors));
    VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=descriptors;ai.descriptorSetCount=1;ai.pSetLayouts=&sourceSet;VkDescriptorSet set{};ok(A(vkAllocateDescriptorSets)(device,&ai,&set));
    VkDescriptorBufferInfo uniform{camera,0,512};VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&uniform;A(vkUpdateDescriptorSets)(device,1,&write,0,nullptr);
    VkAttachmentDescription attachments[2]{};attachments[0].format=VK_FORMAT_R8G8B8A8_UNORM;attachments[1].format=VK_FORMAT_D32_SFLOAT;
    for(auto& a:attachments){a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;a.finalLayout=VK_IMAGE_LAYOUT_GENERAL;}
    VkAttachmentReference cr{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},dr{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&cr;sub.pDepthStencilAttachment=&dr;
    VkSubpassDependency dependencies[2]{};dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    for(auto& dep:dependencies){dep.srcStageMask=dep.dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dep.srcAccessMask=dep.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;}
    VkRenderPassCreateInfo ri{};ri.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;ri.attachmentCount=2;ri.pAttachments=attachments;ri.subpassCount=1;ri.pSubpasses=&sub;ri.dependencyCount=2;ri.pDependencies=dependencies;VkRenderPass pass{};ok(A(vkCreateRenderPass)(device,&ri,nullptr,&pass));
    VkImageView views[]{colorView,depthView};VkFramebufferCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fi.renderPass=pass;fi.attachmentCount=2;fi.pAttachments=views;fi.width=width;fi.height=height;fi.layers=1;VkFramebuffer framebuffer{};ok(A(vkCreateFramebuffer)(device,&fi,nullptr,&framebuffer));
    for(auto& a:attachments){a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.initialLayout=VK_IMAGE_LAYOUT_GENERAL;}
    VkRenderPass continuationPass{};ok(A(vkCreateRenderPass)(device,&ri,nullptr,&continuationPass));
    auto shader=[&](const char* path) {auto code=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/"+path);VkShaderModuleCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;i.codeSize=code.size()*4;i.pCode=code.data();VkShaderModule module{};ok(A(vkCreateShaderModule)(device,&i,nullptr,&module));return module;};
    auto vs=shader("water_scene.vert.spv"),fs=shader("water_color.frag.spv");
    VkShaderModule vertexShader=vs;   // pipeline() below builds with this vertex shader
    bool dynamicPipelines=false;
    auto pipeline=[&](float r,float g,float b,bool water,bool alphaBlend=false,bool pattern=false,VkShaderModule fragment=VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertexShader;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment?fragment:fs;
        VkSpecializationMapEntry entries[]{{0,0,4},{1,4,4},{2,8,4},{3,12,4},{4,16,4},{5,20,4}};struct Constants{float r,g,b;uint32_t hole;float alpha;uint32_t pattern;} rgb{r,g,b,water?1u:0u,water?.65f:1.f,pattern?1u:0u};VkSpecializationInfo spec{6,entries,sizeof(rgb),&rgb};stages[1].pSpecializationInfo=&spec;
        // Binding 1 is declared with an attribute the vertex shader never reads and nothing binds it: legal, and what DXVK's pipelines look like.
        VkVertexInputBindingDescription bd[2]{{0,0,VK_VERTEX_INPUT_RATE_VERTEX},{1,16,VK_VERTEX_INPUT_RATE_VERTEX}};VkVertexInputAttributeDescription ad[2]{{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{7,1,VK_FORMAT_R32G32B32A32_SFLOAT,0}};
        VkPipelineVertexInputStateCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;vi.vertexBindingDescriptionCount=vi.vertexAttributeDescriptionCount=2;vi.pVertexBindingDescriptions=bd;vi.pVertexAttributeDescriptions=ad;
        VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.lineWidth=1;raster.depthClampEnable=VK_TRUE; // as the real WoW water pipeline
        VkPipelineViewportStateCreateInfo viewport{};viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;viewport.viewportCount=viewport.scissorCount=0;
        VkPipelineMultisampleStateCreateInfo multi{};multi.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;multi.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthState{};depthState.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;depthState.depthTestEnable=VK_TRUE;depthState.depthWriteEnable=!water && !fragment;depthState.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;const bool additive=fragment!=VK_NULL_HANDLE;   // the particle: ONE + ONE, no depth write
        blend.blendEnable=water || additive;blend.srcColorBlendFactor=alphaBlend?VK_BLEND_FACTOR_SRC_ALPHA:VK_BLEND_FACTOR_ONE;blend.dstColorBlendFactor=additive?VK_BLEND_FACTOR_ONE:alphaBlend?VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:VK_BLEND_FACTOR_ZERO;blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;
        VkPipelineColorBlendStateCreateInfo colorState{};colorState.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;colorState.attachmentCount=1;colorState.pAttachments=&blend;
        VkDynamicState ds[]{VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
            VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_CULL_MODE,VK_DYNAMIC_STATE_FRONT_FACE,VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,VK_DYNAMIC_STATE_STENCIL_OP,VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            VK_DYNAMIC_STATE_DEPTH_BIAS,VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE};VkPipelineDynamicStateCreateInfo dyn{};dyn.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;dyn.dynamicStateCount=std::size(ds);dyn.pDynamicStates=ds;
        VkGraphicsPipelineCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;ci.stageCount=2;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&assembly;ci.pRasterizationState=&raster;ci.pViewportState=&viewport;ci.pMultisampleState=&multi;ci.pDepthStencilState=&depthState;ci.pColorBlendState=&colorState;ci.pDynamicState=&dyn;ci.layout=layout;ci.renderPass=pass;
        VkFormat colorFormat=VK_FORMAT_R8G8B8A8_UNORM;VkPipelineRenderingCreateInfo dynamicInfo{};dynamicInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;dynamicInfo.colorAttachmentCount=1;dynamicInfo.pColorAttachmentFormats=&colorFormat;dynamicInfo.depthAttachmentFormat=VK_FORMAT_D32_SFLOAT;
        if(dynamicPipelines){ci.renderPass={};ci.pNext=&dynamicInfo;}
        VkPipeline result{};ok(A(vkCreateGraphicsPipelines)(device,{},1,&ci,nullptr,&result));return result;
    };
    auto gray=pipeline(.15,.15,.15,false),red=pipeline(1,0,0,false),green=pipeline(0,1,0,false),water=pipeline(0,0,1,true);
    auto alphaWater=pipeline(0,0,1,true,true);   // the real WoW water's blend (SRC_ALPHA, 1-SRC_ALPHA): blend mode 1
    auto particleFs=shader("water_particle.frag.spv");auto particle=pipeline(0,0,0,false,false,false,particleFs);   // an effect over the water (fishing pool)
    auto patternWater=pipeline(0,0,1,true,true,true);   // the same, with a checker standing in for the game's water texture
    dynamicPipelines=true;auto dynamicGray=pipeline(.15,.15,.15,false),dynamicRed=pipeline(1,0,0,false),dynamicGreen=pipeline(0,1,0,false),dynamicWater=pipeline(0,0,1,true);
    auto dynamicAlphaWater=pipeline(0,0,1,true,true),dynamicParticle=pipeline(0,0,0,false,false,false,particleFs);dynamicPipelines=false;
    CHECK(water_pipeline_eligible(*devices.at(device).pipelines.at(id(dynamicWater))));
    auto& state=devices.at(device);auto& w=water_state(state);auto waterMeta=state.pipelines.at(id(water));
    CHECK(water_pipeline_eligible(*waterMeta));
    // The synthetic fixture exercises the same register contract; production UI
    // accepts only the known captured WoW shader hashes.
    w.selectedVS=waterMeta->vs->hash;w.selectedFS=waterMeta->fs->hash;w.strength=1;
    w.tone=0; // natural: a tone would flavor the reflection color
    w.layers=waterLayerReflection; // the exact-pixel invariants below are about the reflection alone; the other layers have their own test
    w.look.glitter=0;             // sun sparkle is an extra on top of the reflection: off for the exact-pixel invariants, on in the all-layers test
    VkCommandPoolCreateInfo poolInfo{};poolInfo.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;poolInfo.queueFamilyIndex=family;VkCommandPool pool{};ok(A(vkCreateCommandPool)(device,&poolInfo,nullptr,&pool));
    VkCommandBufferAllocateInfo ca{};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd{};ok(A(vkAllocateCommandBuffers)(device,&ca,&cmd));
    bool continuation=false,wideIndices=false,dynamicFrame=false;unsigned renderSerial=0;
    bool variantTile=false;VkPipeline variantPipeline{};   // draw water tile 1 with a vertex-shader variant
    bool extraLevels=false;                                 // also draw the second pond (z .4) and the sloped patch
    bool mixedFirst=false;                                  // first water draw = one draw holding both levels (sea z 0 + pond z .4)
    bool highFirst=false;                                   // first water draw = a pool above the camera, then the sea tiles
    double endSeconds=0;   // time spent inside the pass-end call (where the layer records its water passes)
    unsigned waterTiles=2;   // water draws per frame (one quad = 2); raised by the CPU-cost scenario
    bool alphaFrame=false;   // the water drawn with alpha blending (SRC_ALPHA, 1-SRC_ALPHA) instead of a plain write
    bool patternFrame=false; // ... and with a checker in its colour (the game's texture)
    bool particleFrame=false;   // an additive effect drawn over the sea after the water tiles (its vertices, not indexed)
    bool crossingWall=false;    // the rock slab just above the water surface (vertices 42..47)
    auto render=[&](uint32_t mode,uint32_t debugView) {
        w.mode=mode;w.debug=debugView;VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ok(A(vkBeginCommandBuffer)(cmd,&bi));
        VkClearValue clears[2]{};clears[1].depthStencil.depth=1;for(int c=0;c<3;++c)clears[0].color.float32[c]=skyClear[c];VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;begin.renderPass=pass;begin.framebuffer=framebuffer;begin.renderArea.extent={width,height};begin.clearValueCount=2;begin.pClearValues=clears;
        if(dynamicFrame) {
            // As DXVK does: explicit transitions, then dynamic rendering; the attachments keep the layouts they began with.
            VkImageMemoryBarrier to[2]{};for(auto& t:to){t.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;t.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;t.srcQueueFamilyIndex=t.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;}
            to[0].newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;to[0].image=color;to[0].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            to[1].newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;to[1].image=depth;to[1].subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
            A(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,to);
            VkRenderingAttachmentInfo colorAttachment{},depthAttachment{};colorAttachment.sType=depthAttachment.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView=colorView;colorAttachment.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;depthAttachment.imageView=depthView;depthAttachment.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            for(auto* a:{&colorAttachment,&depthAttachment}){a->loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a->storeOp=VK_ATTACHMENT_STORE_OP_STORE;}for(int c=0;c<3;++c)colorAttachment.clearValue.color.float32[c]=skyClear[c];depthAttachment.clearValue.depthStencil.depth=1;
            VkRenderingInfo rendering{};rendering.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;rendering.renderArea.extent={width,height};rendering.layerCount=1;rendering.colorAttachmentCount=1;rendering.pColorAttachments=&colorAttachment;rendering.pDepthAttachment=&depthAttachment;
            A(vkCmdBeginRendering)(cmd,&rendering);
        } else A(vkCmdBeginRenderPass)(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0,float(height),float(width),-float(height),0,.94f};VkRect2D scissor{{0,0},{width,height}};A(vkCmdSetViewportWithCount)(cmd,1,&viewport);A(vkCmdSetScissorWithCount)(cmd,1,&scissor);
        A(vkCmdSetDepthTestEnable)(cmd,VK_TRUE);A(vkCmdSetDepthCompareOp)(cmd,VK_COMPARE_OP_LESS_OR_EQUAL);A(vkCmdSetCullMode)(cmd,VK_CULL_MODE_NONE);
        A(vkCmdSetFrontFace)(cmd,VK_FRONT_FACE_COUNTER_CLOCKWISE);A(vkCmdSetPrimitiveTopology)(cmd,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);A(vkCmdSetStencilTestEnable)(cmd,VK_FALSE);
        A(vkCmdSetDepthBoundsTestEnable)(cmd,VK_FALSE);A(vkCmdSetDepthBiasEnable)(cmd,VK_FALSE);A(vkCmdSetDepthBias)(cmd,0,0,0);
        A(vkCmdSetStencilOp)(cmd,VK_STENCIL_FACE_FRONT_AND_BACK,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS);
        A(vkCmdSetStencilCompareMask)(cmd,VK_STENCIL_FACE_FRONT_AND_BACK,255);A(vkCmdSetStencilWriteMask)(cmd,VK_STENCIL_FACE_FRONT_AND_BACK,255);A(vkCmdSetStencilReference)(cmd,VK_STENCIL_FACE_FRONT_AND_BACK,0);
        A(vkCmdSetDepthWriteEnable)(cmd,VK_TRUE);
        A(vkCmdBindDescriptorSets)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,2,1,&set,0,nullptr);VkDeviceSize offset=0,size=vertices.size()*4,stride=12;A(vkCmdBindVertexBuffers2)(cmd,0,1,&vertex,&offset,&size,&stride);
        float constants[24];std::fill_n(constants,24,1.f);
        uint32_t first=0;for(auto p:{gray,red,green}){constants[0]=constants[1]=constants[2]=p==green?.2f:1.f;A(vkCmdPushConstants)(cmd,layout,pushRange.stageFlags,0,96,constants);A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p);A(vkCmdDraw)(cmd,6,1,first,0);first+=6;}
        if(crossingWall) {   // (.3, .3, .8): undone as if water (0, 0, 1) at .65 covered it, a plausible bed (.86, .86, .43) - only depth tells it apart
            std::fill_n(constants,24,1.f);constants[0]=constants[1]=2.f;constants[2]=5.333f;A(vkCmdPushConstants)(cmd,layout,pushRange.stageFlags,0,96,constants);
            A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gray);A(vkCmdDraw)(cmd,6,1,42,0);
        }
        std::fill_n(constants,24,1.f);A(vkCmdPushConstants)(cmd,layout,pushRange.stageFlags,0,96,constants);
        A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,patternFrame?patternWater:alphaFrame?alphaWater:water);A(vkCmdSetDepthWriteEnable)(cmd,VK_FALSE);
        A(vkCmdBindIndexBuffer)(cmd,index,0,wideIndices?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16);
        // The sea is many tiles, one draw each: the quad is two water draws (only the first validates the plane).
        if(highFirst)A(vkCmdDraw)(cmd,6,1,36,0);
        if(mixedFirst)A(vkCmdDraw)(cmd,12,1,18,0);
        for(unsigned tile=0;tile<waterTiles && !mixedFirst;++tile){if(variantTile && tile==1)A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,variantPipeline);A(vkCmdDrawIndexed)(cmd,3,1,(tile&1)*3,0,0);}
        if(extraLevels){A(vkCmdDraw)(cmd,6,1,24,0);A(vkCmdDraw)(cmd,6,1,30,0);}
        if(particleFrame){A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,particle);A(vkCmdDraw)(cmd,6,1,18,0);}
        {const auto passEnd=std::chrono::steady_clock::now();if(dynamicFrame)A(vkCmdEndRendering)(cmd);else A(vkCmdEndRenderPass)(cmd);endSeconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-passEnd).count();}
        if(continuation) {
            // No pipeline/descriptors/vertex/index/viewport/scissor setters here.
            // Rendering the same original water shader over the wall checks that
            // injection has restored application graphics state at the boundary.
            w.mode=0;begin.renderPass=continuationPass;
            A(vkCmdBeginRenderPass)(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);A(vkCmdDraw)(cmd,6,1,6,0);A(vkCmdEndRenderPass)(cmd);w.mode=mode;
        }
        VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.oldLayout=dynamicFrame?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=color;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};D(vkCmdCopyImageToBuffer)(cmd,color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        ok(A(vkEndCommandBuffer)(cmd));VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;ok(A(vkQueueSubmit)(queue,1,&submit,{}));ok(D(vkQueueWaitIdle)(queue));
        auto* pixels=static_cast<uint8_t*>(readMap);std::vector<uint8_t> out(pixels,pixels+width*height*4);CHECK(!errors);
        std::ofstream ppm("/tmp/water-"+std::to_string(renderSerial++)+".ppm",std::ios::binary);ppm<<"P6\n"<<width<<" "<<height<<"\n255\n";for(unsigned i=0;i<width*height;++i)ppm.write(reinterpret_cast<const char*>(pixels+i*4),3);
        std::printf("mode=%u debug=%u passes=%llu status=%s\n",mode,debugView,(unsigned long long)w.passes,w.status.c_str());return out;
    };
    auto baseline=render(0,0);CHECK(w.passes==0);
    auto mask=render(1,1);CHECK(w.passes==1);CHECK(mask!=baseline);
    auto ssr=render(1,0);CHECK(w.passes==2);CHECK(ssr!=baseline);
    auto planar=render(2,0);CHECK(w.passes==3);CHECK(planar!=baseline);
    size_t covered=0,ssrHits=0,planarHits=0;
    for(unsigned i=0;i<width*height;++i) {
        auto at=i*4;bool masked=mask[at+1]>180 && mask[at+2]>200;
        if(masked){++covered;CHECK(baseline[at+2]>200);}
        if(ssr[at]!=baseline[at]){CHECK(masked);CHECK(ssr[at+1]==0);++ssrHits;}
        if(planar[at]!=baseline[at]){CHECK(masked);CHECK(planar[at+1]==0);++planarHits;}
        if(!masked)for(unsigned channel=0;channel<4;++channel){CHECK(ssr[at+channel]==baseline[at+channel]);CHECK(planar[at+channel]==baseline[at+channel]);}
    }
    CHECK(covered>1000 && ssrHits>50 && planarHits>50);
    std::printf("verified pixels: water=%zu SSR=%zu planar=%zu; foreground unchanged; submerged green geometry clipped\n",covered,ssrHits,planarHits);
    auto restored=render(0,0);CHECK(restored==baseline);
    // Several water levels (real capture 26156-2, Shadowglen: two ponds at z 1306.97 and 1308.36 - the second one used to be
    // dropped whole because only the first tile's level was accepted). A horizontal pond at another level gets the water
    // look; a sloped patch still does not (neighbouring water at a different height).
    {
        extraLevels=true;auto levels=render(1,1);extraLevels=false;
        auto masked=[&](const std::vector<uint8_t>& img,unsigned x,unsigned y){auto at=(y*width+x)*4;return img[at+1]>180 && img[at+2]>200;};
        unsigned pond=0,slope=0;
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){pond+=masked(levels,27+dx,43+dy);slope+=masked(levels,111+dx,40+dy);}
        std::printf("multi-level water: second pond %u/25 pixels get the water look, sloped patch %u/25\n",pond,slope);
        CHECK(pond==25 && slope==0);
        // Real capture 27777-1 (same ponds, another view): WoW's first water draw held BOTH levels (54 indices at z 1306.97, 186
        // at 1308.36); requiring one level per draw rejected the whole plane and the water look vanished on both ponds.
        mixedFirst=true;auto mixed=render(1,1);mixedFirst=false;
        unsigned mixedPond=0,mixedSea=0;
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){mixedPond+=masked(mixed,27+dx,43+dy);mixedSea+=masked(mixed,96+dx,60+dy);}
        std::printf("first water draw with two levels: sea %u/25, pond %u/25 get the water look (status: %s)\n",mixedSea,mixedPond,w.status.c_str());
        CHECK(mixedSea==25 && mixedPond==25);
        // Real capture 39521-2 (Booty Bay): the pass's first water draw was pools up the hill (z 45.8/84.8/104) above a camera at
        // z 14.6. Taking the first level of the first draw as THE plane rejected the frame (camera below it): no mask, no water
        // look, and the fog ran down to the seabed - depending on the view. The sea must keep the look (SSR, and in planar mode
        // too: the mirror falls back to SSR when no reference level lies below the camera), the pool above stays untouched.
        highFirst=true;auto high=render(1,1);auto highPlanar=render(2,1);highFirst=false;
        unsigned highSea=0,highPlanarSea=0;
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){highSea+=masked(high,96+dx,60+dy);highPlanarSea+=masked(highPlanar,96+dx,60+dy);}
        std::printf("first water draw above the camera: sea %u/25 (SSR), %u/25 (planar mode)\n",highSea,highPlanarSea);
        CHECK(highSea==25 && highPlanarSea==25);
    }
    // WoW draws parts of the sea with vertex-shader variants that share the water fragment shader and the same transform
    // prologue (real captures: vs-ed2bfda7fbd0b2c4, vs-35d602d33ba6ba90 next to the selected vs-2fce10e0177c7507). They are the
    // same water: a tile drawn with a variant must be in the mask and get exactly what it gets with the selected shader.
    {
        auto variantVs=shader("water_scene_variant.vert.spv");
        vertexShader=variantVs;variantPipeline=pipeline(0,0,1,true);vertexShader=vs;
        variantTile=true;auto variantMask=render(1,1),variantSsr=render(1,0);variantTile=false;
        size_t maskDiffer=0,ssrDiffer=0;for(size_t i=0;i<mask.size();++i){maskDiffer+=variantMask[i]!=mask[i];ssrDiffer+=variantSsr[i]!=ssr[i];}
        std::printf("water tile drawn with a VS variant: mask view %zu bytes differ, SSR %zu bytes differ\n",maskDiffer,ssrDiffer);
        CHECK(maskDiffer==0 && ssrDiffer==0);
        A(vkDestroyPipeline)(device,variantPipeline,nullptr);A(vkDestroyShaderModule)(device,variantVs,nullptr);
    }
    {
        // A blocked replay must leave the source exactly as the game drew it and name every blocker at once.
        auto& vertexTracked=*state.buffers.at(id(vertex));auto& depthTracked=*state.images.at(id(depth));auto& layoutTracked=*state.layouts.at(id(layout));
        const auto vertexUsage=vertexTracked.usage,depthUsage=depthTracked.usage;const auto ranges=layoutTracked.pushRanges;
        auto has=[&](const char* part){return w.status.find(part)!=std::string::npos;};
        vertexTracked.usage&=~VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto passes=w.passes;CHECK(render(1,0)==baseline && w.passes==passes);
        CHECK(has("Water replay blocked") && has("buffers[need-storage-usage") && !has("source-image[") && !has("restore-bindings["));
        vertexTracked.usage=vertexUsage;
        // Flip, never erase: the registry entry owns the layer's depth view and frees it at device teardown.
        bool depthReady=false;
        {std::lock_guard lock(g_state_mutex);auto found=g_depth_candidates.find({device,image_handle_u64(depth)});if(found!=g_depth_candidates.end()){depthReady=found->second.sampledReady;found->second.sampledReady=false;}}
        depthTracked.usage&=~VK_IMAGE_USAGE_SAMPLED_BIT;for(auto& range:layoutTracked.pushRanges)range.stageFlags=VK_SHADER_STAGE_GEOMETRY_BIT;
        CHECK(render(1,0)==baseline && w.passes==passes);
        CHECK(has("source-image[depth-image-not-sampled]") && has("restore-bindings[push-range-no-vertex-or-fragment]") && has("water-draw[") && has("push-range-no-vertex-or-fragment"));
        depthTracked.usage=depthUsage;layoutTracked.pushRanges=ranges;
        {std::lock_guard lock(g_state_mutex);auto found=g_depth_candidates.find({device,image_handle_u64(depth)});if(found!=g_depth_candidates.end())found->second.sampledReady=depthReady;}
        CHECK(render(1,0)!=baseline && w.passes==passes+1);
    }
    {
        // DXVK rotates many command buffers. Each gets its own small slot but they must all share ONE set of heavy resources,
        // so no command buffer ever goes without a reflection (the old per-command-buffer limit made frames blink).
        std::array<VkCommandBuffer,7> many{};VkCommandBufferAllocateInfo more=ca;more.commandBufferCount=7;ok(A(vkAllocateCommandBuffers)(device,&more,many.data()));
        const auto original=cmd;const auto before=w.passes;
        for(auto extra:many){cmd=extra;CHECK(render(1,0)==ssr);}
        CHECK(w.passes==before+7);
        std::set<const WaterBatch*> distinct;for(auto extra:many)distinct.insert(state.commands.at(id(extra)).waterSlots.at(0)->batch.get());
        CHECK(distinct.size()==1);
        size_t liveBatches=0,liveSlots=0;for(auto& weak:w.batches)if(auto batch=weak.lock();batch && batch->alive)++liveBatches;
        for(auto& weak:w.slots)if(auto slot=weak.lock();slot && slot->alive)++liveSlots;
        CHECK(liveBatches==1 && liveSlots>=7);
        cmd=original;
    }
    {
        // Sky memory learns the sky the screen shows: with an orange "sky", water that reflects nothing else must turn orange.
        auto orangeReflections=[&](const std::vector<uint8_t>& image){size_t n=0;for(unsigned i=0;i<width*height;++i)n+=image[i*4+1]>10;return n;}; // RGBA8: green appears only via the sky
        const auto blackSky=orangeReflections(ssr);
        skyClear[0]=.9f;skyClear[1]=.6f;skyClear[2]=.1f;
        std::vector<uint8_t> orange;for(unsigned frame=0;frame<45;++frame)orange=render(1,0); // ~0.12 per frame: converges well within this
        CHECK(orangeReflections(orange)>blackSky+1000);
        // Back to a black sky and a re-initialised memory so the exact-pixel scenarios that follow are unaffected.
        skyClear[0]=skyClear[1]=skyClear[2]=0;
        for(auto& weak:w.batches)if(auto batch=weak.lock())batch->skyReady=false;
        render(1,0);render(0,0);
    }
    {
        // Every layer at once: refraction wobble, depth tint and foam only ever touch water pixels (outside the mask stays byte-identical).
        w.layers=waterLayersAll;w.tone=1; // every layer and a tone
        w.look.glitter=1;w.look.whitecaps=1;w.look.caustics=1.6f;w.look.clarity=1;
        auto all=render(1,0);auto plain=render(0,0);auto mask=render(1,1);
        size_t changedInside=0;for(unsigned i=0;i<width*height;++i){
            const bool masked=mask[i*4+1]>180&&mask[i*4+2]>200;bool changed=false;for(int c=0;c<4;++c)changed|=all[i*4+c]!=plain[i*4+c];
            if(!masked)CHECK(!changed);else changedInside+=changed;
        }
        CHECK(changedInside>1000);
        w.layers=waterLayerReflection;w.tone=0;w.look=WaterLook{};w.look.glitter=0;
    }
    {
        // One surface on both sides: the composite's DISPLACEMENT view (debug 6: r = height, g/b = horizontal orbit, .5 + 2 * yards) equals the
        // CPU reference (water_surface.cpp) at every water pixel it writes - each pixel's view ray, rebuilt from the capture's mask and camera,
        // intersected with the same packed waves. The waves are really there (the heights vary over the pond).
        const auto shownMask=render(1,1);
        w.captureArmed=true;const auto shown=render(1,6);render(0,0);CHECK(!w.capture);
        std::filesystem::path found;for(auto& entry:std::filesystem::directory_iterator(captureRoot))if(entry.is_directory() && entry.path().filename().string().find("-mode1-view6")!=std::string::npos)found=entry.path();
        CHECK(!found.empty());
        auto read=[&](const char* name){std::ifstream in(found/name,std::ios::binary|std::ios::ate);CHECK(in);std::vector<float> v(size_t(in.tellg())/4);in.seekg(0);in.read(reinterpret_cast<char*>(v.data()),std::streamsize(v.size()*4));return v;};
        const auto mask=read("mask.bin"),camera=read("camera.bin");CHECK(mask.size()==size_t(width)*height*4 && camera.size()==4*waterCameraWidth);
        auto texel=[&](int i){return water::Vec3{camera[i*4],camera[i*4+1],camera[i*4+2]};};
        const auto c5=texel(14),c6=texel(15),c7=texel(16),c8=texel(17);
        auto dot3=[](water::Vec3 a,water::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
        auto worldOf=[&](water::Vec3 v){const water::Vec3 t{v.x-c8.x,v.y-c8.y,v.z-c8.z};return water::Vec3{dot3(t,c5),dot3(t,c6),dot3(t,c7)};};
        const auto packed=water::pack_waves(w.waves,w.look.waveHeight,w.look.waveSteepness,water_time_seconds());
        CHECK(packed.surface[0]==4.f && camera[18*4]==1.f);   // default waves; the smoke's sea is the validated plane (texel 18.x)
        const float baseZ=camera[18*4+1],p11=camera[10*4+3];water::Vec3 up=texel(9);
        const auto cameraWorld=worldOf({0,0,0});
        unsigned compared=0,worst=0;float lowest=1,highest=0;
        for(unsigned i=0;i<width*height;++i) {
            if(!(shownMask[i*4+1]>180 && shownMask[i*4+2]>200))continue;   // the pixels the composite writes (the MASK view: cyan)
            const water::Vec3 v{mask[i*4],mask[i*4+1],mask[i*4+2]};
            if(std::abs(dot3(worldOf(v),{0,0,1})-baseZ)>.05f)continue;       // the other levels of the scene (not drawn by this scenario anyway)
            const float len=std::sqrt(dot3(v,v));water::Vec3 n=up;if(dot3(n,v)>0)n={-n.x,-n.y,-n.z};
            const float cosView=std::max(-dot3(n,v)/len/std::sqrt(dot3(n,n)),.05f),footprint=std::min(v.z*2.f/(p11*float(height))/cosView,64.f);
            const auto flat=worldOf(v);
            water::SurfacePoint s;water::Vec3 hit;
            CHECK(water::intersect_surface(packed,cameraWorld,{flat.x-cameraWorld.x,flat.y-cameraWorld.y,flat.z-cameraWorld.z},baseZ,footprint,s,hit));
            const float expected[3]{s.displacement.z,s.displacement.x,s.displacement.y};
            for(int c=0;c<3;++c) {
                const int want=int(std::lround(std::clamp(.5f+expected[c]*2.f,0.f,1.f)*255.f));
                worst=std::max(worst,unsigned(std::abs(int(shown[i*4+c])-want)));
            }
            lowest=std::min(lowest,s.displacement.z);highest=std::max(highest,s.displacement.z);++compared;
        }
        std::printf("displaced surface: %u water pixels, GPU vs CPU worst %u/255, heights %.3f .. %.3f yd\n",compared,worst,lowest,highest);
        CHECK(compared>1000 && worst<=2 && highest-lowest>.05f);
        std::filesystem::remove_all(found);
        // WAVE HEIGHT 0 is the flat base plane: zero displacement everywhere.
        w.look.waveHeight=0;const auto flatView=render(1,6);w.look.waveHeight=1;
        for(unsigned i=0;i<width*height;++i)if(shownMask[i*4+1]>180 && shownMask[i*4+2]>200)for(int c=0;c<3;++c)CHECK(flatView[i*4+c]==127 || flatView[i*4+c]==128);   // .5 (UNORM rounding of 127.5)
    }
    {
        // MODERN on the GPU = water_shading.cpp: the FRESNEL view (sqrt F) and the ABSORPTION view (1 - T per channel) at every water pixel the
        // composite writes, with a flat surface (no waves, no ripples, the lowest roughness) so the normal is the plane's. And the A/B switch:
        // CLASSIC and MODERN are different pictures, and both change only water pixels.
        const auto look=w.look;
        w.look.waveHeight=0;w.look.mediumNormals=0;w.look.microNormals=0;w.look.roughness=.03f;
        const auto shownMask=render(1,1);
        auto captureView=[&](uint32_t view){
            w.captureArmed=true;auto image=render(1,view);render(0,0);CHECK(!w.capture);
            std::filesystem::path found;
            for(auto& entry:std::filesystem::directory_iterator(captureRoot))
                if(entry.is_directory() && entry.path().filename().string().find("-mode1-view"+std::to_string(view))!=std::string::npos)found=entry.path();
            CHECK(!found.empty());return std::make_pair(image,found);
        };
        auto read=[&](const std::filesystem::path& dir,const char* name){std::ifstream in(dir/name,std::ios::binary|std::ios::ate);CHECK(in);std::vector<float> v(size_t(in.tellg())/4);in.seekg(0);in.read(reinterpret_cast<char*>(v.data()),std::streamsize(v.size()*4));return v;};
        const auto [fresnelView,fresnelDir]=captureView(11);
        const auto [absorbView,absorbDir]=captureView(18);
        const auto mask=read(absorbDir,"mask.bin"),camera=read(absorbDir,"camera.bin"),depthRaw=read(absorbDir,"scene_depth.bin");
        CHECK(mask.size()==size_t(width)*height*4 && depthRaw.size()==size_t(width)*height);
        auto texel=[&](int i,int c){return camera[i*4+c];};
        const float r0=texel(12,0),r1=texel(12,1),p22=texel(13,2),p23=texel(13,3),p00=texel(10,2),p11=texel(10,3);
        float up[3]{texel(9,0),texel(9,1),texel(9,2)};const float upLength=std::sqrt(up[0]*up[0]+up[1]*up[1]+up[2]*up[2]);
        for(auto& u:up)u/=upLength;
        unsigned compared=0,worstF=0,worstA=0;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const size_t i=size_t(y)*width+x;
            if(!(shownMask[i*4+1]>180 && shownMask[i*4+2]>200))continue;
            const float v[3]{mask[i*4],mask[i*4+1],mask[i*4+2]};const float length=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
            float nv=-(up[0]*v[0]+up[1]*v[1]+up[2]*v[2])/length;nv=std::abs(nv);
            if(nv<.05f)continue;                                                                  // (the shader bends facets turned away)
            const float f=water::fresnel_rough(nv,.03f);
            worstF=std::max(worstF,unsigned(std::abs(int(fresnelView[i*4])-int(std::lround(std::sqrt(f)*255.f)))));
            float path=1e3f;const float raw=depthRaw[i];
            if(raw<r1-1e-6f) {
                const float d=(raw-r0)/(r1-r0),z=p23/(d-p22);
                const float ndcX=(x+.5f-texel(11,0))/texel(11,2)*2.f-1.f,ndcY=(y+.5f-texel(11,1))/texel(11,3)*2.f-1.f;
                const float bed[3]{ndcX/p00*z,ndcY/p11*z,z};
                path=std::max(std::sqrt(bed[0]*bed[0]+bed[1]*bed[1]+bed[2]*bed[2])-length,0.f);
            }
            float t[3];water::transmittance(w.absorption,w.look.clarity,path,t);
            for(int c=0;c<3;++c)worstA=std::max(worstA,unsigned(std::abs(int(absorbView[i*4+c])-int(std::lround((1.f-t[c])*255.f)))));
            ++compared;
        }
        std::printf("modern terms: %u water pixels, FRESNEL GPU vs CPU worst %u/255, ABSORPTION worst %u/255\n",compared,worstF,worstA);
        CHECK(compared>1000 && worstF<=2 && worstA<=2);
        std::filesystem::remove_all(fresnelDir);std::filesystem::remove_all(absorbDir);
        w.look=look;
        // A/B: classic and modern differ on the water, not anywhere else.
        w.layers=waterLayersAll;w.style=0;const auto classic=render(1,0);w.style=1;const auto modern=render(1,0);w.layers=waterLayerReflection;
        const auto plain=render(0,0);size_t abDiffer=0;
        for(unsigned i=0;i<width*height;++i) {
            const bool masked=shownMask[i*4+1]>180 && shownMask[i*4+2]>200;bool differ=false;
            for(int c=0;c<3;++c){differ|=classic[i*4+c]!=modern[i*4+c];if(!masked){CHECK(classic[i*4+c]==plain[i*4+c]);CHECK(modern[i*4+c]==plain[i*4+c]);}}
            abDiffer+=differ && masked;
        }
        std::printf("A/B: classic vs modern differ on %zu water pixels, nowhere else\n",abDiffer);
        CHECK(abDiffer>1000);
        // WATER TINT: the game's water colour without its texture (the water colour's mip chain, divided by the averaged alpha). The test water is
        // one flat colour, (0,0,1) at alpha .65, alpha-blended as WoW's (blend mode 1), so every water pixel - the edges too, where the chain also
        // averages the empty texels around the water - must show exactly that colour (without the division the interior reads .65, 166/255).
        // The first frame lets the blend mode (read from the recorded draw) settle.
        w.layers=waterLayersAll;w.style=1;alphaFrame=true;render(1,21);const auto tintView=render(1,21);alphaFrame=false;w.layers=waterLayerReflection;
        unsigned tinted=0,worstTint=0;
        for(unsigned i=0;i<width*height;++i)if(shownMask[i*4+1]>180 && shownMask[i*4+2]>200) {
            const int want[3]{0,0,255};
            for(int c=0;c<3;++c)worstTint=std::max(worstTint,unsigned(std::abs(int(tintView[i*4+c])-want[c])));
            ++tinted;
        }
        std::printf("water tint: %u water pixels, worst %u/255 from the water's flat colour\n",tinted,worstTint);
        CHECK(tinted>1000 && worstTint<=2);
        // The texture must go, not turn into blotches: the same water with an 8-pixel checker (x1 / x.5) in its colour. The tint reads one level
        // per ~6 yards of water, far coarser than the checker, so it is the checker's mean (.75 blue, 191) everywhere, smoothly. A fixed fine level
        // (or none) shows the checker: 128 / 255 apart.
        w.layers=waterLayersAll;patternFrame=true;render(1,21);const auto patternView=render(1,21);patternFrame=false;w.layers=waterLayerReflection;
        unsigned worstPattern=0,patternPixels=0,lowest=255,highest=0;
        for(unsigned i=0;i<width*height;++i)if(shownMask[i*4+1]>180 && shownMask[i*4+2]>200) {
            const unsigned b=patternView[i*4+2];lowest=std::min(lowest,b);highest=std::max(highest,b);
            worstPattern=std::max(worstPattern,unsigned(std::abs(int(b)-191)));++patternPixels;
        }
        std::printf("textured water tint: %u water pixels, blue %u..%u (checker mean 191), worst %u/255\n",patternPixels,lowest,highest,worstPattern);
        CHECK(patternPixels>1000 && worstPattern<=6);
        // An effect blended over the water later in the pass (a fishing pool's swirl: additive, no depth write) must end up on top of the water
        // composite unchanged: held back and issued after it. Taken into the picture the composite undoes the game's blend from, it was
        // amplified by 1 / (1 - alpha) (2.9 here) and tinted by the absorption: a purple ring in game (39664). Dynamic rendering, as DXVK:
        // held draws are issued in a rendering instance of their own (a render pass object cannot be re-begun).
        std::swap(gray,dynamicGray);std::swap(red,dynamicRed);std::swap(green,dynamicGreen);std::swap(water,dynamicWater);
        std::swap(alphaWater,dynamicAlphaWater);std::swap(particle,dynamicParticle);dynamicFrame=true;
        w.layers=waterLayersAll;alphaFrame=true;render(1,0);const auto withoutEffect=render(1,0);
        particleFrame=true;render(1,0);const auto withEffect=render(1,0);particleFrame=false;
        CHECK(fog_state(state).deferredAfterWater>0);
        const int effect[3]{51,26,15};unsigned effectPixels=0,worstEffect=0;
        for(unsigned i=0;i<width*height;++i)if(shownMask[i*4+1]>180 && shownMask[i*4+2]>200) {
            bool clipped=false;for(int c=0;c<3;++c)clipped|=withoutEffect[i*4+c]+effect[c]>250;
            if(clipped)continue;
            for(int c=0;c<3;++c)worstEffect=std::max(worstEffect,unsigned(std::abs(int(withEffect[i*4+c])-int(withoutEffect[i*4+c])-effect[c])));
            ++effectPixels;
        }
        std::printf("effect over the water: %u water pixels, worst %u/255 from composite + effect\n",effectPixels,worstEffect);
        CHECK(effectPixels>500 && worstEffect<=2);
        // The waterline: where a wall stands in the water, the game's water fails its depth test just above the line (the wall is in front),
        // ours passed it within the depth slack and undid a blend that never happened - a black / red line on every shore in game. Every
        // pixel the game's water did not touch (the same with and without the water draws) must stay exactly as the game drew it.
        crossingWall=true;
        const unsigned tiles=waterTiles;waterTiles=0;render(0,0);const auto noWater=render(0,0);waterTiles=tiles;
        render(0,0);const auto gamePicture=render(0,0);render(1,0);const auto ours=render(1,0);
        unsigned untouched=0,changedOutside=0;
        for(unsigned i=0;i<width*height;++i) {
            bool same=true;for(int c=0;c<3;++c)same&=noWater[i*4+c]==gamePicture[i*4+c];
            if(!same)continue;
            ++untouched;bool differ=false;for(int c=0;c<3;++c)differ|=ours[i*4+c]!=gamePicture[i*4+c];changedOutside+=differ;
        }
        std::printf("waterline: %u pixels without the game's water, %u changed by the composite\n",untouched,changedOutside);
        CHECK(untouched>1000 && changedOutside==0);
        crossingWall=false;alphaFrame=false;w.layers=waterLayerReflection;
        std::swap(gray,dynamicGray);std::swap(red,dynamicRed);std::swap(green,dynamicGreen);std::swap(water,dynamicWater);
        std::swap(alphaWater,dynamicAlphaWater);std::swap(particle,dynamicParticle);dynamicFrame=false;
    }
    auto verifyCapture=[&](const char* label,bool expectDepth) {
        // Debug capture: every section must arrive intact and describe what the layer replayed.
        std::filesystem::path found;for(auto& entry:std::filesystem::directory_iterator(captureRoot))if(entry.is_directory() && entry.path().filename().string().find("-mode2-view0")!=std::string::npos)found=entry.path();
        CHECK(!found.empty());
        auto file=[&](const char* name){std::ifstream in(found/name,std::ios::binary|std::ios::ate);CHECK(in);std::vector<char> bytes(size_t(in.tellg()));in.seekg(0);in.read(bytes.data(),std::streamsize(bytes.size()));return bytes;};
        auto floats=[&](const std::vector<char>& b){std::vector<float> v(b.size()/4);std::memcpy(v.data(),b.data(),v.size()*4);return v;};
        const auto json=file("capture.json");const std::string text(json.begin(),json.end());
        CHECK(text.find("\"role\":\"water-plane\"")!=std::string::npos && text.find("\"role\":\"water\"")!=std::string::npos && text.find("\"role\":\"reflected\"")!=std::string::npos && text.find("\"trigger\":\"menu\"")!=std::string::npos);
        CHECK(text.find("\"extra\":[1,")!=std::string::npos && text.find("\"foam\":[1,1,")!=std::string::npos);   // game texture opacity 1 (untouched); foam amount 1, size 1
        CHECK(text.find("\"name\":\"mask\"")!=std::string::npos && text.find("\"shaders\":[\n {")!=std::string::npos);
        auto camera=floats(file("camera.bin"));CHECK(camera.size()==4*waterCameraWidth);
        CHECK(camera[13*4+0]==1.f && camera[13*4+1]==0.f && std::abs(camera[13*4+2]-100.f/99.8f)<1e-5f && std::abs(camera[13*4+3]+20.f/99.8f)<1e-5f); // valid, plane height, P22, P23
        CHECK(std::abs(camera[9*4+2]-.8f)<1e-3f || std::abs(camera[9*4+2]+.8f)<1e-3f || camera[9*4+2]!=0.f);
        auto mask=floats(file("mask.bin"));CHECK(mask.size()==size_t(width)*height*4);size_t covered=0;for(size_t i=0;i<width*height;++i)if(mask[i*4+3]>0.f){++covered;CHECK(mask[i*4+2]>0.f);}
        CHECK(covered>1000);
        {   // The water shader's own output, kept by the mask pass: alpha is the original coverage (same pixels as the mask), rgb the untouched color.
            auto own=file("water_color.bin");CHECK(own.size()==size_t(width)*height*8);size_t ownCovered=0;
            for(size_t i=0;i<width*height;++i){uint16_t half;std::memcpy(&half,own.data()+i*8+6,2);if(half_to_float(half)>0.f)++ownCovered;}
            CHECK(ownCovered==covered);
        }
        auto before=file("scene_before.bin"),after=file("scene_after.bin");CHECK(before.size()==after.size() && before.size()==size_t(width)*height*4);size_t changed=0;for(size_t i=0;i<before.size();++i)changed+=before[i]!=after[i];
        CHECK(changed>50);
        CHECK(expectDepth==std::filesystem::exists(found/"scene_depth.bin") && expectDepth==(text.find("\"name\":\"scene_depth\"")!=std::string::npos));
        if(expectDepth)CHECK(file("scene_depth.bin").size()==size_t(width)*height*4);
        CHECK(file("planar_color.bin").size()==size_t(width)*height*8 && file("planar_depth.bin").size()==size_t(width)*height*4 && file("mask_depth.bin").size()==size_t(width)*height*4);
        auto skyBytes=file("sky_memory.bin");CHECK(skyBytes.size()==size_t(waterSkyWidth)*waterSkyHeight*8);
        for(size_t i=0;i<skyBytes.size()/2;++i){uint16_t half;std::memcpy(&half,skyBytes.data()+i*2,2);CHECK(std::isfinite(half_to_float(half)));}
        auto ubo=floats(file("water_ubo.bin"));CHECK(ubo.size()==128 && std::abs(ubo[64]-1.2f)<1e-6f);   // c0.x = projection scale at byte 256
        auto indices=file("water_indices.bin");CHECK(indices.size()==6);uint16_t first[3];std::memcpy(first,indices.data(),6);for(unsigned i=0;i<3;++i)CHECK(first[i]==18+i); // the plane draw is the first tile: 3 indices
        auto vertexBytes=file("water_vertices.bin");CHECK(vertexBytes.size()==vertices.size()*4 && std::memcmp(vertexBytes.data(),vertices.data(),vertexBytes.size())==0);
        std::printf("capture verified (%s): %s, %zu masked pixels, %zu changed color bytes\n",label,found.filename().c_str(),covered,changed);
        std::filesystem::remove_all(found);
    };
    {
        // The real WoW path: DXVK uses dynamic rendering, not a VkRenderPass. Same scene, same pixels expected.
        std::swap(gray,dynamicGray);std::swap(red,dynamicRed);std::swap(green,dynamicGreen);std::swap(water,dynamicWater);dynamicFrame=true;
        const auto before=w.passes;
        CHECK(render(0,0)==baseline && w.passes==before);
        CHECK(render(1,1)==mask && w.passes==before+1);
        CHECK(render(1,0)==ssr && w.passes==before+2);
        CHECK(render(2,0)==planar && w.passes==before+3);
        CHECK(render(0,0)==baseline);
        // Depth that cannot be copied (no TRANSFER_SRC as far as the layer knows) must be skipped, not recorded invalidly.
        auto& depthTracked=*state.images.at(id(depth));const auto depthUsage=depthTracked.usage;depthTracked.usage&=~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        w.captureArmed=true;CHECK(render(2,0)==planar);CHECK(!w.captureArmed && w.capture);render(0,0);CHECK(!w.capture);verifyCapture("dynamic rendering, depth not copyable",false);
        depthTracked.usage=depthUsage;
        dynamicFrame=false;std::swap(gray,dynamicGray);std::swap(red,dynamicRed);std::swap(green,dynamicGreen);std::swap(water,dynamicWater);
    }
    w.captureArmed=true;CHECK(render(2,0)==planar);render(0,0);CHECK(!w.capture);verifyCapture("render pass",true);
    continuation=true;auto continued=render(2,0);
    for(unsigned i=0;i<width*height;++i)if(baseline[i*4]>200 && baseline[i*4+2]==0){CHECK(continued[i*4]==0 && continued[i*4+2]>200);}
    continuation=false;wideIndices=true;for(unsigned i=0;i<8;++i)static_cast<uint32_t*>(indexMap)[i]=18+i%6;
    CHECK(render(2,0)==planar);wideIndices=false;for(unsigned i=0;i<8;++i)static_cast<uint16_t*>(indexMap)[i]=uint16_t(18+i%6);
    {
        // The second tile lies 0.3 above the validated plane: it is in the mask but the composite must not reflect on it.
        auto changedPixels=[&](const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){size_t n=0;for(unsigned i=0;i<width*height;++i)n+=a[i*4]!=b[i*4]||a[i*4+1]!=b[i*4+1]||a[i*4+2]!=b[i*4+2];return n;};
        const auto flatSsr=changedPixels(ssr,baseline),flatPlanar=changedPixels(planar,baseline);
        for(unsigned v:{21u,22u,23u})static_cast<float*>(vertexMap)[v*3+2]=.3f;
        auto raised=render(0,0);auto raisedSsr=render(1,0),raisedPlanar=render(2,0);
        const auto ssrNow=changedPixels(raisedSsr,raised),planarNow=changedPixels(raisedPlanar,raised);
        CHECK(ssrNow>0 && ssrNow<flatSsr && planarNow>0 && planarNow<flatPlanar);
        for(unsigned v:{21u,22u,23u})static_cast<float*>(vertexMap)[v*3+2]=0.f;
        CHECK(render(0,0)==baseline);
    }
    {
        // Perf instrumentation: after 300 recorded water passes the layer logs CPU time and GPU timestamps per stage (validation-clean).
        w.debug=0;for(unsigned i=0;i<310;++i)render(1,0);render(0,0);
        std::ifstream log("/tmp/depth-probe-water-gpu.log");const std::string text((std::istreambuf_iterator<char>(log)),std::istreambuf_iterator<char>());
        const auto at=text.rfind("[WATER-PERF] cpu record avg ");CHECK(at!=std::string::npos);
        const auto line=text.substr(at,text.find('\n',at)-at);std::puts(line.c_str());
        const auto gpu=line.find("gpu avg ");CHECK(gpu!=std::string::npos && std::atof(line.c_str()+gpu+8)>0.);
        CHECK(line.find("composite ")!=std::string::npos && line.find("waterDraws=")!=std::string::npos);
    }
    {
        // CPU cost of the replay with a sea-sized number of draws (WoW draws ~170 tiles per frame). Printed for reading; the numbers are only
        // meaningful without the validation layer.
        auto seconds=[&](unsigned tiles,uint32_t mode){
            waterTiles=tiles;render(mode,0);endSeconds=0;for(unsigned i=0;i<40;++i)render(mode,0);
            return endSeconds/40;
        };
        const double off=seconds(172,0),ssr=seconds(172,1),planar=seconds(172,2),small=seconds(2,1);
        std::printf("pass-end CPU time with 172 water draws: off %.3f ms, ssr %.3f ms (+%.3f ms = %.1f us per water draw), planar %.3f ms; 2 draws: ssr %.3f ms%s\n",
                    off*1e3,ssr*1e3,(ssr-off)*1e3,(ssr-off)*1e6/172,planar*1e3,small*1e3,validate?" (validation on: not representative)":"");
        waterTiles=2;render(0,0);
    }
    {
        // What the layer's hooks cost per draw on the real driver: the same draw-heavy pass recorded through the layer (A) and straight into the driver (D).
        // WoW records ~1700 draws per frame with several state calls each. Printed for reading; only meaningful without the validation layer.
        w.mode=0;VkCommandBufferAllocateInfo direct=ca;VkCommandBuffer layerCmd{},driverCmd{};ok(A(vkAllocateCommandBuffers)(device,&ca,&layerCmd));ok(D(vkAllocateCommandBuffers)(device,&direct,&driverCmd));
        const unsigned draws=1700;float constants[24];std::fill_n(constants,24,1.f);
        auto record=[&](bool viaLayer)->double{
            const auto f=[&](auto layerFn,auto driverFn){return viaLayer?layerFn:driverFn;};
            VkCommandBuffer c=viaLayer?layerCmd:driverCmd;VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VkClearValue clears[2]{};clears[1].depthStencil.depth=1;VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;begin.renderPass=pass;begin.framebuffer=framebuffer;begin.renderArea.extent={width,height};begin.clearValueCount=2;begin.pClearValues=clears;
            VkViewport viewport{0,float(height),float(width),-float(height),0,.94f};VkRect2D scissor{{0,0},{width,height}};VkDeviceSize offset=0,size=vertices.size()*4,stride=12;
            const auto start=std::chrono::steady_clock::now();
            f(A(vkResetCommandBuffer),D(vkResetCommandBuffer))(c,0);f(A(vkBeginCommandBuffer),D(vkBeginCommandBuffer))(c,&bi);f(A(vkCmdBeginRenderPass),D(vkCmdBeginRenderPass))(c,&begin,VK_SUBPASS_CONTENTS_INLINE);
            f(A(vkCmdSetViewportWithCount),D(vkCmdSetViewportWithCount))(c,1,&viewport);f(A(vkCmdSetScissorWithCount),D(vkCmdSetScissorWithCount))(c,1,&scissor);
            f(A(vkCmdSetDepthTestEnable),D(vkCmdSetDepthTestEnable))(c,VK_TRUE);f(A(vkCmdSetDepthCompareOp),D(vkCmdSetDepthCompareOp))(c,VK_COMPARE_OP_LESS_OR_EQUAL);f(A(vkCmdSetCullMode),D(vkCmdSetCullMode))(c,VK_CULL_MODE_NONE);
            f(A(vkCmdSetFrontFace),D(vkCmdSetFrontFace))(c,VK_FRONT_FACE_COUNTER_CLOCKWISE);f(A(vkCmdSetPrimitiveTopology),D(vkCmdSetPrimitiveTopology))(c,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);f(A(vkCmdSetStencilTestEnable),D(vkCmdSetStencilTestEnable))(c,VK_FALSE);
            f(A(vkCmdSetDepthBoundsTestEnable),D(vkCmdSetDepthBoundsTestEnable))(c,VK_FALSE);f(A(vkCmdSetDepthBiasEnable),D(vkCmdSetDepthBiasEnable))(c,VK_FALSE);f(A(vkCmdSetDepthBias),D(vkCmdSetDepthBias))(c,0,0,0);
            f(A(vkCmdSetStencilOp),D(vkCmdSetStencilOp))(c,VK_STENCIL_FACE_FRONT_AND_BACK,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS);
            f(A(vkCmdSetStencilCompareMask),D(vkCmdSetStencilCompareMask))(c,VK_STENCIL_FACE_FRONT_AND_BACK,255);f(A(vkCmdSetStencilWriteMask),D(vkCmdSetStencilWriteMask))(c,VK_STENCIL_FACE_FRONT_AND_BACK,255);f(A(vkCmdSetStencilReference),D(vkCmdSetStencilReference))(c,VK_STENCIL_FACE_FRONT_AND_BACK,0);
            f(A(vkCmdSetDepthWriteEnable),D(vkCmdSetDepthWriteEnable))(c,VK_TRUE);
            f(A(vkCmdBindDescriptorSets),D(vkCmdBindDescriptorSets))(c,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,2,1,&set,0,nullptr);f(A(vkCmdBindVertexBuffers2),D(vkCmdBindVertexBuffers2))(c,0,1,&vertex,&offset,&size,&stride);
            for(unsigned i=0;i<draws;++i) {
                f(A(vkCmdPushConstants),D(vkCmdPushConstants))(c,layout,pushRange.stageFlags,0,96,constants);
                f(A(vkCmdBindPipeline),D(vkCmdBindPipeline))(c,VK_PIPELINE_BIND_POINT_GRAPHICS,(i&1)?red:gray);
                f(A(vkCmdSetDepthWriteEnable),D(vkCmdSetDepthWriteEnable))(c,i&1);f(A(vkCmdSetCullMode),D(vkCmdSetCullMode))(c,VK_CULL_MODE_NONE);
                f(A(vkCmdDraw),D(vkCmdDraw))(c,6,1,(i%3)*6,0);
            }
            f(A(vkCmdEndRenderPass),D(vkCmdEndRenderPass))(c);f(A(vkEndCommandBuffer),D(vkEndCommandBuffer))(c);
            return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        };
        // Submitting: through the layer the submit hook walks every recorded draw (descriptor resolution, state snapshot) before the driver call.
        auto submitSeconds=[&](bool viaLayer)->double{
            VkCommandBuffer c=viaLayer?layerCmd:driverCmd;VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&c;
            record(viaLayer);const auto start=std::chrono::steady_clock::now();
            ok((viaLayer?A(vkQueueSubmit):D(vkQueueSubmit))(queue,1,&submit,{}));const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            ok(D(vkQueueWaitIdle)(queue));return seconds;
        };
        for(unsigned i=0;i<3;++i){submitSeconds(true);submitSeconds(false);}
        double submitLayer=0,submitDriver=0;for(unsigned i=0;i<10;++i){submitLayer+=submitSeconds(true);submitDriver+=submitSeconds(false);}
        std::printf("submitting that command buffer: through the layer %.3f ms, straight to the driver %.3f ms => layer adds %.3f ms per submit of %u draws (%.2f us per draw)\n",
                    submitLayer*100,submitDriver*100,(submitLayer-submitDriver)*100,draws,(submitLayer-submitDriver)/10*1e6/draws);
        for(unsigned i=0;i<5;++i){record(true);record(false);}
        double viaLayer=0,direct2=0;const unsigned rounds=30;for(unsigned i=0;i<rounds;++i){viaLayer+=record(true);direct2+=record(false);}
        viaLayer/=rounds;direct2/=rounds;
        std::printf("recording %u draws (each with push constants, pipeline bind and 2 state calls): through the layer %.3f ms, straight into the driver %.3f ms => layer adds %.2f us per draw (%.2f ms per frame of 1700 draws)%s\n",
                    draws,viaLayer*1e3,direct2*1e3,(viaLayer-direct2)*1e6/draws,(viaLayer-direct2)*1e3,validate?" (validation on: not representative)":"");
    }
    // Non-horizontal water must preserve the source exactly. The whole sea is tilted as ONE plane (18.75%: the near corners 18,
    // 19 and 21 - the second tile's copy of 18 - raised to 1.5): with only one tile tilted the other is a flat level of its own
    // and rightly gets the look, and two tiles tilted differently leave a 1-pixel crease strip whose neighbours read as nearly
    // level (known limitation, docs/water.md).
    for(unsigned v:{18u,19u,21u})static_cast<float*>(vertexMap)[v*3+2]=1.5f;
    auto invalid=render(1,1);CHECK(invalid==render(0,0));
    // A gentle 10% slope across the view too (the x = -5 edge raised to 1): near the camera 3 px sideways are only ~.15 yd, where
    // the old fixed .02 height tolerance took it for level water; the slope criterion (3% + depth precision) does not.
    for(unsigned v:{18u,19u,21u})static_cast<float*>(vertexMap)[v*3+2]=0.f;
    for(unsigned v:{18u,21u,23u})static_cast<float*>(vertexMap)[v*3+2]=1.f;
    {auto gentle=render(1,1),off=render(0,0);unsigned changed=0;for(size_t k=0;k<gentle.size();k+=4)changed+=gentle[k]!=off[k]||gentle[k+1]!=off[k+1]||gentle[k+2]!=off[k+2];
     std::printf("gentle 10%% slope: %u pixels taken for level water\n",changed);CHECK(changed==0);}
    for(unsigned v:{18u,21u,23u})static_cast<float*>(vertexMap)[v*3+2]=0.f;
    for(unsigned v:{18u,19u,21u})static_cast<float*>(vertexMap)[v*3+2]=1.5f;
    CHECK(state.commands.at(id(cmd)).waterSlots.size()==1);
    auto heldSlot=state.commands.at(id(cmd)).waterSlots.front();auto heldBatch=heldSlot->batch;
    // Draws are only snapshotted when something wants them (the water replay wants the water tiles): record one water frame first.
    render(1,0);std::shared_ptr<Draw> draw;
    for(const auto& event:state.commands.at(id(cmd)).events)if(event.draw && event.draw->pipeline==waterMeta)draw=event.draw;
    CHECK(draw);
    {   // Draws are snapshotted only when something wants them: an armed pick/highlight/capture, or the water replay for its own shader pair
        // (all test pipelines share one shader pair, so what is recorded here is every draw; a different pair must not be).
        // A pair with another fragment shader is not water. A VS with another hash but the same transform prologue and the water FS
        // is a water VS variant (see water_draw_shaders) and is wanted.
        auto scene=std::make_shared<Pipeline>(*waterMeta);scene->fs=std::make_shared<Shader>(*waterMeta->fs);scene->fs->hash^=1;
        auto variant=std::make_shared<Pipeline>(*waterMeta);variant->vs=std::make_shared<Shader>(*waterMeta->vs);variant->vs->hash^=1;
        CHECK(!draw_records_wanted(state) && water_wants_draw(state,waterMeta.get()) && !water_wants_draw(state,scene.get()) && water_wants_draw(state,variant.get()));
        {auto unrelated=std::make_shared<Pipeline>(*waterMeta);unrelated->vs=std::make_shared<Shader>();unrelated->vs->hash=waterMeta->vs->hash^2;
         CHECK(!water_wants_draw(state,unrelated.get()));}   // no transform prologue (no code): not a variant
        w.mode=2;CHECK(water_wants_draw(state,scene.get()));                                              // planar re-draws the opaque scene
        w.mode=0;CHECK(!water_wants_draw(state,waterMeta.get()));w.mode=1;
        state.requestedFrames=1;CHECK(draw_records_wanted(state));state.requestedFrames=0;
        state.explorerUntil=state.frameSerial+5;CHECK(draw_records_wanted(state));state.explorerUntil=0;
    }
    WaterParams params{};VkDescriptorBufferInfo cameraSource{camera,256,144},vertexSource{vertex,0,vertices.size()*4},indexSource{index,0,32};
    {
        // The water textures come from shaders/water_detail.bin: 3 layers x 9 mip levels. Without the file the batch still works with a flat 1x1 fallback.
        CHECK(heldBatch->detail.fromFile && heldBatch->detail.levels==9 && heldBatch->detail.copies.size()==27 && heldBatch->detail.uploads>=waterDetailUploadExecutions);
        const auto bare=std::filesystem::temp_directory_path()/("depth-probe-water-shaders-bare-"+std::to_string(getpid()));std::filesystem::create_directories(bare);
        for(const char* name:{"depth.vert","water_camera.frag","water_composite.frag","water_sky.frag"})std::filesystem::copy_file(std::filesystem::path(DEPTH_PROBE_TEST_ROOT_DIR)/"shaders"/(std::string(name)+".spv"),bare/(std::string(name)+".spv"),std::filesystem::copy_options::overwrite_existing);
        setenv("DEPTH_PROBE_SHADER_DIR",bare.c_str(),1);
        WaterParams fallbackParams{};VkDescriptorBufferInfo cameraInfo{camera,256,144},vertexInfo{vertex,0,vertices.size()*4},indexInfo{index,0,32};std::string why;
        auto fallback=create_water_batch(data,*draw,1,fallbackParams,cameraInfo,vertexInfo,indexInfo,&why);
        CHECK(fallback && !fallback->detail.fromFile && fallback->detail.levels==1 && fallback->detail.copies.size()==3);
        if(fallback)fallback->release();
        setenv("DEPTH_PROBE_SHADER_DIR",DEPTH_PROBE_TEST_ROOT_DIR "/shaders",1);std::filesystem::remove_all(bare);
    }
    setenv("DEPTH_PROBE_SHADER_DIR","/tmp/depth-probe-water-shaders-missing",1);
    std::string failure;CHECK(!create_water_batch(data,*draw,1,params,cameraSource,vertexSource,indexSource,&failure));
    CHECK(failure.find("camera-resolve pipeline")!=std::string::npos && failure.find("shader file missing or unreadable")!=std::string::npos);
    setenv("DEPTH_PROBE_SHADER_DIR",DEPTH_PROBE_TEST_ROOT_DIR "/shaders",1);
    CHECK(heldBatch->alive);
    A(vkDestroyShaderModule)(device,particleFs,nullptr);
    for(auto p:{gray,red,green,water,alphaWater,patternWater,particle,dynamicGray,dynamicRed,dynamicGreen,dynamicWater,dynamicAlphaWater,dynamicParticle})A(vkDestroyPipeline)(device,p,nullptr);
    for(auto module:{vs,fs})A(vkDestroyShaderModule)(device,module,nullptr);
    A(vkDestroyCommandPool)(device,pool,nullptr);A(vkDestroyFramebuffer)(device,framebuffer,nullptr);A(vkDestroyRenderPass)(device,pass,nullptr);A(vkDestroyRenderPass)(device,continuationPass,nullptr);
    A(vkDestroyDescriptorPool)(device,descriptors,nullptr);A(vkDestroyPipelineLayout)(device,layout,nullptr);A(vkDestroyDescriptorSetLayout)(device,sourceSet,nullptr);A(vkDestroyDescriptorSetLayout)(device,empty,nullptr);
    for(auto v:{colorView,depthView})A(vkDestroyImageView)(device,v,nullptr);
    for(auto im:{color,depth})A(vkDestroyImage)(device,im,nullptr);
    CHECK(!heldBatch->alive);
    for(auto b:{camera,vertex,index,readback})A(vkDestroyBuffer)(device,b,nullptr);
    for(auto mem:allocations)A(vkFreeMemory)(device,mem,nullptr);
    destroy_device_discovery(device);g_devices.clear();D(vkDestroyDevice)(device,nullptr);
    CHECK(!errors);if(validate)I(vkDestroyDebugUtilsMessengerEXT)(instance,messenger,nullptr);I(vkDestroyInstance)(instance,nullptr);dlclose(loader);
    std::printf("layer counters: submits seen by the sync hook %llu, draws %llu\n",(unsigned long long)g_perf.submits.load(),(unsigned long long)g_perf.draws.load());
    std::puts("PASS: production water mask, SSR, planar, off, invalid-plane fallback, reuse and teardown");
}
