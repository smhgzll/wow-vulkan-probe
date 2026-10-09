// Real offscreen application, observed through the production discovery hooks.
// A synthetic present boundary replaces WSI only; no game acceptance is claimed.
#include "discovery_internal.hpp"
#include "overlay_uniform.hpp"
#include "overlay_menu.hpp"
#include "shader_resources.hpp"
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
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
    setenv("DEPTH_PROBE_PIPELINE_EXPLORER","1",1);setenv("DEPTH_PROBE_DEPTH_PRESENT","0",1);setenv("DEPTH_PROBE_UI_INPUT","0",1);
    auto directory="/tmp/depth-probe-discovery-gpu-"+std::to_string(getpid());setenv("DEPTH_PROBE_DISCOVERY_DIR",directory.c_str(),1);
    setenv("DEPTH_PROBE_LAYER_LOG",(directory+".log").c_str(),1);
    void* loader=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);CHECK(loader);gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));CHECK(gipa);
    bool validate=std::getenv("DEPTH_PROBE_SMOKE_VALIDATION")!=nullptr;
    const char* validationLayer="VK_LAYER_KHRONOS_validation";const char* debugExtension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug{};debug.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;debug.pfnUserCallback=validation;
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ii.pApplicationInfo=&app;
    if(validate){ii.pNext=&debug;ii.enabledLayerCount=1;ii.ppEnabledLayerNames=&validationLayer;ii.enabledExtensionCount=1;ii.ppEnabledExtensionNames=&debugExtension;}
    ok(I(vkCreateInstance)(&ii,nullptr,&instance));VkDebugUtilsMessengerEXT messenger{};if(validate)ok(I(vkCreateDebugUtilsMessengerEXT)(instance,&debug,nullptr,&messenger));
    uint32_t count=0;ok(I(vkEnumeratePhysicalDevices)(instance,&count,nullptr));CHECK(count);std::vector<VkPhysicalDevice> physical(count);ok(I(vkEnumeratePhysicalDevices)(instance,&count,physical.data()));auto gpu=physical[0];
    I(vkGetPhysicalDeviceQueueFamilyProperties)(gpu,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);I(vkGetPhysicalDeviceQueueFamilyProperties)(gpu,&count,families.data());
    uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;CHECK(family<count);
    float priority=1;VkDeviceQueueCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;ok(I(vkCreateDevice)(gpu,&di,nullptr,&device));
    gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance,"vkGetDeviceProcAddr"));VkQueue queue;D(vkGetDeviceQueue)(device,family,0,&queue);
    DeviceData data;data.device=device;data.physical_device=gpu;data.next_gdpa=gdpa;data.next_queue_present=boundary;
    data.next_create_image=D(vkCreateImage);data.next_destroy_image=D(vkDestroyImage);data.next_create_image_view=D(vkCreateImageView);data.next_destroy_image_view=D(vkDestroyImageView);
    g_devices[dispatch_key(device)]=data;
    VkPhysicalDeviceMemoryProperties memory;I(vkGetPhysicalDeviceMemoryProperties)(gpu,&memory);
    auto allocate=[&](const VkMemoryRequirements& requirements,VkMemoryPropertyFlags flags) {
        uint32_t type=0;while(type<memory.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) || (memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;CHECK(type<memory.memoryTypeCount);
        VkMemoryAllocateInfo info{};info.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;info.allocationSize=requirements.size;info.memoryTypeIndex=type;VkDeviceMemory result;ok(D(vkAllocateMemory)(device,&info,nullptr,&result));return result;
    };
    VkImageCreateInfo imageInfo{};imageInfo.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=VK_FORMAT_R8G8B8A8_UNORM;imageInfo.extent={64,64,1};imageInfo.mipLevels=imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;imageInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage image;ok(A(vkCreateImage)(device,&imageInfo,nullptr,&image));VkMemoryRequirements requirements;D(vkGetImageMemoryRequirements)(device,image,&requirements);auto imageMemory=allocate(requirements,0);ok(D(vkBindImageMemory)(device,image,imageMemory,0));
    VkImageViewCreateInfo viewInfo{};viewInfo.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;viewInfo.image=image;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;viewInfo.format=imageInfo.format;viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view;ok(A(vkCreateImageView)(device,&viewInfo,nullptr,&view));
    VkBufferCreateInfo bufferInfo{};bufferInfo.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bufferInfo.size=64*64*4;bufferInfo.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;VkBuffer buffer;ok(D(vkCreateBuffer)(device,&bufferInfo,nullptr,&buffer));
    D(vkGetBufferMemoryRequirements)(device,buffer,&requirements);auto hostMemory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);ok(D(vkBindBufferMemory)(device,buffer,hostMemory,0));void* mapped;ok(D(vkMapMemory)(device,hostMemory,0,VK_WHOLE_SIZE,0,&mapped));
    VkAttachmentDescription attachment{};attachment.format=imageInfo.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference colorRef{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&colorRef;
    VkSubpassDependency dependency{};dependency.srcSubpass=0;dependency.dstSubpass=VK_SUBPASS_EXTERNAL;dependency.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependency.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dependency.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo passInfo{};passInfo.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;passInfo.attachmentCount=1;passInfo.pAttachments=&attachment;passInfo.subpassCount=1;passInfo.pSubpasses=&sub;passInfo.dependencyCount=1;passInfo.pDependencies=&dependency;VkRenderPass pass;ok(A(vkCreateRenderPass)(device,&passInfo,nullptr,&pass));
    VkFramebufferCreateInfo fbInfo{};fbInfo.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fbInfo.renderPass=pass;fbInfo.attachmentCount=1;fbInfo.pAttachments=&view;fbInfo.width=fbInfo.height=64;fbInfo.layers=1;VkFramebuffer framebuffer;ok(A(vkCreateFramebuffer)(device,&fbInfo,nullptr,&framebuffer));
    VkPipelineLayoutCreateInfo layoutInfo{};layoutInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;VkPipelineLayout layout;ok(A(vkCreatePipelineLayout)(device,&layoutInfo,nullptr,&layout));
    auto shader=[&](const char* file) {
        auto code=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/"+file+".spv");CHECK(!code.empty());VkShaderModuleCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;info.codeSize=code.size()*4;info.pCode=code.data();VkShaderModule module;ok(A(vkCreateShaderModule)(device,&info,nullptr,&module));return module;
    };
    auto vs=shader("discovery_scene.vert"),redFS=shader("discovery_red.frag"),greenFS=shader("discovery_green.frag");
    auto pipeline=[&](VkShaderModule fs) {
        VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
        VkPipelineVertexInputStateCreateInfo vertex{};vertex.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;viewport.viewportCount=viewport.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ca{};ca.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo blend{};blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;blend.attachmentCount=1;blend.pAttachments=&ca;
        VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;ds.dynamicStateCount=2;ds.pDynamicStates=dynamic;
        VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;info.stageCount=2;info.pStages=stages;info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&ms;info.pColorBlendState=&blend;info.pDynamicState=&ds;info.layout=layout;info.renderPass=pass;
        VkPipeline p;ok(A(vkCreateGraphicsPipelines)(device,{},1,&info,nullptr,&p));return p;
    };
    auto red=pipeline(redFS),green=pipeline(greenFS);
    for(auto module:{vs,redFS,greenFS})A(vkDestroyShaderModule)(device,module,nullptr);
    VkCommandPoolCreateInfo poolInfo{};poolInfo.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;poolInfo.queueFamilyIndex=family;poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool pool;ok(D(vkCreateCommandPool)(device,&poolInfo,nullptr,&pool));
    VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;VkCommandBuffer cmd;ok(A(vkAllocateCommandBuffers)(device,&ai,&cmd));
    VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;VkFence fence;ok(D(vkCreateFence)(device,&fi,nullptr,&fence));
    auto render=[&](bool oneTime=true) {
        VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=oneTime?VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT:0;ok(A(vkBeginCommandBuffer)(cmd,&bi));
        VkClearValue clear{};clear.color.float32[3]=1;
        VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;begin.renderPass=pass;begin.framebuffer=framebuffer;begin.renderArea.extent={64,64};begin.clearValueCount=1;begin.pClearValues=&clear;A(vkCmdBeginRenderPass)(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0,0,64,64,0,1};VkRect2D scissor{{0,0},{64,64}};A(vkCmdSetViewport)(cmd,0,1,&viewport);A(vkCmdSetScissor)(cmd,0,1,&scissor);
        A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,red);A(vkCmdDraw)(cmd,3,1,0,0);
        scissor.offset.x=32;scissor.extent.width=32;A(vkCmdSetScissor)(cmd,0,1,&scissor);A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,green);A(vkCmdDraw)(cmd,3,1,0,0);A(vkCmdEndRenderPass)(cmd);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={64,64,1};D(vkCmdCopyImageToBuffer)(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
        VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=buffer;host.size=VK_WHOLE_SIZE;
        D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);ok(A(vkEndCommandBuffer)(cmd));
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;ok(A(vkQueueSubmit)(queue,1,&submit,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));ok(D(vkResetFences)(device,1,&fence));
        VkPresentInfoKHR present{};present.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;ok(A(vkQueuePresentKHR)(queue,&present));
        const auto* bytes=static_cast<unsigned char*>(mapped);return std::vector<unsigned char>(bytes,bytes+64*64*4);
    };
    request_discovery_capture(device,1);auto original=render();
    auto rgb=[&](const auto& pixels,unsigned x){auto i=(32*64+x)*4;return std::array<unsigned char,3>{pixels[i],pixels[i+1],pixels[i+2]};};
    const std::array<unsigned char,3> redColor{255,0,0},greenColor{0,255,0},black{0,0,0};CHECK(rgb(original,16)==redColor && rgb(original,48)==greenColor);
    OverlayUniform ui{};ui.panel[0]=discovery_first_page();populate_discovery_menu(device,ui);CHECK(ui.rows[5][0]==1 && ui.rows[2][3]==8);
    CHECK(discovery_menu_input(discovery_first_page(),0,1,false)); // red -> green
    CHECK(discovery_menu_input(discovery_first_page(),9,0,true));auto hidden=render();CHECK(rgb(hidden,16)==redColor && rgb(hidden,48)==redColor);
    CHECK(discovery_menu_input(discovery_first_page(),9,0,true));auto only=render();CHECK(rgb(only,16)==black && rgb(only,48)==greenColor);
    auto reusable=render(false);CHECK(reusable==original);
    CHECK(discovery_menu_input(discovery_first_page(),12,0,true));CHECK(render()==original);
    flush_discovery_exports();
    const auto output=directory+"/isolation.ppm";FILE* file=std::fopen(output.c_str(),"wb");CHECK(file);std::fprintf(file,"P6\n192 64\n255\n");
    for(unsigned y=0;y<64;++y)for(const auto* pixels:{&original,&hidden,&only})for(unsigned x=0;x<64;++x)CHECK(std::fwrite(pixels->data()+(y*64+x)*4,1,3,file)==3);
    std::fclose(file);
    A(vkDestroyPipeline)(device,red,nullptr);A(vkDestroyPipeline)(device,green,nullptr);A(vkDestroyPipelineLayout)(device,layout,nullptr);
    A(vkDestroyCommandPool)(device,pool,nullptr);D(vkDestroyFence)(device,fence,nullptr);A(vkDestroyFramebuffer)(device,framebuffer,nullptr);A(vkDestroyRenderPass)(device,pass,nullptr);
    A(vkDestroyImageView)(device,view,nullptr);A(vkDestroyImage)(device,image,nullptr);D(vkFreeMemory)(device,imageMemory,nullptr);
    D(vkUnmapMemory)(device,hostMemory);D(vkDestroyBuffer)(device,buffer,nullptr);D(vkFreeMemory)(device,hostMemory,nullptr);
    destroy_device_discovery(device);g_devices.clear();D(vkDestroyDevice)(device,nullptr);
    if(messenger)I(vkDestroyDebugUtilsMessengerEXT)(instance,messenger,nullptr);
    I(vkDestroyInstance)(instance,nullptr);CHECK(validationErrors==0);
    std::printf("PASS: %zu-bit real Vulkan discovery, shader identity after module destruction, capture/export, hide/only/restore pixels and reusable-buffer protection; validation=%s errors=0; image=%s\n",sizeof(void*)*8,validate?"on":"off",output.c_str());
    dlclose(loader);
}
