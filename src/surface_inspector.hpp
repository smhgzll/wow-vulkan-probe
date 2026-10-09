#pragma once
#include "discovery_internal.hpp"
#include "overlay_menu.hpp"
#include "inspector_assets.hpp"

namespace depth_probe {
struct DepthOverlayState;struct DepthOverlayFns;struct OverlayUniform;
bool inspector_enabled();
bool inspector_menu_input(OverlayMenu&,const MenuInput&,bool click);
void inspector_context(VkDevice,DepthOverlayState&);
void populate_inspector_menu(VkDevice,OverlayUniform&);
void inspector_request_pick(VkDevice,uint32_t x,uint32_t y);
void inspector_set_highlight(VkDevice,bool);
void inspector_present_begin(VkDevice,VkQueue,const VkPresentInfoKHR*);
void inspector_cancel(VkDevice);
// Called only after the existing per-image fence wait, under lifecycle lock.
bool create_inspector_gpu(const DeviceData&,const DepthOverlayFns&,DepthOverlayState&);
void destroy_inspector_gpu(const DeviceData&,const DepthOverlayFns&,DepthOverlayState&);
void record_inspector_preview(const DepthOverlayFns&,const DepthOverlayState&,uint32_t);
void record_inspector_gpu(const DepthOverlayFns&,const DepthOverlayState&,uint32_t);
VkResult update_inspector_gpu(const DeviceData&,const DepthOverlayFns&,DepthOverlayState&,uint32_t,OverlayUniform&);
void release_inspector_image(VkDevice,VkImage);
}
namespace depth_probe::discovery {
struct InspectorPipeline {
    VkDevice device{};PFN_vkDestroyPipeline destroy{};VkPipeline query{},tint{};
    bool dynamicScissorCount=false;
    void release(){if(query)destroy(device,query,nullptr);if(tint)destroy(device,tint,nullptr);query=tint=VK_NULL_HANDLE;}
    ~InspectorPipeline(){release();}
};
struct PickQueries {
    VkDevice device{};PFN_vkDestroyQueryPool destroy{};PFN_vkGetQueryPoolResults results{};
    VkQueryPool pool{};uint64_t ticket=0;uint32_t used=0;
    // Samples and availability cached before an application may reset its CB.
    std::array<std::array<uint64_t,2>,1024> values{};
    void release(){if(pool)destroy(device,pool,nullptr);pool=VK_NULL_HANDLE;}
    ~PickQueries(){release();}
    void poll();
};
struct Inspector {
    VkDevice device{};VkSwapchainKHR swapchain{};
    uint32_t width=0,height=0,x=0,y=0,ticket=0,completedTicket=0;
    uint64_t depthImage=0,depthGeneration=0,queue=0,presentImage=0;
    uint32_t page=0,texture=0; // home, inspect, selected, textures, shader, deferred, advanced
    uint32_t pools=0,age=0;
    bool armed=false,staged=false,captureDepth=false,depthReady=false,highlight=false,preview=false,finalVisible=false;
    float depth=0;
    std::string reason="Click Inspect Surface to begin";
    std::string previewStatus="Preview not requested";
    std::string previewDiagnostic;
    std::shared_ptr<AssetExport> assetExport;
    Frame frame;
    std::optional<SubmittedDraw> selected;
    // Historical, positive GPU coverage at this coordinate, before compositing.
    // These are inspectable candidates, never proof of the final visible owner.
    bool geometryMode=true,geometryCandidate=false,candidateLimit=false;
    uint32_t candidateIndex=0;
    uint64_t candidateFrame=0;
    std::vector<SubmittedDraw> geometryCandidates;
    uint64_t drawIdentity=0,meshIdentity=0,materialIdentity=0;
    size_t uniformCaptureBytes=0,uniformCaptureRecords=0;
    size_t uniformGpuBytes=0,uniformGpuRecords=0;
    std::set<uint64_t> autoUniformExports;
};
Inspector& inspector_state(Device&,VkDevice);
void create_inspector_clones(const DeviceData&,Device&,Pipeline&,const VkGraphicsPipelineCreateInfo&);
void inspector_begin_command(const DeviceData&,Device&,Command&,VkCommandBuffer);
void inspector_observe_draw(const DeviceData&,Device&,Command&,Draw&,VkCommandBuffer);
VkPipeline inspector_tint(const DeviceData&,Device&,const Draw&);
void inspector_poll(Device&,Inspector&);
uint64_t texture_identity(const View&);
uint64_t mesh_identity(const Draw&);
uint64_t material_identity(const SubmittedDraw&);
const char* inspector_selection_scope(const Inspector&);
bool select_geometry_candidate(Inspector&,uint32_t index);
// Returns false only while a query is pending. Last-scene mode rejects ambiguity;
// geometry mode exposes explicitly labelled historical coverage candidates.
bool resolve_pick(Inspector&);
}
