#include <chrono>
#include <set>
#include <algorithm>
#include "surface_inspector.hpp"
#include "water_reflections.hpp"
#include "depth_tracking.hpp"
#include "inspector_uniform_gpu.hpp"
#include "overlay_internal.hpp"
#include "shader_resources.hpp"
#include <cstdio>
#include <cmath>
#include <unistd.h>
#include <filesystem>
#include <fstream>
using namespace depth_probe;using namespace depth_probe::discovery;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
namespace {
unsigned cloneCalls=0,moduleCalls=0;VkResult cloneResult=VK_SUCCESS;
VkBool32 expectedClip=VK_TRUE;
VkResult VKAPI_CALL fakeModules(VkDevice,const VkShaderModuleCreateInfo*,const VkAllocationCallbacks*,VkShaderModule* out) {
    *out=(VkShaderModule)uint64_t(++moduleCalls);return VK_SUCCESS;
}
void VKAPI_CALL freeModule(VkDevice,VkShaderModule,const VkAllocationCallbacks*) {}
void VKAPI_CALL freePipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*) {}
VkResult VKAPI_CALL fakeClones(VkDevice,VkPipelineCache,uint32_t count,const VkGraphicsPipelineCreateInfo* info,const VkAllocationCallbacks*,VkPipeline* out) {
    CHECK(count==1);++cloneCalls;
    auto* clip=chain<VkPipelineRasterizationDepthClipStateCreateInfoEXT>(info->pRasterizationState->pNext,VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT);
    CHECK(clip && clip->depthClipEnable==expectedClip && !clip->flags);
    if(cloneResult==VK_SUCCESS)*out=(VkPipeline)uint64_t(cloneCalls);
    return cloneResult;
}
PFN_vkVoidFunction VKAPI_CALL cloneDispatch(VkDevice,const char* name) {
    if(!std::strcmp(name,"vkCreateShaderModule"))return reinterpret_cast<PFN_vkVoidFunction>(fakeModules);
    if(!std::strcmp(name,"vkDestroyShaderModule"))return reinterpret_cast<PFN_vkVoidFunction>(freeModule);
    if(!std::strcmp(name,"vkCreateGraphicsPipelines"))return reinterpret_cast<PFN_vkVoidFunction>(fakeClones);
    if(!std::strcmp(name,"vkDestroyPipeline"))return reinterpret_cast<PFN_vkVoidFunction>(freePipeline);
    return nullptr;
}
void clone_diagnostics() {
    Device state;DeviceData data;data.next_gdpa=cloneDispatch;
    auto shader=std::make_shared<Shader>();shader->words={0x07230203};
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{};clip.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;clip.depthClipEnable=VK_TRUE;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.pNext=&clip;
    VkPipelineViewportStateCreateInfo viewport{};viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;viewport.viewportCount=viewport.scissorCount=1;
    VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend{};blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;blend.attachmentCount=1;blend.pAttachments=&attachment;
    VkPipelineShaderStageCreateInfo stage{};stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.stage=VK_SHADER_STAGE_VERTEX_BIT;stage.pName="main";
    VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;info.pRasterizationState=&raster;info.pViewportState=&viewport;info.pColorBlendState=&blend;info.stageCount=1;info.pStages=&stage;
    auto attempt=[&] {Pipeline p;p.vs=shader;create_inspector_clones(data,state,p,info);return p;};
    {auto p=attempt();CHECK(p.queryStatus=="ready" && p.tintStatus=="ready" && p.inspector);CHECK(cloneCalls==2 && moduleCalls==1);}
    CHECK(raster.pNext==&clip && clip.depthClipEnable && attachment.colorWriteMask==15 && !attachment.blendEnable);
    expectedClip=clip.depthClipEnable=VK_FALSE;{auto p=attempt();CHECK(p.queryStatus=="ready");}
    auto before=cloneCalls;
    info.flags=VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    {auto p=attempt();CHECK(!p.inspector && p.queryStatus.find("pipeline-flags:")!=std::string::npos);CHECK(cloneCalls==before);}
    info.flags=0;VkPipelineRasterizationConservativeStateCreateInfoEXT unsupported{};unsupported.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT;raster.pNext=&unsupported;
    {auto p=attempt();CHECK(!p.inspector && p.queryStatus.find("raster-pnext:")!=std::string::npos);CHECK(cloneCalls==before);}
    raster.pNext=&clip;cloneResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    {auto p=attempt();CHECK(!p.inspector && p.queryStatus=="pipeline-create-result:-2");}
    cloneResult=VK_SUCCESS;
    VkPipeline missing=(VkPipeline)uint64_t(444);VkPipelineLibraryCreateInfoKHR link{};link.sType=VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;link.libraryCount=1;link.pLibraries=&missing;info.pNext=&link;
    {auto p=attempt();CHECK(!p.inspector && p.queryStatus.find("linked-clone-missing:")==0);}
    info.pNext=nullptr;
    for(unsigned i=0;i<1024;++i)state.pipelines[i]={};
    {auto p=attempt();CHECK(!p.inspector && p.queryStatus=="budget:live-pipelines>=1024");}
    for(unsigned i=0;i<100;++i)inspector_diagnostic(state,"test:"+std::to_string(i),"bounded");
    CHECK(state.diagnosticKeys.size()==64);
}
}
#include "inspector_asset_cases.hpp"
#include "inspector_memory_cases.hpp"
// A real WoW water pipeline was once refused only as "recipe-unsupported". Each
// cause must now be named, and the DXVK vertex-input shapes that replay of one
// instance can handle (instance-rate binding, attribute divisor pNext) accepted.
static void water_settings_cases() {
    // Saved water settings: profile, mode, strength, layers, tone. Bad values are refused individually; the rest still load.
    WaterState saved;saved.profileSaved=true;saved.selectedVS=wowWaterVS;saved.selectedFS=wowWaterFS;saved.mode=2;saved.strength=.9f;saved.layers=waterLayerReflection|waterLayerFoam;saved.tone=3;
    WaterState loaded;CHECK(water_settings_parse(loaded,water_settings_text(saved)));
    CHECK(loaded.profileSaved && loaded.selectedVS==wowWaterVS && loaded.selectedFS==wowWaterFS && loaded.depthGeneration==0 && loaded.mode==2 && std::abs(loaded.strength-.9f)<1e-5f && loaded.layers==(waterLayerReflection|waterLayerFoam) && loaded.tone==3);
    WaterState unsaved;unsaved.mode=1;CHECK(water_settings_text(unsaved).find("profile=0")!=std::string::npos); // no selected water: nothing to restore
    WaterState bad;bad.mode=1;bad.strength=.5f;
    CHECK(!water_settings_parse(bad,"mode=7\nstrength=nan\nlayers=99\ntone=-1\nunknown=1\n# comment\nnonsense\nprofile=2\n"));
    CHECK(bad.mode==1 && bad.strength==.5f && bad.layers==waterLayersAll && bad.tone==1 && !bad.profileSaved); // every invalid value kept its previous value
    WaterState partial;CHECK(!water_settings_parse(partial,"mode=2\ntone=99\nstrength=1.2\n"));CHECK(partial.mode==2 && partial.tone==1 && std::abs(partial.strength-1.2f)<1e-5f);
    {
        // Water details: every look option round-trips, out-of-range values keep the previous value, the menu cycles through the listed steps and wraps,
        // and reset restores the defaults.
        WaterState look;look.look.foamAmount=1.5f;look.look.foamSize=.75f;look.look.foamSpeed=0.f;look.look.foamSoftness=1.f;look.look.foamPulse=.35f;look.look.foamReach=2.5f;look.look.detail=1.5f;look.look.quality=2;
        WaterState back;CHECK(water_settings_parse(back,water_settings_text(look)) && back.look==look.look);
        WaterState wild;CHECK(!water_settings_parse(wild,"foam_amount=9\nfoam_size=0\nfoam_speed=abc\nfoam_softness=1.5\nfoam_pulse=-1\nfoam_reach=100\ndetail=2\nquality=3\n"));CHECK(wild.look==WaterLook{});
        WaterState oldFile;CHECK(water_settings_parse(oldFile,"profile=0\nmode=1\nstrength=.6\nlayers=15\ntone=2\n") && oldFile.look==WaterLook{});   // a file from before the details page
        CHECK(oldFile.waves==water::WaveSettings{});   // ... and from before the Gerstner waves: the defaults
        // Gerstner waves: every slot, the wind and the speed round-trip; a wave line with a bad or missing number keeps the whole slot.
        WaterState waves;waves.waves.windDeg=-40.f;waves.waves.speed=1.5f;waves.waves.waves[4]={12.f,4.f,.02f,.5f,2.f};waves.waves.waves[0].amplitude=.3f;waves.look.waveHeight=2.f;
        WaterState wavesBack;CHECK(water_settings_parse(wavesBack,water_settings_text(waves)) && wavesBack.waves==waves.waves && wavesBack.look==waves.look);
        WaterState badWave;
        CHECK(!water_settings_parse(badWave,"wave1=10,20,9,1,1\nwave2=10,20,.1\nwave3=a,1,1,1,1\nwave6=1,1,1,1,1\nwind_direction=999\nwave_speed=-1\n"));
        CHECK(badWave.waves==water::WaveSettings{});
        // MODERN: the A/B style, the absorption and the fallback sun round-trip; defaults = modern, pure water, no fallback sun.
        CHECK(oldFile.style==1 && oldFile.absorption[0]==.31f && !oldFile.sunFallback);
        WaterState modern;modern.style=0;modern.absorption[0]=.9f;modern.absorption[2]=.02f;modern.sunFallback=true;modern.sunFallbackDeg[0]=12.f;modern.sunFallbackDeg[1]=-30.f;
        modern.look.roughness=.16f;modern.look.microNormals=0.f;
        WaterState modernBack;CHECK(water_settings_parse(modernBack,water_settings_text(modern)));
        CHECK(modernBack.style==0 && modernBack.absorption[0]==.9f && modernBack.absorption[2]==.02f && modernBack.sunFallback && modernBack.sunFallbackDeg[0]==12.f &&
              modernBack.sunFallbackDeg[1]==-30.f && modernBack.look==modern.look);
        WaterState off;off.sunFallback=true;CHECK(water_settings_parse(off,"sun_fallback=off\n") && !off.sunFallback);
        WaterState badModern;CHECK(!water_settings_parse(badModern,"style=2\nabsorption=1,2\nsun_fallback=95,0\n"));
        CHECK(badModern.style==1 && badModern.absorption[0]==.31f && !badModern.sunFallback);
        WaterLook cycle;
        for(size_t option=0;option<waterLookOptionCount;++option) {
            const auto& o=waterLookOptions[option];std::vector<float> seen;
            for(unsigned i=0;i<o.count;++i){seen.push_back(cycle.*o.value);water_look_cycle(cycle,option);}
            CHECK(std::abs(cycle.*o.value-seen.front())<1e-6f);          // wrapped back to the start after `count` presses
            std::sort(seen.begin(),seen.end());CHECK(std::adjacent_find(seen.begin(),seen.end())==seen.end());   // every press moved to a new step
        }
        cycle=WaterLook{};water_look_cycle(cycle,0);CHECK(cycle.foamAmount==1.5f && water_look_label(cycle,0)=="FOAM AMOUNT: 150%");
        water_look_cycle_quality(cycle);water_look_cycle_quality(cycle);CHECK(cycle.quality==0);
        cycle.foamAmount=1.2f;water_look_cycle(cycle,0);CHECK(cycle.foamAmount==1.5f);   // a value between steps snaps to the next step after the nearest one
        CHECK(water_look_label(cycle,999).empty());
    }
    {
        // The foam clock accumulates (elapsed x speed): changing the speed changes the pace from then on and never jumps the pattern.
        using namespace std::chrono;WaterState w;const auto t0=steady_clock::now();
        water_advance_foam_clock(w,t0);CHECK(w.foamClock==0);                                        // first call only sets the reference
        water_advance_foam_clock(w,t0+milliseconds(100));CHECK(std::abs(w.foamClock-.1)<1e-9);
        w.look.foamSpeed=2.5f;CHECK(std::abs(w.foamClock-.1)<1e-9);                                  // the speed change itself moves nothing
        water_advance_foam_clock(w,t0+milliseconds(200));CHECK(std::abs(w.foamClock-(.1+.25))<1e-9);
        w.look.foamSpeed=0.f;water_advance_foam_clock(w,t0+milliseconds(300));CHECK(std::abs(w.foamClock-.35)<1e-9);   // speed 0 stops it
        w.look.foamSpeed=1.f;water_advance_foam_clock(w,t0+seconds(30));CHECK(std::abs(w.foamClock-(.35+.25))<1e-9);  // a stall is capped
        size_t opacity=waterLookOptionCount;for(size_t i=0;i<waterLookOptionCount;++i)if(std::string(waterLookOptions[i].key)=="game_opacity")opacity=i;
        CHECK(opacity<waterLookOptionCount && WaterLook{}.gameOpacity==1.f && water_look_label(WaterLook{},opacity)=="GAME WATER TEXTURE OPACITY: 100%");
        // Defaults of the seabed/sky effects: on, moderately.
        const WaterLook defaults;CHECK(defaults.clarity==.6f && defaults.caustics==.8f && defaults.glitter==1.f && defaults.whitecaps==.5f);
    }
    // File round trip through the configured path (never the user's).
    const std::string path="/tmp/depth-probe-water-settings-test-"+std::to_string(getpid())+"/nested/water.ini";setenv("DEPTH_PROBE_WATER_SETTINGS",path.c_str(),1);
    CHECK(water_settings_path()==path);water_settings_save(saved);
    WaterState fromFile;water_settings_load(fromFile);CHECK(fromFile.profileSaved && fromFile.mode==2 && fromFile.tone==3 && fromFile.layers==saved.layers);
    std::filesystem::remove_all(std::filesystem::path(path).parent_path().parent_path());
    setenv("DEPTH_PROBE_WATER_SETTINGS","/dev/null/depth-probe-water.ini",1);
}
static void water_recipe_cases() {
    VkVertexInputBindingDescription bindings[2]{{0,12,VK_VERTEX_INPUT_RATE_VERTEX},{1,16,VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription attributes[2]{{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,1,VK_FORMAT_R32G32B32A32_SFLOAT,0}};
    VkPipelineVertexInputDivisorStateCreateInfoEXT divisor{};divisor.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
    VkPipelineVertexInputStateCreateInfo vertex{};vertex.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex.vertexBindingDescriptionCount=2;vertex.pVertexBindingDescriptions=bindings;vertex.vertexAttributeDescriptionCount=2;vertex.pVertexAttributeDescriptions=attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
    VkPipelineDepthStencilStateCreateInfo depth{};depth.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pRasterizationState=&raster;info.pDepthStencilState=&depth;
    auto capture=[&](){Device s;Pipeline p;capture_water_recipe(s,p,info);CHECK(p.water);return p.water;};
    auto recipe=capture();CHECK(recipe->supported && recipe->why.empty() && recipe->bindings.size()==2 && recipe->bindings[1].inputRate==VK_VERTEX_INPUT_RATE_INSTANCE);
    vertex.pNext=&divisor;recipe=capture();CHECK(recipe->supported);
    VkBaseInStructure unknown{VK_STRUCTURE_TYPE_MAX_ENUM,nullptr};vertex.pNext=&unknown;recipe=capture();
    CHECK(!recipe->supported && recipe->why.find("vertex-input-pnext:")!=std::string::npos);vertex.pNext=nullptr;
    // The real WoW water pipeline has depthClampEnable; replay copies rasterInfo verbatim.
    raster.depthClampEnable=VK_TRUE;recipe=capture();CHECK(recipe->supported && recipe->rasterInfo.depthClampEnable==VK_TRUE);
    raster.polygonMode=VK_POLYGON_MODE_LINE;recipe=capture();
    CHECK(!recipe->supported && recipe->why.find("polygon-mode:1")!=std::string::npos && recipe->why.find("depth-clamp")==std::string::npos);
    raster.polygonMode=VK_POLYGON_MODE_FILL;raster.depthClampEnable=VK_FALSE;
    VkPipelineShaderStageCreateInfo stage{};stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.stage=VK_SHADER_STAGE_VERTEX_BIT;stage.flags=1;
    info.stageCount=1;info.pStages=&stage;recipe=capture();CHECK(!recipe->supported && recipe->why.find("stage-flags:")!=std::string::npos);
    // The layer's own depth patch adds SAMPLED after the app's create info was recorded.
    {
        auto image=[](uint64_t handle,VkFormat format,VkImageUsageFlags usage){auto im=std::make_shared<Image>();im->handle=handle;im->generation=1;im->format=format;im->extent={64,64,1};
            im->usage=usage;im->layers=im->mips=1;im->samples=1;return im;};
        auto view=[](const std::shared_ptr<Image>& im){auto v=std::make_shared<View>();v->image=im;v->range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};v->format=im->format;return v;};
        Target target;target.known=true;target.width=target.height=64;
        target.colors={view(image(9001,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT))};
        target.depth=view(image(9002,VK_FORMAT_D24_UNORM_S8_UINT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        VkDevice device=reinterpret_cast<VkDevice>(uintptr_t(0x1000));
        CHECK(std::string(water_target_problem(device,target))=="depth-image-not-sampled");
        DepthCandidate candidate{};candidate.device=device;candidate.image=(VkImage)9002;candidate.sampledReady=true;
        {std::lock_guard lock(g_state_mutex);g_depth_candidates[{device,9002}]=candidate;}
        CHECK(water_target_problem(device,target)==nullptr);
        {std::lock_guard lock(g_state_mutex);g_depth_candidates[{device,9002}].sampledReady=false;}
        CHECK(std::string(water_target_problem(device,target))=="depth-image-not-sampled");
        {std::lock_guard lock(g_state_mutex);g_depth_candidates.erase({device,9002});}
        target.colors[0]->image->usage&=~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;target.depth->image->usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
        CHECK(std::string(water_target_problem(device,target))=="color-image-no-transfer-src");
    }
    // DXVK push ranges cover more stages than VS/FS; they must still be restorable.
    {
        Pipeline p;p.layout=std::make_shared<Layout>();p.layout->pushSignature=77;
        std::array<PushState,2> push{};for(auto& stage:push){stage.signature=77;stage.known=0xfff;}
        auto range=[&](VkShaderStageFlags stages,uint32_t offset,uint32_t size){p.layout->pushRanges={{stages,offset,size}};};
        range(VK_SHADER_STAGE_ALL_GRAPHICS,0,48);CHECK(water_push_problem(p,push)==nullptr);
        range(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,48);CHECK(water_push_problem(p,push)==nullptr);
        range(VK_SHADER_STAGE_GEOMETRY_BIT,0,48);CHECK(std::string(water_push_problem(p,push))=="push-range-no-vertex-or-fragment");
        range(VK_SHADER_STAGE_ALL_GRAPHICS,2,48);CHECK(std::string(water_push_problem(p,push))=="push-range-size");
        range(VK_SHADER_STAGE_ALL_GRAPHICS,0,48);push[1].known=0xff;CHECK(std::string(water_push_problem(p,push))=="push-word-unset");
        push[1].known=0xfff;push[1].words[3]=5;CHECK(std::string(water_push_problem(p,push))=="push-stage-values-differ");
    }
    // The reason survives library linking so a linked pipeline names the failing library.
    Device s;Pipeline library;capture_water_recipe(s,library,info);library.handle=77;s.pipelines[77]=std::make_shared<Pipeline>(library);
    VkPipeline libraries[1]{(VkPipeline)77};VkPipelineLibraryCreateInfoKHR link{};link.sType=VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;link.libraryCount=1;link.pLibraries=libraries;
    VkGraphicsPipelineCreateInfo linked{};linked.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;linked.pNext=&link;
    Pipeline p;capture_water_recipe(s,p,linked);CHECK(!p.water->supported && p.water->why.find("library{")!=std::string::npos && p.water->why.find("stage-flags:")!=std::string::npos);
}
// The water replay's texture layouts (2026-09-29, Booty Bay): a texture the water draw samples is in the layout its descriptor states when
// nothing transitions it after the draw in the same command buffer - whatever the layer's global layout record says. That record is
// wiped by any truncated submit, and static textures are never transitioned again: the water had stayed off for the rest of the session.
static void water_layout_cases() {
    auto image=std::make_shared<Image>();image->handle=77;image->generation=1;image->mips=1;image->layers=1;image->layoutKnown=false;
    auto other=std::make_shared<Image>();other->handle=78;other->generation=1;other->mips=1;other->layers=1;
    auto draw=std::make_shared<Draw>(),elsewhere=std::make_shared<Draw>();
    const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT,0,VK_REMAINING_MIP_LEVELS,0,VK_REMAINING_ARRAY_LAYERS};
    Command c;
    c.events.push_back(Event{nullptr,0,0,LayoutEvent{image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,true,whole}});   // before the draw: does not matter
    c.events.push_back(Event{draw,0,0,{}});
    c.events.push_back(Event{nullptr,0,0,LayoutEvent{other,VK_IMAGE_LAYOUT_GENERAL,true,whole}});                // another image
    bool untouched=false;
    CHECK(layout_since_draw(c,*draw,*image,untouched)==VK_IMAGE_LAYOUT_UNDEFINED && untouched);
    // Transitioned after the draw: that layout (the gate then compares it with the descriptor's).
    c.events.push_back(Event{nullptr,0,0,LayoutEvent{image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,true,whole}});
    CHECK(layout_since_draw(c,*draw,*image,untouched)==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && !untouched);
    // Only part of the image transitioned last, or an unknown layout: not provable.
    image->mips=2;c.events.back().layout.range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    CHECK(layout_since_draw(c,*draw,*image,untouched)==VK_IMAGE_LAYOUT_UNDEFINED && !untouched);
    c.events.back().layout.range=whole;c.events.back().layout.known=false;
    CHECK(layout_since_draw(c,*draw,*image,untouched)==VK_IMAGE_LAYOUT_UNDEFINED && !untouched);
    // A recreated image with the same handle is another image.
    c.events.back().layout.known=true;
    auto recreated=std::make_shared<Image>(*image);recreated->generation=2;
    CHECK(layout_since_draw(c,*draw,*recreated,untouched)==VK_IMAGE_LAYOUT_UNDEFINED && untouched);
    // A draw that is not in this command buffer proves nothing.
    CHECK(!(layout_since_draw(c,*elsewhere,*image,untouched),untouched));
}
int main() {
    setenv("DEPTH_PROBE_WATER_SETTINGS","/dev/null/depth-probe-water.ini",1); /* tests never read or write the user's water settings */
    setenv("DEPTH_PROBE_SURFACE_INSPECTOR","1",1);setenv("DEPTH_PROBE_LAYER_LOG","/tmp/depth-probe-inspector-policy.log",1);
    setenv("DEPTH_PROBE_INSPECTOR_AUTO_CONSTANTS","0",1);
    water_settings_cases();
    water_layout_cases();
    water_recipe_cases();
    clone_diagnostics();
    memory_cases::run();
    asset_policy_cases();
    char diagnosticTemplate[]="/tmp/depth-probe-inspector-policy-XXXXXX";
    const auto diagnosticDir=mkdtemp(diagnosticTemplate);CHECK(diagnosticDir);setenv("DEPTH_PROBE_DISCOVERY_DIR",diagnosticDir,1);
    auto image=std::make_shared<Image>();image->handle=1;image->generation=10;image->extent={64,64,1};image->mips=image->layers=image->samples=1;
    auto view=std::make_shared<View>();view->image=image;view->range={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    auto shader=std::make_shared<Shader>();shader->hash=123;shader->replaySafe=true;
    auto pipeline=std::make_shared<Pipeline>();pipeline->scissor={{0,0},{64,64}};pipeline->serial=44;pipeline->colorMask=15;pipeline->vs=pipeline->fs=shader;
    auto queries=std::make_shared<PickQueries>();queries->ticket=7;queries->used=3;queries->values[0]={1,1};queries->values[1]={0,1};queries->values[2]={1,1};
    auto draw=[&](uint32_t query,uint64_t ordinal) {
        SubmittedDraw out;out.draw.pipeline=pipeline;out.draw.target.known=true;out.draw.target.depth=view;out.draw.target.width=out.draw.target.height=64;
        out.draw.command=12;out.draw.commandGeneration=99;out.draw.ordinal=ordinal;out.draw.pickTicket=7;out.draw.pick=queries;out.draw.query=query;out.draw.count=3;out.draw.instances=1;
        out.draw.dynamic.valid=Scissor|Blend;out.draw.dynamic.scissor={{0,0},{64,64}};out.submit=77;return out;
    };
    auto request=[&](std::vector<SubmittedDraw> draws) {Inspector in;in.geometryMode=false;in.depthImage=1;in.ticket=7;in.x=in.y=32;in.frame.number=4;in.frame.draws=std::move(draws);return in;};
    auto a=draw(0,1),miss=draw(1,2),front=draw(2,3);
    auto in=request({a,miss,front});CHECK(resolve_pick(in) && in.selected && in.selected->draw.ordinal==3);auto identity=in.drawIdentity;
    in=request({a,miss});CHECK(resolve_pick(in) && in.selected && in.selected->draw.ordinal==1);
    auto unknown=draw(UINT32_MAX,4);in=request({a,unknown});CHECK(resolve_pick(in) && !in.selected);
    in=request({unknown,front});CHECK(resolve_pick(in) && in.selected && in.selected->draw.ordinal==3);
    auto blended=front;blended.draw.pipeline=std::make_shared<Pipeline>(*pipeline);blended.draw.pipeline->blend=1;in=request({a,blended});CHECK(resolve_pick(in) && !in.selected);
    auto stale=front;stale.draw.pickTicket=6;in=request({a,stale});CHECK(resolve_pick(in) && !in.selected);
    auto transfer=front;transfer.draw.kind=7;in=request({a,transfer});CHECK(resolve_pick(in) && !in.selected);
    auto untracked=front;untracked.draw.target.known=false;untracked.draw.unsupported=true;in=request({a,untracked});CHECK(resolve_pick(in) && !in.selected);
    in=request({a});in.frame.missingCommands=1;CHECK(resolve_pick(in) && !in.selected);
    in=request({a});in.frame.truncated=true;CHECK(resolve_pick(in) && !in.selected);
    queries->values[2][1]=0;in=request({a,front});CHECK(!resolve_pick(in) && !in.selected);queries->values[2][1]=1;
    in=request({a,miss,front});in.frame.number=5;CHECK(resolve_pick(in) && in.drawIdentity!=identity);
    auto color=std::make_shared<View>();color->image=std::make_shared<Image>();color->image->handle=2;a.draw.target.colors={color};
    in=request({a});in.presentImage=2;CHECK(resolve_pick(in) && in.finalVisible);
    auto ui=front;ui.draw.target.depth.reset();ui.draw.target.colors={color};in=request({a,ui});in.presentImage=2;CHECK(resolve_pick(in) && in.selected && !in.finalVisible);
    ui.draw.pipeline=std::make_shared<Pipeline>(*pipeline);ui.draw.pipeline->dynamic.insert(VK_DYNAMIC_STATE_SCISSOR);ui.draw.dynamic.scissor={{0,0},{8,8}};
    in=request({a,ui});in.presentImage=2;CHECK(resolve_pick(in) && in.finalVisible);
    // WoW: an opaque depth-disabled scene composite shares the scene depth
    // attachment. Strict last-pass picking finds it; geometry inspection must
    // retain actual positive coverage from before that pass, with honest scope.
    auto geometry=a;geometry.draw.pipeline=std::make_shared<Pipeline>(*pipeline);
    geometry.draw.pipeline->depthTest=geometry.draw.pipeline->depthWrite=1;
    geometry.draw.pipeline->compare=VK_COMPARE_OP_LESS_OR_EQUAL;
    auto nearer=geometry;nearer.draw.ordinal=2;nearer.draw.query=2;
    auto screen=front;screen.draw.pipeline=std::make_shared<Pipeline>(*pipeline);
    screen.draw.pipeline->compare=VK_COMPARE_OP_ALWAYS;
    auto geometryRequest=[&](std::vector<SubmittedDraw> ds){auto value=request(std::move(ds));value.geometryMode=true;return value;};
    in=request({geometry,screen});CHECK(resolve_pick(in) && in.selected->draw.ordinal==3);
    in=geometryRequest({geometry,nearer,transfer,screen});in.presentImage=2;
    CHECK(resolve_pick(in) && in.selected->draw.ordinal==2 && in.geometryCandidates.size()==2);
    CHECK(in.geometryCandidate && !in.finalVisible && !in.selected->draw.pick);
    CHECK(std::string(inspector_selection_scope(in))=="depth-tested-geometry-candidate");
    auto candidateId=in.drawIdentity;in.frame={};in.texture=9;in.preview=true;
    CHECK(select_geometry_candidate(in,1) && in.selected->draw.ordinal==1 && !in.preview && in.texture==0);
    CHECK(select_geometry_candidate(in,0) && in.drawIdentity==candidateId);
    CHECK(!select_geometry_candidate(in,2));
    auto transparent=nearer;transparent.draw.pipeline=std::make_shared<Pipeline>(*nearer.draw.pipeline);transparent.draw.pipeline->blend=1;
    in=geometryRequest({geometry,transparent,screen});CHECK(resolve_pick(in) && in.selected->draw.ordinal==1);
    CHECK(select_geometry_candidate(in,0) && in.selected->draw.ordinal==2 && !in.highlight && !in.finalVisible);
    in=geometryRequest({screen});CHECK(resolve_pick(in) && !in.selected); // never fall back to the composite
    auto noDepthState=geometry;noDepthState.draw.pipeline=std::make_shared<Pipeline>(*geometry.draw.pipeline);
    noDepthState.draw.pipeline->dynamic.insert(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE);
    in=geometryRequest({noDepthState});CHECK(resolve_pick(in) && !in.selected);
    in=geometryRequest({stale});CHECK(resolve_pick(in) && !in.selected);
    auto geometryMiss=nearer;geometryMiss.draw.query=1;
    in=geometryRequest({geometryMiss});CHECK(resolve_pick(in) && !in.selected);
    queries->values[2][1]=0;in=geometryRequest({geometry,nearer});CHECK(!resolve_pick(in) && !in.selected);queries->values[2][1]=1;
    in=geometryRequest({geometry});in.frame.truncated=true;CHECK(resolve_pick(in) && !in.selected);
    in=geometryRequest({geometry});in.frame.missingCommands=1;CHECK(resolve_pick(in) && !in.selected);
    std::vector<SubmittedDraw> many;for(unsigned i=0;i<70;++i){auto hit=geometry;hit.draw.ordinal=i+1;many.push_back(hit);}
    in=geometryRequest(many);CHECK(resolve_pick(in) && in.candidateLimit && in.geometryCandidates.size()==64);
    CHECK(in.selected->draw.ordinal==70 && in.geometryCandidates.back().draw.ordinal==7);
    auto material=material_identity(front);CHECK(material!=0);front.descriptorsIncomplete=true;CHECK(material_identity(front)==0);
    auto checkShader=[](const char* name) {auto words=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/"+name+".spv");CHECK(!words.empty());return inspect_spirv(words.data(),words.size()*4);};
    CHECK(checkShader("inspector_discard.frag").replaySafe);
    const auto early=checkShader("inspector_early.frag");CHECK(early.earlyTests && early.replaySafe);
    const auto earlyDiscard=checkShader("inspector_early_discard.frag");CHECK(earlyDiscard.earlyTests && !earlyDiscard.replaySafe);
    int dispatchKey=0;struct FakeDispatch {void* key;};FakeDispatch dispatch{&dispatchKey};auto fakeDevice=reinterpret_cast<VkDevice>(&dispatch);DepthOverlayState context;context.depthImage=(VkImage)uint64_t(1);context.depthGeneration=1;context.width=context.height=64;
    DeviceData fakeData;fakeData.device=fakeDevice;fakeData.next_gdpa=[](VkDevice,const char*) -> PFN_vkVoidFunction{return nullptr;};g_devices[dispatch_key(fakeDevice)]=fakeData;
    CHECK(vkGetDeviceProcAddr(fakeDevice,"vkCmdDrawMeshTasksEXT")==nullptr);
    inspector_context(fakeDevice,context);inspector_request_pick(fakeDevice,30,40);CHECK(devices.at(fakeDevice).inspector->armed);
    OverlayUniform marker{};populate_inspector_menu(fakeDevice,marker);CHECK(marker.inspector[2]==1);
    auto& state=devices.at(fakeDevice);auto& selection=*state.inspector;
    VkQueue queue=reinterpret_cast<VkQueue>(uintptr_t(2));VkPresentInfoKHR present{};
    present.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;present.swapchainCount=1;present.pSwapchains=&context.swapchain;
    auto old=a;old.draw.pickTicket=0;state.pending[id(queue)].draws={old};
    inspector_present_begin(fakeDevice,queue,&present);
    CHECK(selection.armed && !selection.staged && !selection.captureDepth);
    auto fresh=a;fresh.draw.pickTicket=selection.ticket;state.pending[id(queue)].draws={old,fresh};
    inspector_present_begin(fakeDevice,queue,&present);CHECK(selection.armed && !selection.staged);
    state.pending[id(queue)].draws={fresh};inspector_present_begin(fakeDevice,queue,&present);
    CHECK(!selection.armed && selection.staged && selection.captureDepth);
    // Reusable/untracked commands cannot keep a request pending forever.
    inspector_request_pick(fakeDevice,30,40);state.pending[id(queue)].draws={old};
    for(unsigned i=0;i<8;++i){inspector_present_begin(fakeDevice,queue,&present);CHECK(selection.armed);}
    inspector_present_begin(fakeDevice,queue,&present);CHECK(!selection.armed && selection.staged);
    // The presenting queue is recorded on every present, not only while a pick is armed: the water replay's
    // "sampled image layout observed on the same queue" proof depends on it right after launch, before any pick.
    VkQueue presentQueue=reinterpret_cast<VkQueue>(uintptr_t(3));
    inspector_present_begin(fakeDevice,presentQueue,&present);CHECK(!selection.armed && selection.queue==id(presentQueue));
    inspector_present_begin(fakeDevice,queue,&present);
    inspector_poll(state,selection);CHECK(!selection.selected && state.inspectorFailureCaptured);flush_discovery_exports();
    CHECK(state.captureSerial==1);
    selection.staged=true;selection.frame.draws={old};inspector_poll(state,selection);CHECK(state.captureSerial==1);
    unsigned files=0;for(const auto& entry:std::filesystem::directory_iterator(diagnosticDir))if(entry.path().extension()==".json") {
        ++files;std::ifstream stream(entry.path());std::string json((std::istreambuf_iterator<char>(stream)),{});
        CHECK(json.find("\"diagnosticRevision\":2")!=std::string::npos && json.find("\"pickTicket\":2")!=std::string::npos);
        CHECK(json.find("\"queryStatus\":")!=std::string::npos && json.find("\"unsupportedReason\":")!=std::string::npos);
    }CHECK(files==1);
    inspector_request_pick(fakeDevice,30,40);
    context.depthGeneration=2;inspector_context(fakeDevice,context);CHECK(!devices.at(fakeDevice).inspector->armed && !devices.at(fakeDevice).inspector->selected);
    populate_inspector_menu(fakeDevice,marker);CHECK(marker.inspector[2]==0);
    inspector_request_pick(fakeDevice,30,40);inspector_cancel(fakeDevice);CHECK(!devices.at(fakeDevice).inspector->armed);
    destroy_device_discovery(fakeDevice);CHECK(!devices.count(fakeDevice));g_devices.clear();
    std::puts("PASS: inspector policy - query availability, strict unknown, opacity, transfer/clear invalidation, stale tickets, bounded missing data, frame identity, shader early/discard safety");
}
