// Micro-benchmark of the layer's hot command-buffer hooks against a mock driver (no GPU): nanoseconds per hooked call.
// Not part of ctest. Usage: hook_bench [iterations]
#include "discovery_internal.hpp"
#include "pipeline_discovery.hpp"
#include "sync_tracking.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
using namespace depth_probe;
namespace disc=depth_probe::discovery;
#define H(api) reinterpret_cast<PFN_##api>(discovery_hook(#api))
struct Dispatchable {void* dispatch;};
static int key;static Dispatchable dev{&key},queueObject{&key},buffers[4];static unsigned allocated=0;
static VkDevice device=reinterpret_cast<VkDevice>(&dev);
static uint64_t nextHandle=100;
static VkResult allocate(VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out){for(uint32_t i=0;i<info->commandBufferCount;++i){buffers[allocated].dispatch=&key;out[i]=reinterpret_cast<VkCommandBuffer>(&buffers[allocated++]);}return VK_SUCCESS;}
static VkResult okResult(){return VK_SUCCESS;}
static void noop(){}
static VkResult pipelines(VkDevice,VkPipelineCache,uint32_t count,const VkGraphicsPipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline* out){for(uint32_t i=0;i<count;++i)out[i]=(VkPipeline)nextHandle++;return VK_SUCCESS;}
static PFN_vkVoidFunction VKAPI_CALL dispatch(VkDevice,const char* name) {
    if(!std::strcmp(name,"vkAllocateCommandBuffers"))return reinterpret_cast<PFN_vkVoidFunction>(allocate);
    if(!std::strcmp(name,"vkCreateGraphicsPipelines"))return reinterpret_cast<PFN_vkVoidFunction>(pipelines);
    if(!std::strncmp(name,"vkCmd",5))return reinterpret_cast<PFN_vkVoidFunction>(noop);
    return reinterpret_cast<PFN_vkVoidFunction>(okResult);
}
template<class F> double measure(const char* label,unsigned iterations,F&& body) {
    for(unsigned i=0;i<1000;++i)body(i);
    const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<iterations;++i)body(i);
    const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
    std::printf("%-34s %8.1f ns/call\n",label,ns);return ns;
}
int main(int argc,char** argv) {
    const unsigned iterations=argc>1?unsigned(std::atoi(argv[1])):500000;
    setenv("DEPTH_PROBE_PIPELINE_EXPLORER","1",1);setenv("DEPTH_PROBE_SURFACE_INSPECTOR","1",1);setenv("DEPTH_PROBE_UI_INPUT","0",1);setenv("DEPTH_PROBE_DEPTH_PRESENT","0",1);
    setenv("DEPTH_PROBE_LAYER_LOG","/tmp/depth-probe-hook-bench.log",1);setenv("DEPTH_PROBE_WATER_SETTINGS","/dev/null/depth-probe-water.ini",1);setenv("DEPTH_PROBE_DISCOVERY_DIR","/tmp/depth-probe-hook-bench-dir",1);
    DeviceData data;data.device=device;data.next_gdpa=dispatch;data.sync=std::make_shared<SyncDeviceState>();g_devices[&key]=data;
    VkCommandBufferAllocateInfo ca{};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd{};
    if(H(vkAllocateCommandBuffers)(device,&ca,&cmd)!=VK_SUCCESS){std::fprintf(stderr,"allocate failed\n");return 1;}
    VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;H(vkBeginCommandBuffer)(cmd,&bi);
    VkViewport vp{0,0,1920,1080,0,1};VkRect2D sc{{0,0},{1920,1080}};
    VkGraphicsPipelineCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;VkPipeline pipeline{};H(vkCreateGraphicsPipelines)(device,{},1,&pi,nullptr,&pipeline);
    std::printf("iterations per measurement: %u\n",iterations);
    double total=0;unsigned counted=0;
    // The application resolves an entry point once (vkGetDeviceProcAddr); resolving it inside the timed loop would measure the name
    // search instead of the hook. A hook that does not exist is a plain pass-through to the driver (not measured).
    auto viewport=H(vkCmdSetViewport);auto scissor=H(vkCmdSetScissor);auto depthWrite=H(vkCmdSetDepthWriteEnable);auto bias=H(vkCmdSetDepthBias);
    auto stencil=H(vkCmdSetStencilReference);auto bindPipeline=H(vkCmdBindPipeline);auto draw=H(vkCmdDraw);
    auto bench=[&](const char* label,auto hook,auto&& call){
        if(!hook){std::printf("%-34s not hooked (pass-through)\n",label);return;}
        total+=measure(label,iterations,call);++counted;
    };
    bench("vkCmdSetViewport",viewport,[&](unsigned){viewport(cmd,0,1,&vp);});
    bench("vkCmdSetScissor",scissor,[&](unsigned){scissor(cmd,0,1,&sc);});
    bench("vkCmdSetDepthWriteEnable",depthWrite,[&](unsigned i){depthWrite(cmd,i&1);});
    bench("vkCmdSetDepthBias",bias,[&](unsigned){bias(cmd,0,0,0);});
    bench("vkCmdSetStencilReference",stencil,[&](unsigned i){stencil(cmd,VK_STENCIL_FACE_FRONT_AND_BACK,i&7);});
    bench("vkCmdBindPipeline",bindPipeline,[&](unsigned){bindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);});
    bench("vkCmdDraw",draw,[&](unsigned){draw(cmd,3,1,0,0);});
    // The loop above hammers ONE command buffer, so after maxCommandEvents draws every draw takes the "truncated" early-out. A real frame records a
    // few hundred draws per command buffer and resets it: measure that (draw snapshot + the reset that frees it), alone and with the usual state calls.
    {
        auto reset=H(vkResetCommandBuffer);auto begin=H(vkBeginCommandBuffer);auto end=H(vkEndCommandBuffer);
        auto indexed=H(vkCmdDrawIndexed);auto push=H(vkCmdPushConstants);auto bindSets=H(vkCmdBindDescriptorSets);auto vertices=H(vkCmdBindVertexBuffers);auto indices=H(vkCmdBindIndexBuffer);
        const unsigned perBuffer=400;VkDescriptorSet sets[3]{(VkDescriptorSet)1,(VkDescriptorSet)2,(VkDescriptorSet)3};VkBuffer buffer=(VkBuffer)7;VkDeviceSize offset=0;float constants[24]{};
        H(vkEndCommandBuffer)(cmd);
        std::printf("--- realistic recording: %u draws per command buffer, then reset ---\n",perBuffer);
        auto cycle=[&](unsigned i,bool stateCalls){
            if(i%perBuffer==0){reset(cmd,0);begin(cmd,&bi);bindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);viewport(cmd,0,1,&vp);scissor(cmd,0,1,&sc);}
            if(stateCalls){
                if(i%4==0)bindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
                bindSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkPipelineLayout)9,0,3,sets,0,nullptr);
                push(cmd,(VkPipelineLayout)9,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,96,constants);
                vertices(cmd,0,1,&buffer,&offset);indices(cmd,buffer,0,VK_INDEX_TYPE_UINT16);
                viewport(cmd,0,1,&vp);scissor(cmd,0,1,&sc);depthWrite(cmd,i&1);
            }
            indexed(cmd,600,1,0,0,0);
        };
        const double drawOnly=measure("draw + amortized reset",iterations/2,[&](unsigned i){cycle(i,false);});
        const double withState=measure("draw with 8 state calls",iterations/2,[&](unsigned i){cycle(i,true);});
        std::printf("=> a draw with its usual state calls costs %.2f us of layer CPU time; at 1700 draws per frame that is %.2f ms (WoW measured 12.5 us/draw in total, game included)\n",withState/1000,withState*1700/1e6);
        (void)drawOnly;
    }
    // Pieces every hook pays for.
    std::printf("--- pieces ---\n");
    measure("get_device_data (copies DeviceData)",iterations,[&](unsigned){auto d=get_device_data(device);asm volatile(""::"r"(&d):"memory");});
    measure("lock_guard(disc::mutex) only",iterations,[&](unsigned){std::lock_guard lock(disc::mutex);asm volatile(""::: "memory");});
    measure("lock + device_state lookup",iterations,[&](unsigned){std::lock_guard lock(disc::mutex);auto& s=disc::device_state(device);asm volatile(""::"r"(&s):"memory");});
    measure("cached_downstream lookup",iterations,[&](unsigned){auto d=g_devices.at(&key);auto f=disc::cached_downstream(d,"vkCmdSetViewport");asm volatile(""::"r"(f):"memory");});
    if(counted)std::printf("mean over %u hooked calls: %.1f ns; at 40000 hooked calls per frame that is %.2f ms CPU per frame\n",counted,total/counted,total/counted*40000/1e6);
    return 0;
}
