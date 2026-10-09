// Real offscreen application, observed through the production discovery hooks.
// A synthetic present boundary replaces WSI only; no game acceptance is claimed.
#include "surface_inspector.hpp"
#include "inspector_uniform_gpu.hpp"
#include "overlay_internal.hpp"
#include "depth_tracking.hpp"
#include "overlay_uniform.hpp"
#include "overlay_menu.hpp"
#include "shader_resources.hpp"
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace depth_probe;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
static void ok(VkResult r){CHECK(r==VK_SUCCESS);}
static VkDevice device;
static VkInstance instance;
static PFN_vkGetDeviceProcAddr gdpa;
static PFN_vkGetInstanceProcAddr gipa;
static std::atomic<unsigned> validationErrors{0};
#define D(api) reinterpret_cast<PFN_##api>(gdpa(device,#api))
#define I(api) reinterpret_cast<PFN_##api>(gipa(instance,#api))
#define A(api) reinterpret_cast<PFN_##api>(discovery_hook(#api)?discovery_hook(#api):gdpa(device,#api))
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*){++validationErrors;std::fprintf(stderr,"VALIDATION: %s\n",data->pMessage);return VK_FALSE;}
static VKAPI_ATTR VkResult VKAPI_CALL boundary(VkQueue,const VkPresentInfoKHR*){return VK_SUCCESS;}
int main() {
    setenv("DEPTH_PROBE_WATER_SETTINGS","/dev/null/depth-probe-water.ini",1); /* tests never read or write the user's water settings */
    setenv("DEPTH_PROBE_SURFACE_INSPECTOR","1",1);setenv("DEPTH_PROBE_SHADER_DIR",(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/shaders").c_str(),1);setenv("DEPTH_PROBE_DEPTH_PRESENT","0",1);setenv("DEPTH_PROBE_UI_INPUT","0",1);
    char directoryTemplate[]="/tmp/depth-probe-inspector-gpu-XXXXXX";const auto temporary=mkdtemp(directoryTemplate);CHECK(temporary);
    const std::string directory=temporary;setenv("DEPTH_PROBE_DISCOVERY_DIR",directory.c_str(),1);setenv("DEPTH_PROBE_EXPORT_DIR",(directory+"/assets").c_str(),1);
    setenv("DEPTH_PROBE_LAYER_LOG",(directory+".log").c_str(),1);
    void* loader=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);CHECK(loader);gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));CHECK(gipa);
    bool validate=std::getenv("DEPTH_PROBE_SMOKE_VALIDATION")!=nullptr;
    const char* validationLayer="VK_LAYER_KHRONOS_validation";const char* debugExtension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug{};debug.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;debug.pfnUserCallback=validation;
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ii.pApplicationInfo=&app;
    if(validate){ii.pNext=&debug;ii.enabledLayerCount=1;ii.ppEnabledLayerNames=&validationLayer;ii.enabledExtensionCount=1;ii.ppEnabledExtensionNames=&debugExtension;}
    ok(I(vkCreateInstance)(&ii,nullptr,&instance));VkDebugUtilsMessengerEXT messenger{};if(validate)ok(I(vkCreateDebugUtilsMessengerEXT)(instance,&debug,nullptr,&messenger));
    uint32_t count=0;ok(I(vkEnumeratePhysicalDevices)(instance,&count,nullptr));CHECK(count);std::vector<VkPhysicalDevice> physical(count);ok(I(vkEnumeratePhysicalDevices)(instance,&count,physical.data()));auto gpu=physical[0];
    I(vkGetPhysicalDeviceQueueFamilyProperties)(gpu,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);I(vkGetPhysicalDeviceQueueFamilyProperties)(gpu,&count,families.data());
    uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;CHECK(family<count);
    float priority=1;VkDeviceQueueCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    uint32_t extensionCount=0;ok(I(vkEnumerateDeviceExtensionProperties)(gpu,nullptr,&extensionCount,nullptr));std::vector<VkExtensionProperties> extensions(extensionCount);ok(I(vkEnumerateDeviceExtensionProperties)(gpu,nullptr,&extensionCount,extensions.data()));
    auto hasExtension=[&](const char* name){return std::any_of(extensions.begin(),extensions.end(),[&](auto& e){return !std::strcmp(e.extensionName,name);});};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{};gpl.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT identifiers{};identifiers.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT;
    VkPhysicalDevicePipelineCreationCacheControlFeatures cacheControl{};cacheControl.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES;gpl.pNext=&identifiers;identifiers.pNext=&cacheControl;
    VkPhysicalDeviceDepthClipEnableFeaturesEXT depthClip{};depthClip.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT;cacheControl.pNext=&depthClip;
    VkPhysicalDeviceFeatures2 features{};features.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;VkPhysicalDeviceDescriptorIndexingFeatures indexing{};indexing.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;indexing.pNext=&gpl;features.pNext=&indexing;I(vkGetPhysicalDeviceFeatures2)(gpu,&features);
    bool haveGPL=gpl.graphicsPipelineLibrary && hasExtension(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && hasExtension(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    bool haveIdentifier=identifiers.shaderModuleIdentifier && cacheControl.pipelineCreationCacheControl && hasExtension(VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME);
    const bool haveDepthClip=depthClip.depthClipEnable && hasExtension(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    const bool haveSamplerHeap=indexing.runtimeDescriptorArray && indexing.descriptorBindingSampledImageUpdateAfterBind && indexing.descriptorBindingPartiallyBound && indexing.descriptorBindingUpdateUnusedWhilePending && features.features.shaderSampledImageArrayDynamicIndexing;
    VkPhysicalDeviceFeatures enabledFeatures{};enabledFeatures.textureCompressionBC=features.features.textureCompressionBC;
    VkPhysicalDeviceDescriptorIndexingFeatures enabledIndexing{};enabledIndexing.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    std::vector<const char*> enabledExtensions;void* featureChain=nullptr;
    if(haveSamplerHeap){enabledFeatures.shaderSampledImageArrayDynamicIndexing=VK_TRUE;enabledIndexing.runtimeDescriptorArray=enabledIndexing.descriptorBindingSampledImageUpdateAfterBind=enabledIndexing.descriptorBindingPartiallyBound=enabledIndexing.descriptorBindingUpdateUnusedWhilePending=VK_TRUE;featureChain=&enabledIndexing;}
    if(haveDepthClip){enabledExtensions.push_back(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);depthClip.pNext=featureChain;featureChain=&depthClip;}
    if(haveIdentifier){enabledExtensions.push_back(VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME);identifiers.pNext=&cacheControl;cacheControl.pNext=featureChain;featureChain=&identifiers;}
    if(haveGPL){enabledExtensions.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);enabledExtensions.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);gpl.pNext=featureChain;featureChain=&gpl;}
    VkDeviceCreateInfo di{};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;di.pNext=featureChain;di.pEnabledFeatures=&enabledFeatures;di.enabledExtensionCount=enabledExtensions.size();di.ppEnabledExtensionNames=enabledExtensions.data();di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;ok(I(vkCreateDevice)(gpu,&di,nullptr,&device));
    gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance,"vkGetDeviceProcAddr"));VkQueue queue;D(vkGetDeviceQueue)(device,family,0,&queue);
    DeviceData data;data.device=device;data.physical_device=gpu;data.next_gdpa=gdpa;data.next_queue_present=boundary;
    data.next_create_image=D(vkCreateImage);data.next_destroy_image=D(vkDestroyImage);data.next_create_image_view=D(vkCreateImageView);data.next_destroy_image_view=D(vkDestroyImageView);
    data.next_cmd_pipeline_barrier=D(vkCmdPipelineBarrier);data.next_cmd_pipeline_barrier2=D(vkCmdPipelineBarrier2);
    data.get_format_properties=I(vkGetPhysicalDeviceFormatProperties);data.get_image_format_properties=I(vkGetPhysicalDeviceImageFormatProperties);
    data.get_image_format_properties2=I(vkGetPhysicalDeviceImageFormatProperties2);data.get_memory_properties=I(vkGetPhysicalDeviceMemoryProperties);
    data.get_queue_properties=I(vkGetPhysicalDeviceQueueFamilyProperties);
    g_devices[dispatch_key(device)]=data;
    VkPhysicalDeviceMemoryProperties memory;I(vkGetPhysicalDeviceMemoryProperties)(gpu,&memory);
    auto allocate=[&](const VkMemoryRequirements& requirements,VkMemoryPropertyFlags flags) {
        uint32_t type=0;while(type<memory.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) || (memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;CHECK(type<memory.memoryTypeCount);
        VkMemoryAllocateInfo info{};info.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;info.allocationSize=requirements.size;info.memoryTypeIndex=type;VkDeviceMemory result;ok(D(vkAllocateMemory)(device,&info,nullptr,&result));return result;
    };
    VkImageCreateInfo imageInfo{};imageInfo.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=VK_FORMAT_R8G8B8A8_UNORM;imageInfo.extent={64,64,1};imageInfo.mipLevels=imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;imageInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    auto textureImageInfo=imageInfo;textureImageInfo.mipLevels=2;textureImageInfo.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;textureImageInfo.usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image;ok(A(vkCreateImage)(device,&textureImageInfo,nullptr,&image));VkMemoryRequirements requirements;D(vkGetImageMemoryRequirements)(device,image,&requirements);auto imageMemory=allocate(requirements,0);ok(D(vkBindImageMemory)(device,image,imageMemory,0));
    VkImageViewCreateInfo viewInfo{};viewInfo.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;viewInfo.image=image;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;viewInfo.format=imageInfo.format;viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view;ok(A(vkCreateImageView)(device,&viewInfo,nullptr,&view));
    auto depthInfo=imageInfo;depthInfo.format=VK_FORMAT_D32_SFLOAT;depthInfo.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage depthImage;ok(A(vkCreateImage)(device,&depthInfo,nullptr,&depthImage));D(vkGetImageMemoryRequirements)(device,depthImage,&requirements);auto depthMemory=allocate(requirements,0);ok(D(vkBindImageMemory)(device,depthImage,depthMemory,0));
    auto depthViewInfo=viewInfo;depthViewInfo.image=depthImage;depthViewInfo.format=depthInfo.format;depthViewInfo.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;VkImageView depthView;ok(A(vkCreateImageView)(device,&depthViewInfo,nullptr,&depthView));
    VkBufferCreateInfo bufferInfo{};bufferInfo.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bufferInfo.size=768*720*4;bufferInfo.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;VkBuffer buffer;ok(D(vkCreateBuffer)(device,&bufferInfo,nullptr,&buffer));
    D(vkGetBufferMemoryRequirements)(device,buffer,&requirements);auto hostMemory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);ok(D(vkBindBufferMemory)(device,buffer,hostMemory,0));void* mapped;ok(D(vkMapMemory)(device,hostMemory,0,VK_WHOLE_SIZE,0,&mapped));
    VkAttachmentDescription attachment{};attachment.format=imageInfo.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference colorRef{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&colorRef;
    VkAttachmentDescription attachments[2]{attachment,attachment};attachments[1].format=VK_FORMAT_D32_SFLOAT;attachments[1].finalLayout=VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference depthRef{1,VK_IMAGE_LAYOUT_GENERAL};sub.pDepthStencilAttachment=&depthRef;
    VkSubpassDependency dependency{};dependency.srcSubpass=0;dependency.dstSubpass=VK_SUBPASS_EXTERNAL;dependency.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependency.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dependency.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo passInfo{};passInfo.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;passInfo.attachmentCount=2;passInfo.pAttachments=attachments;passInfo.subpassCount=1;passInfo.pSubpasses=&sub;passInfo.dependencyCount=1;passInfo.pDependencies=&dependency;VkRenderPass pass;ok(A(vkCreateRenderPass)(device,&passInfo,nullptr,&pass));
    VkImageView views[]{view,depthView};VkFramebufferCreateInfo fbInfo{};fbInfo.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fbInfo.renderPass=pass;fbInfo.attachmentCount=2;fbInfo.pAttachments=views;fbInfo.width=fbInfo.height=64;fbInfo.layers=1;VkFramebuffer framebuffer;ok(A(vkCreateFramebuffer)(device,&fbInfo,nullptr,&framebuffer));
    VkSamplerCreateInfo samplerInfo{};samplerInfo.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;samplerInfo.magFilter=samplerInfo.minFilter=VK_FILTER_NEAREST;samplerInfo.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;samplerInfo.addressModeU=samplerInfo.addressModeV=samplerInfo.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler;ok(A(vkCreateSampler)(device,&samplerInfo,nullptr,&sampler));
    // Real mapped application uniform allocation, observed by the new hooks.
    auto uniformInfo=bufferInfo;uniformInfo.size=1024;uniformInfo.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VkBuffer applicationUniform;ok(A(vkCreateBuffer)(device,&uniformInfo,nullptr,&applicationUniform));
    D(vkGetBufferMemoryRequirements)(device,applicationUniform,&requirements);
    uint32_t uniformType=0;while(uniformType<memory.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<uniformType)) || (memory.memoryTypes[uniformType].propertyFlags&6)!=6))++uniformType;CHECK(uniformType<memory.memoryTypeCount);
    VkMemoryAllocateInfo uniformAllocation{};uniformAllocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;uniformAllocation.allocationSize=requirements.size;uniformAllocation.memoryTypeIndex=uniformType;
    VkDeviceMemory applicationMemory;ok(A(vkAllocateMemory)(device,&uniformAllocation,nullptr,&applicationMemory));
    VkBindBufferMemoryInfo applicationBinding{};applicationBinding.sType=VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO;applicationBinding.buffer=applicationUniform;applicationBinding.memory=applicationMemory;ok(A(vkBindBufferMemory2)(device,1,&applicationBinding));
    void* uniformMap;ok(A(vkMapMemory)(device,applicationMemory,0,VK_WHOLE_SIZE,0,&uniformMap));
    for(unsigned i=0;i<1024;++i)static_cast<uint8_t*>(uniformMap)[i]=uint8_t(i);
    VkDescriptorSetLayoutBinding textureBindings[]{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo setInfo{};setInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;setInfo.bindingCount=2;setInfo.pBindings=textureBindings;VkDescriptorSetLayout textureLayout;ok(A(vkCreateDescriptorSetLayout)(device,&setInfo,nullptr,&textureLayout));
    VkDescriptorPoolSize textureSizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}};VkDescriptorPoolCreateInfo texturePoolInfo{};texturePoolInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;texturePoolInfo.maxSets=1;texturePoolInfo.poolSizeCount=2;texturePoolInfo.pPoolSizes=textureSizes;VkDescriptorPool texturePool;ok(A(vkCreateDescriptorPool)(device,&texturePoolInfo,nullptr,&texturePool));
    VkDescriptorSetAllocateInfo textureAlloc{};textureAlloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;textureAlloc.descriptorPool=texturePool;textureAlloc.descriptorSetCount=1;textureAlloc.pSetLayouts=&textureLayout;VkDescriptorSet textureSet;ok(A(vkAllocateDescriptorSets)(device,&textureAlloc,&textureSet));
    VkDescriptorImageInfo textureInfo{sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet textureWrite{};textureWrite.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;textureWrite.dstSet=textureSet;textureWrite.dstBinding=0;textureWrite.descriptorCount=1;textureWrite.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;textureWrite.pImageInfo=&textureInfo;A(vkUpdateDescriptorSets)(device,1,&textureWrite,0,nullptr);
    VkDescriptorBufferInfo applicationDescriptor{applicationUniform,256,128};auto uniformWrite=textureWrite;uniformWrite.dstBinding=1;uniformWrite.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;uniformWrite.pImageInfo=nullptr;uniformWrite.pBufferInfo=&applicationDescriptor;A(vkUpdateDescriptorSets)(device,1,&uniformWrite,0,nullptr);
    VkPipelineLayoutCreateInfo layoutInfo{};layoutInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,4};layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&textureLayout;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;VkPipelineLayout layout;ok(A(vkCreatePipelineLayout)(device,&layoutInfo,nullptr,&layout));
    auto shader=[&](const char* file) {
        auto code=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/"+file+".spv");CHECK(!code.empty());VkShaderModuleCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;info.codeSize=code.size()*4;info.pCode=code.data();VkShaderModule module;ok(A(vkCreateShaderModule)(device,&info,nullptr,&module));return module;
    };
    auto vs=shader("inspector_scene.vert"),uniformVS=shader("inspector_uniform.vert"),redFS=shader("discovery_red.frag"),greenFS=shader("discovery_green.frag"),discardFS=shader("inspector_discard.frag");
    VkPipelineCacheCreateInfo cacheInfo{};cacheInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;VkPipelineCache pipelineCache;ok(D(vkCreatePipelineCache)(device,&cacheInfo,nullptr,&pipelineCache));
    std::vector<VkPipeline> appLibraries;
    auto pipeline=[&](VkShaderModule fs,unsigned source=0,VkPipelineLayout selectedLayout=VK_NULL_HANDLE,bool useDepth=true) {
        VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=selectedLayout?vs:uniformVS;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
        VkPipelineVertexInputStateCreateInfo vertex{};vertex.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;viewport.viewportCount=viewport.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.lineWidth=1;
        VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{};clip.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;clip.depthClipEnable=VK_TRUE;if(haveDepthClip)raster.pNext=&clip;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ca{};ca.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo blend{};blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;blend.attachmentCount=1;blend.pAttachments=&ca;
        VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;ds.dynamicStateCount=2;ds.pDynamicStates=dynamic;
        VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;info.stageCount=2;info.pStages=stages;info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&ms;info.pColorBlendState=&blend;info.pDynamicState=&ds;info.layout=selectedLayout?selectedLayout:layout;info.renderPass=pass;
        VkPipelineDepthStencilStateCreateInfo depth{};depth.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;depth.depthCompareOp=VK_COMPARE_OP_LESS;info.pDepthStencilState=&depth;
        if(!useDepth){depth.depthTestEnable=depth.depthWriteEnable=VK_FALSE;depth.depthCompareOp=VK_COMPARE_OP_ALWAYS;}
        VkShaderModuleCreateInfo inlineCode[2]{};VkShaderModuleIdentifierEXT moduleIds[2]{};VkPipelineShaderStageModuleIdentifierCreateInfoEXT ids[2]{};
        if(source==2 || source==3)for(unsigned i=0;i<2;++i) {
            auto snapshot=discovery::devices.at(device).shaders.at(discovery::id(stages[i].module));
            if(source==2){inlineCode[i].sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;inlineCode[i].codeSize=snapshot->words.size()*4;inlineCode[i].pCode=snapshot->words.data();stages[i].pNext=&inlineCode[i];}
            else {moduleIds[i].sType=VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT;A(vkGetShaderModuleIdentifierEXT)(device,stages[i].module,&moduleIds[i]);ids[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT;ids[i].identifierSize=moduleIds[i].identifierSize;ids[i].pIdentifier=moduleIds[i].identifier;stages[i].pNext=&ids[i];info.flags|=VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;}
            stages[i].module=VK_NULL_HANDLE;
        }
        VkGraphicsPipelineLibraryCreateInfoEXT library{};library.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;library.flags=3;
        if(source==1){info.flags=VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;info.pNext=&library;info.stageCount=1;info.pDepthStencilState=nullptr;info.pColorBlendState=nullptr;info.pMultisampleState=nullptr;}
        VkPipeline p{};auto result=A(vkCreateGraphicsPipelines)(device,pipelineCache,1,&info,nullptr,&p);
        if(source==3 && result==VK_PIPELINE_COMPILE_REQUIRED){std::puts("SKIP: identifier cache miss (bytes remain resolved, original fallback required)");return VkPipeline{};}ok(result);
        if(source==1) {
            appLibraries.push_back(p);
            library.flags=12;info.stageCount=1;info.pStages=&stages[1];info.pDepthStencilState=&depth;info.pColorBlendState=&blend;info.pMultisampleState=&ms;
            info.pVertexInputState=nullptr;info.pInputAssemblyState=nullptr;info.pViewportState=nullptr;info.pRasterizationState=nullptr;info.pDynamicState=nullptr;
            VkPipeline fragmentLibrary{};ok(A(vkCreateGraphicsPipelines)(device,pipelineCache,1,&info,nullptr,&fragmentLibrary));appLibraries.push_back(fragmentLibrary);
            VkPipeline libraries[]{p,fragmentLibrary};VkPipelineLibraryCreateInfoKHR link{};link.sType=VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;link.libraryCount=2;link.pLibraries=libraries;
            VkGraphicsPipelineCreateInfo linked{};linked.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;linked.pNext=&link;linked.layout=layout;linked.renderPass=pass;VkPipeline final{};
            ok(A(vkCreateGraphicsPipelines)(device,pipelineCache,1,&linked,nullptr,&final));p=final;
        }
        auto tracked=discovery::devices.at(device).pipelines.at(discovery::id(p));CHECK(tracked->inspector && tracked->inspector->query);CHECK(tracked->queryStatus=="ready");
        return p;
    };
    auto red=pipeline(redFS),green=pipeline(greenFS),discard=pipeline(discardFS),composite=pipeline(greenFS,0,VK_NULL_HANDLE,false);
    VkPipeline linked{},inlined{},identified{};if(haveGPL){linked=pipeline(greenFS,1);inlined=pipeline(greenFS,2);}if(haveIdentifier)identified=pipeline(greenFS,3);
    VkImage heapImage{};VkImageView heapView{};VkDeviceMemory heapMemory{};VkDescriptorSetLayout heapLayouts[2]{};VkDescriptorSet heapSets[2]{};
    VkDescriptorPool heapPool{};VkPipelineLayout heapLayout{};VkPipeline heapPipeline{},helperPipeline{};
    if(haveSamplerHeap) {
        auto imageCreate=imageInfo;imageCreate.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ok(A(vkCreateImage)(device,&imageCreate,nullptr,&heapImage));D(vkGetImageMemoryRequirements)(device,heapImage,&requirements);heapMemory=allocate(requirements,0);ok(D(vkBindImageMemory)(device,heapImage,heapMemory,0));
        auto vi=viewInfo;vi.image=heapImage;ok(A(vkCreateImageView)(device,&vi,nullptr,&heapView));
        VkDescriptorBindingFlags bindingFlags=VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT|VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT|VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags{};flags.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;flags.bindingCount=1;flags.pBindingFlags=&bindingFlags;
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_SAMPLER,4096,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;si.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;si.pNext=&flags;si.bindingCount=1;si.pBindings=&binding;ok(A(vkCreateDescriptorSetLayout)(device,&si,nullptr,&heapLayouts[0]));
        binding.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;binding.descriptorCount=1;si.flags=0;si.pNext=nullptr;ok(A(vkCreateDescriptorSetLayout)(device,&si,nullptr,&heapLayouts[1]));
        VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_SAMPLER,4096},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1}};VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.flags=VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;pool.maxSets=2;pool.poolSizeCount=2;pool.pPoolSizes=sizes;ok(A(vkCreateDescriptorPool)(device,&pool,nullptr,&heapPool));
        VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=heapPool;ai.descriptorSetCount=2;ai.pSetLayouts=heapLayouts;ok(A(vkAllocateDescriptorSets)(device,&ai,heapSets));
        VkDescriptorImageInfo samplers[]{{sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED},{sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED}},texture{VK_NULL_HANDLE,heapView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[2]{};for(auto& w:writes)w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet=heapSets[0];writes[0].dstArrayElement=900;writes[0].descriptorCount=2;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;writes[0].pImageInfo=samplers;
        writes[1].dstSet=heapSets[1];writes[1].descriptorCount=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[1].pImageInfo=&texture;A(vkUpdateDescriptorSets)(device,2,writes,0,nullptr);
        VkPushConstantRange ranges[]{{VK_SHADER_STAGE_VERTEX_BIT,0,4},{VK_SHADER_STAGE_FRAGMENT_BIT,4,4}};auto li=layoutInfo;li.setLayoutCount=2;li.pSetLayouts=heapLayouts;li.pushConstantRangeCount=2;li.pPushConstantRanges=ranges;ok(A(vkCreatePipelineLayout)(device,&li,nullptr,&heapLayout));
        auto fs=shader("inspector_sampler.frag"),helper=shader("inspector_sampler_helper.frag");heapPipeline=pipeline(fs,0,heapLayout);helperPipeline=pipeline(helper,0,heapLayout);A(vkDestroyShaderModule)(device,fs,nullptr);A(vkDestroyShaderModule)(device,helper,nullptr);
    }
    for(auto module:{vs,uniformVS,redFS,greenFS,discardFS})A(vkDestroyShaderModule)(device,module,nullptr);
    VkCommandPoolCreateInfo poolInfo{};poolInfo.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;poolInfo.queueFamilyIndex=family;poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool pool;ok(A(vkCreateCommandPool)(device,&poolInfo,nullptr,&pool));
    VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;VkCommandBuffer cmd;ok(A(vkAllocateCommandBuffers)(device,&ai,&cmd));
    VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;VkFence fence;ok(D(vkCreateFence)(device,&fi,nullptr,&fence));
    VkSwapchainKHR fakeSwapchain=(VkSwapchainKHR)uint64_t(0x123456);DepthOverlayState context;context.swapchain=fakeSwapchain;context.depthImage=depthImage;context.depthView=depthView;context.depthGeneration=1;context.width=context.height=64;inspector_context(device,context);
    VkBuffer gpuUniform{};VkDeviceMemory gpuUniformMemory{};bool gpuUniformMode=false;
    auto render=[&](bool oneTime=true,float frontDepth=.25f,bool reverse=false,bool discardTest=false,bool clearAfter=false,VkPipeline overrideFront=VK_NULL_HANDLE,bool compose=false) {
        VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=oneTime?VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT:0;ok(A(vkBeginCommandBuffer)(cmd,&bi));
        if(gpuUniformMode) {
            std::array<uint32_t,32> constants{};for(size_t i=0;i<constants.size();++i)constants[i]=uint32_t(i)*256;
            D(vkCmdUpdateBuffer)(cmd,gpuUniform,256,sizeof(constants),constants.data());
            VkMemoryBarrier available{};available.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;available.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;available.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT;
            D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,0,1,&available,0,nullptr,0,nullptr);
        }
        if(heapImage) {
            VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=heapImage;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);VkClearColorValue white{{1,1,1,1}};D(vkCmdClearColorImage)(cmd,heapImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&white,1,&b.subresourceRange);
            b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;A(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        }
        VkClearValue clears[2]{};clears[0].color.float32[3]=1;clears[1].depthStencil.depth=1;
        VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;begin.renderPass=pass;begin.framebuffer=framebuffer;begin.renderArea.extent={64,64};begin.clearValueCount=2;begin.pClearValues=clears;A(vkCmdBeginRenderPass)(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0,0,64,64,0,1};VkRect2D scissor{{0,0},{64,64}};A(vkCmdSetViewport)(cmd,0,1,&viewport);A(vkCmdSetScissor)(cmd,0,1,&scissor);
        A(vkCmdBindDescriptorSets)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&textureSet,0,nullptr);
        auto redDraw=[&] {float z=.5f;A(vkCmdBindDescriptorSets)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&textureSet,0,nullptr);A(vkCmdPushConstants)(cmd,layout,VK_SHADER_STAGE_VERTEX_BIT,0,4,&z);VkRect2D rect{{0,0},{64,64}};A(vkCmdSetScissor)(cmd,0,1,&rect);A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,red);A(vkCmdDraw)(cmd,3,1,0,0);};
        auto greenDraw=[&] {const bool heap=overrideFront && (overrideFront==heapPipeline || overrideFront==helperPipeline);auto currentLayout=heap?heapLayout:layout;
            A(vkCmdBindDescriptorSets)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,currentLayout,0,heap?2:1,heap?heapSets:&textureSet,0,nullptr);
            A(vkCmdPushConstants)(cmd,currentLayout,VK_SHADER_STAGE_VERTEX_BIT,0,4,&frontDepth);
            if(heap){uint32_t packed=(901u<<16)|900;A(vkCmdPushConstants)(cmd,currentLayout,VK_SHADER_STAGE_FRAGMENT_BIT,4,4,&packed);}VkRect2D rect{{32,0},{32,64}};A(vkCmdSetScissor)(cmd,0,1,&rect);A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,overrideFront?overrideFront:discardTest?discard:green);A(vkCmdDraw)(cmd,3,1,0,0);};
        if(reverse){greenDraw();redDraw();}else {redDraw();greenDraw();}
        if(compose) {
            // Fullscreen depth-disabled final pass with the SAME depth attached
            // and scene render-target descriptor, matching the real WoW state.
            float z=.1f;A(vkCmdPushConstants)(cmd,layout,VK_SHADER_STAGE_VERTEX_BIT,0,4,&z);
            A(vkCmdBindDescriptorSets)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&textureSet,0,nullptr);
            VkRect2D rect{{0,0},{64,64}};A(vkCmdSetScissor)(cmd,0,1,&rect);
            A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,composite);A(vkCmdDraw)(cmd,3,1,0,0);
        }
        if(clearAfter){VkClearAttachment clears[2]{};clears[0].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;clears[1].aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;clears[1].clearValue.depthStencil.depth=1;VkClearRect rect{{{0,0},{64,64}},0,1};A(vkCmdClearAttachments)(cmd,2,clears,1,&rect);}
        A(vkCmdEndRenderPass)(cmd);
        if(gpuUniformMode)D(vkCmdFillBuffer)(cmd,gpuUniform,256,128,0xeeeeeeee);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={64,64,1};D(vkCmdCopyImageToBuffer)(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
        VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=buffer;host.size=VK_WHOLE_SIZE;
        D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);ok(A(vkEndCommandBuffer)(cmd));
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;ok(A(vkQueueSubmit)(queue,1,&submit,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));ok(D(vkResetFences)(device,1,&fence));
        VkPresentInfoKHR present{};present.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;present.swapchainCount=1;present.pSwapchains=&fakeSwapchain;uint32_t imageIndex=0;present.pImageIndices=&imageIndex;ok(A(vkQueuePresentKHR)(queue,&present));
        const auto* bytes=static_cast<unsigned char*>(mapped);return std::vector<unsigned char>(bytes,bytes+64*64*4);
    };
    auto state=[&]() -> discovery::Inspector& {return *discovery::devices.at(device).inspector;};
    CHECK(state().geometryMode);state().geometryMode=false; // existing strict visibility regressions
    auto selected=[&](VkPipeline expected) {auto& in=state();discovery::inspector_poll(discovery::devices.at(device),in);CHECK(in.selected);CHECK(in.selected->draw.pipeline->handle==discovery::id(expected));};
    auto original=render();inspector_request_pick(device,48,32);auto picked=render();selected(green);CHECK(original==picked);
    inspector_request_pick(device,16,32);CHECK(render()==original);selected(red);
    inspector_request_pick(device,48,32);auto reverse=render(true,.25f,true);selected(green);CHECK(reverse==original);
    inspector_request_pick(device,48,32);auto coplanar=render(true,.5f);selected(red);CHECK(coplanar[(32*64+48)*4]==255);
    if(haveDepthClip){inspector_request_pick(device,48,32);auto clipped=render(true,-.25f);selected(red);CHECK(clipped[(32*64+48)*4]==255);}
    inspector_request_pick(device,40,32);render(true,.25f,false,true);selected(red);
    inspector_request_pick(device,56,32);auto discardPixels=render(true,.25f,false,true);selected(discard);
    CHECK(state().highlight);auto highlighted=render(true,.25f,false,true);
    CHECK(highlighted[(32*64+56)*4+2]>64 && highlighted[(32*64+56)*4+2]<100 && discardPixels[(32*64+56)*4+2]==255);
    CHECK(highlighted[(32*64+16)*4]==255);inspector_set_highlight(device,false);CHECK(render(true,.25f,false,true)==discardPixels);
    inspector_request_pick(device,48,32);render(false);discovery::inspector_poll(discovery::devices.at(device),state());CHECK(!state().selected);
    inspector_request_pick(device,48,32);render(true,.25f,false,false,true);discovery::inspector_poll(discovery::devices.at(device),state());CHECK(!state().selected);
    for(auto p:{linked,inlined,identified})if(p) {
        inspector_request_pick(device,48,32);CHECK(render(true,.25f,false,false,false,p)==original);selected(p);
        auto record=discovery::devices.at(device).pipelines.at(discovery::id(p));CHECK(record->vs && record->fs && record->vs->hash && record->fs->hash);
    }
    if(heapPipeline) {
        inspector_request_pick(device,48,32);auto heapPixels=render(true,.25f,false,false,false,heapPipeline);selected(heapPipeline);
        CHECK(state().selected->resources.size()==3 && state().materialIdentity && state().highlight);
        CHECK(!state().selected->descriptorsIncomplete);
        auto tint=render(true,.25f,false,false,false,heapPipeline);const size_t pixel=(32*64+48)*4;
        CHECK(heapPixels[pixel+2]==255 && tint[pixel+2]>64 && tint[pixel+2]<100 && tint[(32*64+16)*4]==255);
        inspector_set_highlight(device,false);CHECK(render(true,.25f,false,false,false,heapPipeline)==heapPixels);
        inspector_request_pick(device,48,32);CHECK(render(true,.25f,false,false,false,helperPipeline)==heapPixels);selected(helperPipeline);
        CHECK(state().selected->descriptorIssues==discovery::SamplerIndexUnknown && !state().materialIdentity && !state().highlight);
        inspector_request_pick(device,48,32);render(true,.75f,false,false,false,helperPipeline);selected(red);
        std::puts("PASS: 4096 UAB sampler array, sparse indices 900/901, original pixels, pick, per-draw tint, helper-local writes and unknown-index occlusion");
    } else std::puts("SKIP: sampler-array GPU features unsupported by ICD");
    auto geometryPipeline=heapPipeline?heapPipeline:green;
    inspector_request_pick(device,48,32);auto composed=render(true,.25f,false,false,false,geometryPipeline,true);selected(composite);
    state().geometryMode=true;inspector_request_pick(device,48,32);
    CHECK(render(true,.25f,false,false,false,geometryPipeline,true)==composed);selected(geometryPipeline);
    CHECK(state().geometryCandidate && !state().finalVisible && state().geometryCandidates.size()==2);
    const auto pickedGeometry=state().selected;
    if(heapPipeline) {
        auto sampled=std::find_if(state().selected->resources.begin(),state().selected->resources.end(),[](const auto& r){return bool(r.descriptor.view);});
        CHECK(sampled!=state().selected->resources.end() && sampled->descriptor.view->image->handle==discovery::id(heapImage));
        CHECK(!(sampled->descriptor.view->image->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
    }
    CHECK(discovery::select_geometry_candidate(state(),1) && state().selected->draw.pipeline->handle==discovery::id(red));
    inspector_request_pick(device,48,32);render(true,.25f,false,false,false,geometryPipeline,true);selected(geometryPipeline);
    std::puts("PASS: fullscreen depth-disabled pass selected only in last-scene mode; geometry mode retains exact GPU hits and original geometry texture binding");
    std::printf("depth-clip=%s\n",haveDepthClip?"tested incl. clipped triangle":"unsupported by ICD");
    std::printf("GPL/inline=%s identifier=%s\n",haveGPL?"tested":"unsupported by ICD",identified?"tested":"cache miss or unsupported by ICD");
    // Edge-trigger and exact coordinate round-trip through the existing scale.
    auto& menu=overlay_menu();menu.visible=true;state().page=1;MenuInput input;input.pointerValid=true;input.leftDown=true;float scale=menu_scale(64,64);input.x=17.75f/scale-16;input.y=29.25f/scale-16;
    menu.input(input);auto ticket=state().ticket;CHECK(state().x==17 && state().y==29);menu.input(input);CHECK(state().ticket==ticket);
    input.leftDown=false;menu.input(input);state().page=1;input.leftDown=true;menu.input(input);CHECK(state().ticket==ticket+1);
    input.key=MenuKey::Escape;menu.input(input);CHECK(!menu.visible && !state().armed);
    // Texture identity uses allocation generation and view range, not view handle.
    auto metadata=discovery::devices.at(device).views.at(discovery::id(view));auto same=*metadata;same.handle+=1;same.generation+=1;CHECK(discovery::texture_identity(*metadata)==discovery::texture_identity(same));
    same.range.baseMipLevel=1;CHECK(discovery::texture_identity(*metadata)!=discovery::texture_identity(same));
    const auto output=directory+".ppm";FILE* file=std::fopen(output.c_str(),"wb");CHECK(file);std::fprintf(file,"P6\n128 64\n255\n");
    for(unsigned y=0;y<64;++y)for(const auto* pixels:{&discardPixels,&highlighted})for(unsigned x=0;x<64;++x)CHECK(std::fwrite(pixels->data()+(y*64+x)*4,1,3,file)==3);
    std::fclose(file);
    // Exercise production 1x1 depth readback and borrowed texture preview. The
    // fixture submits offscreen commands explicitly; production uses its existing
    // overlay submission and per-image fences instead.
    inspector_request_pick(device,48,32);render();selected(green);CHECK(state().selected->resources.size()==2);
    CHECK(state().selected->uniforms.size()==1);auto constantsSnapshot=state().selected->uniforms[0];
    CHECK(constantsSnapshot->reason.empty() && constantsSnapshot->bytes.size()==128 && constantsSnapshot->bytes[127]==127 && constantsSnapshot->offset==256);
    std::memset(uniformMap,255,1024);CHECK(constantsSnapshot->bytes[0]==0);
    discovery::request_asset_export(state(),false,true);discovery::flush_asset_exports();
    CHECK(std::filesystem::exists(std::filesystem::path(state().assetExport->directory)/"uniform-s0-b1-e0.bin"));
    A(vkUnmapMemory)(device,applicationMemory);
    std::puts("PASS: real host-coherent uniform binding, pick submit snapshot, immutable bytes after ring overwrite, binary export and unmap");
    // GPU-written, never mapped source. The original usage lacks TRANSFER_SRC;
    // creation must add it without modifying the application create-info.
    uniformInfo.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ok(A(vkCreateBuffer)(device,&uniformInfo,nullptr,&gpuUniform));CHECK(!(uniformInfo.usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    CHECK(discovery::devices.at(device).buffers.at(discovery::id(gpuUniform))->transferPatched);
    D(vkGetBufferMemoryRequirements)(device,gpuUniform,&requirements);
    uint32_t gpuType=0;while(gpuType<memory.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<gpuType)) || !(memory.memoryTypes[gpuType].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))++gpuType;CHECK(gpuType<memory.memoryTypeCount);
    uniformAllocation.allocationSize=requirements.size;uniformAllocation.memoryTypeIndex=gpuType;ok(A(vkAllocateMemory)(device,&uniformAllocation,nullptr,&gpuUniformMemory));
    ok(A(vkBindBufferMemory)(device,gpuUniform,gpuUniformMemory,0));
    applicationDescriptor.buffer=gpuUniform;A(vkUpdateDescriptorSets)(device,1,&uniformWrite,0,nullptr);gpuUniformMode=true;
    inspector_request_pick(device,48,32);CHECK(render()==original);selected(green);
    CHECK(state().selected->uniforms.size()==1);auto gpuConstants=state().selected->uniforms[0];
    CHECK(gpuConstants->reason.empty() && gpuConstants->bytes.size()==128 && gpuConstants->bytes[0]==0 && gpuConstants->bytes[5]==1 && gpuConstants->bytes[125]==31);
    CHECK(gpuConstants->captureMoment.find("gpu-end-of-render-pass")==0);
    discovery::request_asset_export(state(),false,true);discovery::flush_asset_exports();CHECK(std::filesystem::exists(std::filesystem::path(state().assetExport->directory)/"uniform-s0-b1-e0.bin"));
    // Retire the source before the CPU polls the next marker; destruction waits
    // only for this private copy, and immutable export bytes survive destruction.
    inspector_request_pick(device,48,32);render();A(vkDestroyBuffer)(device,gpuUniform,nullptr);A(vkFreeMemory)(device,gpuUniformMemory,nullptr);selected(green);
    CHECK(state().selected->uniforms[0]->bytes==gpuConstants->bytes);gpuUniformMode=false;gpuUniform=VK_NULL_HANDLE;
    applicationDescriptor.buffer=applicationUniform;A(vkUpdateDescriptorSets)(device,1,&uniformWrite,0,nullptr);
    std::puts("PASS: GPU-written unmapped uniform, transfer-source create patch, render-pass-end capture before overwrite, marker completion, source retirement and binary export");
    auto& st=g_depth_overlay_states[{device,discovery::id(fakeSwapchain)}];st.width=768;st.height=720;st.depthImage=depthImage;st.depthView=depthView;st.depthGeneration=1;st.swapchain=fakeSwapchain;st.format=VK_FORMAT_R8G8B8A8_UNORM;
    auto outputImageInfo=imageInfo;outputImageInfo.extent={768,720,1};VkImage outputImage;ok(D(vkCreateImage)(device,&outputImageInfo,nullptr,&outputImage));D(vkGetImageMemoryRequirements)(device,outputImage,&requirements);auto outputMemory=allocate(requirements,0);ok(D(vkBindImageMemory)(device,outputImage,outputMemory,0));
    auto outputViewInfo=viewInfo;outputViewInfo.image=outputImage;VkImageView outputView;ok(D(vkCreateImageView)(device,&outputViewInfo,nullptr,&outputView));
    st.colorImages={outputImage};st.commandRecorded={false};st.sync.resize(1);st.sampler=sampler;
    VkAttachmentDescription outputAttachment=attachment;outputAttachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkSubpassDescription outputSub{};outputSub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;outputSub.colorAttachmentCount=1;outputSub.pColorAttachments=&colorRef;
    auto outputPassInfo=passInfo;outputPassInfo.attachmentCount=1;outputPassInfo.pAttachments=&outputAttachment;outputPassInfo.pSubpasses=&outputSub;
    ok(D(vkCreateRenderPass)(device,&outputPassInfo,nullptr,&st.renderPass));
    auto outputFBInfo=fbInfo;outputFBInfo.renderPass=st.renderPass;outputFBInfo.attachmentCount=1;outputFBInfo.pAttachments=&outputView;outputFBInfo.width=768;outputFBInfo.height=720;VkFramebuffer outputFB;ok(D(vkCreateFramebuffer)(device,&outputFBInfo,nullptr,&outputFB));
    VkDescriptorSetLayoutBinding overlayBindings[3]{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    auto overlaySetInfo=setInfo;overlaySetInfo.bindingCount=3;overlaySetInfo.pBindings=overlayBindings;ok(D(vkCreateDescriptorSetLayout)(device,&overlaySetInfo,nullptr,&st.descriptorSetLayout));
    VkPipelineLayoutCreateInfo overlayLayoutInfo{};overlayLayoutInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;overlayLayoutInfo.setLayoutCount=1;overlayLayoutInfo.pSetLayouts=&st.descriptorSetLayout;ok(D(vkCreatePipelineLayout)(device,&overlayLayoutInfo,nullptr,&st.pipelineLayout));
    auto f=load_depth_overlay_fns(data);CHECK(create_overlay_uniforms(data,f,st));
    VkDescriptorPoolSize overlaySizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}};auto overlayPoolInfo=texturePoolInfo;overlayPoolInfo.poolSizeCount=2;overlayPoolInfo.pPoolSizes=overlaySizes;ok(D(vkCreateDescriptorPool)(device,&overlayPoolInfo,nullptr,&st.descriptorPool));
    auto overlayAlloc=textureAlloc;overlayAlloc.descriptorPool=st.descriptorPool;overlayAlloc.pSetLayouts=&st.descriptorSetLayout;st.descriptorSets.resize(1);ok(D(vkAllocateDescriptorSets)(device,&overlayAlloc,st.descriptorSets.data()));
    VkDescriptorImageInfo depthDescriptor{sampler,depthView,VK_IMAGE_LAYOUT_GENERAL};VkDescriptorBufferInfo uniformDescriptor{st.uniforms[0].buffer,0,sizeof(OverlayUniform)};
    VkWriteDescriptorSet writes[3]{};for(auto& w:writes){w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=st.descriptorSets[0];w.descriptorCount=1;}
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[0].pImageInfo=&depthDescriptor;writes[1].dstBinding=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;writes[1].pBufferInfo=&uniformDescriptor;writes[2]=writes[0];writes[2].dstBinding=2;D(vkUpdateDescriptorSets)(device,3,writes,0,nullptr);
    CHECK(create_inspector_gpu(data,f,st));
    VkCommandBuffer privateCmd;ok(D(vkAllocateCommandBuffers)(device,&ai,&privateCmd));st.commandBuffers={privateCmd};
    auto submitPrivate=[&] {
        ok(D(vkEndCommandBuffer)(privateCmd));VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&privateCmd;ok(D(vkQueueSubmit)(queue,1,&submit,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));ok(D(vkResetFences)(device,1,&fence));
    };
    VkCommandBufferBeginInfo privateBegin{};privateBegin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;privateBegin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    OverlayFrameData frame{};frame.uniform.viewport[0]=768;frame.uniform.viewport[1]=720;frame.uniform.viewport[2]=menu_scale(768,720);frame.uniform.pick[0]=48;frame.uniform.pick[1]=32;frame.uniform.pick[2]=state().ticket;frame.pickDraw={3,1,0,0};
    ok(update_inspector_gpu(data,f,st,0,frame.uniform));ok(write_overlay_uniform(data,f,st.uniforms[0],frame));
    ok(D(vkBeginCommandBuffer)(privateCmd,&privateBegin));VkImageMemoryBarrier readDepth{};readDepth.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;readDepth.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;readDepth.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;readDepth.oldLayout=readDepth.newLayout=VK_IMAGE_LAYOUT_GENERAL;readDepth.srcQueueFamilyIndex=readDepth.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;readDepth.image=depthImage;readDepth.subresourceRange=depthViewInfo.subresourceRange;
    D(vkCmdPipelineBarrier)(privateCmd,VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&readDepth);
    record_inspector_gpu(f,st,0);submitPrivate();frame.uniform.pick[2]=0;ok(update_inspector_gpu(data,f,st,0,frame.uniform));CHECK(state().depthReady && std::abs(state().depth-.25f)<.000001f);
    // Layout observation uses the application command path, then successful submit.
    ok(A(vkBeginCommandBuffer)(cmd,&privateBegin));VkImageMemoryBarrier readable=readDepth;readable.image=image;readable.subresourceRange=viewInfo.subresourceRange;readable.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;readable.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;readable.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    A(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&readable);ok(A(vkEndCommandBuffer)(cmd));VkSubmitInfo makeReadable{};makeReadable.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;makeReadable.commandBufferCount=1;makeReadable.pCommandBuffers=&cmd;ok(A(vkQueueSubmit)(queue,1,&makeReadable,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));ok(D(vkResetFences)(device,1,&fence));
    CHECK(discovery::devices.at(device).images.at(discovery::id(image))->mipLayouts[0].known);
    state().page=3;state().texture=0;state().preview=true;frame.uniform.menu[0]=1;populate_inspector_menu(device,frame.uniform);ok(update_inspector_gpu(data,f,st,0,frame.uniform));CHECK(frame.uniform.pick[3]==1);populate_inspector_menu(device,frame.uniform);frame.pickDraw.vertexCount=0;frame.previewDraw={3,1,0,0};ok(write_overlay_uniform(data,f,st.uniforms[0],frame));
    ok(D(vkBeginCommandBuffer)(privateCmd,&privateBegin));VkClearValue outputClear{};VkRenderPassBeginInfo outputBegin{};outputBegin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;outputBegin.renderPass=st.renderPass;outputBegin.framebuffer=outputFB;outputBegin.renderArea.extent={768,720};outputBegin.clearValueCount=1;outputBegin.pClearValues=&outputClear;
    VkPipeline unusedEffect{},menuPipeline{},unusedGizmo{};CHECK(create_overlay_pipelines(data,f,st,unusedEffect,menuPipeline,unusedGizmo));
    D(vkCmdBeginRenderPass)(privateCmd,&outputBegin,VK_SUBPASS_CONTENTS_INLINE);D(vkCmdBindPipeline)(privateCmd,VK_PIPELINE_BIND_POINT_GRAPHICS,menuPipeline);D(vkCmdBindDescriptorSets)(privateCmd,VK_PIPELINE_BIND_POINT_GRAPHICS,st.pipelineLayout,0,1,st.descriptorSets.data(),0,nullptr);D(vkCmdDraw)(privateCmd,6,1,0,0);record_inspector_preview(f,st,0);D(vkCmdEndRenderPass)(privateCmd);
    VkBufferImageCopy previewCopy{};previewCopy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};previewCopy.imageExtent={768,720,1};D(vkCmdCopyImageToBuffer)(privateCmd,outputImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&previewCopy);VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=buffer;host.size=VK_WHOLE_SIZE;D(vkCmdPipelineBarrier)(privateCmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);submitPrivate();
    auto previewPixel=[&](float px,float py,unsigned channel){unsigned x=unsigned((px+16)*frame.uniform.viewport[2]),y=unsigned((py+16)*frame.uniform.viewport[2]);return static_cast<unsigned char*>(mapped)[(y*768+x)*4+channel];};
    CHECK(previewPixel(440,230,0)>240 && previewPixel(625,230,1)>240);
    const auto previewOutput=directory+"-preview.ppm";FILE* previewFile=std::fopen(previewOutput.c_str(),"wb");CHECK(previewFile);std::fprintf(previewFile,"P6\n768 720\n255\n");for(unsigned i=0;i<768*720;++i)CHECK(std::fwrite(static_cast<unsigned char*>(mapped)+i*4,1,3,previewFile)==3);std::fclose(previewFile);
    unsigned textInk=0;for(unsigned y=46;y<56;++y)for(unsigned x=32;x<270;++x){auto px=static_cast<unsigned char*>(mapped)+(y*768+x)*4;if(px[1]>150 && px[2]>100)++textInk;}CHECK(textInk>80);
    // A partial mip transition must not invalidate the readable base mip.
    auto mipInfo=viewInfo;mipInfo.subresourceRange.baseMipLevel=1;VkImageView mipView;ok(A(vkCreateImageView)(device,&mipInfo,nullptr,&mipView));
    ok(A(vkBeginCommandBuffer)(cmd,&privateBegin));auto mipBarrier=readable;mipBarrier.subresourceRange=mipInfo.subresourceRange;mipBarrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;mipBarrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;mipBarrier.srcAccessMask=0;mipBarrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    A(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&mipBarrier);
    VkClearColorValue blue{{0,0,1,.5f}};A(vkCmdClearColorImage)(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&blue,1,&mipBarrier.subresourceRange);
    mipBarrier.oldLayout=mipBarrier.newLayout;mipBarrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;mipBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mipBarrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    A(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&mipBarrier);ok(A(vkEndCommandBuffer)(cmd));ok(A(vkQueueSubmit)(queue,1,&makeReadable,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));ok(D(vkResetFences)(device,1,&fence));
    auto mipResource=state().selected->resources[0];mipResource.binding=1;mipResource.descriptor.view=discovery::devices.at(device).views.at(discovery::id(mipView));state().selected->resources.push_back(mipResource);
    state().texture=1;ok(update_inspector_gpu(data,f,st,0,frame.uniform));CHECK(frame.uniform.pick[3]==1);
    // The RGB preview must show the blue mip independently of its 0.5 alpha.
    ok(write_overlay_uniform(data,f,st.uniforms[0],frame));ok(D(vkBeginCommandBuffer)(privateCmd,&privateBegin));
    D(vkCmdBeginRenderPass)(privateCmd,&outputBegin,VK_SUBPASS_CONTENTS_INLINE);record_inspector_preview(f,st,0);D(vkCmdEndRenderPass)(privateCmd);
    D(vkCmdCopyImageToBuffer)(privateCmd,outputImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&previewCopy);D(vkCmdPipelineBarrier)(privateCmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);submitPrivate();
    CHECK(previewPixel(440,230,2)>250 && previewPixel(440,230,0)<3);
    discovery::request_asset_export(state(),true);discovery::flush_asset_exports();auto assetJob=state().assetExport;CHECK(assetJob && assetJob->ready && assetJob->textures.size()==2);
    auto exportStep=[&] {
        ok(update_inspector_gpu(data,f,st,0,frame.uniform));frame.pickDraw.vertexCount=0;ok(write_overlay_uniform(data,f,st.uniforms[0],frame));
        ok(D(vkBeginCommandBuffer)(privateCmd,&privateBegin));record_inspector_gpu(f,st,0);submitPrivate();
        ok(update_inspector_gpu(data,f,st,0,frame.uniform));discovery::flush_asset_exports();
    };
    // Polling before any command submission cannot claim uninitialized bytes.
    ok(update_inspector_gpu(data,f,st,0,frame.uniform));ok(update_inspector_gpu(data,f,st,0,frame.uniform));CHECK(assetJob->textures[0].status=="gpu");
    for(unsigned n=0;n<6 && discovery::asset_export_busy(assetJob);++n)exportStep();
    CHECK(!discovery::asset_export_busy(assetJob) && assetJob->textures[0].status=="saved" && assetJob->textures[1].status=="saved");
    auto readFile=[](const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);return std::vector<unsigned char>(std::istreambuf_iterator<char>(file),{});};
    auto bmp=readFile(std::filesystem::path(assetJob->directory)/assetJob->textures[0].file);CHECK(bmp.size()==122+64*64*4 && bmp[0]=='B' && bmp[124]==255 && bmp[122]==0);
    bmp=readFile(std::filesystem::path(assetJob->directory)/assetJob->textures[1].file);CHECK(bmp.size()==122+32*32*4 && bmp[18]==32 && bmp[22]==224 && bmp[122]==255 && bmp[123]==0 && bmp[124]==0 && bmp[125]==128);
    const auto originalVS=state().selected->draw.pipeline->vs;auto bytes=readFile(std::filesystem::path(assetJob->directory)/("vs-"+discovery::hex(originalVS->hash)+".spv"));CHECK(bytes.size()==originalVS->words.size()*4 && !std::memcmp(bytes.data(),originalVS->words.data(),bytes.size()));
    std::printf("PASS: mutable texture preview + partial mip layouts; two native-size BMPs, alpha, shader bytes, completion marker; directory=%s\n",assetJob->directory.c_str());
    if(heapPipeline) {
        const auto previous=state().selected;const auto previousTexture=state().texture;
        state().selected=pickedGeometry;state().geometryCandidate=true;state().texture=0;state().preview=false;
        discovery::request_asset_export(state(),true);discovery::flush_asset_exports();auto geometryJob=state().assetExport;
        for(unsigned n=0;n<3 && discovery::asset_export_busy(geometryJob);++n)exportStep();
        CHECK(geometryJob->textures.size()==1 && geometryJob->textures[0].status=="saved");
        auto pixels=readFile(std::filesystem::path(geometryJob->directory)/geometryJob->textures[0].file);
        CHECK(pixels.size()==122+64*64*4 && std::all_of(pixels.begin()+122,pixels.end(),[](auto c){return c==255;}));
        auto manifest=readFile(std::filesystem::path(geometryJob->directory)/"manifest.json");std::string json(manifest.begin(),manifest.end());
        CHECK(json.find("\"renderTargetCapable\":false")!=std::string::npos && json.find("depth-tested-geometry-candidate")!=std::string::npos);
        auto shaderBytes=readFile(std::filesystem::path(geometryJob->directory)/("fs-"+discovery::hex(pickedGeometry->draw.pipeline->fs->hash)+".spv"));
        CHECK(shaderBytes.size()==pickedGeometry->draw.pipeline->fs->words.size()*4 && !std::memcmp(shaderBytes.data(),pickedGeometry->draw.pipeline->fs->words.data(),shaderBytes.size()));
        state().selected=previous;state().texture=previousTexture;state().preview=true;
        std::printf("PASS: picked geometry exports original sampled asset (white), not the green final pass; exact geometry FS bytes; directory=%s\n",geometryJob->directory.c_str());
    }
    // Cancellation before recording leaves no fake bitmap; source lifetime is released.
    discovery::request_asset_export(state(),false);discovery::flush_asset_exports();ok(update_inspector_gpu(data,f,st,0,frame.uniform));auto cancelled=state().assetExport;release_inspector_image(device,image);discovery::flush_asset_exports();CHECK(cancelled->textures[0].status=="unavailable");
    // Export allocation cleanup is independent from the working overlay.
    setenv("DEPTH_PROBE_SHADER_DIR",(directory+"/missing-shaders").c_str(),1);discovery::request_asset_export(state(),false);discovery::flush_asset_exports();ok(update_inspector_gpu(data,f,st,0,frame.uniform));discovery::flush_asset_exports();
    CHECK(state().assetExport->textures[0].status=="unavailable" && frame.uniform.pick[3]);setenv("DEPTH_PROBE_SHADER_DIR",(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/shaders").c_str(),1);
    if(enabledFeatures.textureCompressionBC) {
        VkImageCreateInfo bcInfo=imageInfo;bcInfo.format=VK_FORMAT_BC3_SRGB_BLOCK;bcInfo.extent={8,8,1};bcInfo.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;bcInfo.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        VkImage bcImage;ok(A(vkCreateImage)(device,&bcInfo,nullptr,&bcImage));D(vkGetImageMemoryRequirements)(device,bcImage,&requirements);auto bcMemory=allocate(requirements,0);ok(D(vkBindImageMemory)(device,bcImage,bcMemory,0));
        auto bcViewInfo=viewInfo;bcViewInfo.image=bcImage;bcViewInfo.format=bcInfo.format;VkImageView bcView;ok(A(vkCreateImageView)(device,&bcViewInfo,nullptr,&bcView));
        auto uploadInfo=bufferInfo;uploadInfo.size=64;uploadInfo.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;VkBuffer upload;ok(D(vkCreateBuffer)(device,&uploadInfo,nullptr,&upload));D(vkGetBufferMemoryRequirements)(device,upload,&requirements);auto uploadMemory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);ok(D(vkBindBufferMemory)(device,upload,uploadMemory,0));void* uploadMap;ok(D(vkMapMemory)(device,uploadMemory,0,VK_WHOLE_SIZE,0,&uploadMap));std::memset(uploadMap,0,64);
        for(unsigned n=0;n<4;++n){auto* p=static_cast<unsigned char*>(uploadMap)+n*16;p[0]=64;p[8]=0x10;p[9]=0x84;}
        ok(A(vkBeginCommandBuffer)(cmd,&privateBegin));auto bcBarrier=mipBarrier;bcBarrier.image=bcImage;bcBarrier.subresourceRange=bcViewInfo.subresourceRange;bcBarrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;bcBarrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;bcBarrier.srcAccessMask=0;bcBarrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        A(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&bcBarrier);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={8,8,1};A(vkCmdCopyBufferToImage)(cmd,upload,bcImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        bcBarrier.oldLayout=bcBarrier.newLayout;bcBarrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;bcBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bcBarrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;A(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&bcBarrier);ok(A(vkEndCommandBuffer)(cmd));ok(A(vkQueueSubmit)(queue,1,&makeReadable,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));ok(D(vkResetFences)(device,1,&fence));
        auto bcResource=mipResource;bcResource.descriptor.view=discovery::devices.at(device).views.at(discovery::id(bcView));state().selected->resources={bcResource};state().texture=0;state().preview=true;ok(update_inspector_gpu(data,f,st,0,frame.uniform));CHECK(frame.uniform.pick[3]);
        discovery::request_asset_export(state(),true);discovery::flush_asset_exports();auto bcJob=state().assetExport;for(unsigned n=0;n<3 && discovery::asset_export_busy(bcJob);++n)exportStep();CHECK(bcJob->textures[0].status=="saved");
        auto decoded=readFile(std::filesystem::path(bcJob->directory)/bcJob->textures[0].file);CHECK(decoded.size()==122+8*8*4 && std::abs(int(decoded[122])-132)<=1 && std::abs(int(decoded[123])-130)<=1 && std::abs(int(decoded[124])-132)<=1 && decoded[125]==64);
        A(vkDestroyImageView)(device,bcView,nullptr);A(vkDestroyImage)(device,bcImage,nullptr);D(vkFreeMemory)(device,bcMemory,nullptr);D(vkUnmapMemory)(device,uploadMemory);D(vkDestroyBuffer)(device,upload,nullptr);D(vkFreeMemory)(device,uploadMemory,nullptr);
        std::printf("PASS: GPU BC3 SRGB decode to BMP preserves encoded RGB and alpha; directory=%s\n",bcJob->directory.c_str());
    }
    A(vkDestroyImageView)(device,mipView,nullptr);
    D(vkDestroyPipeline)(device,unusedEffect,nullptr);D(vkDestroyPipeline)(device,menuPipeline,nullptr);D(vkDestroyPipeline)(device,unusedGizmo,nullptr);
    // Borrowed source destroy releases the layer's private view/descriptor first.
    release_inspector_image(device,image);frame.uniform.pick[3]=0;CHECK(!st.commandRecorded[0]);
    destroy_inspector_gpu(data,f,st);D(vkFreeCommandBuffers)(device,pool,1,&privateCmd);destroy_overlay_uniforms(data,f,st);
    D(vkDestroyDescriptorPool)(device,st.descriptorPool,nullptr);D(vkDestroyPipelineLayout)(device,st.pipelineLayout,nullptr);D(vkDestroyDescriptorSetLayout)(device,st.descriptorSetLayout,nullptr);D(vkDestroyFramebuffer)(device,outputFB,nullptr);D(vkDestroyRenderPass)(device,st.renderPass,nullptr);g_depth_overlay_states.clear();
    D(vkDestroyImageView)(device,outputView,nullptr);D(vkDestroyImage)(device,outputImage,nullptr);D(vkFreeMemory)(device,outputMemory,nullptr);
    if(heapPipeline) {
        A(vkDestroyPipeline)(device,heapPipeline,nullptr);A(vkDestroyPipeline)(device,helperPipeline,nullptr);A(vkDestroyPipelineLayout)(device,heapLayout,nullptr);
        A(vkDestroyDescriptorPool)(device,heapPool,nullptr);for(auto l:heapLayouts)A(vkDestroyDescriptorSetLayout)(device,l,nullptr);
        A(vkDestroyImageView)(device,heapView,nullptr);A(vkDestroyImage)(device,heapImage,nullptr);D(vkFreeMemory)(device,heapMemory,nullptr);
    }
    A(vkDestroyDescriptorPool)(device,texturePool,nullptr);A(vkDestroyDescriptorSetLayout)(device,textureLayout,nullptr);A(vkDestroySampler)(device,sampler,nullptr);
    A(vkDestroyBuffer)(device,applicationUniform,nullptr);A(vkFreeMemory)(device,applicationMemory,nullptr);
    for(auto p:{linked,inlined,identified})if(p)A(vkDestroyPipeline)(device,p,nullptr);
    for(auto p:appLibraries)A(vkDestroyPipeline)(device,p,nullptr);
    D(vkDestroyPipelineCache)(device,pipelineCache,nullptr);
    A(vkDestroyPipeline)(device,discard,nullptr);A(vkDestroyPipeline)(device,composite,nullptr);
    A(vkDestroyPipeline)(device,red,nullptr);A(vkDestroyPipeline)(device,green,nullptr);A(vkDestroyPipelineLayout)(device,layout,nullptr);
    A(vkDestroyCommandPool)(device,pool,nullptr);D(vkDestroyFence)(device,fence,nullptr);A(vkDestroyFramebuffer)(device,framebuffer,nullptr);A(vkDestroyRenderPass)(device,pass,nullptr);
    A(vkDestroyImageView)(device,view,nullptr);A(vkDestroyImage)(device,image,nullptr);D(vkFreeMemory)(device,imageMemory,nullptr);
    A(vkDestroyImageView)(device,depthView,nullptr);A(vkDestroyImage)(device,depthImage,nullptr);D(vkFreeMemory)(device,depthMemory,nullptr);
    D(vkUnmapMemory)(device,hostMemory);D(vkDestroyBuffer)(device,buffer,nullptr);D(vkFreeMemory)(device,hostMemory,nullptr);
    destroy_device_discovery(device);g_devices.clear();D(vkDestroyDevice)(device,nullptr);
    if(messenger)I(vkDestroyDebugUtilsMessengerEXT)(instance,messenger,nullptr);
    I(vkDestroyInstance)(instance,nullptr);CHECK(validationErrors==0);
    std::printf("PASS: %zu-bit real Vulkan inspector: exact draw, overlap/frontmost, coplanar LESS, discard, unchanged picking pixels, draw-only tint, clear/reusable unknown, click edge, texture identity, exact depth readback, sampled texture preview/lifetime; validation=%s errors=0; image=%s\n",sizeof(void*)*8,validate?"on":"off",output.c_str());
    dlclose(loader);
}
