// GPU exercise of the volumetric fog through the production hooks: world rendering (WoW viewport: half-pixel
// offset, negative height, depth range 0..0.94) into color+depth, then a UI rendering instance. Checks the fog is
// composited before the UI (UI pixels untouched), matches a CPU reference of the same physics, is applied once
// across a split world pass, and is dropped - never drawn over UI - when the UI shares the world's targets.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iterator>
#include "volumetric_fog.hpp"
#include "overlay_uniform.hpp"
#include "scene_debugger.hpp"
#include "camera_hud_input.hpp"
#include "memory_camera.hpp"
#include "scene_debugger.hpp"
#include "shader_resources.hpp"
#include "water_reflections.hpp"
#include <dlfcn.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
using namespace depth_probe;
using namespace depth_probe::discovery;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)

static VkDevice device{};static VkInstance instance{};
static PFN_vkGetDeviceProcAddr gdpa;static PFN_vkGetInstanceProcAddr gipa;
#define D(api) reinterpret_cast<PFN_##api>(gdpa(device,#api))
#define I(api) reinterpret_cast<PFN_##api>(gipa(instance,#api))
#define A(api) reinterpret_cast<PFN_##api>(discovery_hook(#api)?discovery_hook(#api):gdpa(device,#api))
static void ok(VkResult r){CHECK(r==VK_SUCCESS);}
// DEPTH_PROBE_SMOKE_VALIDATION=1: run under the Khronos validation layer; any error fails the test.
static std::atomic<unsigned> errors{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* p,void*){++errors;std::fprintf(stderr,"VALIDATION: %s\n",p->pMessage);return VK_FALSE;}
template<class T> T vk(VkStructureType t){T v{};v.sType=t;return v;}

// A CGCamera-shaped struct in this process's memory, read by the layer through DEPTH_PROBE_MEM_CAMERA_ADDR.
alignas(16) static uint8_t cgcamera[0x48];

int main() {
    // DEPTH_PROBE_FOG_BENCH=1: a GPU cost measurement at the game's resolution instead of the checks (see the bench block below).
    const bool bench=std::getenv("DEPTH_PROBE_FOG_BENCH")!=nullptr;
    const unsigned W=bench?1920:128,H=bench?1080:128;
    // Camera: at (100, 200, 30), looking along +X and 15 degrees down; vertical FOV 60 = raw fov * 0.6.
    const float pitch=15.f*3.14159265f/180.f;
    const Vec3 position{100,200,30},forward{std::cos(pitch),0,-std::sin(pitch)},left{0,1,0},up{std::sin(pitch),0,std::cos(pitch)};
    const float rawFov=(60.f/.6f)*3.14159265f/180.f,aspect=float(W)/float(H),nearClip=.2f,farClip=800.f;
    auto put=[](uint32_t o,const void* p,size_t n){std::memcpy(cgcamera+o,p,n);};
    put(0x08,&position,12);put(0x14,&forward,12);put(0x20,&left,12);put(0x2C,&up,12);
    put(0x38,&nearClip,4);put(0x3C,&farClip,4);put(0x40,&rawFov,4);put(0x44,&aspect,4);
    char address[32];std::snprintf(address,sizeof address,"0x%llx",(unsigned long long)(uintptr_t)cgcamera);
    setenv("DEPTH_PROBE_MEM_CAMERA_ADDR",address,1);
    {   // Every custom effect is on by default (the user, 2026-10-08) - also what RESET TO DEFAULTS restores.
        const FogSettings d;
        CHECK(d.softShadows>0 && d.shadowFade>0 && d.shadowSoftness>1 && d.contactShadows>0 && d.ambientOcclusion>0 && d.terrainShadows>0 &&
              d.pointShadows>0 && d.shadows && d.sunEnabled && d.sunRays>0 && d.sunGlare>0 && d.sunAureole>0 && d.autoExposure && d.noiseAmount>0 && d.transparentFog);
    }
    const std::string ini="/tmp/depth-probe-fog-smoke-"+std::to_string(getpid())+".ini";
    // The defaults are the full look (volumetric_fog.hpp FogSettings); this test measures one feature at a time against the CPU reference, so the
    // settings it does not set are pinned to the old off values.
    const char* offDefaults="game_fog_distance=1\nshadow_softness=1\nsoft_shadows=0\nshadow_fade=0\nshadow_fade_distance=10\nindoor_shadows=1\n"
        "contact_shadows=0\nambient_occlusion=0\nterrain_shadows=0\nterrain_shadows_from=20\npoint_shadows=0\npoint_shadow_ignore=1.5\nmax_opacity=1\n"
        "light_limit=0\nauto_exposure=0\nsun_aureole=0\nsun_glare=0\nsun_rays=0\nsky_light=0.5\ntransparent_fog=0\n";
    {std::ofstream o(ini);o<<offDefaults<<"preset=medium\nmax_distance=400\ndistribution=8\nbase_density=0.01\nheight_falloff=0.02\nbase_height=0\nmax_extinction=1\n"
                             "albedo=0.9,0.9,0.9\nemissive=0,0,0\nanisotropy=0.3\nambient=0.3,0.3,0.35\nsun_enabled=1\nsun_direction=1,0,0.3\nsun_color=1,1,1\nshadows=0\nopen_sun=1\ntemporal_weight=0\nnoise_amount=0\n";}
    setenv("DEPTH_PROBE_FOG_SETTINGS",ini.c_str(),1);
    setenv("DEPTH_PROBE_SURFACE_INSPECTOR","1",1);setenv("DEPTH_PROBE_DEPTH_PRESENT","0",1);setenv("DEPTH_PROBE_UI_INPUT","0",1);
    setenv("DEPTH_PROBE_SHADER_DIR",DEPTH_PROBE_TEST_ROOT_DIR "/shaders",1);setenv("DEPTH_PROBE_LAYER_LOG","/tmp/depth-probe-fog-gpu.log",1);

    auto loader=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);CHECK(loader);gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));
    auto app=vk<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);app.apiVersion=VK_API_VERSION_1_3;
    auto ii=vk<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);ii.pApplicationInfo=&app;
    const bool validate=std::getenv("DEPTH_PROBE_SMOKE_VALIDATION");const char* layer="VK_LAYER_KHRONOS_validation";const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    auto debug=vk<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;debug.pfnUserCallback=validation;
    if(validate){ii.pNext=&debug;ii.enabledLayerCount=1;ii.ppEnabledLayerNames=&layer;ii.enabledExtensionCount=1;ii.ppEnabledExtensionNames=&extension;}
    ok(I(vkCreateInstance)(&ii,nullptr,&instance));
    VkDebugUtilsMessengerEXT messenger{};if(validate)ok(I(vkCreateDebugUtilsMessengerEXT)(instance,&debug,nullptr,&messenger));
    uint32_t count=0;ok(I(vkEnumeratePhysicalDevices)(instance,&count,nullptr));CHECK(count);std::vector<VkPhysicalDevice> physical(count);ok(I(vkEnumeratePhysicalDevices)(instance,&count,physical.data()));auto gpu=physical[0];
    float priority=1;auto qi=vk<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);qi.queueFamilyIndex=0;qi.queueCount=1;qi.pQueuePriorities=&priority;
    auto f13=vk<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);f13.dynamicRendering=VK_TRUE;
    auto di=vk<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);di.pNext=&f13;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    ok(I(vkCreateDevice)(gpu,&di,nullptr,&device));gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance,"vkGetDeviceProcAddr"));VkQueue queue{};D(vkGetDeviceQueue)(device,0,0,&queue);
    DeviceData data;data.device=device;data.physical_device=gpu;data.next_gdpa=gdpa;
    data.next_create_image=D(vkCreateImage);data.next_destroy_image=D(vkDestroyImage);data.next_create_image_view=D(vkCreateImageView);data.next_destroy_image_view=D(vkDestroyImageView);
    data.get_properties=I(vkGetPhysicalDeviceProperties);data.get_memory_properties=I(vkGetPhysicalDeviceMemoryProperties);data.get_format_properties=I(vkGetPhysicalDeviceFormatProperties);
    data.get_queue_properties=I(vkGetPhysicalDeviceQueueFamilyProperties);data.get_image_format_properties=I(vkGetPhysicalDeviceImageFormatProperties);
    data.get_image_format_properties2=I(vkGetPhysicalDeviceImageFormatProperties2);data.next_cmd_pipeline_barrier=D(vkCmdPipelineBarrier);
    g_devices[dispatch_key(device)]=data;

    auto allocate=[&](VkMemoryRequirements r,VkMemoryPropertyFlags flags){
        VkPhysicalDeviceMemoryProperties m{};data.get_memory_properties(gpu,&m);uint32_t i=0;
        while(i<m.memoryTypeCount && (!(r.memoryTypeBits&(1u<<i)) || (m.memoryTypes[i].propertyFlags&flags)!=flags))++i;CHECK(i<m.memoryTypeCount);
        auto a=vk<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);a.allocationSize=r.size;a.memoryTypeIndex=i;VkDeviceMemory out{};ok(A(vkAllocateMemory)(device,&a,nullptr,&out));return out;};
    auto image=[&](VkFormat format,VkImageUsageFlags usage,VkImageAspectFlags aspect,VkImage& out,VkImageView& view){
        auto i=vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);i.imageType=VK_IMAGE_TYPE_2D;i.format=format;i.extent={W,H,1};i.mipLevels=i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=usage;
        ok(A(vkCreateImage)(device,&i,nullptr,&out));VkMemoryRequirements r{};D(vkGetImageMemoryRequirements)(device,out,&r);ok(A(vkBindImageMemory)(device,out,allocate(r,0),0));
        auto v=vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);v.image=out;v.format=format;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.subresourceRange={aspect,0,1,0,1};ok(A(vkCreateImageView)(device,&v,nullptr,&view));};
    VkImage color{},depth{};VkImageView colorView{},depthView{};
    image(VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT,color,colorView);
    image(VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,depth,depthView);
    auto buffer=[&](VkDeviceSize size,VkBufferUsageFlags usage,void** mapped){
        auto i=vk<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);i.size=size;i.usage=usage;VkBuffer out{};ok(A(vkCreateBuffer)(device,&i,nullptr,&out));
        VkMemoryRequirements r{};D(vkGetBufferMemoryRequirements)(device,out,&r);auto m=allocate(r,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        ok(A(vkBindBufferMemory)(device,out,m,0));ok(A(vkMapMemory)(device,m,0,VK_WHOLE_SIZE,0,mapped));return out;};

    // The layer's own camera math projects the scene, so the test and the fog agree on the camera by construction.
    RawMemoryCamera raw;raw.readOk=true;raw.position=position;raw.rows[0]=forward;raw.rows[1]=left;raw.rows[2]=up;raw.nearClip=nearClip;raw.farClip=farClip;raw.fov=rawFov;raw.aspect=aspect;
    const auto cam=memory_camera_to_derived(raw,SignedPermutation{{1,3,-2}},synthesize_projection(rawFov*.6f,FovAxis::Vertical,aspect,nearClip,farClip));
    CHECK(cam.valid);
    const auto vp=mat4_mul(cam.projection,cam.view);
    std::vector<float> vertices;
    std::vector<std::pair<size_t,std::array<float,3>>> world;   // (vertex index, world position) of every world vertex, to re-project
    auto quad=[&](std::array<float,3> a,std::array<float,3> b,std::array<float,3> c,std::array<float,3> d,bool inFront=true){
        for(auto p:{a,b,c,a,c,d}){auto clip=mat4_mul_vec4(vp,Vec4{p[0],p[1],p[2],1});CHECK(clip.w>0 || !inFront);world.push_back({vertices.size()/4,p});vertices.insert(vertices.end(),{clip.x,clip.y,clip.z,clip.w});}};
    quad({101,-400,0},{700,-400,0},{700,800,0},{101,800,0});   // ground, 6 vertices
    quad({130,206,0},{130,220,0},{130,220,40},{130,206,40});   // near wall at 30 yd, left of centre
    quad({400,150,0},{400,250,0},{400,250,60},{400,150,60});   // far wall at 300 yd
    const float uiQuad[]{-1,-1,.5f,1, -.6f,-1,.5f,1, -.6f,-.6f,.5f,1, -1,-1,.5f,1, -.6f,-.6f,.5f,1, -1,-.6f,.5f,1};
    vertices.insert(vertices.end(),std::begin(uiQuad),std::end(uiQuad));
    // Canopy (drawn only by the canopy test, vertices 24..47): a roof 20 yd above the camera, x 105..170, y 150..260, with a
    // hole x 130..145, y 195..212. From the normal camera it is entirely above the screen.
    quad({105,150,50},{130,150,50},{130,260,50},{105,260,50},false);
    quad({145,150,50},{170,150,50},{170,260,50},{145,260,50},false);
    quad({130,150,50},{145,150,50},{145,195,50},{130,195,50},false);
    quad({130,212,50},{145,212,50},{145,260,50},{130,260,50},false);
    // A white sun disc in the sky (vertices 48..53, drawn only by the sun-finder test with the sky depth range): a small
    // square around where the direction sunDiscDir lands on the screen.
    const Vec3 sunDiscDir=[]{Vec3 d{1,-.15f,.2f};const float l=std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);return Vec3{d.x/l,d.y/l,d.z/l};}();
    {const auto c=mat4_mul_vec4(vp,Vec4{sunDiscDir.x,sunDiscDir.y,sunDiscDir.z,0});const float nx=c.x/c.w,ny=c.y/c.w,h=.12f;
     for(auto [dx,dy]:{std::pair{-h,-h},{h,-h},{h,h},{-h,-h},{h,h},{-h,h}})vertices.insert(vertices.end(),{nx+dx,ny+dy,.5f,1.f});}
    // A "particle" (vertices 54..59, drawn only by the transparent-fog test): additive, no depth write, 50 yd away in front of
    // the far wall, right of the centre - the kind of draw that took the fog of the wall behind it.
    quad({150,180,24},{150,194,24},{150,194,38},{150,180,38});
    // A block on the ground (vertices 60..65, drawn only by the scene-shading test): 45 yd ahead, 8 yd wide, 6 yd high - a
    // wall foot the screen shows (the near wall's is just below it).
    quad({145,196,0},{145,204,0},{145,204,6},{145,196,6});
    // A ridge of land (vertices 66..71, drawn only by the scene-shading test): the same 8 yd wide, rising 45 degrees from the
    // ground at 40 yd to 6 yd high at 46 yd - what terrain shadows are for (the block above is thin: a name or an icon).
    quad({140,196,0},{140,204,0},{146,204,6},{146,196,6});
    // A board floating 3.8..4.4 yd over the ground at 45 yd (vertices 72..77, the scene-shading test only): what a name over
    // a character is to the fog - .6 yd tall, it writes depth, casts no shadow in the game.
    quad({145,196,3.8f},{145,204,3.8f},{145,204,4.4f},{145,196,4.4f});
    void* vmap{};auto vertexBuffer=buffer(vertices.size()*4,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,&vmap);std::memcpy(vmap,vertices.data(),vertices.size()*4);
    void* rmap{};auto readback=buffer(W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&rmap);

    auto shader=[&](const char* name){auto code=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/"+name);CHECK(!code.empty());
        auto i=vk<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);i.codeSize=code.size()*4;i.pCode=code.data();VkShaderModule m{};ok(A(vkCreateShaderModule)(device,&i,nullptr,&m));return m;};
    auto vs=shader("fog_scene.vert.spv"),fs=shader("fog_color.frag.spv");
    auto li=vk<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);VkPipelineLayout layout{};ok(A(vkCreatePipelineLayout)(device,&li,nullptr,&layout));
    auto pipeline=[&](float r,float g,float b,bool world,bool withDepth,bool additive=false,VkCompareOp compare=VK_COMPARE_OP_LESS_OR_EQUAL){
        VkSpecializationMapEntry e[]{{0,0,4},{1,4,4},{2,8,4}};float rgb[]{r,g,b};VkSpecializationInfo spec{3,e,sizeof rgb,rgb};
        VkPipelineShaderStageCreateInfo st[2]{};for(auto& s:st){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        st[0].stage=VK_SHADER_STAGE_VERTEX_BIT;st[0].module=vs;st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;st[1].module=fs;st[1].pSpecializationInfo=&spec;
        VkVertexInputBindingDescription bd{0,16,VK_VERTEX_INPUT_RATE_VERTEX};VkVertexInputAttributeDescription ad{0,0,VK_FORMAT_R32G32B32A32_SFLOAT,0};
        auto vi=vk<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);vi.vertexBindingDescriptionCount=vi.vertexAttributeDescriptionCount=1;vi.pVertexBindingDescriptions=&bd;vi.pVertexAttributeDescriptions=&ad;
        auto ia=vk<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        auto vps=vk<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);vps.viewportCount=vps.scissorCount=1;
        auto rs=vk<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);rs.lineWidth=1;
        auto ms=vk<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        auto ds=vk<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);ds.depthTestEnable=world;ds.depthWriteEnable=world && !additive;ds.depthCompareOp=compare;
        VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;
        if(additive){blend.blendEnable=VK_TRUE;blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;blend.colorBlendOp=VK_BLEND_OP_ADD;
                     blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;blend.alphaBlendOp=VK_BLEND_OP_ADD;}
        auto cb=vk<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);cb.attachmentCount=1;cb.pAttachments=&blend;
        VkDynamicState dyn[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};auto dy=vk<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);dy.dynamicStateCount=2;dy.pDynamicStates=dyn;
        VkFormat cf=VK_FORMAT_R8G8B8A8_UNORM;auto ri=vk<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);ri.colorAttachmentCount=1;ri.pColorAttachmentFormats=&cf;ri.depthAttachmentFormat=withDepth?VK_FORMAT_D32_SFLOAT:VK_FORMAT_UNDEFINED;
        auto ci=vk<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);ci.pNext=&ri;ci.stageCount=2;ci.pStages=st;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vps;
        ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&cb;ci.pDynamicState=&dy;ci.layout=layout;
        VkPipeline p{};ok(A(vkCreateGraphicsPipelines)(device,{},1,&ci,nullptr,&p));return p;};
    auto discPipeline=pipeline(1,1,1,true,true),warmDiscPipeline=pipeline(1,1,.75f,true,true);   // white midday / yellow morning sun
    auto ground=pipeline(.35f,.3f,.2f,true,true),nearWall=pipeline(.8f,.1f,.1f,true,true),farWall=pipeline(.1f,.7f,.1f,true,true),canopyPipeline=pipeline(.1f,.3f,.1f,true,true);
    bool canopy=false,block=false,ridge=false,board=false;
    auto particlePipeline=pipeline(.35f,.2f,.05f,true,true,true),layerPipeline=pipeline(.2f,.1f,0,true,true,true,VK_COMPARE_OP_EQUAL);
    bool particle=false,materialLayer=false;   // transparent-fog test: the particle quad / an additive layer on the far wall's own geometry
    bool skySplit=false;   // the sky drawn in a pass of its own after the world (depth range .998..1), on the same targets
    bool sunDisc=false;    // a white sun disc in the sky
    bool warmDisc=false;   // ... drawn yellow (1, 1, .75): the morning sun of Booty Bay the white-only finder missed
    auto ui=pipeline(1,0,1,false,false),uiOnWorld=pipeline(1,0,1,false,true);

    auto poolInfo=vk<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool pool{};ok(A(vkCreateCommandPool)(device,&poolInfo,nullptr,&pool));
    auto ca=vk<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd{};ok(A(vkAllocateCommandBuffers)(device,&ca,&cmd));
    auto fi=vk<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);VkFence fence{};ok(D(vkCreateFence)(device,&fi,nullptr,&fence));

    auto& state=devices.at(device);
    scene_debugger_state(state).sceneDepthImage=id(depth);
    fog_state(state).sceneDepthImage=id(depth);
    const VkViewport worldViewport{.5f,H+.5f,float(W),-float(H),0,.94f},uiViewport{0,0,float(W),float(H),0,1};
    const VkRect2D scissor{{0,0},{W,H}};
    const float sky[3]{.2f,.3f,.5f};
    auto rendering=[&](bool clear,bool withDepth){
        auto c=vk<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO),d=c;
        c.imageView=colorView;c.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;d.imageView=depthView;d.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        for(auto* a:{&c,&d}){a->loadOp=clear?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD;a->storeOp=VK_ATTACHMENT_STORE_OP_STORE;}
        for(int i=0;i<3;++i)c.clearValue.color.float32[i]=sky[i];c.clearValue.color.float32[3]=1;d.clearValue.depthStencil.depth=1;
        auto r=vk<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);r.renderArea.extent={W,H};r.layerCount=1;r.colorAttachmentCount=1;r.pColorAttachments=&c;r.pDepthAttachment=withDepth?&d:nullptr;
        A(vkCmdBeginRendering)(cmd,&r);};
    auto drawQuad=[&](VkPipeline p,uint32_t first){A(vkCmdBindPipeline)(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p);A(vkCmdDraw)(cmd,6,1,first,0);};
    auto barrier=[&](VkImage img,VkImageAspectFlags aspect,VkImageLayout from,VkImageLayout to){
        auto b=vk<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);b.srcAccessMask=b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout=from;b.newLayout=to;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=img;b.subresourceRange={aspect,0,1,0,1};
        A(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);};
    enum class Ui { Separate, SharesWorldTargets };
    const uint32_t keepView=99;   // frame(keepView, ...): leave the fog view as the layer set it
    auto frame=[&](uint32_t fogMode,bool splitWorld,Ui uiKind){
        if(fogMode!=keepView)camera_hud_input().fogMode=fogMode;
        {std::lock_guard lock(mutex);fog_present(state);}   // loads fog.ini, closes the previous frame's bookkeeping
        auto bi=vk<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ok(A(vkBeginCommandBuffer)(cmd,&bi));
        barrier(color,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        barrier(depth,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkDeviceSize zero=0;
        rendering(true,true);
        A(vkCmdSetViewport)(cmd,0,1,&worldViewport);A(vkCmdSetScissor)(cmd,0,1,&scissor);A(vkCmdBindVertexBuffers)(cmd,0,1,&vertexBuffer,&zero);
        drawQuad(ground,0);drawQuad(nearWall,6);
        if(splitWorld){A(vkCmdEndRendering)(cmd);rendering(false,true);A(vkCmdSetViewport)(cmd,0,1,&worldViewport);}
        drawQuad(farWall,12);
        if(materialLayer)drawQuad(layerPipeline,12);   // WoW's second material pass: same geometry, EQUAL test
        if(particle)drawQuad(particlePipeline,54);
        if(canopy)for(uint32_t q=0;q<4;++q)drawQuad(canopyPipeline,24+6*q);
        if(block)drawQuad(canopyPipeline,60);
        if(ridge)drawQuad(canopyPipeline,66);
        if(board)drawQuad(canopyPipeline,72);
        if(sunDisc){const VkViewport skyViewport{.5f,H+.5f,float(W),-float(H),.998f,1.f};A(vkCmdSetViewport)(cmd,0,1,&skyViewport);
                    drawQuad(warmDisc?warmDiscPipeline:discPipeline,48);A(vkCmdSetViewport)(cmd,0,1,&worldViewport);}
        if(skySplit){A(vkCmdEndRendering)(cmd);rendering(false,true);const VkViewport skyViewport{.5f,H+.5f,float(W),-float(H),.998f,1.f};
                     A(vkCmdSetViewport)(cmd,0,1,&skyViewport);drawQuad(farWall,12);}
        A(vkCmdEndRendering)(cmd);
        rendering(false,uiKind==Ui::SharesWorldTargets);
        A(vkCmdSetViewport)(cmd,0,1,&uiViewport);drawQuad(uiKind==Ui::SharesWorldTargets?uiOnWorld:ui,18);
        A(vkCmdEndRendering)(cmd);
        barrier(color,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={W,H,1};
        if(!bench)D(vkCmdCopyImageToBuffer)(cmd,color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);   // bench: keep the GPU busy
        ok(A(vkEndCommandBuffer)(cmd));
        auto si=vk<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);si.commandBufferCount=1;si.pCommandBuffers=&cmd;
        ok(D(vkResetFences)(device,1,&fence));ok(A(vkQueueSubmit)(queue,1,&si,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));
        std::vector<uint8_t> out(bench?0:W*H*4);std::memcpy(out.data(),rmap,out.size());return out;};
    auto px=[&](const std::vector<uint8_t>& img,unsigned x,unsigned y,int c){return img[(y*W+x)*4+c]/255.f;};

    // Bench: the fog as the user plays it (fog.ini of 2026-09-26: high preset, camera-relative base, sun 12, shadows 200 yd,
    // smoke 12 yd with wind, temporal) at 1920x1080 over ground, a near wall, a far wall and sky; 400 frames so the
    // smoothed per-stage GPU times settle. DEPTH_PROBE_FOG_BENCH_PRESET=low|medium|high overrides the preset, DEPTH_PROBE_FOG_BENCH_INI="key=value" appends a line. No checks.
    if(bench){
        const char* preset=std::getenv("DEPTH_PROBE_FOG_BENCH_PRESET");
        {std::ofstream o(ini);o<<offDefaults<<"preset="<<(preset?preset:"high")<<"\nmax_distance=400\ndistribution=8\nbase_density=0.02\nheight_falloff=0.04\nheight_reference=camera\n"
            "base_height=-12\nmax_extinction=1\nalbedo=0.9,0.9,0.92\nanisotropy=0.35\nambient=0.25,0.28,0.33\nsun_enabled=1\nsun_source=manual\n"
            "sun_direction=1,0,0.6\nsun_color=12,11.28,9.72\ntemporal_weight=0.9\nnoise_amount=0.3\nnoise_scale=12\nnoise_wind=1\nshadows=1\n"
            "shadow_length=200\nsky_light=0.85\nopen_sun=0.25\n";
         if(const char* extra=std::getenv("DEPTH_PROBE_FOG_BENCH_INI"))o<<extra<<"\n";}   // e.g. "noise_amount=0": what one feature costs
        auto& st=fog_state(state);st.settingsTime={};st.presentsSinceSettingsCheck=59;
        const auto started=std::chrono::steady_clock::now();
        for(int i=0;i<400;++i)frame(1,false,Ui::Separate);
        const double frameMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()/400;
        std::printf("fog bench: %.2f ms per harness frame (GPU busy share = fog total / this)\n",frameMs);
        std::printf("fog bench %ux%u grid %ux%ux%u (%s): GPU ms memories %.3f (clear %.3f scatter %.3f sun find %.3f) inject %.3f light %.3f integrate %.3f "
                    "composite %.3f total %.3f | shadows %s, history %s\n",W,H,st.lastGrid[0],st.lastGrid[1],st.lastGrid[2],preset?preset:"high",
                    fog_gpu_memories(st),st.gpuMemoryClear,st.gpuMemoryScatter,st.gpuSunFind,st.gpuInject,st.gpuLight,st.gpuIntegrate,st.gpuComposite,fog_gpu_total(st),
                    st.lastShadowProblem.empty()?"marching":st.lastShadowProblem.c_str(),st.lastHistoryUsed?"used":"reset");
        camera_hud_input().fogMode=0;
        {std::lock_guard lock(mutex);fog_destroy(state);}
        std::remove(ini.c_str());
        ok(D(vkDeviceWaitIdle)(device));
        {std::lock_guard lock(mutex);devices.erase(device);}
        return 0;
    }

    const auto base=frame(0,false,Ui::Separate);
    // UI quad (NDC x,y in [-1,-.6]: top-left with the UI's positive viewport) is magenta; sky is the clear color.
    CHECK(px(base,5,5,0)>.99f && px(base,5,5,1)<.01f);
    const auto fogged=frame(1,false,Ui::Separate);
    auto& st=fog_state(state);
    {std::lock_guard lock(mutex);fog_present(state);}
    CHECK(st.lastComposites==1 && st.lastDropped==0);
    CHECK(st.computeSubmits>=1);
    CHECK(st.lastTrigger=="next pass on other targets");
    // Semantic draw classes (draw_classifier.cpp, record time, nothing drawn differently): one frame of this scene is the ground
    // and two walls (world depth range, depth write, no blend: OPAQUE on the scene target, alpha test unknown - no DXVK shader)
    // and the UI quad (full range, no depth test, drawn to a target without depth: UI elsewhere).
    auto classCounts=[&]{std::lock_guard lock(mutex);CHECK(state.drawClasses);return std::make_pair(state.drawClasses->scene,state.drawClasses->other);};
    auto classDelta=[&](const auto& before,const auto& after,DrawClass c,bool scene){const size_t k=size_t(c);return (scene?after.first[k]-before.first[k]:after.second[k]-before.second[k]);};
    {
        const auto before=classCounts();frame(1,false,Ui::Separate);const auto after=classCounts();
        CHECK(classDelta(before,after,DrawClass::Opaque,true)==3 && classDelta(before,after,DrawClass::Ui,false)==1);
        uint64_t all=0;for(size_t k=0;k<size_t(DrawClass::Count);++k)all+=after.first[k]-before.first[k]+after.second[k]-before.second[k];
        CHECK(all==4);
    }
    // UI untouched, bit for bit.
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)if(px(base,x,y,0)>.99f && px(base,x,y,1)<.01f && px(base,x,y,2)>.99f)
        for(int c=0;c<4;++c)CHECK(base[(y*W+x)*4+c]==fogged[(y*W+x)*4+c]);

    // CPU reference: march each ray through the same medium with fine analytic slabs, same lighting model.
    const auto grid=fog::grid_for(fog::Preset::Medium,W,H,400,8);
    const auto& set=st.settings;
    // Analytic sun visibility for the shadow test: the segment from p toward the sun (from .5 to .5 + length yards, as the
    // GPU marches) against the two walls.
    // The canopy (plate at z 50 minus its hole) for the canopy test.
    auto underCanopy=[&](float x,float y){return x>=105 && x<=170 && y>=150 && y<=260 && !(x>130 && x<145 && y>195 && y<212);};
    auto sunVisible=[&](float px_,float py,float pz,float length,bool withCanopy=false){
        const float l=std::sqrt(set.sunDirection[0]*set.sunDirection[0]+set.sunDirection[1]*set.sunDirection[1]+set.sunDirection[2]*set.sunDirection[2]);
        const float ux=set.sunDirection[0]/l,uy=set.sunDirection[1]/l,uz=set.sunDirection[2]/l;
        for(auto [wx,y0,y1,z1]:{std::array<float,4>{130,206,220,40},std::array<float,4>{400,150,250,60}})if(ux>0){
            const float t=(wx-px_)/ux;if(t<.5f || t>.5f+length)continue;
            const float hy=py+uy*t,hz=pz+uz*t;if(hy>=y0 && hy<=y1 && hz>=0 && hz<=z1)return 0.f;}
        if(withCanopy && pz<50 && uz>0){const float t=(50-pz)/uz;if(t<=.5f+length && underCanopy(px_+ux*t,py+uy*t))return 0.f;}
        return 1.f;};
    auto reference=[&](unsigned x,unsigned y,std::array<float,3>& result,float& transmittance,float surface=1e9f,float shadowLength=0,bool withCanopy=false,float skyShare=0){
        const float ndcX=((x+.5f)-worldViewport.x)/worldViewport.width*2-1,ndcY=((y+.5f)-worldViewport.y)/worldViewport.height*2-1;
        const Vec3 v{ndcX/cam.projection.c0.x,ndcY/cam.projection.c1.y,1};
        const Vec3 dir{cam.right.x*v.x+cam.up.x*v.y+cam.forward.x,cam.right.y*v.x+cam.up.y*v.y+cam.forward.y,cam.right.z*v.x+cam.up.z*v.y+cam.forward.z};
        // View depth of the first surface along the ray (analytic: ground z=0 and the two walls at fixed x).
        float z=1e9f;
        if(dir.z<0){float t=-position.z/dir.z;float gx=position.x+dir.x*t,gy=position.y+dir.y*t;if(gx>=101 && gx<=700 && gy>=-400 && gy<=800)z=std::min(z,t);}
        for(auto [wx,y0,y1,z1]:{std::array<float,4>{130,206,220,40},std::array<float,4>{400,150,250,60}})if(dir.x>0){
            float t=(wx-position.x)/dir.x;float hy=position.y+dir.y*t,hz=position.z+dir.z*t;if(hy>=y0 && hy<=y1 && hz>=0 && hz<=z1)z=std::min(z,t);}
        const float end=std::min({z,400.f,surface});
        const float len=std::sqrt(dir.x*dir.x+dir.y*dir.y+dir.z*dir.z);const Vec3 unit{dir.x/len,dir.y/len,dir.z/len};
        const float cosTheta=unit.x*set.sunDirection[0]+unit.y*set.sunDirection[1]+unit.z*set.sunDirection[2];
        const float phase=fog::sun_phase(cosTheta,set.anisotropy,set.sunAureole,set.sunAureoleG);
        fog::Accumulated acc;const int steps=4000;
        for(int i=0;i<steps;++i){
            const float d0=end*i/steps,d1=end*(i+1)/steps,mid=.5f*(d0+d1);
            const float sigma=fog::extinction_at(set.medium,position.z+dir.z*mid);
            const float qx=position.x+dir.x*mid,qy=position.y+dir.y*mid,qz=position.z+dir.z*mid;
            const float visible=shadowLength>0?sunVisible(qx,qy,qz,shadowLength,withCanopy):1.f;
            const float sky=withCanopy && qz<50 && underCanopy(qx,qy)?0.f:1.f;   // sky light from straight above
            float S[3];for(int c=0;c<3;++c)S[c]=set.medium.albedo[c]*sigma*(set.ambient[c]*(1-skyShare+skyShare*sky)+phase*set.sunColor[c]*visible);
            fog::accumulate(acc,fog::integrate_slab(S,sigma,(d1-d0)*len));
        }
        for(int c=0;c<3;++c)result[c]=acc.scatter[c];transmittance=acc.transmittance;};
    // Pixels at froxel column centres (XY interpolation is the grid's resolution, not what this checks).
    unsigned checked=0;double worst=0;
    for(unsigned j=0;j<grid.y;++j)for(unsigned i=0;i<grid.x;++i){
        const unsigned x=unsigned((i+.5f)/grid.x*W),y=unsigned((1-(j+.5f)/grid.y)*H);
        if(x<W*.2f && y<H*.2f)continue;   // the UI corner
        std::array<float,3> in;float T;reference(x,y,in,T);
        for(int c=0;c<3;++c){
            const float expected=std::min(px(base,x,y,c)*T+in[c],1.f),got=px(fogged,x,y,c);
            worst=std::max(worst,double(std::fabs(expected-got)));
        }
        ++checked;
    }
    std::fprintf(stderr,"fog vs CPU reference: %u froxel-centre pixels, worst |error| %.4f (8-bit step %.4f)\n",checked,worst,1/255.0);
    CHECK(checked>=80);
    CHECK(worst<6/255.0);
    // Window pixel of a world point, through the same camera and viewport.
    auto pixelOf=[&](float x,float y,float z){auto c=mat4_mul_vec4(vp,Vec4{x,y,z,1});
        return std::array<unsigned,2>{unsigned(worldViewport.x+(c.x/c.w*.5f+.5f)*worldViewport.width),unsigned(worldViewport.y+(c.y/c.w*.5f+.5f)*worldViewport.height)};};
    const auto nearPx=pixelOf(130,213,20),farPx=pixelOf(400,200,30),groundPx=pixelOf(250,200,0);
    for(auto p:{nearPx,farPx,groundPx})std::fprintf(stderr,"pixel %u,%u base %.2f %.2f %.2f\n",p[0],p[1],px(base,p[0],p[1],0),px(base,p[0],p[1],1),px(base,p[0],p[1],2));
    CHECK(px(base,nearPx[0],nearPx[1],0)>.7f && px(base,farPx[0],farPx[1],1)>.6f);   // red near wall, green far wall
    // Fog grows with distance: the far wall changed more than the near wall.
    auto change=[&](std::array<unsigned,2> p){float d=0;for(int c=0;c<3;++c)d+=std::fabs(px(fogged,p[0],p[1],c)-px(base,p[0],p[1],c));return d;};
    CHECK(change(farPx)>change(nearPx)*2);

    // Transmittance view: 1 at the camera, lower with distance (near wall 30 yd > ground 150 yd > far wall 300 yd).
    const auto transmittance=frame(6,false,Ui::Separate);
    const float tNear=px(transmittance,nearPx[0],nearPx[1],0),tGround=px(transmittance,groundPx[0],groundPx[1],0),tFar=px(transmittance,farPx[0],farPx[1],0);
    std::fprintf(stderr,"transmittance near %.3f ground %.3f far %.3f\n",tNear,tGround,tFar);
    CHECK(tNear>tGround && tGround>tFar);
    CHECK(px(transmittance,5,5,0)>.99f && px(transmittance,5,5,1)<.01f);   // UI still untouched in debug views

    // Split world (DXVK flush mid-pass): fogged once, identical to the single pass.
    const auto split=frame(1,true,Ui::Separate);
    {std::lock_guard lock(mutex);fog_present(state);}
    CHECK(st.lastComposites==1 && st.lastWorldPasses==2);
    for(size_t i=0;i<split.size();++i)CHECK(std::abs(int(split[i])-int(fogged[i]))<=1);

    // UI drawn on the world's own color+depth targets: the fog is dropped, never drawn over the UI.
    const auto shared=frame(1,false,Ui::SharesWorldTargets);
    {std::lock_guard lock(mutex);fog_present(state);}
    CHECK(st.lastComposites==0 && st.lastDropped==1);
    for(size_t i=0;i<shared.size();++i)CHECK(shared[i]==base[i]);

    // Water: the replay's mask says a water surface lies at view depth 20 on every pixel (WoW water writes no depth,
    // the depth buffer holds the seabed). The fog must stop at the surface: 20 yards everywhere, not the scene depth.
    {
        VkImage mask{};VkImageView maskView{};
        auto i=vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);i.imageType=VK_IMAGE_TYPE_2D;i.format=VK_FORMAT_R32G32B32A32_SFLOAT;i.extent={W,H,1};i.mipLevels=i.arrayLayers=1;
        i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ok(D(vkCreateImage)(device,&i,nullptr,&mask));VkMemoryRequirements r{};D(vkGetImageMemoryRequirements)(device,mask,&r);ok(D(vkBindImageMemory)(device,mask,allocate(r,0),0));
        auto v=vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);v.image=mask;v.format=i.format;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};ok(D(vkCreateImageView)(device,&v,nullptr,&maskView));
        auto bi=vk<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ok(D(vkBeginCommandBuffer)(cmd,&bi));
        auto b=vk<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=mask;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkClearColorValue surface{};surface.float32[2]=20;surface.float32[3]=1;const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        D(vkCmdClearColorImage)(cmd,mask,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&surface,1,&whole);
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        ok(D(vkEndCommandBuffer)(cmd));auto si=vk<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);si.commandBufferCount=1;si.pCommandBuffers=&cmd;
        ok(D(vkResetFences)(device,1,&fence));ok(D(vkQueueSubmit)(queue,1,&si,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));
        auto& water=water_state(state);++water.maskSerial;water.maskColorImage=id(color);water.maskView=maskView;
        const auto overWater=frame(1,false,Ui::Separate);
        {std::lock_guard lock(mutex);fog_present(state);}
        CHECK(st.lastComposites==1 && st.lastWaterSurface);
        double waterWorst=0;unsigned waterChecked=0;
        for(unsigned j=0;j<grid.y;++j)for(unsigned i2=0;i2<grid.x;++i2){
            const unsigned x=unsigned((i2+.5f)/grid.x*W),y=unsigned((1-(j+.5f)/grid.y)*H);
            if(x<W*.2f && y<H*.2f)continue;
            std::array<float,3> in;float T;reference(x,y,in,T,20.f);
            for(int c=0;c<3;++c)waterWorst=std::max(waterWorst,double(std::fabs(std::min(px(base,x,y,c)*T+in[c],1.f)-px(overWater,x,y,c))));
            ++waterChecked;
        }
        std::fprintf(stderr,"fog stopped at the water surface: %u pixels, worst |error| %.4f\n",waterChecked,waterWorst);
        CHECK(waterWorst<6/255.0);
        // Far wall seen "through" water at 20 yd gets far less fog than without the surface.
        CHECK(change(farPx)>0);
        float lessFog=0;for(int c=0;c<3;++c)lessFog+=std::fabs(px(overWater,farPx[0],farPx[1],c)-px(base,farPx[0],farPx[1],c));
        CHECK(lessFog<change(farPx)*.5f);
        // Water with nothing behind it in the depth buffer (the sky's depth: open sea, no seabed in view) is a surface like
        // any other - sky_fog must not thin its fog. Booty Bay at sky_fog .3: the seabed and the pier posts under the water
        // showed through as the edges between full fog (geometry behind the water) and thinned fog (nothing behind it).
        {
            std::string settings;{std::ifstream in(ini);std::stringstream text;text<<in.rdbuf();settings=text.str();}
            {std::ofstream o(ini,std::ios::app);o<<"sky_fog=0\n";}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            ++water.maskSerial;
            const auto noSkyFog=frame(1,false,Ui::Separate);
            {std::ofstream o(ini);o<<settings;}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            int worst=0;for(size_t k=0;k<noSkyFog.size();++k)worst=std::max(worst,std::abs(int(noSkyFog[k])-int(overWater[k])));
            std::fprintf(stderr,"water over the sky's depth with sky_fog 0 vs 1: max |diff| %d\n",worst);
            CHECK(worst<=1);
        }
        // max_opacity: the fog hides at most that share of what lies behind it, its light scaled with the opacity - the CPU
        // reference with the same cap (Booty Bay: a shaft-dense fog erased the far side of the harbour into a bright veil).
        {
            std::string settings;{std::ifstream in(ini);std::stringstream text;text<<in.rdbuf();settings=text.str();}
            {std::ofstream o(ini,std::ios::app);o<<"max_opacity=0.3\n";}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            const auto capped=frame(1,false,Ui::Separate);
            {std::ofstream o(ini);o<<settings;}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            double cappedWorst=0;unsigned cappedPixels=0,cappedChecked=0;
            for(unsigned j=0;j<grid.y;++j)for(unsigned i2=0;i2<grid.x;++i2){
                const unsigned x=unsigned((i2+.5f)/grid.x*W),y=unsigned((1-(j+.5f)/grid.y)*H);
                if(x<W*.2f && y<H*.2f)continue;
                std::array<float,3> in;float T;reference(x,y,in,T);
                const float opacity=1-T,scale=opacity>.3f?.3f/opacity:1.f;
                cappedPixels+=opacity>.3f;++cappedChecked;
                for(int c=0;c<3;++c)cappedWorst=std::max(cappedWorst,double(std::fabs(std::min(px(base,x,y,c)*(1-opacity*scale)+in[c]*scale,1.f)-px(capped,x,y,c))));
            }
            std::fprintf(stderr,"max_opacity 0.3: %u of %u pixels capped, worst |error| %.4f\n",cappedPixels,cappedChecked,cappedWorst);
            CHECK(cappedPixels>=20 && cappedWorst<6/255.0);
        }
        // light_limit: the light the fog adds is bent toward the ceiling above half of it (fog::soft_limit on its brightest
        // channel, colour kept) - the CPU reference with the same limit (Shadowglen: looking into the sun went white).
        {
            std::string settings;{std::ifstream in(ini);std::stringstream text;text<<in.rdbuf();settings=text.str();}
            {std::ofstream o(ini,std::ios::app);o<<"light_limit=0.1\n";}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            ++water.maskSerial;water.maskView=VkImageView{};
            const auto limited=frame(1,false,Ui::Separate);
            {std::ofstream o(ini);o<<settings;}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            double limitWorst=0;unsigned limitedPixels=0,limitChecked=0;
            for(unsigned j=0;j<grid.y;++j)for(unsigned i2=0;i2<grid.x;++i2){
                const unsigned x=unsigned((i2+.5f)/grid.x*W),y=unsigned((1-(j+.5f)/grid.y)*H);
                if(x<W*.2f && y<H*.2f)continue;
                std::array<float,3> in;float T;reference(x,y,in,T);
                const float peak=std::max({in[0],in[1],in[2]}),scale=peak>0?fog::soft_limit(peak,.1f)/peak:1.f;
                limitedPixels+=scale<.999f;++limitChecked;
                for(int c=0;c<3;++c)limitWorst=std::max(limitWorst,double(std::fabs(std::min(px(base,x,y,c)*T+in[c]*scale,1.f)-px(limited,x,y,c))));
            }
            std::fprintf(stderr,"light_limit 0.1: %u of %u pixels bent, worst |error| %.4f\n",limitedPixels,limitChecked,limitWorst);
            CHECK(limitedPixels>=20 && limitWorst<6/255.0);
        }
        // sun_aureole: a narrow forward lobe mixed into the sun's phase (fog::sun_phase) - the CPU reference with the same phase
        // (the test camera looks toward the sun: the lobe matters here).
        {
            std::string settings;{std::ifstream in(ini);std::stringstream text;text<<in.rdbuf();settings=text.str();}
            {std::ofstream o(ini,std::ios::app);o<<"sun_aureole=0.3\nsun_aureole_g=0.9\n";}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            const auto aureole=frame(1,false,Ui::Separate);
            double worst=0;unsigned changed=0,checked=0;
            for(unsigned j=0;j<grid.y;++j)for(unsigned i2=0;i2<grid.x;++i2){
                const unsigned x=unsigned((i2+.5f)/grid.x*W),y=unsigned((1-(j+.5f)/grid.y)*H);
                if(x<W*.2f && y<H*.2f)continue;
                std::array<float,3> in;float T;reference(x,y,in,T);   // st.settings has the aureole now
                bool differs=false;
                for(int c=0;c<3;++c){worst=std::max(worst,double(std::fabs(std::min(px(base,x,y,c)*T+in[c],1.f)-px(aureole,x,y,c))));differs|=std::abs(int(aureole[(size_t(y)*W+x)*4+c])-int(fogged[(size_t(y)*W+x)*4+c]))>2;}
                changed+=differs;++checked;
            }
            {std::ofstream o(ini);o<<settings;}st.settingsTime={};st.presentsSinceSettingsCheck=59;frame(1,false,Ui::Separate);
            std::fprintf(stderr,"sun_aureole 0.3: %u of %u pixels changed, worst |error| vs the CPU reference %.4f\n",changed,checked,worst);
            CHECK(changed>=10 && worst<6/255.0);
        }
        // sun_glare: the eye's glare around the drawn disc by its visible share - over the sky near the sun (and whatever stands
        // there), nothing far from it, nothing when the disc is behind the far wall; the fog itself unchanged (sun_glare 0 = as
        // before). The sun 10 degrees up is above the far wall (its top ~5.7 degrees up from the camera), 2 degrees is behind it.
        {
            std::string settings;{std::ifstream in(ini);std::stringstream text;text<<in.rdbuf();settings=text.str();}
            auto run=[&](const char* extra){{std::ofstream o(ini);o<<settings<<"sun_source=manual\n"<<extra;}st.settingsTime={};st.presentsSinceSettingsCheck=59;frame(1,false,Ui::Separate);return frame(1,false,Ui::Separate);};
            const auto upPlain=run("sun_direction=0.985,0,0.174\nsun_glare=0\n");
            const auto upGlare=run("sun_direction=0.985,0,0.174\nsun_glare=1\n");
            const auto behindPlain=run("sun_direction=0.9994,0,0.035\nsun_glare=0\n");
            const auto behindGlare=run("sun_direction=0.9994,0,0.035\nsun_glare=1\n");
            // half hidden: the disc's centre on the wall's top edge - the glare reaches down over the wall (2 degrees below it)
            const auto edgePlain=run("sun_direction=0.9951,0,0.0993\nsun_glare=0\n");
            const auto edgeGlare=run("sun_direction=0.9951,0,0.0993\nsun_glare=1\n");
            {std::ofstream o(ini);o<<settings;}st.settingsTime={};st.presentsSinceSettingsCheck=59;frame(1,false,Ui::Separate);
            // near: the sky 3 degrees above the disc (the disc itself is left to WoW: its colour must not be dimmed)
            const auto sunPx=pixelOf(100+985,200,30+174),nearPx=pixelOf(100+974,200,30+225),groundPx2=pixelOf(250,260,0);
            float nearGain=0,discDiff=0;
            for(int c=0;c<3;++c){nearGain+=px(upGlare,nearPx[0],nearPx[1],c)-px(upPlain,nearPx[0],nearPx[1],c);discDiff+=std::fabs(px(upGlare,sunPx[0],sunPx[1],c)-px(upPlain,sunPx[0],sunPx[1],c));}
            int farDiff=0;for(int c=0;c<3;++c)farDiff=std::max(farDiff,std::abs(int(upGlare[(size_t(groundPx2[1])*W+groundPx2[0])*4+c])-int(upPlain[(size_t(groundPx2[1])*W+groundPx2[0])*4+c])));
            int uiDiff=0;for(unsigned y=0;y<H*.15f;++y)for(unsigned x=0;x<W*.15f;++x)for(int c=0;c<3;++c)uiDiff=std::max(uiDiff,std::abs(int(upGlare[(size_t(y)*W+x)*4+c])-int(upPlain[(size_t(y)*W+x)*4+c])));
            int behindDiff=0;for(size_t k=0;k<behindGlare.size();++k)behindDiff=std::max(behindDiff,std::abs(int(behindGlare[k])-int(behindPlain[k])));
            const auto wallPx=pixelOf(100+998,200,30+64.5f);
            float wallGain=0;for(int c=0;c<3;++c)wallGain+=px(edgeGlare,wallPx[0],wallPx[1],c)-px(edgePlain,wallPx[0],wallPx[1],c);
            std::fprintf(stderr,"sun_glare: the sky 3 degrees from the sun +%.3f (sum of channels), the disc |diff| %.3f, far ground |diff| %d, UI corner |diff| %d; "
                         "sun behind the wall: frame |diff| %d; half behind its top edge: the wall 2 degrees below +%.3f\n",nearGain,discDiff,farDiff,uiDiff,behindDiff,wallGain);
            CHECK(nearGain>.1f && discDiff<2/255.f && farDiff==0 && uiDiff==0 && behindDiff==0 && wallGain>.05f);
        }
        // sun_rays: the beams' structure only - where the disc is half behind an edge (the near wall's right edge, 11.3 degrees
        // left, the disc 10 degrees up) light fans past it; a disc alone in open sky gives next to nothing (no smooth glow); the
        // disc itself and the UI stay; with the disc behind the camera nothing changes (bit for bit).
        {
            std::string settings;{std::ifstream in(ini);std::stringstream text;text<<in.rdbuf();settings=text.str();}
            auto run=[&](const char* extra){{std::ofstream o(ini);o<<settings<<"sun_source=manual\n"<<extra;}st.settingsTime={};st.presentsSinceSettingsCheck=59;frame(1,false,Ui::Separate);return frame(1,false,Ui::Separate);};
            const auto edgePlain=run("sun_direction=0.9657,0.1930,0.1736\nsun_rays=0\n");
            const auto edgeRays=run("sun_direction=0.9657,0.1930,0.1736\nsun_rays=1.5\n");
            const auto openPlain=run("sun_direction=0.9224,-0.3357,0.1908\nsun_rays=0\n");   // 11 up, 20 right: past the far wall's end
            const auto openRays=run("sun_direction=0.9224,-0.3357,0.1908\nsun_rays=1.5\n");
            const auto backPlain=run("sun_direction=-0.985,0,0.174\nsun_rays=0\n");
            const auto backRays=run("sun_direction=-0.985,0,0.174\nsun_rays=1.5\n");
            {std::ofstream o(ini);o<<settings;}st.settingsTime={};st.presentsSinceSettingsCheck=59;frame(1,false,Ui::Separate);
            auto maxGain=[&](const std::vector<uint8_t>& with,const std::vector<uint8_t>& without){
                float best=0;for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){float g=0;for(int c=0;c<3;++c)g+=px(with,x,y,c)-px(without,x,y,c);best=std::max(best,g);}return best;};
            const float edgeGain=maxGain(edgeRays,edgePlain),openGain=maxGain(openRays,openPlain);
            const auto discPx=pixelOf(100+922.4f,200-335.7f,30+190.8f);
            const bool discOnScreen=discPx[0]<W && discPx[1]<H;
            float discDiff=0;if(discOnScreen)for(int c=0;c<3;++c)discDiff+=std::fabs(px(openRays,discPx[0],discPx[1],c)-px(openPlain,discPx[0],discPx[1],c));
            int uiDiff=0;for(unsigned y=0;y<H*.15f;++y)for(unsigned x=0;x<W*.15f;++x)for(int c=0;c<3;++c)uiDiff=std::max(uiDiff,std::abs(int(edgeRays[(size_t(y)*W+x)*4+c])-int(edgePlain[(size_t(y)*W+x)*4+c])));
            int backDiff=0;for(size_t k=0;k<backRays.size();++k)backDiff=std::max(backDiff,std::abs(int(backRays[k])-int(backPlain[k])));
            std::fprintf(stderr,"sun_rays 1.5: brightest beam past the near wall's edge +%.3f (sum of channels), in open sky +%.3f; the disc |diff| %.3f, UI corner |diff| %d; disc behind the camera: frame |diff| %d\n",
                         edgeGain,openGain,discDiff,uiDiff,backDiff);
            CHECK(discOnScreen && edgeGain>.1f && edgeGain>3*openGain && discDiff<2/255.f && uiDiff==0 && backDiff==0);
        }
        // Scene shading (fog_shade.frag), with a block on the ground 45 yd ahead and the sun low (30 degrees) behind it: off = bit
        // for bit as before; ambient occlusion darkens the ground at the block's foot, not open ground; contact shadows the
        // ground right in front of it. Terrain shadows: a ridge of land in its place shadows the ground inside its 10 yd shadow
        // (beyond the contact march), not beside; a board floating over the ground there (high over the floor memory, edges
        // near, in no game map - like a name or an icon writing depth) casts none.
        {
            std::string settings;{std::ifstream in(ini);std::stringstream text;text<<in.rdbuf();settings=text.str();}
            block=true;
            auto run=[&](const char* extra){{std::ofstream o(ini);o<<settings<<"sun_source=manual\nsun_direction=0.866,0,0.5\nauto_exposure=0\n"<<extra;}st.settingsTime={};st.presentsSinceSettingsCheck=59;frame(1,false,Ui::Separate);return frame(1,false,Ui::Separate);};
            const auto plain=run("");
            const auto zero=run("contact_shadows=0\nambient_occlusion=0\nterrain_shadows=0\n");
            const auto ao=run("ambient_occlusion=1\n");
            const auto contact=run("contact_shadows=1\n");
            // Point-light shadows: a light 4 yd behind the block (x 149, 3 yd up; injected - in game the torches from the lit
            // models): the ground 3 yd in front of the block is in its shadow, 14 yd beside it is not; off = the frame as it was.
            st.pointLightsInjected=std::vector<std::array<float,3>>{{149.f,200.f,3.f}};
            const auto pointPlain=run("");
            const auto pointOn=run("point_shadows=1\npoint_shadow_range=30\n");
            const auto pointShort=run("point_shadows=1\npoint_shadow_range=30\npoint_shadow_length=2\n");   // gone 2 yd behind the block (the ground is 3.3 behind it)
            const auto pointLong=run("point_shadows=1\npoint_shadow_range=30\npoint_shadow_length=20\n");
            // Indoors (the game's cascades seen this session, none for 30+ frames - the Den): no sun shadows (contact), the
            // point light at full weight instead of the daylight 30%.
            const auto serial=state.frameSerial;
            state.frameSerial+=100;st.cascadesLastValid=serial+1;   // (the smoke's frames do not advance the present serial)
            const auto indoorContact=run("contact_shadows=1\n");
            const auto indoorPoint=run("point_shadows=1\npoint_shadow_range=30\n");
            state.frameSerial=serial;st.cascadesLastValid=0;
            // A light .8 yd behind the block (as in a brazier that is not centred on it): within point_shadow_ignore (1.5) the
            // block is its brazier and casts nothing; with .5 it casts.
            st.pointLightsInjected=std::vector<std::array<float,3>>{{145.8f,200.f,3.f}};
            const auto inBrazier=run("point_shadows=1\npoint_shadow_range=30\npoint_shadow_length=20\n");
            const auto inBrazierSmall=run("point_shadows=1\npoint_shadow_range=30\npoint_shadow_ignore=0.5\npoint_shadow_length=20\n");
            st.pointLightsInjected.reset();
            const auto pointFront=pixelOf(142,200,0),pointBeside=pixelOf(142,182,0);
            block=false;board=true;
            const auto boardPlain=run("");
            const auto boardTerrain=run("terrain_shadows=1\n");
            board=false;ridge=true;
            const auto ridgePlain=run("");
            const auto terrain=run("terrain_shadows=1\n");
            const auto terrainFrom80=run("terrain_shadows=1\nterrain_shadows_from=80\n");   // the ridge's shadow is ~40 yd away: none
            ridge=false;
            {std::ofstream o(ini);o<<settings;}st.settingsTime={};st.presentsSinceSettingsCheck=59;frame(1,false,Ui::Separate);
            int zeroDiff=0;for(size_t k=0;k<plain.size();++k)zeroDiff=std::max(zeroDiff,std::abs(int(plain[k])-int(zero[k])));
            auto lum=[&](const std::vector<uint8_t>& img,std::array<unsigned,2> q){float v=0;if(q[0]<W && q[1]<H)for(int c=0;c<3;++c)v+=px(img,q[0],q[1],c);return v;};
            const auto foot=pixelOf(144.4f,200,.05f),open=pixelOf(160,185,0),front=pixelOf(144.4f,200,0),inShadow=pixelOf(138,200,0),beside=pixelOf(138,185,0);
            bool onScreen=true;for(auto q:{foot,open,front,inShadow,beside})onScreen&=q[0]<W && q[1]<H;
            const float aoFoot=lum(ao,foot)-lum(plain,foot),aoOpen=lum(ao,open)-lum(plain,open);
            const float contactFront=lum(contact,front)-lum(plain,front),contactIn=lum(contact,inShadow)-lum(plain,inShadow);
            const float terrainIn=lum(terrain,inShadow)-lum(ridgePlain,inShadow),terrainBeside=lum(terrain,beside)-lum(ridgePlain,beside);
            const float blockIn=lum(boardTerrain,inShadow)-lum(boardPlain,inShadow),from80In=lum(terrainFrom80,inShadow)-lum(ridgePlain,inShadow);
            std::fprintf(stderr,"scene shading: off |diff| %d; ambient occlusion at the block's foot %+.3f, open ground %+.3f; contact shadow right in front %+.3f, 7 yd in front %+.3f; "
                         "terrain shadow of a ridge 7 yd in front %+.3f, beside %+.3f; of a name-like board floating 3.8 yd over the ground %+.3f; from 80 yd on %+.3f (sums of channels; pixels on screen %d)\n",
                         zeroDiff,aoFoot,aoOpen,contactFront,contactIn,terrainIn,terrainBeside,blockIn,from80In,int(onScreen));
            // (the 64-bit build lands the block's foot a little differently: -0.020 there, -0.024 in 32 bits)
            CHECK(onScreen && zeroDiff==0 && aoFoot<-.015f && std::fabs(aoOpen)<.01f && contactFront<-.02f && std::fabs(contactIn)<.01f && terrainIn<-.02f && std::fabs(terrainBeside)<.01f);
            CHECK(std::fabs(blockIn)<.01f && std::fabs(from80In)<.01f);
            {
                int pointOff=0;for(size_t k=0;k<plain.size();++k)pointOff=std::max(pointOff,std::abs(int(plain[k])-int(pointPlain[k])));
                const float inFront=lum(pointOn,pointFront)-lum(pointPlain,pointFront),besideIt=lum(pointOn,pointBeside)-lum(pointPlain,pointBeside);
                std::fprintf(stderr,"point-light shadows: light behind the block - the ground in front %+.3f, beside %+.3f; with no light / off |diff| %d (pixels on screen %d)\n",
                             inFront,besideIt,pointOff,int(pointFront[0]<W && pointFront[1]<H && pointBeside[0]<W && pointBeside[1]<H));
                CHECK(pointFront[0]<W && pointFront[1]<H && pointBeside[0]<W && pointBeside[1]<H && inFront<-.02f && std::fabs(besideIt)<.01f && pointOff==0);
                const float shortFront=lum(pointShort,pointFront)-lum(pointPlain,pointFront),longFront=lum(pointLong,pointFront)-lum(pointPlain,pointFront);
                std::fprintf(stderr,"point-light shadows 3.3 yd behind the block: length 2 yd %+.3f, 5 yd %+.3f, 20 yd %+.3f\n",shortFront,inFront,longFront);
                CHECK(std::fabs(shortFront)<.01f && longFront<inFront-.02f);
                const float indoorContactFront=lum(indoorContact,front)-lum(plain,front),indoorPointFront=lum(indoorPoint,pointFront)-lum(pointPlain,pointFront);
                std::fprintf(stderr,"indoors: contact shadow right in front %+.3f (outdoors %+.3f); point shadow %+.3f (outdoors %+.3f)\n",indoorContactFront,contactFront,indoorPointFront,inFront);
                CHECK(std::fabs(indoorContactFront)<.01f && indoorPointFront<inFront-.02f);
                const float brazier=lum(inBrazier,pointFront)-lum(pointPlain,pointFront),brazierSmall=lum(inBrazierSmall,pointFront)-lum(pointPlain,pointFront);
                // Grain: over the shadowed ground, the mean difference between horizontally neighbouring pixels of the shadow.
                double rough=0;int roughN=0;
                for(unsigned y=0;y<H;++y)for(unsigned x=0;x+1<W;++x){
                    const float a=lum(pointOn,{x,y})-lum(pointPlain,{x,y}),b=lum(pointOn,{x+1,y})-lum(pointPlain,{x+1,y});
                    if(a<-.01f || b<-.01f){rough+=std::fabs(a-b);++roughN;}}
                std::fprintf(stderr,"point-light shadows: grain (mean neighbour difference over %d shadowed pixels) %.4f\n",roughN,roughN?rough/roughN:0.);
                std::fprintf(stderr,"point-light shadows: light .8 yd behind the block - the ground in front %+.3f (ignore 1.5 yd), %+.3f (ignore .5 yd)\n",brazier,brazierSmall);
                CHECK(std::fabs(brazier)<.01f && brazierSmall<-.02f);
            }
        }
        // Auto exposure: a frame brighter than exposure_target is darkened - the whole fogged world by one factor (both terms of
        // the composite), down to exposure_min at most; the measurement comes back a few frames later.
        {
            std::string settings;{std::ifstream in(ini);std::stringstream text;text<<in.rdbuf();settings=text.str();}
            const auto plain=frame(1,false,Ui::Separate);
            {std::ofstream o(ini,std::ios::app);o<<"auto_exposure=1\nexposure_target=0.05\nexposure_min=0.5\n";}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            st.exposureValid=false;st.exposure=1;
            std::vector<uint8_t> darkened;for(int i=0;i<8;++i)darkened=frame(1,false,Ui::Separate);
            const float e=st.exposure;
            {std::ofstream o(ini);o<<settings;}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            double exposureWorst=0;unsigned exposureChecked=0;
            for(unsigned y=0;y<H;y+=3)for(unsigned x=0;x<W;x+=3){
                if(x<W*.2f && y<H*.2f)continue;
                for(int c=0;c<3;++c)if(px(plain,x,y,c)<.95f){exposureWorst=std::max(exposureWorst,double(std::fabs(px(plain,x,y,c)*e-px(darkened,x,y,c))));++exposureChecked;}
            }
            std::fprintf(stderr,"auto exposure: key %.3f -> goal %.3f, applied %.3f after %llu readings; frame = plain x exposure within %.4f (%u values)\n",
                         double(st.exposureKey),double(st.exposureGoal),double(e),(unsigned long long)st.exposureReadings,exposureWorst,exposureChecked);
            CHECK(st.exposureReadings>0 && e>=.5f-1e-4f && e<.95f && exposureWorst<3/255.0 && exposureChecked>1000);
            frame(1,false,Ui::Separate);
        }
        // The floor under water: WoW's water writes no depth - a frame without the mask shows the seabed (here the ground plane
        // 30 yd under the camera) and the floor memory keeps its minimum; the ground still reads the water surface (the area's
        // water level, kept from the masked frames) until a teleport (Booty Bay, 2026-09-29: the fog's ground sank to the
        // seabed, -3 -> -11 yd, and the fog thinned over the sea).
        {
            std::string settings;{std::ifstream in(ini);std::stringstream text;text<<in.rdbuf();settings=text.str();}
            {std::ofstream o(ini);o<<settings<<"shadows=1\nheight_reference=ground\n";}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            // The mask of a sea at z 10 over the ground plane (the seabed at z 0): on every pixel of the ground, the plane's view
            // depth scaled by (30 - 10) / 30 (the camera is 30 yd over the seabed) - the same cells the seabed covers.
            {
                std::vector<float> sea(size_t(W)*H*4,0.f);
                for(float gx=101;gx<700;gx+=.25f)for(float gy=-400;gy<800;gy+=.25f){
                    const auto c=mat4_mul_vec4(vp,Vec4{gx,gy,0,1});if(c.w<=.3f)continue;
                    const float nx=c.x/c.w,ny=c.y/c.w;if(std::fabs(nx)>=1||std::fabs(ny)>=1)continue;
                    const auto q=pixelOf(gx,gy,0);if(q[0]>=W || q[1]>=H)continue;
                    float* t=&sea[(size_t(q[1])*W+q[0])*4];t[2]=c.w*(30.f-10.f)/30.f;t[3]=1;
                }
                void* smap{};auto staging=buffer(sea.size()*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&smap);std::memcpy(smap,sea.data(),sea.size()*4);
                auto bi2=vk<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);bi2.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ok(D(vkBeginCommandBuffer)(cmd,&bi2));
                auto b2=vk<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);b2.srcQueueFamilyIndex=b2.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b2.image=mask;b2.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                b2.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b2.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b2.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b2.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b2);
                VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={W,H,1};
                D(vkCmdCopyBufferToImage)(cmd,staging,mask,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
                b2.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b2.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b2.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b2.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b2);
                ok(D(vkEndCommandBuffer)(cmd));auto si2=vk<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);si2.commandBufferCount=1;si2.pCommandBuffers=&cmd;
                ok(D(vkResetFences)(device,1,&fence));ok(D(vkQueueSubmit)(queue,1,&si2,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));
            }
            Vec3 away=position;away.x+=400;put(0x08,&away,12);   // a fresh area: the memories clear
            auto masked=[&](int n){for(int i=0;i<n;++i){water.maskView=maskView;water.maskColorImage=id(color);++water.maskSerial;frame(1,false,Ui::Separate);}return st.groundReading;};
            auto unmasked=[&](int n){for(int i=0;i<n;++i)frame(1,false,Ui::Separate);return st.groundReading;};
            const float surface=masked(12);const uint32_t surfaceCells=st.groundCells;   // (a reading comes back a few frames later)
            const float seabed=unmasked(12);const uint32_t seabedCells=st.groundCells;
            const double level=st.waterZ;const bool levelKept=st.waterValid;
            put(0x08,&position,12);unmasked(8);   // back: a teleport - another area, with its own water (none here)
            std::fprintf(stderr,"floor under water: ground reading %.1f over the water surface (%u cells), %.1f once the seabed showed too (%u cells), water level z %.2f kept %d; after a teleport: water level %d, ground %.1f\n",
                         surface,surfaceCells,seabed,seabedCells,level,int(levelKept),int(st.waterValid),st.groundReading);
            CHECK(surface>8 && seabedCells>surfaceCells+300 && seabed>surface-1 && levelKept && std::fabs(level-10)<1 && !st.waterValid && std::fabs(st.groundReading)<1);
            {std::ofstream o(ini);o<<settings;}st.settingsTime={};st.presentsSinceSettingsCheck=59;
            ++water.maskSerial;frame(1,false,Ui::Separate);
        }
        // A mask from an earlier frame is not reused.
        const auto noNewMask=frame(1,false,Ui::Separate);
        {std::lock_guard lock(mutex);fog_present(state);}
        CHECK(!st.lastWaterSurface);
        for(size_t k=0;k<noNewMask.size();++k)CHECK(std::abs(int(noNewMask[k])-int(fogged[k]))<=1);
        water.maskView=VkImageView{};
        D(vkDestroyImageView)(device,maskView,nullptr);
    }

    // CAMERA height reference: the camera is at z=30, so base offset -30 puts the medium's base at z=0 - the same fog as the
    // WORLD base 0 frame above (the reference snaps to the camera on first use).
    {
        {std::ofstream o(ini);o<<offDefaults<<"preset=medium\nmax_distance=400\ndistribution=8\nbase_density=0.01\nheight_falloff=0.02\nheight_reference=camera\nbase_height=-30\n"
                                 "max_extinction=1\nalbedo=0.9,0.9,0.9\nemissive=0,0,0\nanisotropy=0.3\nambient=0.3,0.3,0.35\nsun_enabled=1\nsun_direction=1,0,0.3\nsun_color=1,1,1\nshadows=0\nopen_sun=1\ntemporal_weight=0\nnoise_amount=0\n";}
        st.settingsTime={};st.presentsSinceSettingsCheck=59;st.haveReference=false;
        const auto relative=frame(1,false,Ui::Separate);
        CHECK(st.settings.heightReference==fog::HeightReference::Camera && std::fabs(st.lastBaseHeight)<1e-3f);
        for(size_t k=0;k<relative.size();++k)CHECK(std::abs(int(relative[k])-int(fogged[k]))<=1);
        // The F10 page: a click cycles the value and saves it; the file then reads back the same settings.
        const float before=st.settings.medium.baseDensity;
        fog_menu_click(state,2);   // DENSITY
        CHECK(st.settings.medium.baseDensity>before && fog_menu_label(state,2).find("DENSITY")==0);
        const float saved=st.settings.medium.baseDensity;
        st.settingsTime={};st.presentsSinceSettingsCheck=59;
        {std::lock_guard lock(mutex);fog_present(state);}
        CHECK(st.settings.medium.baseDensity==saved && st.settings.heightReference==fog::HeightReference::Camera && st.settings.medium.baseHeight==-30.f);
        fog_menu_click(state,4);   // HEIGHT REFERENCE -> WORLD keeps the fog where it is: base becomes the current world z
        CHECK(st.settings.heightReference==fog::HeightReference::World && std::fabs(st.settings.medium.baseHeight)<1e-3f);
    }
    // GROUND height reference: the base is measured from the area's ground - the floor memory (lowest surface seen per cell)
    // within 128 yd: here the ground plane z=0 and the walls' feet - not from the camera. With base_height 0 the base is the
    // ground; raising the camera 20 yd leaves it there (the CAMERA reference moved the fog up with it: Teldrassil, "the fog
    // changes as I walk around").
    {
        {std::ofstream o(ini);o<<offDefaults<<"preset=medium\nmax_distance=400\ndistribution=8\nbase_density=0.01\nheight_falloff=0.02\nheight_reference=ground\nbase_height=0\n"
                                 "max_extinction=1\nalbedo=0.9,0.9,0.9\nemissive=0,0,0\nanisotropy=0.3\nambient=0.3,0.3,0.35\nsun_enabled=1\nsun_direction=1,0,0.3\nsun_color=1,1,1\nshadows=1\nopen_sun=1\ntemporal_weight=0\nnoise_amount=0\n";}
        st.settingsTime={};st.presentsSinceSettingsCheck=59;
        for(int i=0;i<8;++i)frame(1,false,Ui::Separate);
        std::fprintf(stderr,"ground reference: valid %d, ground z %.2f from %u floor cells, base z %.2f\n",int(st.groundValid),st.groundZ,st.groundCells,double(st.lastBaseHeight));
        CHECK(st.settings.heightReference==fog::HeightReference::Ground && st.groundValid && st.groundCells>1000 && std::fabs(st.lastBaseHeight)<1.f);
        Vec3 up20=position;up20.z+=20;put(0x08,&up20,12);
        for(int i=0;i<8;++i)frame(1,false,Ui::Separate);
        std::fprintf(stderr,"ground reference, camera 20 yd higher (z %.1f): ground z %.2f, base z %.2f\n",st.memoryCamera[2],st.groundZ,double(st.lastBaseHeight));
        CHECK(std::fabs(st.lastBaseHeight)<1.f && st.memoryCamera[2]>49);
        put(0x08,&position,12);
        for(int i=0;i<4;++i)frame(1,false,Ui::Separate);
    }

    auto writeIni=[&](const char* extra){std::ofstream o(ini);o<<offDefaults<<"preset=medium\nmax_distance=400\ndistribution=8\nbase_density=0.01\nheight_falloff=0.02\nheight_reference=world\nbase_height=0\n"
        "max_extinction=1\nalbedo=0.9,0.9,0.9\nemissive=0,0,0\nanisotropy=0.3\nambient=0.3,0.3,0.35\nsun_enabled=1\nsun_direction=1,0,0.3\nsun_color=1,1,1\nshadows=0\nopen_sun=1\n"<<extra;
        o.close();st.settingsTime={};st.presentsSinceSettingsCheck=59;};
    auto moveCamera=[&](float dx){Vec3 p=position;p.x+=dx;put(0x08,&p,12);};
    auto maxDiff=[&](const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){int m=0;for(size_t k=0;k<a.size();++k)m=std::max(m,std::abs(int(a[k])-int(b[k])));return m;};
    // Temporal: jittered sampling inside each froxel, blended with the reprojected previous volume. With a still camera
    // it converges to (about) the non-temporal result; history is in use; a camera cut drops it.
    {
        writeIni("temporal_weight=0.9\nnoise_amount=0\n");
        std::vector<uint8_t> converged;for(int i=0;i<40;++i)converged=frame(1,false,Ui::Separate);
        CHECK(st.lastHistoryUsed && st.settings.temporalWeight>.89f && std::fabs(st.lastHistoryWeight-.9f)<1e-6f);   // still: the full weight
        const int d=maxDiff(converged,fogged);
        std::fprintf(stderr,"temporal (40 frames, still camera) vs non-temporal: max |diff| %d/255\n",d);
        CHECK(d<=4);
        moveCamera(20);frame(1,false,Ui::Separate);
        CHECK(!st.lastHistoryUsed && st.lastReset.find("moved")!=std::string::npos);
        moveCamera(0);
        // Moving camera: rise 1 yd per frame through a steep height fog (base near the camera, the fog seen changes strongly
        // every frame). Correct reprojection follows it; history sampled without the camera motion would lag ~20 frames.
        auto setHeight=[&](float z){Vec3 p=position;p.z=z;put(0x08,&p,12);};
        auto steep=[&](const char* temporal){std::string e=std::string("base_density=0.05\nheight_falloff=0.15\nheight_reference=world\nbase_height=35\nnoise_amount=0\n")+temporal;
            std::ofstream o(ini);o<<offDefaults<<"preset=medium\nmax_distance=400\ndistribution=8\nmax_extinction=1\nalbedo=0.9,0.9,0.9\nemissive=0,0,0\nanisotropy=0.3\n"
            "ambient=0.3,0.3,0.35\nsun_enabled=1\nsun_direction=1,0,0.3\nsun_color=1,1,1\nshadows=0\nopen_sun=1\n"<<e;o.close();st.settingsTime={};st.presentsSinceSettingsCheck=59;};
        steep("temporal_weight=0.9\n");
        std::vector<uint8_t> moving;for(int i=0;i<=20;++i){setHeight(30.f+i);moving=frame(1,false,Ui::Separate);}
        CHECK(st.lastHistoryUsed && std::fabs(st.lastHistoryWeight-.8f)<1e-6f);   // 1 yd a frame: temporal_moving (.8)
        steep("temporal_weight=0\n");
        const auto still=frame(1,false,Ui::Separate);
        setHeight(30.f);const auto start=frame(1,false,Ui::Separate);
        auto meanDiff=[&](const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){double t=0;for(size_t k=0;k<a.size();++k)t+=std::abs(int(a[k])-int(b[k]));return t/a.size();};
        std::fprintf(stderr,"temporal while rising 20 yd: to the final height mean %.2f max %d; start vs final mean %.2f max %d\n",
                     meanDiff(moving,still),maxDiff(moving,still),meanDiff(start,still),maxDiff(start,still));
        CHECK(meanDiff(start,still)>8);                  // the motion really changes the fog
        // Measured: correct reprojection 4.4 mean, history without the camera motion 10.5 (start vs final 14.5). The rest
        // is trilinear resampling blur of this 11x11-froxel test grid (12 px froxels of a 128 px image = 5.6 deg each).
        CHECK(meanDiff(moving,still)<meanDiff(start,still)*.45);   // and the temporal result follows it
    }
    // Noise is world-anchored: moving the camera by a whole number of noise periods (2 x fog::noise_period(24) = 6144 yd) over the same depth
    // buffer shows exactly the same fog; moving it 700 yd shows different fog (the pattern stays in the world, not on screen).
    {
        writeIni("temporal_weight=0\nnoise_amount=0.6\nnoise_scale=24\nnoise_wind=0\n");
        const auto a=frame(1,false,Ui::Separate);
        CHECK(st.settings.noiseAmount>.59f && maxDiff(a,fogged)>2);
        moveCamera(float(2*fog::noise_period(24.f)));const auto b=frame(1,false,Ui::Separate);
        moveCamera(700.f);const auto c=frame(1,false,Ui::Separate);
        moveCamera(0);
        double visible=0;unsigned counted=0;for(size_t k=0;k<a.size();k+=4)if(fogged[k]!=base[k]||fogged[k+1]!=base[k+1]){for(int ch=0;ch<3;++ch)visible+=std::abs(int(a[k+ch])-int(fogged[k+ch]));counted+=3;}
        std::fprintf(stderr,"noise: shift 6144 yd max |diff| %d, shift 700 yd max |diff| %d; noise 0.6 vs none: mean |diff| %.2f/255 over fogged pixels\n",maxDiff(a,b),maxDiff(a,c),visible/std::max(counted,1u));
        CHECK(maxDiff(a,b)<=1 && maxDiff(a,c)>2);
        // Continuity where the origin wraps (the camera crosses a multiple of fog::noise_period): a 1 yd step there changes
        // the fog as little as a 1 yd step elsewhere. A modulus that is not the lattice's period jumps to another part of it.
        {
            const float period=float(fog::noise_period(24.f));
            moveCamera(period-100.5f);const auto w0=frame(1,false,Ui::Separate);
            moveCamera(period-99.5f);const auto w1=frame(1,false,Ui::Separate);
            moveCamera(99.5f);const auto s0=frame(1,false,Ui::Separate);
            moveCamera(100.5f);const auto s1=frame(1,false,Ui::Separate);
            moveCamera(0);
            std::fprintf(stderr,"noise: 1 yd step across the origin wrap max |diff| %d, elsewhere %d\n",maxDiff(w0,w1),maxDiff(s0,s1));
            CHECK(maxDiff(w0,w1)<=maxDiff(s0,s1)+3);
        }
        // Smoke must be visible: the coverage-shaped field changes the fogged pixels by 4.9/255 on average here; the old
        // +-% value noise changed them by 0.85/255 (averaged out along each ray, invisible in the game).
        CHECK(visible/std::max(counted,1u)>3);
    }

    // Sun shadows (screen space, from the previous frame's depth): with the sun behind the near wall, the fog between the camera
    // and the wall is in its shadow. The GPU result must follow the analytic shadow reference (exact ray/wall intersections),
    // not the unshadowed one, and leave pixels whose sun rays are clear unchanged.
    {
        const char* common="temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=manual\nsun_color=4,4,4\nbase_density=0.02\nsky_light=0\n";
        writeIni((std::string(common)+"shadows=0\nopen_sun=1\n").c_str());
        const auto plain=frame(1,false,Ui::Separate);
        writeIni((std::string(common)+"shadows=1\nshadow_length=60\n").c_str());
        frame(1,false,Ui::Separate);   // the first frame writes the depth the next one marches through
        const auto shadowed=frame(1,false,Ui::Separate);
        std::fprintf(stderr,"sun shadows: state '%s'\n",st.lastShadowProblem.c_str());
        CHECK(st.lastShadowProblem.empty() && st.settings.shadows);
        double errShadowRef=0,errPlainRef=0;unsigned compared=0;double worst=0;
        for(unsigned j=0;j<grid.y;++j)for(unsigned i=0;i<grid.x;++i){
            const unsigned x=unsigned((i+.5f)/grid.x*W),y=unsigned((1-(j+.5f)/grid.y)*H);
            if(x<W*.2f && y<H*.2f)continue;
            std::array<float,3> inS,inP;float TS,TP;reference(x,y,inS,TS,1e9f,60.f);reference(x,y,inP,TP);
            float dRef=0;for(int c=0;c<3;++c)dRef=std::max(dRef,std::fabs(inS[c]-inP[c]));
            if(dRef<3/255.f)continue;   // pixels the shadow does not change
            ++compared;
            for(int c=0;c<3;++c){const float got=px(shadowed,x,y,c);
                const float eS=std::fabs(std::min(px(base,x,y,c)*TS+inS[c],1.f)-got),eP=std::fabs(std::min(px(base,x,y,c)*TP+inP[c],1.f)-got);
                errShadowRef+=eS;errPlainRef+=eP;worst=std::max(worst,double(eS));}
        }
        const auto nearChange=[&](){float d=0;for(int c=0;c<3;++c)d+=px(plain,nearPx[0],nearPx[1],c)-px(shadowed,nearPx[0],nearPx[1],c);return d;}();
        const int groundDiff=[&](){int m=0;for(int c=0;c<3;++c){auto k=(groundPx[1]*W+groundPx[0])*4+c;m=std::max(m,std::abs(int(plain[k])-int(shadowed[k])));}return m;}();
        std::fprintf(stderr,"sun shadows: %u shadowed froxel-centre pixels, mean |error| vs shadow reference %.4f, vs unshadowed %.4f, worst %.4f; near wall darker by %.3f, ground pixel |diff| %d\n",
                     compared,compared?errShadowRef/(3*compared):0.,compared?errPlainRef/(3*compared):0.,worst,nearChange,groundDiff);
        CHECK(compared>=10);
        CHECK(errShadowRef<errPlainRef*.35);
        CHECK(nearChange>.02f && groundDiff<=1);
    }
    // Height memory + sky light: a canopy entirely ABOVE the screen (roof 20 yd over the camera, one hole). Screen-space alone
    // cannot see it (everything under it lit). After the camera looked up once, the memory holds it: the fog under the roof
    // loses the sun and the sky share of the ambient, a column of light comes down through the hole - the GPU must follow the
    // reference WITH the canopy. A teleport (jump over 200 yd) clears the memory: back home without looking up, it is gone.
    {
        auto aimCamera=[&](float degrees){   // pitch, positive up; re-projects the scene for it
            const float a=degrees*3.14159265f/180.f;const Vec3 f{std::cos(a),0,std::sin(a)},u{-std::sin(a),0,std::cos(a)};
            put(0x08,&position,12);put(0x14,&f,12);put(0x20,&left,12);put(0x2C,&u,12);
            RawMemoryCamera rc=raw;rc.rows[0]=f;rc.rows[2]=u;
            const auto c=memory_camera_to_derived(rc,SignedPermutation{{1,3,-2}},synthesize_projection(rawFov*.6f,FovAxis::Vertical,aspect,nearClip,farClip));CHECK(c.valid);
            const auto m=mat4_mul(c.projection,c.view);float* v=static_cast<float*>(vmap);
            for(const auto& [i,p]:world){const auto clip=mat4_mul_vec4(m,Vec4{p[0],p[1],p[2],1});v[i*4]=clip.x;v[i*4+1]=clip.y;v[i*4+2]=clip.z;v[i*4+3]=clip.w;}};
        const std::string common="temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=manual\nsun_color=4,4,4\nsun_direction=0.8,0.1,0.8\n"
                                 "base_density=0.02\nheight_falloff=0.01\nshadows=1\nshadow_length=120\nsky_light=0.6\nopen_sun=1\n";
        canopy=true;
        CHECK(frame(0,false,Ui::Separate)==base);   // the roof is not on the normal camera's screen at all
        // GROUND reference while only the roof is in view (a teleport emptied the memories, the camera looks 35 degrees up): the
        // roof's underside is no floor - only surfaces under the camera count (Shadowglen: after a reset the canopy seen from
        // below became the "ground" 59 yd over the camera and the fog base with it).
        {
            writeIni((common+"height_reference=ground\n").c_str());
            moveCamera(230);frame(1,false,Ui::Separate);
            aimCamera(35);for(int i=0;i<6;++i)frame(1,false,Ui::Separate);
            std::fprintf(stderr,"ground reference looking up at the roof: valid %d, ground z %.1f from %u cells (camera z %.1f)\n",int(st.groundValid),st.groundZ,st.groundCells,position.z);
            CHECK(!st.groundValid || st.groundZ<position.z);
        }
        writeIni(common.c_str());
        aimCamera(35);for(int i=0;i<3;++i)frame(1,false,Ui::Separate);   // looking up: the memory learns the roof
        aimCamera(-15);frame(1,false,Ui::Separate);
        const auto learned=frame(1,false,Ui::Separate);
        CHECK(st.lastShadowProblem.empty());
        auto compare=[&](const std::vector<uint8_t>& img,double& errWith,double& errWithout){
            unsigned n=0;errWith=errWithout=0;
            for(unsigned j=0;j<grid.y;++j)for(unsigned i=0;i<grid.x;++i){
                const unsigned x=unsigned((i+.5f)/grid.x*W),y=unsigned((1-(j+.5f)/grid.y)*H);
                if(x<W*.2f && y<H*.2f)continue;
                std::array<float,3> a,b;float ta,tb;reference(x,y,a,ta,1e9f,120.f,true,.6f);reference(x,y,b,tb,1e9f,120.f,false,.6f);
                float d=0;for(int c=0;c<3;++c)d=std::max(d,std::fabs(a[c]-b[c]));
                if(d<3/255.f)continue;   // the roof changes nothing there
                ++n;
                for(int c=0;c<3;++c){const float got=px(img,x,y,c);
                    errWith+=std::fabs(std::min(px(base,x,y,c)*ta+a[c],1.f)-got);errWithout+=std::fabs(std::min(px(base,x,y,c)*tb+b[c],1.f)-got);}
            }
            errWith/=3.*std::max(n,1u);errWithout/=3.*std::max(n,1u);return n;};
        double withRoof=0,withoutRoof=0;
        const unsigned affected=compare(learned,withRoof,withoutRoof);
        // The sun alone (sky share 0): with the sun 45 degrees off the zenith the lit air under the hole is displaced sideways;
        // a top-down height memory darkened everything under the roof (its column test), the sun map lets the rays through.
        double sunWith=0,sunWithout=0;
        {
            writeIni((common+"sky_light=0\n").c_str());
            frame(1,false,Ui::Separate);
            const auto sunOnly=frame(1,false,Ui::Separate);
            unsigned n=0;
            for(unsigned j=0;j<grid.y;++j)for(unsigned i=0;i<grid.x;++i){
                const unsigned x=unsigned((i+.5f)/grid.x*W),y=unsigned((1-(j+.5f)/grid.y)*H);
                if(x<W*.2f && y<H*.2f)continue;
                std::array<float,3> a,b;float ta,tb;reference(x,y,a,ta,1e9f,120.f,true,0.f);reference(x,y,b,tb,1e9f,120.f,false,0.f);
                float d=0;for(int c=0;c<3;++c)d=std::max(d,std::fabs(a[c]-b[c]));
                if(d<3/255.f)continue;
                ++n;for(int c=0;c<3;++c){const float got=px(sunOnly,x,y,c);
                    sunWith+=std::fabs(std::min(px(base,x,y,c)*ta+a[c],1.f)-got);sunWithout+=std::fabs(std::min(px(base,x,y,c)*tb+b[c],1.f)-got);}
            }
            sunWith/=3.*std::max(n,1u);sunWithout/=3.*std::max(n,1u);
            std::fprintf(stderr,"canopy, sun only (45 degrees): %u pixels; mean |error| vs reference with the roof %.4f, without %.4f\n",n,sunWith,sunWithout);
            // Measured: sun map .030; the old height memory in the sun march .076 (it darkened all air under the roof); no
            // memory .113. The residue is the roof edges at this 11x11-froxel test grid and roof parts never on screen.
            CHECK(n>=20 && sunWith<.045 && sunWith<sunWithout*.5);
            writeIni(common.c_str());frame(1,false,Ui::Separate);
        }
        std::fprintf(stderr,"canopy above the screen: %u froxel-centre pixels changed by it; mean |error| vs reference with the roof %.4f, without %.4f\n",affected,withRoof,withoutRoof);
        CHECK(affected>=20);
        CHECK(withRoof<withoutRoof*.35);
        const auto clears=st.memoryClears;
        // 230 yd: over the 200 yd teleport threshold but the roof stays inside the 512 yd window (a 300 yd jump would push it out
        // and the per-cell tags alone would drop it - that did not test the teleport clear).
        moveCamera(230);frame(1,false,Ui::Separate);moveCamera(0);frame(1,false,Ui::Separate);
        const auto forgot=frame(1,false,Ui::Separate);
        double forgotWith=0,forgotWithout=0;compare(forgot,forgotWith,forgotWithout);
        std::fprintf(stderr,"after a teleport and back (memory clears %llu -> %llu): vs reference with the roof %.4f, without %.4f\n",
                     (unsigned long long)clears,(unsigned long long)st.memoryClears,forgotWith,forgotWithout);
        CHECK(st.memoryClears==clears+2 && forgotWithout<forgotWith*.35);
        canopy=false;
    }
    // A pass holding only sky draws on the world targets is the world continuing (DXVK can split between the world and the
    // sky): the fog must be applied, once. It was taken for the UI and the fog dropped for that frame (flicker in game).
    {
        writeIni("temporal_weight=0\nnoise_amount=0\n");
        frame(1,false,Ui::Separate);
        const auto classesBefore=classCounts();
        skySplit=true;const auto split=frame(1,false,Ui::Separate);skySplit=false;
        CHECK(classDelta(classesBefore,classCounts(),DrawClass::Sky,true)==1);   // the far wall again, in the sky's depth range (0.998..1)
        {std::lock_guard lock(mutex);fog_present(state);}
        std::fprintf(stderr,"sky in a pass of its own: composites %u, dropped %u\n",st.lastComposites,st.lastDropped);
        CHECK(st.lastComposites==1 && st.lastDropped==0);
        CHECK(maxDiff(split,frame(1,false,Ui::Separate))<=1);
    }
    // The disc the game draws in the sky is found on the picture (fog_sun_find.comp): a compact white cluster of sky pixels,
    // turned into a world direction with that frame's camera. With sun_position = sky the fog's sun comes from it - the
    // game's own direction values missed the visible disc by up to 40 degrees in Booty Bay.
    {
        writeIni("temporal_weight=0\nnoise_amount=0\nshadows=1\nsun_source=game\nsun_position=sky\n");
        const auto before=st.seenDiscCount;
        frame(1,false,Ui::Separate);frame(1,false,Ui::Separate);
        CHECK(st.seenDiscCount==before);                    // no disc drawn: nothing found
        // Three sightings in a row confirm it (fog_math.hpp DiscTrack), each read back three frames after its search.
        sunDisc=true;for(int i=0;i<10;++i)frame(1,false,Ui::Separate);
        const float c=st.seenDisc[0]*sunDiscDir.x+st.seenDisc[1]*sunDiscDir.y+st.seenDisc[2]*sunDiscDir.z;
        const float deg=std::acos(std::min(1.f,c))*180/3.14159265f;
        std::string rejected;for(const auto& [why,count]:st.discStats.rejected)rejected+=" "+std::to_string(count)+" "+why;
        std::fprintf(stderr,"sun finder: %llu sightings (waiting %llu, rejected:%s), disc at pixel %.1f,%.1f (%u texels), direction off by %.2f degrees; fog sun from '%s'\n",
                     (unsigned long long)(st.seenDiscCount-before),(unsigned long long)st.discStats.waiting,rejected.c_str(),st.seenDiscPixel[0],st.seenDiscPixel[1],st.seenDiscTexels,deg,st.lastSunSource.c_str());
        CHECK(st.seenDiscValid && st.seenDiscCount>before && deg<2.f && st.lastSunSource=="seen disc");
        CHECK(std::fabs(st.lastSunToward[0]-st.seenDisc[0])<1e-6f && std::fabs(st.lastSunToward[2]-st.seenDisc[2])<1e-6f);
        sunDisc=false;
        // A teleport (a jump over 200 yd) forgets it: another zone's sky has its disc elsewhere.
        moveCamera(230);frame(1,false,Ui::Separate);moveCamera(0);
        for(int i=0;i<4;++i)frame(1,false,Ui::Separate);   // searches made before the jump are read back now: they must not bring it back
        CHECK(!st.seenDiscValid);
        // A yellow disc (1, 1, .75: fails the white test) is found by the warm set.
        const auto beforeWarm=st.seenDiscCount;
        sunDisc=warmDisc=true;for(int i=0;i<10;++i)frame(1,false,Ui::Separate);sunDisc=warmDisc=false;
        const float cw=st.seenDisc[0]*sunDiscDir.x+st.seenDisc[1]*sunDiscDir.y+st.seenDisc[2]*sunDiscDir.z;
        const float degWarm=std::acos(std::min(1.f,cw))*180/3.14159265f;
        std::fprintf(stderr,"sun finder, yellow disc: %llu sightings, off by %.2f degrees\n",(unsigned long long)(st.seenDiscCount-beforeWarm),degWarm);
        CHECK(st.seenDiscValid && st.seenDiscCount>beforeWarm && degWarm<2.f);
        moveCamera(230);frame(1,false,Ui::Separate);moveCamera(0);for(int i=0;i<4;++i)frame(1,false,Ui::Separate);
        writeIni("temporal_weight=0\nnoise_amount=0\nshadows=1\nsun_source=manual\n");frame(1,false,Ui::Separate);
    }
    // open_sun: under an open sky the sun in the fog is scaled (shafts need a strong sun under a canopy, the same strength
    // made open zones a white glare). The test scene's sky is open almost everywhere: sun 4 x .25 = sun 1 at full strength
    // (differences only next to the walls, whose columns close part of the sky), and clearly not sun 4.
    {
        auto meanOf=[&](const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){double t=0;for(size_t k=0;k<a.size();++k)t+=std::abs(int(a[k])-int(b[k]));return t/a.size();};
        writeIni("temporal_weight=0\nnoise_amount=0\nshadows=1\nsky_light=0\nsun_source=manual\nsun_color=4,4,4\nopen_sun=0.25\n");
        for(int i=0;i<3;++i)frame(1,false,Ui::Separate);
        const auto scaled=frame(1,false,Ui::Separate);
        writeIni("temporal_weight=0\nnoise_amount=0\nshadows=1\nsky_light=0\nsun_source=manual\nsun_color=1,1,1\nopen_sun=1\n");
        for(int i=0;i<3;++i)frame(1,false,Ui::Separate);
        const auto weak=frame(1,false,Ui::Separate);
        writeIni("temporal_weight=0\nnoise_amount=0\nshadows=1\nsky_light=0\nsun_source=manual\nsun_color=4,4,4\nopen_sun=1\n");
        for(int i=0;i<3;++i)frame(1,false,Ui::Separate);
        const auto strong=frame(1,false,Ui::Separate);
        std::fprintf(stderr,"open sun .25: vs sun/4 mean |diff| %.2f (max %d), vs full sun mean %.2f\n",meanOf(scaled,weak),maxDiff(scaled,weak),meanOf(scaled,strong));
        CHECK(meanOf(scaled,weak)<.5 && meanOf(scaled,strong)>2);
    }
    // Light gizmo (fog menu): the overlay gets the fog sun's world direction and the view projection; the gizmo shader puts
    // the marker where the direction lands - here, the sun toward +x and 0.3 up must land where the test camera sees that
    // direction (right of nothing, above the horizon line).
    {
        writeIni("temporal_weight=0\nnoise_amount=0\nlight_gizmo=1\nsun_source=manual\n");
        frame(1,false,Ui::Separate);
        auto u=std::make_unique<OverlayUniform>();
        update_camera_telemetry(device,*u,id(depth));
        CHECK(u->lightGizmo[0]==1.f && u->lightMarkers[0][3]==1.f);
        const float* m=u->lightMarkers[0];
        const float l=std::sqrt(1+.09f);
        CHECK(std::fabs(m[0]-1/l)<1e-5f && std::fabs(m[2]-.3f/l)<1e-5f);
        // clip = VP * (dir, 0): the gizmo shader's projection of a direction
        float clip[4]{};for(int r=0;r<4;++r)for(int c=0;c<3;++c)clip[r]+=u->gizmoVP[c*4+r]*m[c];
        const auto expect=mat4_mul_vec4(vp,Vec4{m[0],m[1],m[2],0});
        std::fprintf(stderr,"light gizmo: sun marker ndc %.3f %.3f (test camera %.3f %.3f)\n",clip[0]/clip[3],clip[1]/clip[3],expect.x/expect.w,expect.y/expect.w);
        CHECK(clip[3]>0 && std::fabs(clip[0]/clip[3]-expect.x/expect.w)<1e-3f && std::fabs(clip[1]/clip[3]-expect.y/expect.w)<1e-3f);
        writeIni("temporal_weight=0\nnoise_amount=0\nlight_gizmo=0\n");frame(1,false,Ui::Separate);
        update_camera_telemetry(device,*u,id(depth));
        CHECK(u->lightGizmo[0]==0.f);
    }
    // Fog capture: a request file arms it; two frames later the capture holds the frame before and after the fog (identical to
    // what the test read back, outside the UI corner that is drawn later), the volumes and the SUN VISIBILITY frame, and the
    // view the user had is back.
    {
        const auto root=std::filesystem::path("/tmp")/("depth-probe-fog-capture-test-"+std::to_string(getpid()));
        std::filesystem::remove_all(root);std::filesystem::create_directories(root);setenv("DEPTH_PROBE_EXPORT_DIR",root.c_str(),1);
        writeIni("temporal_weight=0\nnoise_amount=0\nshadows=1\nsky_light=0.5\n");
        frame(1,false,Ui::Separate);
        std::ofstream(root/"fog-capture-request").put('1');
        st.presentsSinceCaptureCheck=29;
        auto captured=frame(1,false,Ui::Separate);   // present: armed; FINAL is captured once the fog has been on for 60 frames
        CHECK(!std::filesystem::exists(root/"fog-capture-request"));
        for(unsigned i=0;st.captureStage==1 && i<80;++i)captured=frame(1,false,Ui::Separate);
        CHECK(st.captureStage==4);                          // one frame: nothing on screen changes (the old second frame flashed a debug view)
        frame(1,false,Ui::Separate);                        // present: done; the copies are written
        {std::lock_guard lock(mutex);fog_present(state);}
        CHECK(st.captureStage==0 && camera_hud_input().fogMode==1);
        std::filesystem::path dir;for(auto& e:std::filesystem::directory_iterator(root))if(e.is_directory())dir=e.path();
        CHECK(!dir.empty());
        auto file=[&](const char* name){std::ifstream in(dir/name,std::ios::binary);return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),{});};
        for(auto name:{"color_before.bin","color_after.bin","view_depth.bin","height_memory.bin","sun_map.bin","medium.bin","lighting.bin","integrated.bin","visibility.bin",
                       "final_params.json","final_sections.json","fog.ini","README.txt"})
            if(!std::filesystem::exists(dir/name)){std::fprintf(stderr,"missing %s\n",name);CHECK(false);}
        const auto after=file("color_after.bin"),before=file("color_before.bin");
        CHECK(after.size()==captured.size() && before.size()==base.size());
        unsigned differ=0,differBefore=0;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){if(x<W*.2f && y<H*.2f)continue;const auto k=(y*W+x)*4;
            for(int c=0;c<3;++c){differ+=after[k+c]!=captured[k+c];differBefore+=before[k+c]!=base[k+c];}}
        // The visibility volume (RGBA8, r = sun, b = sky) of the captured frame: the sun reaches part of the fog, not all of it
        // (the near wall shadows the fog in front of it with this sun).
        const auto vis=file("visibility.bin");size_t bad=0,lit=0,count=0;
        for(size_t i=0;i+4<=vis.size();i+=4){bad+=vis[i+3]!=255;lit+=vis[i]>127;++count;}
        std::fprintf(stderr,"fog capture visibility volume: %zu froxels, %zu sun-lit\n",count,lit);
        bad+=count!=size_t(grid.x)*grid.y*grid.z || lit==0 || lit==count;
        const auto depthBytes=file("view_depth.bin");
        std::fprintf(stderr,"fog capture %s: color_after vs frame %u bytes differ, color_before vs unfogged %u, visibility texels out of range %zu, view depth %zu bytes\n",
                     dir.filename().c_str(),differ,differBefore,bad,depthBytes.size());
        CHECK(differ==0 && differBefore==0 && bad==0 && depthBytes.size()==size_t(W)*H*4);
        // Armed while the fog is off (the view is not saved and starts OFF each launch): the capture turns it on for its two
        // frames and off again, instead of waiting forever.
        frame(0,false,Ui::Separate);
        std::ofstream(root/"fog-capture-request").put('1');
        st.presentsSinceCaptureCheck=29;
        frame(keepView,false,Ui::Separate);
        CHECK(st.captureStage==1 && st.captureTurnedFogOn && camera_hud_input().fogMode==1);
        // It waits for the fog to warm up (60 frames: game sun, a depth, the memories, the temporal history) - the first
        // in-game capture otherwise caught the very first fog frame.
        unsigned warm=1;while(st.captureStage==1 && warm<80){frame(keepView,false,Ui::Separate);++warm;}
        std::fprintf(stderr,"fog capture waited %u frames after turning the fog on\n",warm);
        CHECK(st.captureStage==4 && warm>=59 && warm<=62);
        frame(keepView,false,Ui::Separate);
        {std::lock_guard lock(mutex);fog_present(state);}
        CHECK(st.captureStage==0 && camera_hud_input().fogMode==0);
        unsigned captureDirs=0;for(auto& e:std::filesystem::directory_iterator(root))captureDirs+=e.is_directory();
        std::fprintf(stderr,"fog capture while the fog was off: %u capture dirs, view back to %u\n",captureDirs,camera_hud_input().fogMode);
        CHECK(captureDirs==2);
        camera_hud_input().fogMode=1;
        // The one-click look for light shafts (menu row 20) sets the rows above it and saves them.
        CHECK(fog_menu_label(state,20).rfind("LOOK: FOREST LIGHT SHAFTS",0)==0);
        fog_menu_click(state,20);
        CHECK(st.settings.medium.baseDensity==.012f && st.settings.medium.heightFalloff==.04f && st.settings.sunColor[0]==19.2f && st.settings.anisotropy==.35f &&
              st.settings.skyLight==.85f && st.settings.sunColorFromGame && st.settings.heightReference==fog::HeightReference::Ground &&
              st.settings.medium.baseHeight==-7.f && st.settings.maxOpacity==.6f && st.settings.sunGlare==.7f && st.settingsNote=="saved");
        if(std::getenv("DEPTH_PROBE_KEEP_FOG_CAPTURE"))std::fprintf(stderr,"kept %s\n",dir.c_str());else std::filesystem::remove_all(root);
    }
    // sky_fog: sky pixels take that share of the fog - 0 leaves the sky exactly as the game drew it (its haze and sun glow),
    // 0.5 is halfway to the full fog, and no world pixel changes (Booty Bay morning: the full fog halved the sun's halo).
    {
        const std::string base0="temporal_weight=0\nnoise_amount=0\n";
        writeIni((base0+"sky_fog=0\n").c_str());const auto untouched=frame(1,false,Ui::Separate);
        writeIni((base0+"sky_fog=0.5\n").c_str());const auto halfway=frame(1,false,Ui::Separate);
        writeIni(base0.c_str());const auto full=frame(1,false,Ui::Separate);
        const size_t skyAt=(1*W+W/2)*4;   // top middle: sky (the camera looks 15 degrees down, 30 degrees above it are visible)
        unsigned skyPixels=0;int skyWorst=0,halfWorst=0,worldWorst=0;
        for(size_t k=0;k<base.size();k+=4) {
            const bool sky=base[k]==base[skyAt] && base[k+1]==base[skyAt+1] && base[k+2]==base[skyAt+2];
            for(int ch=0;ch<3;++ch) {
                if(sky){skyWorst=std::max(skyWorst,std::abs(int(untouched[k+ch])-int(base[k+ch])));
                        halfWorst=std::max(halfWorst,std::abs(2*int(halfway[k+ch])-int(base[k+ch])-int(full[k+ch])));}
                else worldWorst=std::max(worldWorst,std::abs(int(untouched[k+ch])-int(full[k+ch])));
            }
            skyPixels+=sky;
        }
        std::fprintf(stderr,"sky fog: %u sky pixels; sky_fog 0 vs no fog %d, 0.5 vs the mean of none and full %d (x2), world pixels 0 vs 1: %d; full fog changes the sky by %d\n",
                     skyPixels,skyWorst,halfWorst,worldWorst,std::abs(int(full[skyAt])-int(base[skyAt])));
        CHECK(skyPixels>1000 && skyWorst<=1 && halfWorst<=2 && worldWorst==0 && std::abs(int(full[skyAt])-int(base[skyAt]))>4);
    }
    // The game's sun cascades (roadmap M10, game_shadows=1): four depth maps the world samples with a comparison and their light
    // matrices (here injected: WoW's are found on its draws). Light straight down, maps cleared to depth .5 = a flat occluder at
    // the camera's height: the fog's sun must follow them - the ground (z 0) in shadow, the far wall's top (z 50) lit - reduced
    // at the composite and tested by the next frame's light pass in that frame's view space, and the fog's sun is their light.
    {
        writeIni("temporal_weight=0\nnoise_amount=0\nshadows=0\ngame_shadows=0\n");
        frame(7,false,Ui::Separate);
        const auto plainVisibility=frame(7,false,Ui::Separate);
        const float* R=st.prevFrame.params.right;const float* U=st.prevFrame.params.up;const float* F=st.prevFrame.params.forward;
        auto view=[&](float x,float y,float z){return std::array<float,3>{x*R[0]+y*R[1]+z*R[2],x*U[0]+y*U[1]+z*U[2],x*F[0]+y*F[1]+z*F[2]};};
        FogState::GameCascades injected;
        const auto ex=view(1,0,0),ey=view(0,1,0),toward=view(0,0,1);
        std::array<VkImage,4> maps{};
        for(int k=0;k<4;++k){
            const float scale=2.f/(60.f*float(1<<(2*k)));   // 60, 240, 960, 3840 yd wide around the camera
            for(int i=0;i<3;++i){injected.rows[3*k][i]=ex[i]*scale;injected.rows[3*k+1][i]=ey[i]*scale;injected.rows[3*k+2][i]=-toward[i]/2000.f;}
            injected.rows[3*k][3]=injected.rows[3*k+1][3]=0;injected.rows[3*k+2][3]=.5f;
            auto i=vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);i.imageType=VK_IMAGE_TYPE_2D;i.format=VK_FORMAT_D32_SFLOAT;i.extent={1024,1024,1};i.mipLevels=i.arrayLayers=1;
            i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ok(A(vkCreateImage)(device,&i,nullptr,&maps[k]));VkMemoryRequirements r{};D(vkGetImageMemoryRequirements)(device,maps[k],&r);ok(A(vkBindImageMemory)(device,maps[k],allocate(r,0),0));
            {std::lock_guard lock(mutex);injected.images[k]=state.images.at(id(maps[k]));}
            injected.layouts[k]=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;injected.bindings[k]=uint32_t(k+1);
        }
        CHECK(!fog::cascade_rows_problem(injected.rows));
        {
            auto bi=vk<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ok(D(vkBeginCommandBuffer)(cmd,&bi));
            const VkClearDepthStencilValue half{.5f,0};const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
            for(auto m:maps){
                auto b=vk<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=m;b.subresourceRange=whole;
                b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
                D(vkCmdClearDepthStencilImage)(cmd,m,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&half,1,&whole);
                b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                D(vkCmdPipelineBarrier)(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
            }
            ok(D(vkEndCommandBuffer)(cmd));auto si=vk<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);si.commandBufferCount=1;si.pCommandBuffers=&cmd;
            ok(D(vkResetFences)(device,1,&fence));ok(D(vkQueueSubmit)(queue,1,&si,fence));ok(D(vkWaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX));
        }
        st.cascadesInjected=injected;
        writeIni("temporal_weight=0\nnoise_amount=0\nshadows=0\ngame_shadows=1\n");
        const auto usedBefore=st.cascadeFramesUsed;
        frame(7,false,Ui::Separate);frame(7,false,Ui::Separate);
        const auto gameVisibility=frame(7,false,Ui::Separate);
        const auto groundPx=pixelOf(250,200,0),wallTopPx=pixelOf(400,200,50);
        std::fprintf(stderr,"game cascades: sun from '%s' toward %.3f %.3f %.3f, used in %llu frames; sun visibility ground %.2f (without %.2f), far wall top %.2f (without %.2f)\n",
                     st.lastSunSource.c_str(),st.lastSunToward[0],st.lastSunToward[1],st.lastSunToward[2],(unsigned long long)(st.cascadeFramesUsed-usedBefore),
                     px(gameVisibility,groundPx[0],groundPx[1],0),px(plainVisibility,groundPx[0],groundPx[1],0),px(gameVisibility,wallTopPx[0],wallTopPx[1],0),px(plainVisibility,wallTopPx[0],wallTopPx[1],0));
        CHECK(st.lastSunSource=="game shadows" && st.lastSunToward[2]>.999f && st.cascadeFramesUsed>usedBefore);
        CHECK(px(plainVisibility,groundPx[0],groundPx[1],0)>.9f && px(gameVisibility,groundPx[0],groundPx[1],0)<.1f);
        CHECK(px(gameVisibility,wallTopPx[0],wallTopPx[1],0)>.9f);
        st.cascadesInjected.reset();
        writeIni("temporal_weight=0\nnoise_amount=0\n");frame(1,false,Ui::Separate);
        D(vkDeviceWaitIdle)(device);
        {std::lock_guard lock(mutex);fog_destroy(state);}   // the fog's views of the maps go before the maps
        for(auto m:maps)A(vkDestroyImage)(device,m,nullptr);
        frame(1,false,Ui::Separate);
    }
    // Transparent-aware fog (roadmap milestone 3, transparent_fog=1): the additive particle - no depth write, 50 yd away, in
    // front of the far wall at 300 yd - is held back from the world pass and drawn right after the fog composite. The result
    // must be the fogged world plus the particle, exactly, where it used to be the particle fogged as if it were AT the wall;
    // every other pixel identical. An additive layer on the far wall's own geometry (EQUAL test, WoW's second material pass)
    // keeps its place: it is at the wall's depth. With the fog off nothing is held back.
    {
        const std::string base="temporal_weight=0\nnoise_amount=0\n";
        st.deferred=st.deferredAfterFog=st.deferredFallback=st.deferLayers=0;st.deferRejected.clear();
        writeIni((base+"transparent_fog=1\n").c_str());
        const auto without=frame(1,false,Ui::Separate);
        particle=true;
        const auto held=frame(1,false,Ui::Separate);
        std::string rejected;for(auto& [why,n]:st.deferRejected)rejected+=std::string(why)+" x"+std::to_string(n)+"; ";
        std::fprintf(stderr,"transparent fog: held %llu, drawn after the fog %llu, fallback %llu, not held: %s\n",(unsigned long long)st.deferred,
                     (unsigned long long)st.deferredAfterFog,(unsigned long long)st.deferredFallback,rejected.empty()?"none":rejected.c_str());
        CHECK(st.deferred==1 && st.deferredAfterFog==1 && st.deferredFallback==0);
        writeIni((base+"transparent_fog=0\n").c_str());
        const auto inPlace=frame(1,false,Ui::Separate);
        particle=false;
        const int add[3]{int(.35f*255+.5f),int(.2f*255+.5f),int(.05f*255+.5f)};
        unsigned particlePixels=0,outsideChanged=0;int worst=0,inPlaceDiff=0;
        auto differs=[&](const std::vector<uint8_t>& a,size_t k){return a[k]!=without[k] || a[k+1]!=without[k+1] || a[k+2]!=without[k+2];};
        for(size_t k=0;k<held.size();k+=4) {
            const bool covered=differs(held,k);
            if(covered!=differs(inPlace,k))++outsideChanged;   // the particle covers the same pixels either way; nothing else moved
            if(!covered)continue;
            ++particlePixels;
            for(int ch=0;ch<3;++ch){worst=std::max(worst,std::abs(int(held[k+ch])-std::min(255,int(without[k+ch])+add[ch])));inPlaceDiff=std::max(inPlaceDiff,std::abs(int(held[k+ch])-int(inPlace[k+ch])));}
        }
        std::fprintf(stderr,"transparent fog: %u particle pixels, held = fogged world + particle within %d/255; the particle fogged as if at the wall differs by %d\n",
                     particlePixels,worst,inPlaceDiff);
        CHECK(particlePixels>20 && worst<=1 && inPlaceDiff>8 && outsideChanged==0);
        // The layer: kept in place (counted), so the image is the same with and without the feature.
        writeIni((base+"transparent_fog=1\n").c_str());
        materialLayer=true;
        const auto layerOn=frame(1,false,Ui::Separate);
        CHECK(st.deferLayers>=1 && st.deferred==1);
        writeIni((base+"transparent_fog=0\n").c_str());
        const auto layerOff=frame(1,false,Ui::Separate);
        materialLayer=false;
        CHECK(maxDiff(layerOn,layerOff)==0 && maxDiff(layerOn,without)>2);
        // Fog off: nothing is held back, the particle is drawn where it was.
        writeIni((base+"transparent_fog=1\n").c_str());
        particle=true;frame(0,false,Ui::Separate);particle=false;
        CHECK(st.deferred==1);
        writeIni(base.c_str());
    }
    // Game sun: a WoW-shaped light block (memory_sun.hpp) in this process, found through DEPTH_PROBE_MEM_SUN_STATIC. sun_source=game
    // must give exactly the fog of sun_source=manual with that direction (and not the fog.ini direction); a sun below the
    // horizon gives the fog without a directional term.
    if constexpr(sizeof(void*)==4) {
        static uint32_t sunPointer;alignas(16) static float sunBlock[0xB8/4];
        auto plant=[&](float x,float y,float z){const float l=std::sqrt(x*x+y*y+z*z);const float dir[3]{-x/l,-y/l,-z/l},colors[9]{.2f,.25f,.36f,.72f,.48f,.35f,.98f,.93f,.87f};
            std::memcpy(reinterpret_cast<uint8_t*>(sunBlock)+0x7C,dir,12);std::memcpy(reinterpret_cast<uint8_t*>(sunBlock)+0x88,colors,36);};
        sunPointer=uint32_t(uintptr_t(sunBlock));plant(-.6f,.5f,.6245f);
        char pointer[32];std::snprintf(pointer,sizeof pointer,"%llx",(unsigned long long)(uintptr_t)&sunPointer);
        setenv("DEPTH_PROBE_MEM_SUN_STATIC",pointer,1);
        writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=game\n");
        const auto game=frame(1,false,Ui::Separate);
        CHECK(st.gameSun.ok && st.lastSunFromGame && std::fabs(st.gameSun.elevationDeg-38.65f)<.1f);
        char manualDir[96];std::snprintf(manualDir,sizeof manualDir,"temporal_weight=0\nnoise_amount=0\nsun_source=manual\nsun_direction=%.7f,%.7f,%.7f\n",
                                         double(st.gameSun.toward[0]),double(st.gameSun.toward[1]),double(st.gameSun.toward[2]));
        writeIni(manualDir);
        const auto manual=frame(1,false,Ui::Separate);
        CHECK(!st.lastSunFromGame);
        std::fprintf(stderr,"game sun: vs manual same direction max |diff| %d, vs fog.ini direction %d\n",maxDiff(game,manual),maxDiff(game,fogged));
        CHECK(maxDiff(game,manual)<=1 && maxDiff(game,fogged)>2);
        plant(-.6f,.5f,-.3f);   // below the horizon
        writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=game\n");
        const auto night=frame(1,false,Ui::Separate);
        writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=manual\nsun_enabled=0\n");
        const auto noSun=frame(1,false,Ui::Separate);
        std::fprintf(stderr,"game sun below the horizon vs no sun: max |diff| %d\n",maxDiff(night,noSun));
        CHECK(st.gameSun.elevationDeg<0 && maxDiff(night,noSun)<=1);
        // Ambient from the game: the block's ambient (.2,.25,.36) x ambient_scale 1.5 must give exactly the fog of that colour
        // entered by hand, and not the fog.ini colour (.3,.3,.35).
        writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=game\nambient_scale=1.5\n");
        const auto gameAmbient=frame(1,false,Ui::Separate);
        CHECK(st.lastAmbientFromGame && std::fabs(st.lastAmbient[2]-.54f)<1e-5f);
        writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=manual\nambient=0.3,0.375,0.54\n");
        const auto manualAmbient=frame(1,false,Ui::Separate);
        CHECK(!st.lastAmbientFromGame);
        std::fprintf(stderr,"game ambient: vs same colour by hand max |diff| %d, vs fog.ini colour %d\n",maxDiff(gameAmbient,manualAmbient),maxDiff(gameAmbient,fogged));
        CHECK(maxDiff(gameAmbient,manualAmbient)<=1 && maxDiff(gameAmbient,fogged)>2);
        // The sun's colour from the game (the glow colour, block +0xA0 = (.98,.93,.87) here) x intensity 4 must equal that
        // colour entered by hand, and differ from the fixed fog.ini colour.
        {
            writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=manual\nsun_color_source=game\nsun_color=4,4,4\n");
            const auto glow=frame(1,false,Ui::Separate);
            CHECK(st.lastSunColorFromGame);
            writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=manual\nsun_color_source=manual\nsun_color=3.92,3.72,3.48\n");
            const auto byHand=frame(1,false,Ui::Separate);
            writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=manual\nsun_color_source=manual\nsun_color=4,4,4\n");
            const auto white=frame(1,false,Ui::Separate);
            std::fprintf(stderr,"game glow colour: vs same colour by hand max |diff| %d, vs white %d\n",maxDiff(glow,byHand),maxDiff(glow,white));
            CHECK(!st.lastSunColorFromGame && maxDiff(glow,byHand)<=1 && maxDiff(glow,white)>2);
        }
        // Weather (memory_sun.hpp): a WoW-shaped weather object with rain on, found through DEPTH_PROBE_MEM_WEATHER. Medium rain
        // (0.466, the live Booty Bay value) hides the sun: exactly the fog without a sun; weather_sun=0 ignores it; light rain
        // (0.35) halves it (= sun colour 2 by hand); rain over (no effect object) does not dim, whatever the intensity.
        {
            static uint32_t weatherPointer;alignas(16) static uint8_t weather[0x150];
            auto rain=[&](float intensity,uint32_t effect){std::memcpy(weather,&intensity,4);std::memcpy(weather+8,&intensity,4);std::memcpy(weather+0x13C,&effect,4);};
            weatherPointer=uint32_t(uintptr_t(weather));rain(.466f,0x1234);
            char where[32];std::snprintf(where,sizeof where,"%llx",(unsigned long long)(uintptr_t)&weatherPointer);setenv("DEPTH_PROBE_MEM_WEATHER",where,1);
            const std::string base="temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=manual\nsun_color_source=manual\nsun_color=4,4,4\n";
            writeIni((base+"weather_sun=1\n").c_str());
            const auto rainy=frame(1,false,Ui::Separate);
            CHECK(st.weather.ok && st.weather.kind==1 && std::fabs(st.weather.intensity-.466f)<1e-6f && st.lastSunWeatherScale==0.f);
            CHECK(fog_menu_label(state,11)=="SUN LIGHT: 4.0 (RAIN 0.47, SUN 0%)");
            writeIni((base+"sun_enabled=0\n").c_str());
            const auto sunless=frame(1,false,Ui::Separate);
            writeIni((base+"weather_sun=0\n").c_str());
            const auto ignored=frame(1,false,Ui::Separate);
            CHECK(st.lastSunWeatherScale==1.f);
            rain(.35f,0x1234);
            writeIni((base+"weather_sun=1\n").c_str());
            const auto light=frame(1,false,Ui::Separate);
            CHECK(std::fabs(st.lastSunWeatherScale-.5f)<1e-5f);
            writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=manual\nsun_color_source=manual\nsun_color=2,2,2\nweather_sun=0\n");
            const auto half=frame(1,false,Ui::Separate);
            rain(.466f,0);
            writeIni((base+"weather_sun=1\n").c_str());
            const auto over=frame(1,false,Ui::Separate);
            CHECK(st.weather.ok && st.weather.kind==0 && st.lastSunWeatherScale==1.f);
            std::fprintf(stderr,"weather: medium rain vs no sun max |diff| %d, vs weather ignored %d; light rain vs half sun %d; rain over vs ignored %d\n",
                         maxDiff(rainy,sunless),maxDiff(rainy,ignored),maxDiff(light,half),maxDiff(over,ignored));
            CHECK(maxDiff(rainy,sunless)<=1 && maxDiff(rainy,ignored)>2 && maxDiff(light,half)<=1 && maxDiff(over,ignored)<=1);
            unsetenv("DEPTH_PROBE_MEM_WEATHER");
        }
        // The sun/moon disc WoW draws in the sky (a static vector, not where the lighting points): sun_position=sky must give
        // exactly the fog of that direction entered by hand; sun_position=light the block's direction.
        {
            alignas(16) static float disc[3];
            const float toward[3]{.2f,-.3f,.93f};const float l=std::sqrt(toward[0]*toward[0]+toward[1]*toward[1]+toward[2]*toward[2]);
            for(int i=0;i<3;++i)disc[i]=-toward[i]/l;
            plant(-.6f,.5f,.6245f);
            char where[32];std::snprintf(where,sizeof where,"%llx",(unsigned long long)(uintptr_t)disc);setenv("DEPTH_PROBE_MEM_SKY_SUN",where,1);
            writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=game\nsun_position=sky\nambient_source=manual\n");
            const auto fromDisc=frame(1,false,Ui::Separate);
            CHECK(st.gameSun.skyOk && st.lastSunFromDisc && std::fabs(st.gameSun.skyToward[2]-toward[2]/l)<1e-6f);
            char manualDisc[160];std::snprintf(manualDisc,sizeof manualDisc,"temporal_weight=0\nnoise_amount=0\nsun_source=manual\nambient_source=manual\nsun_direction=%.7f,%.7f,%.7f\n",
                                              double(toward[0]/l),double(toward[1]/l),double(toward[2]/l));
            writeIni(manualDisc);
            const auto byHand=frame(1,false,Ui::Separate);
            writeIni("temporal_weight=0\nnoise_amount=0\nsun_source=game\nsun_position=light\nambient_source=manual\n");
            const auto fromLight=frame(1,false,Ui::Separate);
            CHECK(!st.lastSunFromDisc && st.lastSunFromGame);
            std::fprintf(stderr,"sky disc: vs same direction by hand max |diff| %d, vs the lighting direction %d\n",maxDiff(fromDisc,byHand),maxDiff(fromDisc,fromLight));
            CHECK(maxDiff(fromDisc,byHand)<=1 && maxDiff(fromDisc,fromLight)>2);
            unsetenv("DEPTH_PROBE_MEM_SKY_SUN");
        }
        // With the game light read, the disc finder searches only along the light's azimuth (WoW draws its disc there; Booty
        // Bay's bright midday sky threw the whole-sky search onto haze and horizon glints): the disc on that arc is found and
        // kept exactly on it, the same disc with the light's azimuth 50 degrees away is no sighting, and while the camera
        // turns nothing is searched.
        {
            writeIni("temporal_weight=0\nnoise_amount=0\nshadows=1\nsun_source=game\nsun_position=sky\nambient_source=manual\n");
            auto forget=[&]{moveCamera(230);frame(1,false,Ui::Separate);moveCamera(0);for(int i=0;i<4;++i)frame(1,false,Ui::Separate);};
            const float discAzimuth=std::atan2(sunDiscDir.y,sunDiscDir.x);
            forget();
            plant(sunDiscDir.x,sunDiscDir.y,.5f);   // the light's azimuth is the disc's, its elevation another (the arc is vertical)
            auto before=st.seenDiscCount;
            sunDisc=true;for(int i=0;i<10;++i)frame(1,false,Ui::Separate);sunDisc=false;
            const float c=st.seenDisc[0]*sunDiscDir.x+st.seenDisc[1]*sunDiscDir.y+st.seenDisc[2]*sunDiscDir.z;
            const float deg=std::acos(std::min(1.f,c))*180/3.14159265f;
            const float azimuthOff=std::fabs(std::atan2(st.seenDisc[1],st.seenDisc[0])-discAzimuth)*180/3.14159265f;
            std::fprintf(stderr,"sun finder on the light's arc: %llu sightings, off by %.2f degrees, azimuth off by %.5f\n",
                         (unsigned long long)(st.seenDiscCount-before),deg,azimuthOff);
            CHECK(st.seenDiscCount>before && deg<2.f && azimuthOff<.01f);
            forget();
            plant(std::cos(discAzimuth+.8727f),std::sin(discAzimuth+.8727f),.5f);   // 50 degrees away
            before=st.seenDiscCount;
            sunDisc=true;for(int i=0;i<10;++i)frame(1,false,Ui::Separate);sunDisc=false;
            std::fprintf(stderr,"sun finder, light 50 degrees away: %llu sightings\n",(unsigned long long)(st.seenDiscCount-before));
            CHECK(st.seenDiscCount==before && !st.seenDiscValid);
            forget();
            plant(sunDiscDir.x,sunDiscDir.y,.5f);
            before=st.seenDiscCount;
            const auto turningBefore=st.discStats.turning;
            sunDisc=true;
            for(int i=1;i<=10;++i){   // yaw 1 degree a frame
                const float y=float(i)*3.14159265f/180.f;
                const Vec3 f{std::cos(pitch)*std::cos(y),std::cos(pitch)*std::sin(y),-std::sin(pitch)},l{-std::sin(y),std::cos(y),0},
                           u{std::sin(pitch)*std::cos(y),std::sin(pitch)*std::sin(y),std::cos(pitch)};
                put(0x14,&f,12);put(0x20,&l,12);put(0x2C,&u,12);frame(1,false,Ui::Separate);
            }
            sunDisc=false;
            put(0x14,&forward,12);put(0x20,&left,12);put(0x2C,&up,12);
            std::fprintf(stderr,"sun finder while turning: %llu sightings, %llu searches skipped\n",(unsigned long long)(st.seenDiscCount-before),
                         (unsigned long long)(st.discStats.turning-turningBefore));
            CHECK(st.seenDiscCount==before && st.discStats.turning-turningBefore>=8);
            forget();
        }
        unsetenv("DEPTH_PROBE_MEM_SUN_STATIC");
    } else std::fprintf(stderr,"game sun: skipped (the WoW block holds 32-bit pointers)\n");

    camera_hud_input().fogMode=0;
    {std::lock_guard lock(mutex);fog_destroy(state);}
    std::remove(ini.c_str());
    std::fprintf(stderr,"validation: %s, %u errors\n",validate?"on":"off",errors.load());
    CHECK(!errors);
    // Tear the layer's per-device state down now, while the driver chain (and the validation layer) is alive: left to static
    // destruction it destroyed our pipelines after the validation layer's globals were gone (abort at exit).
    ok(D(vkDeviceWaitIdle)(device));
    {std::lock_guard lock(mutex);devices.erase(device);}
    std::puts("PASS: volumetric fog composited before UI (UI bit-exact), matches the CPU physics reference, stops at the water surface, split world fogged once, UI-sharing targets dropped, camera-relative base, menu saves, temporal converges, resets on cuts and weighs history less while moving, noise world-anchored, game sun direction used (below horizon = no sun term), game ambient colour used, sun shadows follow the analytic reference without false shadows on open ground, height memory keeps an off-screen canopy (sun + sky light) and forgets it on a teleport, the sun can come from the sky disc, fog capture writes what was drawn, the sun map lets light through canopy gaps at a slant, the sun takes the game glow colour, the light gizmo marks the fog sun, the sun disc is found on the picture, the sun is weaker under an open sky, rain hides the sun, particles are drawn after the fog (transparent_fog), sky_fog spares the sky, a yellow sun disc is found, the disc is searched only on the game light's arc and not while the camera turns, light_limit bends the fog light, auto exposure darkens a bright frame, the game's sun cascades shadow the fog and give its sun, the sun aureole lobe matches the CPU reference, sun glare follows the disc's visible share, sun rays fan past an edge, not round a disc in open sky, scene shading (ambient occlusion, contact and terrain shadows) where it belongs, land shadows from land only and only from terrain_shadows_from on, point-light shadows behind what blocks a light but not from its own brazier, fading out behind the caster, the floor under water is its surface");
    return 0;
}
