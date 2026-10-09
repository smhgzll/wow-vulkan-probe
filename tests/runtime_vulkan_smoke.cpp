// Real offscreen Vulkan renderer probe. Only PRESENT layout transitions are
// adapted to GENERAL: these images are not WSI images. No present claims here.
#include "overlay_internal.hpp"
#include "overlay_menu.hpp"
#include "depth_tracking.hpp"
#include "shader_resources.hpp"
#include "pipeline_discovery.hpp"
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
using namespace depth_probe;
namespace depth_probe {
bool create_depth_overlay_state(const DeviceData&, VkSwapchainKHR, const SceneDepthChoice&, DepthOverlayState&);
}
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
static void ok(VkResult r) { CHECK(r == VK_SUCCESS); }
static PFN_vkGetDeviceProcAddr driver;
static VkDevice device;
#define D(name) reinterpret_cast<PFN_##name>(driver(device,#name))
static std::atomic<unsigned> errors{0};
static VkPipeline baselinePipeline{}, effectPipeline{};
static VkPipelineLayout baselineLayout{};
static HeartbeatConstants heartbeat{.25f,1.f,1280.f/720.f,0};
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    ++errors; std::fprintf(stderr,"VALIDATION: %s\n",data->pMessage); return VK_FALSE;
}
static VKAPI_ATTR VkResult VKAPI_CALL createLayout(VkDevice d, const VkPipelineLayoutCreateInfo* info,
    const VkAllocationCallbacks* a, VkPipelineLayout* result) {
    auto copy = *info;
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(HeartbeatConstants)};
    copy.pushConstantRangeCount=1; copy.pPushConstantRanges=&range;
    ok(D(vkCreatePipelineLayout)(d,&copy,a,&baselineLayout));
    return D(vkCreatePipelineLayout)(d,info,a,result);
}
static VKAPI_ATTR VkResult VKAPI_CALL createPipelines(VkDevice d, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* info, const VkAllocationCallbacks* a, VkPipeline* result) {
    const auto r=D(vkCreateGraphicsPipelines)(d,cache,count,info,a,result);
    if (r==VK_SUCCESS && !baselinePipeline) {
        CHECK(count==1);
        effectPipeline=*result;
        auto code=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/effects/rainbow-wow/shaders/depth.frag.spv");
        CHECK(!code.empty());
        VkShaderModuleCreateInfo sm{}; sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize=code.size()*4;sm.pCode=code.data();VkShaderModule module{};
        ok(D(vkCreateShaderModule)(d,&sm,nullptr,&module));
        auto copy=*info;
        VkPipelineShaderStageCreateInfo stages[]{info->pStages[0],info->pStages[1]};
        stages[1].module=module;copy.pStages=stages;copy.layout=baselineLayout;
        ok(D(vkCreateGraphicsPipelines)(d,cache,1,&copy,nullptr,&baselinePipeline));
        D(vkDestroyShaderModule)(d,module,nullptr);
    }
    return r;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL dispatch(VkDevice d,const char* name) {
    if (!std::strcmp(name,"vkCreatePipelineLayout")) return reinterpret_cast<PFN_vkVoidFunction>(createLayout);
    if (!std::strcmp(name,"vkCreateGraphicsPipelines")) return reinterpret_cast<PFN_vkVoidFunction>(createPipelines);
    return driver(d,name);
}
static VKAPI_ATTR void VKAPI_CALL offscreenBarrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,
    VkDependencyFlags flags,uint32_t mc,const VkMemoryBarrier* m,uint32_t bc,const VkBufferMemoryBarrier* b,
    uint32_t ic,const VkImageMemoryBarrier* images) {
    std::vector<VkImageMemoryBarrier> copy(images,images+ic);
    for(auto& i:copy) {
        if(i.oldLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)i.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
        if(i.newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)i.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    }
    D(vkCmdPipelineBarrier)(cmd,src,dst,flags,mc,m,bc,b,ic,copy.data());
}
static VKAPI_ATTR void VKAPI_CALL baselineBind(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline pipeline) {
    if(pipeline==effectPipeline)D(vkCmdBindPipeline)(c,p,baselinePipeline);
}
static VKAPI_ATTR void VKAPI_CALL baselineSets(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout,
    uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dc,const uint32_t* offsets) {
    D(vkCmdBindDescriptorSets)(c,p,baselineLayout,first,count,sets,dc,offsets);
    D(vkCmdPushConstants)(c,baselineLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(heartbeat),&heartbeat);
}
static VKAPI_ATTR void VKAPI_CALL noMenu(VkCommandBuffer,VkBuffer,VkDeviceSize,uint32_t,uint32_t) {}
static VKAPI_ATTR VkResult VKAPI_CALL alreadyInitialized(VkDevice,void*) { return VK_SUCCESS; }
int main() {
    setenv("DEPTH_PROBE_UI_INPUT","0",1);
    char temporaryShaders[]="/tmp/depth-probe-gpu-shaders-XXXXXX";
    CHECK(mkdtemp(temporaryShaders));
    for(const auto* name : {"depth.vert.spv","depth.frag.spv","menu.vert.spv","menu.frag.spv","gizmo.vert.spv","gizmo.frag.spv"})
        std::filesystem::copy_file(std::filesystem::path(DEPTH_PROBE_TEST_ROOT_DIR)/"shaders"/name,std::filesystem::path(temporaryShaders)/name);
    setenv("DEPTH_PROBE_SHADER_DIR",temporaryShaders,1);setenv("DEPTH_PROBE_SHADER_HOT_RELOAD","1",1);
    setenv("XDG_CONFIG_HOME",("/tmp/depth-probe-gpu-config-"+std::to_string(getpid())).c_str(),1);
    setenv("DEPTH_PROBE_LAYER_LOG","/tmp/depth-probe-runtime-smoke.log",1);
    void* loader=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);CHECK(loader);
    auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));CHECK(gipa);
    VkInstance instance{};
#define I(name) reinterpret_cast<PFN_##name>(gipa(instance,#name))
    bool validate=std::getenv("DEPTH_PROBE_SMOKE_VALIDATION")!=nullptr;
    const char* validationLayer="VK_LAYER_KHRONOS_validation";
    const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug{};debug.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;debug.pfnUserCallback=validation;
    VkValidationFeatureEnableEXT sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{};validationFeatures.sType=VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount=1;validationFeatures.pEnabledValidationFeatures=&sync;validationFeatures.pNext=&debug;
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pApplicationInfo=&app;
    if(validate){ci.enabledLayerCount=1;ci.ppEnabledLayerNames=&validationLayer;ci.enabledExtensionCount=1;ci.ppEnabledExtensionNames=&extension;ci.pNext=&validationFeatures;}
    ok(I(vkCreateInstance)(&ci,nullptr,&instance));
    VkDebugUtilsMessengerEXT messenger{};
    if(validate)ok(I(vkCreateDebugUtilsMessengerEXT)(instance,&debug,nullptr,&messenger));
    uint32_t count=0;ok(I(vkEnumeratePhysicalDevices)(instance,&count,nullptr));CHECK(count);
    std::vector<VkPhysicalDevice> devices(count);ok(I(vkEnumeratePhysicalDevices)(instance,&count,devices.data()));
    auto physical=devices[0];
    I(vkGetPhysicalDeviceQueueFamilyProperties)(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);I(vkGetPhysicalDeviceQueueFamilyProperties)(physical,&count,queues.data());
    CHECK(queues[0].queueFlags&VK_QUEUE_GRAPHICS_BIT); // production scope: family 0
    float priority=1;
    VkDeviceQueueCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    ok(I(vkCreateDevice)(physical,&di,nullptr,&device));driver=I(vkGetDeviceProcAddr);
    VkQueue queue{};D(vkGetDeviceQueue)(device,0,0,&queue);
    VkPhysicalDeviceMemoryProperties memory{};I(vkGetPhysicalDeviceMemoryProperties)(physical,&memory);
    auto allocate=[&](VkMemoryRequirements req,VkMemoryPropertyFlags flags) {
        uint32_t type=0;while(type<memory.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;
        CHECK(type<memory.memoryTypeCount);
        VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        VkDeviceMemory mem{};ok(D(vkAllocateMemory)(device,&ai,nullptr,&mem));return mem;
    };
    constexpr uint32_t width=1280,height=720;
    auto makeImage=[&](VkFormat format,VkImageUsageFlags usage,VkImage& image,VkDeviceMemory& mem) {
        VkImageCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ii.imageType=VK_IMAGE_TYPE_2D;ii.format=format;
        ii.extent={width,height,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;
        ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=usage;
        ok(D(vkCreateImage)(device,&ii,nullptr,&image));VkMemoryRequirements req{};
        D(vkGetImageMemoryRequirements)(device,image,&req);mem=allocate(req,0);ok(D(vkBindImageMemory)(device,image,mem,0));
    };
    VkImage color{},depth{};VkDeviceMemory colorMemory{},depthMemory{};
    makeImage(VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,color,colorMemory);
    makeImage(VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,depth,depthMemory);
    VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=depth;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=VK_FORMAT_D32_SFLOAT;vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};VkImageView view{};
    ok(D(vkCreateImageView)(device,&vi,nullptr,&view));
    VkBuffer staging{};VkDeviceMemory stagingMemory{};void* mapped{};
    VkBufferCreateInfo bi{};bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=width*height*4;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ok(D(vkCreateBuffer)(device,&bi,nullptr,&staging));VkMemoryRequirements req{};D(vkGetBufferMemoryRequirements)(device,staging,&req);
    stagingMemory=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ok(D(vkBindBufferMemory)(device,staging,stagingMemory,0));ok(D(vkMapMemory)(device,stagingMemory,0,VK_WHOLE_SIZE,0,&mapped));
    auto* depths=static_cast<float*>(mapped);
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
        depths[y*width+x]=x>1100?1.f:(((x/48+y/48)%2) ? .72f:.76f);
    setenv("DEPTH_PROBE_SCENE_COLOR", "1", 1);
    setenv("DEPTH_PROBE_GPU_TIMING", "1", 1);
    DeviceData data{};data.device=device;data.physical_device=physical;data.next_gdpa=dispatch;
    data.get_memory_properties=I(vkGetPhysicalDeviceMemoryProperties);data.set_device_loader_data=alreadyInitialized;
    auto fakeSwapchain=(VkSwapchainKHR)(uintptr_t)1;
    data.get_properties=I(vkGetPhysicalDeviceProperties);data.get_queue_properties=I(vkGetPhysicalDeviceQueueFamilyProperties);
    data.get_format_properties=I(vkGetPhysicalDeviceFormatProperties);
    data.get_image_format_properties=I(vkGetPhysicalDeviceImageFormatProperties);
    SwapchainInfo sc{};sc.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;sc.width=width;sc.height=height;sc.format=VK_FORMAT_R8G8B8A8_UNORM;sc.images={color};
    g_swapchains[{device,1}]=sc;
    SceneDepthChoice selected{};auto& candidate=selected.candidate;
    candidate.device=device;candidate.image=depth;candidate.depthView=view;candidate.format=VK_FORMAT_D32_SFLOAT;
    candidate.extent={width,height,1};candidate.samples=VK_SAMPLE_COUNT_1_BIT;candidate.mipLevels=candidate.arrayLayers=1;
    candidate.originalUsage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    candidate.sampledSupported=candidate.sampledReady=true;candidate.generation=1;
    DepthOverlayState state;CHECK(create_depth_overlay_state(data,fakeSwapchain,selected,state));
    auto f=load_depth_overlay_fns(data);f.cmdPipelineBarrier=offscreenBarrier;
    VkCommandPool pool{};VkCommandPoolCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ok(D(vkCreateCommandPool)(device,&pi,nullptr,&pool));
    VkCommandBuffer commands[4]{};VkCommandBufferAllocateInfo ca{};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=4;
    ok(D(vkAllocateCommandBuffers)(device,&ca,commands));
    VkFence fence{};VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;ok(D(vkCreateFence)(device,&fi,nullptr,&fence));
    auto begin=[&](VkCommandBuffer c){VkCommandBufferBeginInfo info{};info.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;ok(D(vkBeginCommandBuffer)(c,&info));};
    auto submit=[&](const std::vector<VkCommandBuffer>& cmds){
        VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.commandBufferCount=cmds.size();si.pCommandBuffers=cmds.data();
        ok(D(vkQueueSubmit)(queue,1,&si,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));ok(D(vkResetFences)(device,1,&fence));
    };
    auto barrier=[&](VkCommandBuffer c,VkImage image,VkImageAspectFlags aspect,VkImageLayout oldLayout,VkImageLayout newLayout,
        VkAccessFlags src,VkAccessFlags dst){
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcAccessMask=src;b.dstAccessMask=dst;
        b.oldLayout=oldLayout;b.newLayout=newLayout;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=image;b.subresourceRange={aspect,0,1,0,1};
        D(vkCmdPipelineBarrier)(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copy.imageExtent={width,height,1};
    begin(commands[0]);
    barrier(commands[0],depth,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    D(vkCmdCopyBufferToImage)(commands[0],staging,depth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier(commands[0],depth,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    barrier(commands[0],color,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    ok(D(vkEndCommandBuffer)(commands[0]));submit({commands[0]});
    // Clear/readback command buffers and both overlay versions are recorded once.
    begin(commands[0]);
    barrier(commands[0],color,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue background{{.18f,.24f,.32f,1.f}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    D(vkCmdClearColorImage)(commands[0],color,VK_IMAGE_LAYOUT_GENERAL,&background,1,&range);
    ok(D(vkEndCommandBuffer)(commands[0]));
    begin(commands[1]);
    barrier(commands[1],color,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    D(vkCmdCopyImageToBuffer)(commands[1],color,VK_IMAGE_LAYOUT_GENERAL,staging,1,&copy);
    VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=staging;host.size=VK_WHOLE_SIZE;
    D(vkCmdPipelineBarrier)(commands[1],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    ok(D(vkEndCommandBuffer)(commands[1]));
    CHECK(record_depth_overlay_commands(f,state,0));
    auto baseline=state;baseline.commandBuffers[0]=commands[2];
    auto oldFns=f;oldFns.cmdBindPipeline=baselineBind;oldFns.cmdBindDescriptorSets=baselineSets;oldFns.cmdDrawIndirect=noMenu;
    CHECK(record_depth_overlay_commands(oldFns,baseline,0));
    OverlayFrameData value{};value.uniform.heartbeat=heartbeat;
    value.uniform.viewport[0]=width;value.uniform.viewport[1]=height;value.uniform.viewport[2]=1;
    bool patternedScene=false;
    auto render=[&](bool old=false){
        if(patternedScene) {
            auto* bytes=static_cast<unsigned char*>(mapped);
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
                size_t at=(y*width+x)*4;bytes[at]=x%255;bytes[at+1]=y%255;
                bytes[at+2]=(x/16+y/16)%2?190:80;bytes[at+3]=255;
            }
        }
        ok(write_overlay_uniform(data,f,state.uniforms[0],value));
        submit({commands[0],old?commands[2]:state.commandBuffers[0],commands[1]});
        const auto* start=static_cast<unsigned char*>(mapped);return std::vector<unsigned char>(start,start+width*height*4);
    };
    value.uniform.viewport[3]=1;
    CHECK(state.sceneColor.size()==1);
    const auto current=render(), previous=render(true);
    unsigned maxDifference=0, changed=0;
    for(size_t i=0;i<current.size();++i){const unsigned delta=std::abs(int(current[i])-int(previous[i]));maxDifference=std::max(maxDifference,delta);changed+=delta!=0;}
    CHECK(maxDifference<=1);
    value.uniform.settings.effectEnabled=0;const auto disabled=render();
    CHECK(disabled!=current);
    value.uniform.settings={};value.uniform.settings.debugMode=4;const auto copied=render();
    CHECK(copied==disabled);
    value.uniform.settings={};value.uniform.settings.debugMode=1;const auto raw=render();
    CHECK(std::abs(int(raw[0])-194)<=1 && raw[0]==raw[1] && raw[1]==raw[2]);
    value.uniform.settings.depthConvention=2; CHECK(render()==raw);
    value.uniform.settings.debugMode=5;const auto gradient=render();CHECK(gradient!=raw);
    value.uniform.settings.depthConvention=1;CHECK(render()==gradient);
    value.uniform.settings.depthConvention=0;
    value.uniform.settings.debugMode=6;CHECK(render()==disabled); // Auto/unknown must skip reconstruction.
    value.uniform.settings.depthConvention=1;value.uniform.settings.projectionMode=1;
    value.uniform.settings.nearPlane=1;value.uniform.settings.farPlane=101;
    const auto linear=render(); CHECK(std::abs(int(linear[0])-10)<=1);
    value.uniform.settings.debugMode=8;CHECK(render()==disabled); // no FOV
    value.uniform.settings.verticalFov=60;CHECK(render()==linear);
    value.uniform.settings.debugMode=14;const auto positions=render();
    const auto pxIndex=(180*width+320)*4;
    // Analytic inverse projection at this pixel (raw .72 on the checker).
    const float expectedZ=101.f/(101.f-.72f*100.f);
    const float ux=(320.5f/width)*2-1, uy=1-(180.5f/height)*2;
    CHECK(std::abs(int(positions[pxIndex])-int(std::lround((.5f+ux*1.7777778f*.57735027f*expectedZ/202)*255)))<=1);
    CHECK(std::abs(int(positions[pxIndex+1])-int(std::lround((.5f+uy*.57735027f*expectedZ/202)*255)))<=1);
    value.uniform.settings.debugMode=6;
    value.uniform.settings.depthConvention=2; const auto reversedLinear=render();CHECK(std::abs(int(reversedLinear[0])-3)<=1);
    value.uniform.settings.depthConvention=1; value.uniform.settings.projectionMode=2;
    const auto ortho=render();CHECK(std::abs(int(ortho[0])-194)<=1);
    value.uniform.settings={};
    value.uniform.settings.debugMode=2;const auto ao=render();CHECK(ao!=raw);
    for(size_t i=0;i<ao.size();i+=4)CHECK(ao[i]==ao[i+1]&&ao[i+1]==ao[i+2]);
    value.uniform.settings.debugMode=0;const auto normal=render();CHECK(normal!=current && normal!=raw);
    value.uniform.settings={};value.uniform.settings.aoStrength=2.5f;const auto stronger=render();CHECK(stronger!=current);
    value.uniform.settings={};value.uniform.menu[0]=1;value.uniform.menu[1]=1;value.menuDraw={6,1,0,0};
    const auto menu=render();CHECK(menu!=current);
    // Outside the panel the exact same effect remains visible.
    for(uint32_t y=0;y<height;++y)for(uint32_t x=420;x<width;++x)for(unsigned c=0;c<4;++c)
        CHECK(menu[(y*width+x)*4+c]==current[(y*width+x)*4+c]);
    const std::string output="/tmp/depth-probe-runtime-menu-"+std::to_string(sizeof(void*)*8)+".ppm";
    FILE* file=std::fopen(output.c_str(),"wb");CHECK(file);std::fprintf(file,"P6\n%u %u\n255\n",width,height);
    for(size_t i=0;i<menu.size();i+=4) CHECK(std::fwrite(menu.data()+i,1,3,file)==3);
    std::fclose(file);
    overlay_menu().page=1;populate_menu_uniform(value.uniform);
    const auto advancedMenu=render();CHECK(advancedMenu!=menu);
    // A different image alone would miss empty field names. Check the actual
    // ink glyphs on every settings/diagnostics row, including enum/toggle rows.
    const auto checkRowText=[&](const auto& pixels) {
        for(unsigned row=0;row<10;++row) {
            if(value.uniform.rows[row][3]<.5f)continue;
            unsigned inkPixels=0;
            for(unsigned y=94+44*row;y<105+44*row;++y)
                for(unsigned x=40;x<260;++x) {
                    const auto at=(y*width+x)*4;
                    inkPixels+=pixels[at]>210 && pixels[at+1]>220 && pixels[at+2]>235;
                }
            CHECK(inkPixels>20);
        }
    };
    checkRowText(advancedMenu);
    for(uint32_t page=2;page<menu_page_count();++page) {
        overlay_menu().page=page;populate_menu_uniform(value.uniform);populate_diagnostics(data,state,value.uniform);populate_discovery_menu(data.device,value.uniform);
        checkRowText(render());
    }
    std::puts("PASS: readable field labels on every real GPU settings and diagnostics page");
    // Exercise exact high/low 32-bit identity display, including the least
    // significant nibble. A float conversion would lose these hash digits.
    overlay_menu().page=discovery_first_page();populate_menu_uniform(value.uniform);populate_discovery_menu(data.device,value.uniform);
    value.uniform.rows[2][0]=0;value.uniform.rows[2][3]=8;
    value.uniform.explorer[0]=0x89abcdef;value.uniform.explorer[1]=0x01234567;value.uniform.explorer[15]=1;
    const auto explorerMenu=render();checkRowText(explorerMenu);
    // Verify actual label identity, not just the presence of lit pixels: a
    // miscompiled label lookup can draw readable but entirely wrong words.
    const unsigned alphabet[26][7]={
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{15,16,16,16,16,16,15},
        {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
        {15,16,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
        {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},{17,17,17,21,21,27,17},{17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
    };
    const auto expectText=[&](const auto& pixels,const char* text,float originX,float originY,float scale,int palette=0) {
        const int colors[3][3]={{222,232,247},{255,186,89},{71,217,196}};
        for(unsigned col=0;text[col];++col)for(unsigned y=0;y<7;++y)for(unsigned x=0;x<5;++x) {
            const char c=text[col];unsigned bits=0;
            if(c>='A' && c<='Z')bits=alphabet[c-'A'][y];
            else if(c=='-')bits=y==3?31:0;
            else if(c=='8'){const unsigned eight[7]={14,17,17,14,17,17,14};bits=eight[y];}
            else CHECK(c==' ');
            const auto px=uint32_t(originX+(col*6+x+.5f)*scale),py=uint32_t(originY+(y+.5f)*scale);
            const auto at=(py*width+px)*4;
            bool ink=true;for(unsigned channel=0;channel<3;++channel)ink&=std::abs(int(pixels[at+channel])-colors[palette][channel])<=2;
            if(ink!=bool(bits&(1u<<(4-x)))){std::fprintf(stderr,"wrong menu text '%s' col=%u x=%u y=%u\n",text,col,x,y);CHECK(false);}
        }
    };
    const char* explorerLabels[]={"SELECTED PIPELINE","CAPTURE REMAINING","PIPELINE HASH","VERTEX SHADER HASH","FRAGMENT SHADER HASH",
        "DRAWS LAST INTERVAL","DEPTH WRITE","ALPHA BLEND","RENDER TARGET","ISOLATION MODE"};
    for(unsigned row=0;row<10;++row)expectText(explorerMenu,explorerLabels[row],40,94+44*row,1.5f);
    expectText(explorerMenu,"PROTOTYPE - CATEGORIES UNKNOWN",40,68,1,1);
    expectText(explorerMenu,"CAPTURE 8 FRAMES",52,581,1);
    expectText(explorerMenu,"STOP ISOLATION",236,581,1);
    expectText(explorerMenu,"UNKNOWN TARGET",40,466,1,2);
    expectText(explorerMenu,"OFF",40,510,1,2);
    overlay_menu().page=discovery_first_page()+1;populate_menu_uniform(value.uniform);populate_discovery_menu(data.device,value.uniform);
    const auto explorerResources=render();
    const char* resourceLabels[]={"PIPELINES SEEN","TARGET WIDTH","TARGET HEIGHT","DEPTH FORMAT","DEPTH IMAGE","BOUND DEPTH IMAGES",
        "COMPARE SAMPLERS","FS STATIC DREF OPS","SEMANTIC CATEGORY","METADATA INCOMPLETE"};
    for(unsigned row=0;row<10;++row)expectText(explorerResources,resourceLabels[row],40,94+44*row,1.5f);
    const auto hashInk=[&](unsigned column,unsigned x,unsigned y) {
        const auto px=uint32_t(40+(column*6+x+.5f)*1.5f);
        const auto py=uint32_t(202+(y+.5f)*1.5f);
        const auto at=(py*width+px)*4;
        return explorerMenu[at]>65 && explorerMenu[at+1]>200 && explorerMenu[at+2]>180;
    };
    CHECK(hashInk(0,0,1) && !hashInk(0,2,1) && hashInk(0,4,1));
    CHECK(!hashInk(1,0,1) && hashInk(1,2,1));
    CHECK(hashInk(15,0,0) && hashInk(15,4,0) && !hashInk(15,4,1));
    std::puts("PASS: Pipeline Explorer menu labels and exact 64-bit hexadecimal glyphs on real GPU");
    overlay_menu().page=0;value.uniform.panel[0]=0;value.uniform.panel[3]=0;
    value.menuDraw={0,1,0,0};value.uniform.menu[0]=0;
    value.uniform.settings.rainbowEnabled=0;CHECK(render()==disabled);
    value.uniform.settings.legacyAOEnabled=1;CHECK(render()!=disabled);
    value.uniform.settings={};CHECK(render()==current);
    // Analytic surfaces exercise the production reconstruction shader.
    auto uploadDepth=[&](auto depthAt) {
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)depths[y*width+x]=depthAt(x,y);
        begin(commands[3]);
        VkBufferMemoryBarrier hostWrite{};hostWrite.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        hostWrite.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;hostWrite.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        hostWrite.srcQueueFamilyIndex=hostWrite.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        hostWrite.buffer=staging;hostWrite.size=VK_WHOLE_SIZE;
        D(vkCmdPipelineBarrier)(commands[3],VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&hostWrite,0,nullptr);
        barrier(commands[3],depth,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy dc{};dc.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};dc.imageExtent={width,height,1};
        D(vkCmdCopyBufferToImage)(commands[3],staging,depth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&dc);
        barrier(commands[3],depth,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        ok(D(vkEndCommandBuffer)(commands[3]));submit({commands[3]});
    };
    value.menuDraw={0,1,0,0};value.uniform.menu[0]=0;value.uniform.settings={};
    auto& manual=value.uniform.settings;
    manual.depthConvention=1;manual.projectionMode=1;manual.nearPlane=1;manual.farPlane=101;manual.verticalFov=60;manual.debugMode=7;
    auto encodeDepth=[](float z){return 101.f*(z-1)/(z*100.f);};
    uploadDepth([&](uint32_t x,uint32_t){return encodeDepth(x<width/2?5.f:10.f);});
    const auto flatNormals=render();
    for(uint32_t x : {width/2-1,width/2,width/2+1}) {
        const size_t at=(height/2*width+x)*4;
        CHECK(std::abs(int(flatNormals[at])-128)<=1 && std::abs(int(flatNormals[at+1])-128)<=1 && flatNormals[at+2]>=254);
    }
    auto planeDepth=[&](uint32_t x,uint32_t) {
        const float sx=((x+.5f)/width*2-1)*(float(width)/height)*.57735027f;
        return encodeDepth(5.f/(1-.25f*sx));
    };
    uploadDepth(planeDepth);const auto tiltedNormals=render();
    const size_t centerAt=(height/2*width+width/2)*4;
    CHECK(std::abs(int(tiltedNormals[centerAt])-158)<=2 && std::abs(int(tiltedNormals[centerAt+1])-128)<=1 &&
          std::abs(int(tiltedNormals[centerAt+2])-251)<=2);
    manual.depthConvention=2;uploadDepth([&](uint32_t x,uint32_t y){return 1-planeDepth(x,y);});
    const auto reversedNormals=render();
    for(unsigned c=0;c<3;++c)CHECK(std::abs(int(reversedNormals[centerAt+c])-int(tiltedNormals[centerAt+c]))<=1);
    std::puts("PASS: linear/position analytic reference, flat/step/tilted normals and reversed-Z normals on real GPU");
    manual.depthConvention=1;manual.debugMode=9;manual.ssaoEnabled=1;manual.ssaoRadius=8;manual.ssaoBias=.025f;
    uploadDepth([&](uint32_t,uint32_t){return encodeDepth(10.f);});
    const auto flatAO=render();for(size_t i=0;i<flatAO.size();i+=4)CHECK(flatAO[i]>=254);
    uploadDepth([&](uint32_t x,uint32_t){return encodeDepth(x<width/2?5.f:10.f);});
    const auto cornerAO=render();CHECK(cornerAO!=flatAO);
    unsigned darkPixels=0;for(size_t i=0;i<cornerAO.size();i+=4)darkPixels+=cornerAO[i]<250;CHECK(darkPixels>100);
    manual.aoBlur=1;const auto blurredAO=render();CHECK(blurredAO!=cornerAO);
    // Blur must not transfer the far plane's shadow onto the near plane.
    CHECK(blurredAO[(height/2*width+width/2-1)*4]>=254);
    manual.ssaoEnabled=0;CHECK(render()==flatAO);
    std::puts("PASS: hemisphere SSAO flat plane, corner occlusion, depth-aware blur, disabled branch on real GPU");
    // A nonuniform source catches UV/copy errors that a constant clear cannot.
    begin(commands[0]);
    VkBufferMemoryBarrier patternHost{};patternHost.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    patternHost.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;patternHost.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    patternHost.srcQueueFamilyIndex=patternHost.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;patternHost.buffer=staging;patternHost.size=VK_WHOLE_SIZE;
    D(vkCmdPipelineBarrier)(commands[0],VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&patternHost,0,nullptr);
    barrier(commands[0],color,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy pc{};pc.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};pc.imageExtent={width,height,1};
    D(vkCmdCopyBufferToImage)(commands[0],staging,color,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&pc);
    barrier(commands[0],color,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    ok(D(vkEndCommandBuffer)(commands[0]));patternedScene=true;
    manual={};manual.rainbowEnabled=0;
    const auto scenePattern=render();manual.debugMode=4;CHECK(render()==scenePattern);manual.debugMode=3;
    manual.sharpenEnabled=1;CHECK(render()!=scenePattern);manual.sharpenEnabled=0;
    manual.chromaticEnabled=1;manual.chromaticAmount=4;CHECK(render()!=scenePattern);manual.chromaticEnabled=0;
    manual.fogEnabled=1;CHECK(render()==scenePattern); // Unknown projection skips fog.
    manual.depthConvention=1;manual.projectionMode=1;manual.nearPlane=1;manual.farPlane=101;manual.verticalFov=60;
    manual.fogStart=1;manual.fogEnd=20;CHECK(render()!=scenePattern);manual.fogEnabled=0;
    manual.contoursEnabled=1;manual.contourFrequency=20.2f;CHECK(render()!=scenePattern);manual.contoursEnabled=0;
    manual.outlineEnabled=1;CHECK(render()!=scenePattern);manual.outlineEnabled=0;
    CHECK(render()==scenePattern);
    std::puts("PASS: nonuniform scene copy, sharpen/chromatic/fog/contour/outline toggles and unavailable fog skip on real GPU");
    // Analytic floor y=-2 and a wall at z=-8: visible wall pixels can be reflected by the floor.
    // uploadDepth uses staging temporarily; render restores the nonuniform color pattern each frame.
    uploadDepth([&](uint32_t x,uint32_t y) {
        float sx=((x+.5f)/width*2-1)*(float(width)/height)*.57735027f;
        float sy=(1-(y+.5f)/height*2)*.57735027f;
        float z=101;
        if(sy<0){float floorZ=-2/sy;if(floorZ>=4 && floorZ<=40)z=floorZ;}
        if(std::abs(sx*8)<1.5f && sy*8>=-2 && sy*8<=2)z=std::min(z,8.f);
        return encodeDepth(z);
    });
    manual.ssrEnabled=1;manual.ssrSteps=32;manual.ssrStepSize=.25f;manual.ssrThickness=.2f;manual.ssrMaxDistance=15;manual.debugMode=10;
    const auto hits=render();unsigned hitPixels=0;for(size_t i=0;i<hits.size();i+=4)hitPixels+=hits[i]>250;CHECK(hitPixels>100);
    const size_t floorAt=(576*width+640)*4;CHECK(hits[floorAt]>250);
    manual.debugMode=11;const auto hitUV=render();
    const float sy=(1-(576.5f/height)*2)*.57735027f;
    const float floorZ=-2/sy;
    const float hitY=-2+.1f+(-sy)*(8-floorZ);
    const float expectedV=.5f-hitY/(8*.57735027f*2);
    CHECK(std::abs(int(hitUV[floorAt])-128)<=2 && std::abs(int(hitUV[floorAt+1])-int(std::lround(expectedV*255)))<=2);
    manual.debugMode=12;const auto reflections=render();
    CHECK(std::abs(int(reflections[floorAt])-int(640%255))<=4);
    CHECK(std::abs(int(reflections[floorAt+1])-int(uint32_t(expectedV*height)%255))<=4);
    manual.debugMode=3;manual.reflectionStrength=.8f;CHECK(render()!=scenePattern);
    manual.reflectionStrength=0;CHECK(render()==scenePattern);
    manual.reflectionStrength=.35f;manual.debugMode=10;manual.depthConvention=0;CHECK(render()==scenePattern);
    manual.depthConvention=1;value.uniform.viewport[3]=0;CHECK(render()==scenePattern);value.uniform.viewport[3]=1;
    uploadDepth([&](uint32_t,uint32_t){return encodeDepth(10.f);});
    const auto noHits=render();for(size_t i=0;i<noHits.size();i+=4)CHECK(noHits[i]==0);
    manual.ssrEnabled=0;manual.debugMode=3;CHECK(render()==scenePattern);
    std::puts("PASS: SSR analytic floor/wall hit, projected UV, reflection color, no-hit plane, zero strength, missing projection/color and disabled fallback");
    CHECK(state.timestampPool);state.queryPending[0]=true;
    auto timingSettings=overlay_settings();timingSettings.gpuTimingEnabled=1;set_overlay_settings(timingSettings);
    ok(collect_gpu_timing(data,f,state,0));CHECK(state.overlayGpuMs>=0);
    std::printf("PASS: real asynchronous timestamp query: final synthetic frame %.4f ms (software ICD, not a WoW cost estimate)\n",state.overlayGpuMs);
    overlay_menu().page=discovery_first_page()-2;value.menuDraw={6,1,0,0};manual.gpuTimingEnabled=1;
    populate_menu_uniform(value.uniform);populate_diagnostics(data,state,value.uniform);CHECK(value.uniform.rows[0][3]==4);
    const auto diagnosticsImage=render();CHECK(diagnosticsImage!=scenePattern);
    checkRowText(diagnosticsImage);
    // Zero is a useful diagnostic result: it must remain visible, not blank.
    for(unsigned row : {3u,4u,5u}) {
        CHECK(value.uniform.rows[row][0]==0.f && value.uniform.rows[row][3]==4.f);
        unsigned digits=0;
        for(unsigned y=111+44*row;y<123+44*row;++y)for(unsigned x=330;x<394;++x) {
            const auto at=(y*width+x)*4;
            digits+=diagnosticsImage[at]>110 && diagnosticsImage[at+1]>130 && diagnosticsImage[at+2]>155;
        }
        CHECK(digits>50);
    }
    overlay_menu().page=0;
    const auto writeImage=[&](const char* name,const auto& pixels) {
        const auto filename=std::string("/tmp/depth-probe-")+name+"-"+std::to_string(sizeof(void*)*8)+".ppm";
        FILE* image=std::fopen(filename.c_str(),"wb");CHECK(image);std::fprintf(image,"P6\n%u %u\n255\n",width,height);
        for(size_t i=0;i<pixels.size();i+=4)CHECK(std::fwrite(pixels.data()+i,1,3,image)==3);
        std::fclose(image);
    };
    writeImage("advanced-menu",advancedMenu);writeImage("diagnostics-menu",diagnosticsImage);
    writeImage("pipeline-explorer-menu",explorerMenu);
    writeImage("pipeline-explorer-resources",explorerResources);
    writeImage("ssao",cornerAO);writeImage("ssr-hits",hits);writeImage("ssr-reflections",reflections);
    value.menuDraw={0,1,0,0};
    const auto beforeReload=render();const auto oldPipeline=state.pipeline;
    const auto reloadPath=std::filesystem::path(temporaryShaders)/"depth.frag.spv";
    std::filesystem::last_write_time(reloadPath,std::filesystem::last_write_time(reloadPath)+std::chrono::seconds(1));
    state.commandRecorded[0]=true;state.shaderPollAfter={};prepare_shader_frame(data,f,state,0);
    CHECK(state.pipeline!=oldPipeline && state.retiredPipelines.size()==1 && !state.commandRecorded[0]);
    CHECK(record_depth_overlay_commands(f,state,0));state.commandRecorded[0]=true;
    CHECK(render()==beforeReload);prepare_shader_frame(data,f,state,0);CHECK(state.retiredPipelines.empty());
    const auto replacement=state.pipeline;
    {std::ofstream broken(reloadPath,std::ios::binary);broken<<"broken";}
    state.shaderPollAfter={};prepare_shader_frame(data,f,state,0);
    CHECK(state.pipeline==replacement && state.shaderReloadStatus==2 && render()==beforeReload);
    std::puts("PASS: real Vulkan shader reload generation, fence-completed retirement and broken-file rollback");
    g_depth_overlay_states[{device,1}]=std::move(state);destroy_depth_overlay_state(data,fakeSwapchain);g_swapchains.clear();
    D(vkDestroyPipeline)(device,baselinePipeline,nullptr);D(vkDestroyPipelineLayout)(device,baselineLayout,nullptr);
    D(vkDestroyFence)(device,fence,nullptr);D(vkDestroyCommandPool)(device,pool,nullptr);
    D(vkDestroyImageView)(device,view,nullptr);
    D(vkDestroyImage)(device,depth,nullptr);D(vkFreeMemory)(device,depthMemory,nullptr);
    D(vkDestroyImage)(device,color,nullptr);D(vkFreeMemory)(device,colorMemory,nullptr);
    D(vkUnmapMemory)(device,stagingMemory);D(vkDestroyBuffer)(device,staging,nullptr);D(vkFreeMemory)(device,stagingMemory,nullptr);
    D(vkDestroyDevice)(device,nullptr);
    if(messenger)I(vkDestroyDebugUtilsMessengerEXT)(instance,messenger,nullptr);
    I(vkDestroyInstance)(instance,nullptr);CHECK(errors==0);
    std::printf("PASS: %zu-bit real renderer/UBO, legacy/debug/postfx, enable/strength, menu pages, cached commands; baseline max pixel delta=%u (%u changed channels); validation=%s; image=%s\n",sizeof(void*)*8,maxDifference,changed,validate?"on":"off",output.c_str());
    dlclose(loader);
    std::filesystem::remove_all(temporaryShaders);
}
