#include "volumetric_fog.hpp"
#include "game_soft_shadows.hpp"
#include "game_lights_capture.hpp"
#include "game_fog_capture.hpp"
#include "scene_debugger.hpp"
#include "camera_hud_input.hpp"
#include "water_reflections.hpp"
#include "depth_tracking.hpp"
#include "overlay_settings.hpp"
#include "shader_resources.hpp"
#include "layer_state.hpp"
#include <algorithm>
#include <tuple>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <unistd.h>

namespace depth_probe::discovery {
bool scene_debugger_frame_reconstruction(Device&, ReconstructionSetup&, std::string& reason);

namespace {
template<class T> T raw(const DeviceData& d, const char* n) { return reinterpret_cast<T>(d.next_gdpa(d.device, n)); }
template<class T> T vk(VkStructureType type) { T v{}; v.sType = type; return v; }
constexpr uint32_t computeSlots = 3, compositeSlots = 8, retireAfterPresents = 12, maxFramebuffers = 6;
constexpr uint32_t sunFindWords = 16;   // fog_sun_find.comp counters per compute slot: white set, sky count, warm set
constexpr uint32_t groundBins = 256;    // fog_ground.comp: floor cells by height relative to the camera, 1-yard bins from -128
constexpr uint32_t exposureBins = 64;   // fog_exposure.comp: the fogged picture by log2 luminance, from -8 in 1/8 steps
constexpr uint32_t waterBins = 256;     // fog_memory_scatter.comp: water surface pixels by height relative to the camera, as groundBins
constexpr uint32_t waterWords = sunFindWords + groundBins + exposureBins;   // where they start in a slot
constexpr uint32_t readbackWords = waterWords + waterBins;   // per compute slot (fog_common.glsl fogReadbackWords)
constexpr uint32_t computeStamps = 7;   // per compute slot: start + after clear, scatter, sun find, inject, light, integrate

// Everything the fog calls, resolved once from the next layer (never through our own hooks).
struct FogFns {
#define FOG_FN(name) PFN_vk##name name = nullptr;
    FOG_FN(CreateImage) FOG_FN(DestroyImage) FOG_FN(GetImageMemoryRequirements) FOG_FN(AllocateMemory) FOG_FN(FreeMemory)
    FOG_FN(BindImageMemory) FOG_FN(CreateImageView) FOG_FN(DestroyImageView) FOG_FN(CreateSampler) FOG_FN(DestroySampler)
    FOG_FN(CreateBuffer) FOG_FN(DestroyBuffer) FOG_FN(GetBufferMemoryRequirements) FOG_FN(BindBufferMemory)
    FOG_FN(CreateDescriptorSetLayout) FOG_FN(DestroyDescriptorSetLayout) FOG_FN(CreatePipelineLayout) FOG_FN(DestroyPipelineLayout)
    FOG_FN(CreateDescriptorPool) FOG_FN(DestroyDescriptorPool) FOG_FN(AllocateDescriptorSets) FOG_FN(UpdateDescriptorSets)
    FOG_FN(CreateShaderModule) FOG_FN(DestroyShaderModule) FOG_FN(CreateComputePipelines) FOG_FN(CreateGraphicsPipelines) FOG_FN(DestroyPipeline)
    FOG_FN(CreateRenderPass) FOG_FN(DestroyRenderPass) FOG_FN(CreateFramebuffer) FOG_FN(DestroyFramebuffer)
    FOG_FN(CreateCommandPool) FOG_FN(DestroyCommandPool) FOG_FN(AllocateCommandBuffers) FOG_FN(BeginCommandBuffer) FOG_FN(EndCommandBuffer)
    FOG_FN(ResetCommandBuffer) FOG_FN(CreateFence) FOG_FN(DestroyFence) FOG_FN(GetFenceStatus) FOG_FN(ResetFences)
    FOG_FN(CreateQueryPool) FOG_FN(DestroyQueryPool) FOG_FN(GetQueryPoolResults) FOG_FN(QueueSubmit)
    FOG_FN(CmdBindPipeline) FOG_FN(CmdBindDescriptorSets) FOG_FN(CmdDispatch) FOG_FN(CmdPipelineBarrier) FOG_FN(CmdUpdateBuffer)
    FOG_FN(CmdBeginRenderPass) FOG_FN(CmdEndRenderPass) FOG_FN(CmdDraw) FOG_FN(CmdSetViewport) FOG_FN(CmdSetScissor)
    FOG_FN(CmdResetQueryPool) FOG_FN(CmdWriteTimestamp) FOG_FN(CmdClearColorImage)
    FOG_FN(CmdCopyImageToBuffer) FOG_FN(CreateEvent) FOG_FN(DestroyEvent) FOG_FN(CmdSetEvent) FOG_FN(GetEventStatus) FOG_FN(MapMemory) FOG_FN(UnmapMemory)
    FOG_FN(CmdBlitImage) FOG_FN(CmdFillBuffer)
#undef FOG_FN
    bool complete = false;
};
FogFns load_fns(const DeviceData& d) {
    FogFns f;
    bool ok = true;
#define FOG_LOAD(name) f.name = raw<PFN_vk##name>(d, "vk" #name); ok &= f.name != nullptr;
    FOG_LOAD(CreateImage) FOG_LOAD(DestroyImage) FOG_LOAD(GetImageMemoryRequirements) FOG_LOAD(AllocateMemory) FOG_LOAD(FreeMemory)
    FOG_LOAD(BindImageMemory) FOG_LOAD(CreateImageView) FOG_LOAD(DestroyImageView) FOG_LOAD(CreateSampler) FOG_LOAD(DestroySampler)
    FOG_LOAD(CreateBuffer) FOG_LOAD(DestroyBuffer) FOG_LOAD(GetBufferMemoryRequirements) FOG_LOAD(BindBufferMemory)
    FOG_LOAD(CreateDescriptorSetLayout) FOG_LOAD(DestroyDescriptorSetLayout) FOG_LOAD(CreatePipelineLayout) FOG_LOAD(DestroyPipelineLayout)
    FOG_LOAD(CreateDescriptorPool) FOG_LOAD(DestroyDescriptorPool) FOG_LOAD(AllocateDescriptorSets) FOG_LOAD(UpdateDescriptorSets)
    FOG_LOAD(CreateShaderModule) FOG_LOAD(DestroyShaderModule) FOG_LOAD(CreateComputePipelines) FOG_LOAD(CreateGraphicsPipelines) FOG_LOAD(DestroyPipeline)
    FOG_LOAD(CreateRenderPass) FOG_LOAD(DestroyRenderPass) FOG_LOAD(CreateFramebuffer) FOG_LOAD(DestroyFramebuffer)
    FOG_LOAD(CreateCommandPool) FOG_LOAD(DestroyCommandPool) FOG_LOAD(AllocateCommandBuffers) FOG_LOAD(BeginCommandBuffer) FOG_LOAD(EndCommandBuffer)
    FOG_LOAD(ResetCommandBuffer) FOG_LOAD(CreateFence) FOG_LOAD(DestroyFence) FOG_LOAD(GetFenceStatus) FOG_LOAD(ResetFences)
    FOG_LOAD(CreateQueryPool) FOG_LOAD(DestroyQueryPool) FOG_LOAD(GetQueryPoolResults) FOG_LOAD(QueueSubmit)
    FOG_LOAD(CmdBindPipeline) FOG_LOAD(CmdBindDescriptorSets) FOG_LOAD(CmdDispatch) FOG_LOAD(CmdPipelineBarrier) FOG_LOAD(CmdUpdateBuffer)
    FOG_LOAD(CmdBeginRenderPass) FOG_LOAD(CmdEndRenderPass) FOG_LOAD(CmdDraw) FOG_LOAD(CmdSetViewport) FOG_LOAD(CmdSetScissor)
    FOG_LOAD(CmdResetQueryPool) FOG_LOAD(CmdWriteTimestamp) FOG_LOAD(CmdClearColorImage)
    FOG_LOAD(CmdCopyImageToBuffer) FOG_LOAD(CreateEvent) FOG_LOAD(DestroyEvent) FOG_LOAD(CmdSetEvent) FOG_LOAD(GetEventStatus) FOG_LOAD(MapMemory) FOG_LOAD(UnmapMemory)
    FOG_LOAD(CmdBlitImage) FOG_LOAD(CmdFillBuffer)
#undef FOG_LOAD
    f.complete = ok;
    return f;
}

struct Volume { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; };
struct FogFramebuffer { uint64_t image = 0, generation = 0; VkImageView view{}; VkFramebuffer framebuffer{}; };
struct DepthBinding { uint64_t image = 0, generation = 0; VkImageView waterMask{}; uint32_t parity = 0; VkImageView view{}; VkDescriptorSet set{}; };
struct ComputeSlot {
    VkCommandBuffer cmd{}; VkFence fence{}; bool submitted = false, timestamps = false;
    // The sun finder's counters for this submission (a slice of FogResources::sunFound) and the camera of the picture it
    // searched (the previous frame's): read when the slot's fence is next seen signalled.
    bool sunPending = false;
    uint32_t sunEpoch = 0;     // FogState::discEpoch when dispatched (read back 3 frames later - maybe after a teleport)
    float sunCamera[4][4]{};   // right (w = P00), up (w = P11), forward, viewport
    float sunArc[4]{};         // FogParams::sunArc of that search
    // The ground and water histograms of this submission (fog_ground.comp, fog_memory_scatter.comp) and the camera they are
    // relative to.
    bool groundPending = false;
    uint32_t groundEpoch = 0;
    double groundCamera[3]{};
    bool exposurePending = false;   // the exposure histogram of this submission (fog_exposure.comp)
};
struct CompositeSlot { bool pending = false, submitted = false; };
}

struct FogResources {
    DeviceData data;
    FogFns f;
    fog::Grid grid;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    Volume medium, lighting[2], integrated;   // lighting ping-pongs: written this frame / read as history
    bool volumesInitialized = false;
    VkImage noWater{}; VkDeviceMemory noWaterMemory{}; VkImageView noWaterView{}; // 1x1 zero mask when no water surface is known
    // Sun shadows: the world's nearest view depth reduced by shadowFactor (R32F), filled by the composite, read by the next
    // frame's light pass. Kept in SHADER_READ_ONLY_OPTIMAL between uses.
    static constexpr uint32_t shadowFactor = 1;   // full resolution: a 4x4 nearest-depth reduction shadowed grazing ground by its own nearer rows
    uint32_t width = 0, height = 0, shadowWidth = 0, shadowHeight = 0;
    VkImage shadow{}; VkDeviceMemory shadowMemory{}; VkImageView shadowView{};
    VkRenderPass shadowPass{}; VkFramebuffer shadowFramebuffer{}; VkPipeline shadowPipeline{};
    // Height memory: memorySize^2 cells of memoryCell yards (512 yd across), R32_UINT heights + the world cell each texel holds.
    static constexpr int32_t memorySize = 1024;
    static constexpr float memoryCell = .5f;
    Volume heightMemory, tagMemory;   // 2D storage images, GENERAL
    Volume sunMemory, sunTagMemory;   // the sun map, same layout
    Volume floorMemory;               // lowest surface seen per height-memory cell (the ground; fog_ground.comp)
    // The game's sun cascades reduced to cascadeSize^2 (R32F, 4 layers; fog_cascade_reduce.frag at the composite, read by the
    // next frame's light pass), and per cascade image of the game a depth view + descriptor set (written once).
    static constexpr uint32_t cascadeSize = 512;
    VkImage cascadeMaps{}; VkDeviceMemory cascadeMemory{}; VkImageView cascadeArrayView{};
    VkImageView cascadeLayerViews[4]{}; VkFramebuffer cascadeFramebuffers[4]{};
    VkRenderPass cascadePass{}; VkPipeline cascadePipeline{};
    bool cascadeMapsInitialized = false;
    struct CascadeSource { uint64_t image = 0, generation = 0; VkImageLayout layout{}; VkImageView view{}; VkDescriptorSet set{}; };
    std::vector<CascadeSource> cascadeSources;
    // Sun rays (sun_rays): fog_rays_mask.frag's mask (R8) and fog_rays.frag's rays (R16F) at a quarter of the resolution,
    // rendered right before the composite that samples them (composite layout bindings 9 and 10); SHADER_READ_ONLY between uses.
    static constexpr uint32_t raysFactor = 4;   // fog_common.glsl fogRaysFactor
    struct RaysTarget { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkFramebuffer framebuffer{}; };
    uint32_t raysWidth = 0, raysHeight = 0;
    RaysTarget raysMask, rays;
    VkRenderPass raysMaskPass{}, raysPass{}; VkPipeline raysMaskPipeline{}, raysPipeline{};
    // Scene shading (contact shadows, ambient occlusion, terrain shadows): fog_shade.frag's RGBA16F at half the resolution,
    // rendered right before the composite (composite layout binding 12); SHADER_READ_ONLY between uses.
    static constexpr uint32_t shadeFactor = 2;   // fog_common.glsl fogShadeFactor
    uint32_t shadeWidth = 0, shadeHeight = 0;
    RaysTarget shade;
    VkRenderPass shadePass{}; VkPipeline shadePipeline{};
    Volume visibilityVolume;          // RGBA8 3D, written by the light pass on fog capture frames only (r = sun, b = sky)
    Volume noiseLattice;              // RGBA16F 64^3 smoke lattice, baked once by fog_noise_bake.comp, sampled (REPEAT) by inject
    bool noiseBaked = false;
    // Sun finder: the game's picture at a quarter of the resolution (blitted at the composite, before the fog) and one
    // 8-word counter slice per compute slot in a host-visible buffer.
    uint32_t pictureWidth = 0, pictureHeight = 0;
    Volume picture;                   // 2D RGBA8 storage image, GENERAL between uses
    bool pictureInitialized = false, pictureBlitChecked = false, pictureBlitOk = false;
    VkBuffer sunFound{}; VkDeviceMemory sunFoundMemory{}; uint32_t* sunFoundMapped = nullptr;
    VkPipeline sunFind{};
    VkPipeline memoryClear{}, memoryScatter{}, noiseBake{}, ground{}, exposure{};
    VkBuffer params{}; VkDeviceMemory paramsMemory{};
    VkSampler linear{}, nearest{}, repeat{};   // repeat: linear, wrapping (the noise lattice)
    VkDescriptorSetLayout computeSetLayout{}, compositeSetLayout{};
    VkPipelineLayout computeLayout{}, compositeLayout{};
    VkPipeline inject{}, light{}, integrate{}, composite{};
    VkRenderPass pass{};
    VkDescriptorPool pool{};
    VkDescriptorSet computeSet[2]{};             // per lighting parity
    std::vector<FogFramebuffer> framebuffers;
    std::vector<DepthBinding> depths;
    VkCommandPool commandPool{};
    ComputeSlot compute[computeSlots];
    uint32_t nextCompute = 0;
    VkQueryPool queries{};           // computeSlots * computeStamps compute stamps, then compositeSlots * 2 composite stamps
    uint32_t timestampBits = 0; float timestampPeriod = 0;
    CompositeSlot composites[compositeSlots];
    uint32_t nextComposite = 0;
    ~FogResources() { release(); }
    void release() {
        if (!data.device || !f.complete) return;
        const auto d = data.device;
        for (auto& s : compute) if (s.fence) f.DestroyFence(d, s.fence, nullptr);
        if (commandPool) f.DestroyCommandPool(d, commandPool, nullptr);
        if (queries) f.DestroyQueryPool(d, queries, nullptr);
        for (auto& fb : framebuffers) { if (fb.framebuffer) f.DestroyFramebuffer(d, fb.framebuffer, nullptr); if (fb.view) f.DestroyImageView(d, fb.view, nullptr); }
        for (auto& b : depths) if (b.view) f.DestroyImageView(d, b.view, nullptr);
        for (auto& c : cascadeSources) if (c.view) f.DestroyImageView(d, c.view, nullptr);
        for (auto fb : cascadeFramebuffers) if (fb) f.DestroyFramebuffer(d, fb, nullptr);
        for (auto v : cascadeLayerViews) if (v) f.DestroyImageView(d, v, nullptr);
        if (cascadeArrayView) f.DestroyImageView(d, cascadeArrayView, nullptr);
        if (cascadeMaps) f.DestroyImage(d, cascadeMaps, nullptr);
        if (cascadeMemory) f.FreeMemory(d, cascadeMemory, nullptr);
        if (cascadePipeline) f.DestroyPipeline(d, cascadePipeline, nullptr);
        if (cascadePass) f.DestroyRenderPass(d, cascadePass, nullptr);
        for (auto* t : {&raysMask, &rays, &shade}) {
            if (t->framebuffer) f.DestroyFramebuffer(d, t->framebuffer, nullptr);
            if (t->view) f.DestroyImageView(d, t->view, nullptr);
            if (t->image) f.DestroyImage(d, t->image, nullptr);
            if (t->memory) f.FreeMemory(d, t->memory, nullptr);
        }
        for (auto p : {raysMaskPipeline, raysPipeline, shadePipeline}) if (p) f.DestroyPipeline(d, p, nullptr);
        for (auto p : {raysMaskPass, raysPass, shadePass}) if (p) f.DestroyRenderPass(d, p, nullptr);
        for (auto p : {inject, light, integrate, composite, shadowPipeline, memoryClear, memoryScatter, sunFind, noiseBake, ground, exposure}) if (p) f.DestroyPipeline(d, p, nullptr);
        if (sunFound) f.DestroyBuffer(d, sunFound, nullptr);
        if (sunFoundMemory) f.FreeMemory(d, sunFoundMemory, nullptr);
        if (shadowFramebuffer) f.DestroyFramebuffer(d, shadowFramebuffer, nullptr);
        for (auto p : {pass, shadowPass}) if (p) f.DestroyRenderPass(d, p, nullptr);
        if (shadowView) f.DestroyImageView(d, shadowView, nullptr);
        if (shadow) f.DestroyImage(d, shadow, nullptr);
        if (shadowMemory) f.FreeMemory(d, shadowMemory, nullptr);
        if (pool) f.DestroyDescriptorPool(d, pool, nullptr);
        for (auto l : {computeLayout, compositeLayout}) if (l) f.DestroyPipelineLayout(d, l, nullptr);
        for (auto l : {computeSetLayout, compositeSetLayout}) if (l) f.DestroyDescriptorSetLayout(d, l, nullptr);
        for (auto s : {linear, nearest, repeat}) if (s) f.DestroySampler(d, s, nullptr);
        if (noWaterView) f.DestroyImageView(d, noWaterView, nullptr);
        if (noWater) f.DestroyImage(d, noWater, nullptr);
        if (noWaterMemory) f.FreeMemory(d, noWaterMemory, nullptr);
        if (params) f.DestroyBuffer(d, params, nullptr);
        if (paramsMemory) f.FreeMemory(d, paramsMemory, nullptr);
        for (auto* v : {&medium, &lighting[0], &lighting[1], &integrated, &heightMemory, &tagMemory, &sunMemory, &sunTagMemory, &floorMemory, &visibilityVolume, &picture, &noiseLattice}) {
            if (v->view) f.DestroyImageView(d, v->view, nullptr);
            if (v->image) f.DestroyImage(d, v->image, nullptr);
            if (v->memory) f.FreeMemory(d, v->memory, nullptr);
        }
        data.device = VkDevice{};
    }
};

// One frame's fog capture: images copied into a host-visible buffer inside the application's command buffer; the host polls
// an event the GPU sets after the copies (never waits), then writes one .bin per section.
struct FogCapture {
    FogFns f; VkDevice device{};
    std::shared_ptr<FogResources> keep;   // our images stay alive until the copies ran
    VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped = nullptr; VkEvent event{};
    struct Section { std::string name, format; uint32_t width = 0, height = 0, depth = 1; size_t offset = 0, size = 0; };
    std::vector<Section> sections;
    std::string dir, label;
    uint32_t polls = 0;
    ~FogCapture() { release(); }
    void release() {
        if (!device) return;
        if (event) f.DestroyEvent(device, event, nullptr);
        if (mapped) f.UnmapMemory(device, memory);
        if (buffer) f.DestroyBuffer(device, buffer, nullptr);
        if (memory) f.FreeMemory(device, memory, nullptr);
        device = VkDevice{};
    }
};

namespace {
std::filesystem::path fog_capture_root() {
    if (const char* e = std::getenv("DEPTH_PROBE_EXPORT_DIR"); e && *e) return e;
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) : std::filesystem::path("/tmp");
}
// Bytes per texel of the formats the capture copies (0: not supported).
size_t fog_texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 0;
    }
}
const char* preset_name(fog::Preset p) { return p == fog::Preset::Low ? "LOW" : p == fog::Preset::High ? "HIGH" : "MEDIUM"; }

// ---- settings (fog.ini, re-read when it changes) ---------------------------------------------------
std::string settings_path() {
    if (const char* e = std::getenv("DEPTH_PROBE_FOG_SETTINGS"); e && *e) return e;
    return (std::filesystem::path(settings_config_path()).parent_path() / "fog.ini").string();
}
void write_settings(const std::string& path, const FogSettings& s) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream o(path);
    if (!o) return;
    o << "# Volumetric fog (Shift+F5 cycles FINAL and the debug views). Units: yards, 1/yard, world z.\n"
         "# Re-read automatically about once a second while the game runs.\n"
      << "preset = " << (s.preset == fog::Preset::Low ? "low" : s.preset == fog::Preset::High ? "high" : "medium") << "            # low (16 px, 48 slices) | medium (12 px, 64) | high (8 px, 96)\n"
      << "max_distance = " << s.maxDistance << "         # fog volume end, view depth\n"
      << "distribution = " << s.distribution << "            # slices ~linear within this distance, exponential beyond\n"
      << "base_density = " << s.medium.baseDensity << "        # extinction at base_height\n"
      << "height_falloff = " << s.medium.heightFalloff << "       # density halves every ln2/falloff yards up\n"
      << "height_reference = " << fog::height_reference_name(s.heightReference) << "  # ground: base_height is an offset from the area's ground level (the fog stays on the land); camera: from the followed camera height; world: absolute z\n"
      << "base_height = " << s.medium.baseHeight << "\n"
      << "max_extinction = " << s.medium.maxExtinction << "\n"
      << "albedo = " << s.medium.albedo[0] << "," << s.medium.albedo[1] << "," << s.medium.albedo[2] << "\n"
      << "emissive = 0,0,0\n"
      << "anisotropy = " << s.anisotropy << "          # Henyey-Greenstein g\n"
      << "ambient_source = " << (s.ambientFromGame ? "game" : "manual") << "     # game: WoW's ambient light colour (zone, time of day) x ambient_scale; manual: ambient\n"
      << "ambient_scale = " << s.ambientScale << "\n"
      << "ambient = " << s.ambient[0] << "," << s.ambient[1] << "," << s.ambient[2] << "   # manual, and the fallback\n"
      << "sun_enabled = " << (s.sunEnabled ? 1 : 0) << "           # directional light in the fog (phase function)\n"
      << "sun_source = " << (s.sunFromGame ? "game" : "manual") << "         # game: direction from WoW's memory; manual: sun_direction\n"
      << "sun_position = " << (s.sunAtSkyDisc ? "sky" : "light") << "        # game source: sky = the sun/moon disc drawn in the sky, light = the terrain lighting direction\n"
      << "sun_direction = " << s.sunDirection[0] << "," << s.sunDirection[1] << "," << s.sunDirection[2] << "   # toward the light, world axes (manual, and the fallback)\n"
      << "sun_color = " << s.sunColor[0] << "," << s.sunColor[1] << "," << s.sunColor[2] << "   # manual colour; with sun_color_source = game the first value is the intensity\n"
      << "sun_color_source = " << (s.sunColorFromGame ? "game" : "manual") << " # game: WoW's glow colour of the sun (matches the disc) x intensity\n"
      << "temporal_weight = " << s.temporalWeight << "     # 0 = off; history share per frame (jittered froxel sampling, reprojected)\n"
      << "temporal_moving = " << s.temporalMoving << "     # history share while the camera turns 1 deg / steps .5 yd a frame: shafts of the old view fade faster\n"
      << "noise_amount = " << s.noiseAmount << "        # world-anchored density variation, 0 = off\n"
      << "noise_scale = " << s.noiseScale << "         # 12, 24 or 48 yards\n"
      << "noise_wind = " << s.noiseWind << "          # yards per second drift\n"
      << "shadows = " << (s.shadows ? 1 : 0) << "               # sun shadows in the fog (screen space: only what the previous frame saw casts them)\n"
      << "shadow_length = " << s.shadowLength << "        # yards marched toward the sun per froxel\n"
      << "sky_light = " << s.skyLight << "            # share of the ambient that comes from above and is shadowed (canopies, roofs)\n"
      << "light_gizmo = " << (s.lightGizmo ? 1 : 0) << "          # draw where the light sources are (line from the screen centre)\n"
      << "light_sources = " << (s.lightSources ? 1 : 0) << "        # the game's point lights (torches, fires): a circle in their colour where each is\n"
      << "game_fog_distance = " << s.gameFogDistance << "    # WoW's own flat distance fog: 1 = as the game, 2 = twice as far, 0 = none (ours does the distance)\n"
      << "shadow_softness = " << s.shadowSoftness << "      # WoW's own sun shadow edges: 1 = as the game, 2 = twice as soft (its filter spread)\n"
      << "soft_shadows = " << s.softShadows << "         # the game's own sun shadows blurrier the farther from what casts them (0 = the game's; 1, 2, 3, 4 wider; live once the game started with it on)\n"
      << "shadow_fade = " << s.shadowFade << "          # how much of a shadow is gone shadow_fade_distance yd from its caster (0 = none; .5 half; 1 all; live)\n"
      << "indoor_shadows = " << s.indoorShadows << "       # the game's shadows indoors (its interior shadow map): 1 = as the game, 0 = none (live once soft_shadows was on at the game start)\n"
      << "shadow_fade_distance = " << s.shadowFadeDistance << "  # yards from the caster where shadow_fade is reached (live)\n"
      << "contact_shadows = " << s.contactShadows << "      # small sun shadows the game misses (grass, stones, feet), where its own are not (0 = off; ~.5)\n"
      << "ambient_occlusion = " << s.ambientOcclusion << "    # darker corners, wall feet, under trees (0 = off; ~.8)\n"
      << "terrain_shadows = " << s.terrainShadows << "      # hills' and cliffs' sun shadows, which the game draws into no cascade (0 = off; ~.4)\n"
      << "point_shadows = " << s.pointShadows << "        # shadows from the game's point lights (torches, fires) - full at night, 30% at noon (0 = off; ~.6)\n"
      << "point_shadow_range = " << s.pointShadowRange << "   # yards around a point light its shadows reach (fading out)\n"
      << "point_shadow_ignore = " << s.pointShadowIgnore << "  # yards around a point light where nothing casts its shadow (its brazier / lantern)\n"
      << "point_shadow_length = " << s.pointShadowLength << "  # yards behind what casts it a point-light shadow reaches (fading out)\n"
      << "terrain_shadows_from = " << s.terrainShadowsFrom << "  # yards from the camera where terrain shadows begin (faded in; near the character the game's own shadows are there; 0 = everywhere)\n"
      << "open_sun = " << s.openSun << "           # sun strength where the sky is open, relative to under cover (1 = same)\n"
      << "weather_sun = " << s.weatherSun << "        # how far rain / snow / sandstorm (read from WoW) hides the sun: 1 = fully from medium rain, 0 = ignore the weather\n"
      << "transparent_fog = " << (s.transparentFog ? 1 : 0) << "    # 1 = particles / glows (blended, no depth write) are drawn after the fog instead of taking the fog of the surface behind them\n"
      << "sky_fog = " << s.skyFog << "            # share of the fog over the sky (1 = all, 0 = the game's sky and sun glow untouched)\n"
      << "max_opacity = " << s.maxOpacity << "        # the most the fog hides of what is behind it (1 = no cap; .6 keeps distant scenery through a dense shaft fog)\n"
      << "light_limit = " << s.lightLimit << "        # ceiling of the light the fog adds, soft above half of it (0 = none; .8 stops looking into the sun from going white)\n"
      << "auto_exposure = " << (s.autoExposure ? 1 : 0) << "      # 1 = darken the fogged world when it gets brighter than exposure_target (eye adaptation; never brightens, never the UI)\n"
      << "exposure_target = " << s.exposureTarget << "   # luminance of the bright part the picture is brought down to\n"
      << "exposure_min = " << s.exposureMin << "       # the most auto exposure darkens (.6 = to 60%)\n"
      << "game_shadows = " << (s.gameShadows ? 1 : 0) << "       # 1 = the game's own sun shadow cascades for the fog's sun: world-fixed shafts, the ground's shadows\n"
      << "game_shadow_flip = " << s.gameShadowFlip << "   # the cascade maps' v axis: -1 = v = .5 + .5y (WoW's, found by running its shaders), 1 = D3D's .5 - .5y\n"
      << "sun_aureole = " << s.sunAureole << "        # share of a narrow forward lobe: the glow around the sun that outlines what it is half behind (0 = none; .02-.04)\n"
      << "sun_aureole_g = " << s.sunAureoleG << "      # that lobe's anisotropy (.9 = tight around the sun)\n"
      << "sun_glare = " << s.sunGlare << "          # glare around the visible part of the drawn disc, also over what the sun is half behind (0 = none; ~.7)\n"
      << "sun_glare_radius = " << s.sunGlareRadius << "   # the drawn disc's radius in degrees (its visibility is sampled over it)\n"
      << "sun_rays = " << s.sunRays << "           # beams from the disc through the gaps of what stands in front of it (0 = none; ~1.5)\n"
      << "sun_rays_length = " << s.sunRaysLength << "   # share of the way to the disc each pixel looks (longer = longer beams)\n";
}
bool parse_triple(const std::string& v, float out[3]) {
    float a, b, c;
    if (std::sscanf(v.c_str(), "%f , %f , %f", &a, &b, &c) != 3) return false;
    out[0] = a; out[1] = b; out[2] = c;
    return true;
}
void load_settings(FogState& st) {
    namespace fs = std::filesystem;
    if (st.settingsPath.empty()) {
        st.settingsPath = settings_path();
        std::error_code ec;
        if (!fs::exists(st.settingsPath, ec)) write_settings(st.settingsPath, st.settings);
    }
    std::error_code ec;
    const auto time = fs::last_write_time(st.settingsPath, ec);
    if (ec) { st.settingsNote = "defaults (no " + st.settingsPath + ")"; return; }
    if (time == st.settingsTime) return;
    st.settingsTime = time;
    FogSettings s;
    std::ifstream in(st.settingsPath);
    std::string line;
    std::vector<std::string> bad;
    bool sawHeightReference = false;
    while (std::getline(in, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string x) { x.erase(0, x.find_first_not_of(" \t")); x.erase(x.find_last_not_of(" \t\r") + 1); return x; };
        const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        char* end = nullptr;
        const float number = std::strtof(value.c_str(), &end);
        const bool isNumber = end != value.c_str();
        bool ok = true;
        if (key == "height_reference") {
            s.heightReference = value == "ground" ? fog::HeightReference::Ground : value == "camera" ? fog::HeightReference::Camera : fog::HeightReference::World;
            sawHeightReference = true;
        }
        else if (key == "preset") s.preset = value == "low" ? fog::Preset::Low : value == "high" ? fog::Preset::High : fog::Preset::Medium;
        else if (key == "max_distance" && isNumber) s.maxDistance = std::clamp(number, 20.f, 2000.f);
        else if (key == "distribution" && isNumber) s.distribution = std::clamp(number, .5f, 200.f);
        else if (key == "base_density" && isNumber) s.medium.baseDensity = std::clamp(number, 0.f, 1.f);
        else if (key == "height_falloff" && isNumber) s.medium.heightFalloff = std::clamp(number, 0.f, 2.f);
        else if (key == "base_height" && isNumber) s.medium.baseHeight = number;
        else if (key == "max_extinction" && isNumber) s.medium.maxExtinction = std::clamp(number, 0.f, 10.f);
        else if (key == "anisotropy" && isNumber) s.anisotropy = std::clamp(number, -.95f, .95f);
        else if (key == "sun_enabled" && isNumber) s.sunEnabled = number != 0;
        else if (key == "sun_source") s.sunFromGame = value != "manual";
        else if (key == "sun_position") s.sunAtSkyDisc = value != "light";
        else if (key == "sun_color_source") s.sunColorFromGame = value != "manual";
        else if (key == "ambient_source") s.ambientFromGame = value != "manual";
        else if (key == "ambient_scale" && isNumber) s.ambientScale = std::clamp(number, 0.f, 8.f);
        else if (key == "temporal_weight" && isNumber) s.temporalWeight = std::clamp(number, 0.f, .98f);
        else if (key == "temporal_moving" && isNumber) s.temporalMoving = std::clamp(number, 0.f, .98f);
        else if (key == "noise_amount" && isNumber) s.noiseAmount = std::clamp(number, 0.f, 1.f);
        else if (key == "noise_scale" && isNumber) s.noiseScale = number <= 16 ? 12.f : number <= 36 ? 24.f : 48.f;
        else if (key == "noise_wind" && isNumber) s.noiseWind = std::clamp(number, 0.f, 20.f);
        else if (key == "shadows" && isNumber) s.shadows = number != 0;
        else if (key == "shadow_length" && isNumber) s.shadowLength = std::clamp(number, 5.f, 400.f);
        else if (key == "sky_light" && isNumber) s.skyLight = std::clamp(number, 0.f, 1.f);
        else if (key == "light_gizmo" && isNumber) s.lightGizmo = number != 0;
        else if (key == "light_sources" && isNumber) s.lightSources = number != 0;
        else if (key == "game_fog_distance" && isNumber) s.gameFogDistance = number <= 0 ? 0.f : std::clamp(number, 1.f, 10.f);
        else if (key == "shadow_softness" && isNumber) s.shadowSoftness = number < 1.1f ? 1.f : std::clamp(number, 1.1f, 4.f);
        else if (key == "soft_shadows" && isNumber) s.softShadows = std::clamp(number, 0.f, 4.f);
        else if (key == "shadow_fade" && isNumber) s.shadowFade = std::clamp(number, 0.f, 1.f);
        else if (key == "indoor_shadows" && isNumber) s.indoorShadows = std::clamp(number, 0.f, 1.f);
        else if (key == "shadow_fade_distance" && isNumber) s.shadowFadeDistance = std::clamp(number, 1.f, 100.f);
        else if (key == "contact_shadows" && isNumber) s.contactShadows = std::clamp(number, 0.f, 1.f);
        else if (key == "ambient_occlusion" && isNumber) s.ambientOcclusion = std::clamp(number, 0.f, 1.f);
        else if (key == "terrain_shadows" && isNumber) s.terrainShadows = std::clamp(number, 0.f, 1.f);
        else if (key == "point_shadows" && isNumber) s.pointShadows = std::clamp(number, 0.f, 1.f);
        else if (key == "point_shadow_range" && isNumber) s.pointShadowRange = std::clamp(number, 2.f, 60.f);
        else if (key == "point_shadow_ignore" && isNumber) s.pointShadowIgnore = std::clamp(number, 0.f, 5.f);
        else if (key == "point_shadow_length" && isNumber) s.pointShadowLength = std::clamp(number, .5f, 50.f);
        else if (key == "terrain_shadows_from" && isNumber) s.terrainShadowsFrom = std::clamp(number, 0.f, 500.f);
        else if (key == "open_sun" && isNumber) s.openSun = std::clamp(number, 0.f, 1.f);
        else if (key == "weather_sun" && isNumber) s.weatherSun = std::clamp(number, 0.f, 1.f);
        else if (key == "transparent_fog" && isNumber) s.transparentFog = number != 0;
        else if (key == "sky_fog" && isNumber) s.skyFog = std::clamp(number, 0.f, 1.f);
        else if (key == "max_opacity" && isNumber) s.maxOpacity = std::clamp(number, .05f, 1.f);
        else if (key == "light_limit" && isNumber) s.lightLimit = std::clamp(number, 0.f, 4.f);
        else if (key == "auto_exposure" && isNumber) s.autoExposure = number != 0;
        else if (key == "exposure_target" && isNumber) s.exposureTarget = std::clamp(number, .05f, 1.f);
        else if (key == "exposure_min" && isNumber) s.exposureMin = std::clamp(number, .1f, 1.f);
        else if (key == "game_shadows" && isNumber) s.gameShadows = number != 0;
        else if (key == "game_shadow_flip" && isNumber) s.gameShadowFlip = number < 0 ? -1.f : 1.f;
        else if (key == "sun_aureole" && isNumber) s.sunAureole = std::clamp(number, 0.f, .5f);
        else if (key == "sun_aureole_g" && isNumber) s.sunAureoleG = std::clamp(number, .5f, .98f);
        else if (key == "sun_glare" && isNumber) s.sunGlare = std::clamp(number, 0.f, 2.f);
        else if (key == "sun_glare_radius" && isNumber) s.sunGlareRadius = std::clamp(number, .2f, 5.f);
        else if (key == "sun_rays" && isNumber) s.sunRays = std::clamp(number, 0.f, 4.f);
        else if (key == "sun_rays_length" && isNumber) s.sunRaysLength = std::clamp(number, .2f, 1.f);
        else if (key == "albedo") ok = parse_triple(value, s.medium.albedo);
        else if (key == "emissive") ok = parse_triple(value, s.medium.emissive);
        else if (key == "ambient") ok = parse_triple(value, s.ambient);
        else if (key == "sun_direction") ok = parse_triple(value, s.sunDirection);
        else if (key == "sun_color") ok = parse_triple(value, s.sunColor);
        else ok = false;
        if (!ok) bad.push_back(key);
    }
    if (!sawHeightReference) s.heightReference = fog::HeightReference::World; // files from before the key: base_height was written as an absolute z
    const float len = std::sqrt(s.sunDirection[0] * s.sunDirection[0] + s.sunDirection[1] * s.sunDirection[1] + s.sunDirection[2] * s.sunDirection[2]);
    if (len > 1e-6f) for (auto& c : s.sunDirection) c /= len;
    st.settings = s;
    ++st.settingsGeneration;
    st.settingsNote = bad.empty() ? "loaded" : "ignored: " + bad.front();
    log_line("[FOG] settings " + st.settingsPath + " " + st.settingsNote + " preset=" + preset_name(s.preset) + " density=" + std::to_string(s.medium.baseDensity) +
             " falloff=" + std::to_string(s.medium.heightFalloff) + " baseHeight=" + std::to_string(s.medium.baseHeight) + " maxDistance=" + std::to_string(s.maxDistance));
}

// ---- resources -------------------------------------------------------------------------------------
bool memory_for(const DeviceData& d, const FogFns& f, const VkMemoryRequirements& r, VkDeviceMemory& out) {
    if (!d.get_memory_properties) return false;
    VkPhysicalDeviceMemoryProperties p{};
    d.get_memory_properties(d.physical_device, &p);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if (r.memoryTypeBits & (1u << i)) { if (type == UINT32_MAX) type = i; if (p.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { type = i; break; } }
    if (type == UINT32_MAX) return false;
    auto a = vk<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    a.allocationSize = r.size; a.memoryTypeIndex = type;
    return f.AllocateMemory(d.device, &a, nullptr, &out) == VK_SUCCESS;
}
bool make_volume(FogResources& r, Volume& v, std::string& why) {
    auto i = vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    i.imageType = VK_IMAGE_TYPE_3D; i.format = VK_FORMAT_R16G16B16A16_SFLOAT; i.extent = {r.grid.x, r.grid.y, r.grid.z};
    i.mipLevels = 1; i.arrayLayers = 1; i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL;
    i.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; i.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;   // TRANSFER_SRC: fog capture
    if (r.f.CreateImage(r.data.device, &i, nullptr, &v.image) != VK_SUCCESS) { why = "3D RGBA16F image"; return false; }
    VkMemoryRequirements req{};
    r.f.GetImageMemoryRequirements(r.data.device, v.image, &req);
    if (!memory_for(r.data, r.f, req, v.memory) || r.f.BindImageMemory(r.data.device, v.image, v.memory, 0) != VK_SUCCESS) { why = "volume memory"; return false; }
    auto w = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    w.image = v.image; w.viewType = VK_IMAGE_VIEW_TYPE_3D; w.format = i.format; w.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (r.f.CreateImageView(r.data.device, &w, nullptr, &v.view) != VK_SUCCESS) { why = "volume view"; return false; }
    return true;
}
VkShaderModule load_module(FogResources& r, const char* name) {
    const auto code = read_spirv_file(depth_shader_dir() + "/" + name);
    if (code.empty()) return VkShaderModule{};
    auto m = vk<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    m.codeSize = code.size() * 4; m.pCode = code.data();
    VkShaderModule out{};
    return r.f.CreateShaderModule(r.data.device, &m, nullptr, &out) == VK_SUCCESS ? out : VkShaderModule{};
}
bool make_visibility_volume(FogResources& r, Volume& v, std::string& why) {
    auto i = vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    i.imageType = VK_IMAGE_TYPE_3D; i.format = VK_FORMAT_R8G8B8A8_UNORM; i.extent = {r.grid.x, r.grid.y, r.grid.z};
    i.mipLevels = 1; i.arrayLayers = 1; i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL;
    i.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; i.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (r.f.CreateImage(r.data.device, &i, nullptr, &v.image) != VK_SUCCESS) { why = "visibility volume"; return false; }
    VkMemoryRequirements req{};
    r.f.GetImageMemoryRequirements(r.data.device, v.image, &req);
    if (!memory_for(r.data, r.f, req, v.memory) || r.f.BindImageMemory(r.data.device, v.image, v.memory, 0) != VK_SUCCESS) { why = "visibility memory"; return false; }
    auto w = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    w.image = v.image; w.viewType = VK_IMAGE_VIEW_TYPE_3D; w.format = i.format; w.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (r.f.CreateImageView(r.data.device, &w, nullptr, &v.view) != VK_SUCCESS) { why = "visibility view"; return false; }
    return true;
}
// The smoke's noise lattice: fog::noiseLatticeCells^3 RGBA16F, storage (baked once) + sampled (inject, REPEAT).
bool make_noise_lattice(FogResources& r, Volume& v, std::string& why) {
    const uint32_t n = fog::noiseLatticeCells;
    auto i = vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    i.imageType = VK_IMAGE_TYPE_3D; i.format = VK_FORMAT_R16G16B16A16_SFLOAT; i.extent = {n, n, n};
    i.mipLevels = 1; i.arrayLayers = 1; i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL;
    i.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; i.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (r.f.CreateImage(r.data.device, &i, nullptr, &v.image) != VK_SUCCESS) { why = "noise lattice"; return false; }
    VkMemoryRequirements req{};
    r.f.GetImageMemoryRequirements(r.data.device, v.image, &req);
    if (!memory_for(r.data, r.f, req, v.memory) || r.f.BindImageMemory(r.data.device, v.image, v.memory, 0) != VK_SUCCESS) { why = "noise lattice memory"; return false; }
    auto w = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    w.image = v.image; w.viewType = VK_IMAGE_VIEW_TYPE_3D; w.format = i.format; w.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (r.f.CreateImageView(r.data.device, &w, nullptr, &v.view) != VK_SUCCESS) { why = "noise lattice view"; return false; }
    return true;
}
bool make_memory_image(FogResources& r, Volume& v, std::string& why) {
    auto i = vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    i.imageType = VK_IMAGE_TYPE_2D; i.format = VK_FORMAT_R32_UINT; i.extent = {uint32_t(FogResources::memorySize), uint32_t(FogResources::memorySize), 1};
    i.mipLevels = 1; i.arrayLayers = 1; i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL;
    i.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; i.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (r.f.CreateImage(r.data.device, &i, nullptr, &v.image) != VK_SUCCESS) { why = "height memory image"; return false; }
    VkMemoryRequirements req{};
    r.f.GetImageMemoryRequirements(r.data.device, v.image, &req);
    if (!memory_for(r.data, r.f, req, v.memory) || r.f.BindImageMemory(r.data.device, v.image, v.memory, 0) != VK_SUCCESS) { why = "height memory memory"; return false; }
    auto w = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    w.image = v.image; w.viewType = VK_IMAGE_VIEW_TYPE_2D; w.format = i.format; w.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (r.f.CreateImageView(r.data.device, &w, nullptr, &v.view) != VK_SUCCESS) { why = "height memory view"; return false; }
    return true;
}
VkPipeline compute_pipeline(FogResources& r, const char* name) {
    auto module = load_module(r, name);
    if (!module) return VkPipeline{};
    auto c = vk<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    c.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    c.layout = r.computeLayout;
    VkPipeline p{};
    if (r.f.CreateComputePipelines(r.data.device, VkPipelineCache{}, 1, &c, nullptr, &p) != VK_SUCCESS) p = VkPipeline{};
    r.f.DestroyShaderModule(r.data.device, module, nullptr);
    return p;
}
bool set_layout(FogResources& r, VkDescriptorSetLayout& out, std::initializer_list<VkDescriptorSetLayoutBinding> bindings) {
    std::vector<VkDescriptorSetLayoutBinding> list(bindings);
    auto i = vk<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    i.bindingCount = uint32_t(list.size()); i.pBindings = list.data();
    return r.f.CreateDescriptorSetLayout(r.data.device, &i, nullptr, &out) == VK_SUCCESS;
}
bool pipeline_layout(FogResources& r, VkPipelineLayout& out, VkDescriptorSetLayout set) {
    auto i = vk<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    i.setLayoutCount = 1; i.pSetLayouts = &set;
    return r.f.CreatePipelineLayout(r.data.device, &i, nullptr, &out) == VK_SUCCESS;
}
// One color attachment kept in `layout` (composite: the application's color, loaded; shadow: our R32F image, fully rewritten).
bool render_pass(FogResources& r, VkFormat format, bool load, VkImageLayout finalLayout, VkRenderPass& out) {
    VkAttachmentDescription a{};
    a.format = format; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; a.finalLayout = finalLayout;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    auto p = vk<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    p.attachmentCount = 1; p.pAttachments = &a; p.subpassCount = 1; p.pSubpasses = &sub;
    return r.f.CreateRenderPass(r.data.device, &p, nullptr, &out) == VK_SUCCESS;
}
// Fullscreen triangle (`vertex`: fog_composite.vert, or fog_glare.vert for the composite) + `fragment`, composite descriptor
// layout. blend: the composite's (ONE, SRC_ALPHA).
bool fullscreen_pipeline(FogResources& r, VkRenderPass pass, const char* fragment, bool blendOn, VkPipeline& out,
                         const char* vertex = "fog_composite.vert.spv") {
    auto vs = load_module(r, vertex), fs = load_module(r, fragment);
    bool ok = vs && fs;
    if (ok) {
        VkPipelineShaderStageCreateInfo stages[2]{
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
        auto vi = vk<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        auto ia = vk<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        auto vp = vk<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        vp.viewportCount = 1; vp.scissorCount = 1;
        auto rs = vk<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
        auto ms = vk<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = blendOn ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        auto cb = vk<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        cb.attachmentCount = 1; cb.pAttachments = &blend;
        const VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dy = vk<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dy.dynamicStateCount = 2; dy.pDynamicStates = dynamics;
        auto g = vk<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        g.stageCount = 2; g.pStages = stages; g.pVertexInputState = &vi; g.pInputAssemblyState = &ia; g.pViewportState = &vp;
        g.pRasterizationState = &rs; g.pMultisampleState = &ms; g.pColorBlendState = &cb; g.pDynamicState = &dy;
        g.layout = r.compositeLayout; g.renderPass = pass;
        ok = r.f.CreateGraphicsPipelines(r.data.device, VkPipelineCache{}, 1, &g, nullptr, &out) == VK_SUCCESS;
    }
    if (vs) r.f.DestroyShaderModule(r.data.device, vs, nullptr);
    if (fs) r.f.DestroyShaderModule(r.data.device, fs, nullptr);
    return ok;
}

bool shadow_image(FogResources& r, std::string& why) {
    r.shadowWidth = (r.width + FogResources::shadowFactor - 1) / FogResources::shadowFactor;
    r.shadowHeight = (r.height + FogResources::shadowFactor - 1) / FogResources::shadowFactor;
    auto i = vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    i.imageType = VK_IMAGE_TYPE_2D; i.format = VK_FORMAT_R32_SFLOAT; i.extent = {r.shadowWidth, r.shadowHeight, 1}; i.mipLevels = 1; i.arrayLayers = 1;
    i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL; i.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (r.f.CreateImage(r.data.device, &i, nullptr, &r.shadow) != VK_SUCCESS) { why = "shadow image"; return false; }
    VkMemoryRequirements req{};
    r.f.GetImageMemoryRequirements(r.data.device, r.shadow, &req);
    if (!memory_for(r.data, r.f, req, r.shadowMemory) || r.f.BindImageMemory(r.data.device, r.shadow, r.shadowMemory, 0) != VK_SUCCESS) { why = "shadow memory"; return false; }
    auto v = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    v.image = r.shadow; v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = i.format; v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (r.f.CreateImageView(r.data.device, &v, nullptr, &r.shadowView) != VK_SUCCESS) { why = "shadow view"; return false; }
    if (!render_pass(r, i.format, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, r.shadowPass)) { why = "shadow render pass"; return false; }
    auto f = vk<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    f.renderPass = r.shadowPass; f.attachmentCount = 1; f.pAttachments = &r.shadowView; f.width = r.shadowWidth; f.height = r.shadowHeight; f.layers = 1;
    if (r.f.CreateFramebuffer(r.data.device, &f, nullptr, &r.shadowFramebuffer) != VK_SUCCESS) { why = "shadow framebuffer"; return false; }
    if (!fullscreen_pipeline(r, r.shadowPass, "fog_shadow_depth.frag.spv", false, r.shadowPipeline)) { why = "shadow pipeline (fog_shadow_depth.frag.spv)"; return false; }
    return true;
}

// The game's sun cascades, reduced (roadmap M10): R32F cascadeSize^2 x 4 layers, a colour target per layer and an array view
// for the light pass.
bool cascade_maps(FogResources& r, std::string& why) {
    const uint32_t n = FogResources::cascadeSize;
    auto i = vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    i.imageType = VK_IMAGE_TYPE_2D; i.format = VK_FORMAT_R32_SFLOAT; i.extent = {n, n, 1}; i.mipLevels = 1; i.arrayLayers = 4;
    i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL; i.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (r.f.CreateImage(r.data.device, &i, nullptr, &r.cascadeMaps) != VK_SUCCESS) { why = "cascade maps"; return false; }
    VkMemoryRequirements req{};
    r.f.GetImageMemoryRequirements(r.data.device, r.cascadeMaps, &req);
    if (!memory_for(r.data, r.f, req, r.cascadeMemory) || r.f.BindImageMemory(r.data.device, r.cascadeMaps, r.cascadeMemory, 0) != VK_SUCCESS) { why = "cascade maps memory"; return false; }
    auto v = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    v.image = r.cascadeMaps; v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; v.format = i.format; v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 4};
    if (r.f.CreateImageView(r.data.device, &v, nullptr, &r.cascadeArrayView) != VK_SUCCESS) { why = "cascade maps view"; return false; }
    if (!render_pass(r, i.format, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, r.cascadePass)) { why = "cascade render pass"; return false; }
    for (uint32_t k = 0; k < 4; ++k) {
        v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, k, 1};
        if (r.f.CreateImageView(r.data.device, &v, nullptr, &r.cascadeLayerViews[k]) != VK_SUCCESS) { why = "cascade layer view"; return false; }
        auto fb = vk<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        fb.renderPass = r.cascadePass; fb.attachmentCount = 1; fb.pAttachments = &r.cascadeLayerViews[k]; fb.width = n; fb.height = n; fb.layers = 1;
        if (r.f.CreateFramebuffer(r.data.device, &fb, nullptr, &r.cascadeFramebuffers[k]) != VK_SUCCESS) { why = "cascade framebuffer"; return false; }
    }
    if (!fullscreen_pipeline(r, r.cascadePass, "fog_cascade_reduce.frag.spv", false, r.cascadePipeline)) { why = "cascade pipeline (fog_cascade_reduce.frag.spv)"; return false; }
    return true;
}

// Sun rays: the mask (R8) and the rays (R16F) at a quarter of the resolution; the scene shading (RGBA16F) at half: a render
// pass + framebuffer + pipeline each.
bool rays_images(FogResources& r, std::string& why) {
    r.raysWidth = (r.width + FogResources::raysFactor - 1) / FogResources::raysFactor;
    r.raysHeight = (r.height + FogResources::raysFactor - 1) / FogResources::raysFactor;
    r.shadeWidth = (r.width + FogResources::shadeFactor - 1) / FogResources::shadeFactor;
    r.shadeHeight = (r.height + FogResources::shadeFactor - 1) / FogResources::shadeFactor;
    struct Target { FogResources::RaysTarget& t; VkFormat format; VkRenderPass& pass; VkPipeline& pipeline; const char* shader; uint32_t w, h; };
    for (Target x : {Target{r.raysMask, VK_FORMAT_R8_UNORM, r.raysMaskPass, r.raysMaskPipeline, "fog_rays_mask.frag.spv", r.raysWidth, r.raysHeight},
                     Target{r.rays, VK_FORMAT_R16_SFLOAT, r.raysPass, r.raysPipeline, "fog_rays.frag.spv", r.raysWidth, r.raysHeight},
                     Target{r.shade, VK_FORMAT_R16G16B16A16_SFLOAT, r.shadePass, r.shadePipeline, "fog_shade.frag.spv", r.shadeWidth, r.shadeHeight}}) {
        auto i = vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        i.imageType = VK_IMAGE_TYPE_2D; i.format = x.format; i.extent = {x.w, x.h, 1}; i.mipLevels = 1; i.arrayLayers = 1;
        i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL; i.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (r.f.CreateImage(r.data.device, &i, nullptr, &x.t.image) != VK_SUCCESS) { why = "sun rays image"; return false; }
        VkMemoryRequirements req{};
        r.f.GetImageMemoryRequirements(r.data.device, x.t.image, &req);
        if (!memory_for(r.data, r.f, req, x.t.memory) || r.f.BindImageMemory(r.data.device, x.t.image, x.t.memory, 0) != VK_SUCCESS) { why = "sun rays memory"; return false; }
        auto v = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        v.image = x.t.image; v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = x.format; v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (r.f.CreateImageView(r.data.device, &v, nullptr, &x.t.view) != VK_SUCCESS) { why = "sun rays view"; return false; }
        if (!render_pass(r, x.format, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, x.pass)) { why = "sun rays render pass"; return false; }
        auto f = vk<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        f.renderPass = x.pass; f.attachmentCount = 1; f.pAttachments = &x.t.view; f.width = x.w; f.height = x.h; f.layers = 1;
        if (r.f.CreateFramebuffer(r.data.device, &f, nullptr, &x.t.framebuffer) != VK_SUCCESS) { why = "sun rays framebuffer"; return false; }
        if (!fullscreen_pipeline(r, x.pass, x.shader, false, x.pipeline)) { why = std::string("sun rays pipeline (") + x.shader + ")"; return false; }
    }
    return true;
}

std::shared_ptr<FogResources> create_resources(const DeviceData& data, const fog::Grid& grid, VkFormat colorFormat, uint32_t width, uint32_t height,
                                               uint32_t timestampBits, std::string& why) {
    auto r = std::make_shared<FogResources>();
    r->data = data; r->f = load_fns(data); r->grid = grid; r->colorFormat = colorFormat; r->timestampBits = timestampBits;
    r->width = width; r->height = height;
    if (!r->f.complete) { why = "Vulkan entry points missing"; r->data.device = VkDevice{}; return nullptr; }
    auto fail = [&](const std::string& what) { why = what; return std::shared_ptr<FogResources>{}; };
    for (auto* v : {&r->medium, &r->lighting[0], &r->lighting[1], &r->integrated}) if (!make_volume(*r, *v, why)) return nullptr;
    for (auto* v : {&r->heightMemory, &r->tagMemory, &r->sunMemory, &r->sunTagMemory, &r->floorMemory}) if (!make_memory_image(*r, *v, why)) return nullptr;
    if (!make_visibility_volume(*r, r->visibilityVolume, why)) return nullptr;
    if (!make_noise_lattice(*r, r->noiseLattice, why)) return nullptr;
    {   // sun finder: quarter-resolution picture + counters (sunFindWords per compute slot, host visible)
        r->pictureWidth = (width + 3) / 4; r->pictureHeight = (height + 3) / 4;
        auto i = vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        i.imageType = VK_IMAGE_TYPE_2D; i.format = VK_FORMAT_R8G8B8A8_UNORM; i.extent = {r->pictureWidth, r->pictureHeight, 1}; i.mipLevels = 1; i.arrayLayers = 1;
        i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL; i.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (r->f.CreateImage(data.device, &i, nullptr, &r->picture.image) != VK_SUCCESS) return fail("sun finder picture");
        VkMemoryRequirements req{};
        r->f.GetImageMemoryRequirements(data.device, r->picture.image, &req);
        if (!memory_for(data, r->f, req, r->picture.memory) || r->f.BindImageMemory(data.device, r->picture.image, r->picture.memory, 0) != VK_SUCCESS) return fail("sun finder picture memory");
        auto v = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        v.image = r->picture.image; v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = i.format; v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (r->f.CreateImageView(data.device, &v, nullptr, &r->picture.view) != VK_SUCCESS) return fail("sun finder picture view");
        auto b = vk<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        b.size = computeSlots * readbackWords * sizeof(uint32_t); b.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (r->f.CreateBuffer(data.device, &b, nullptr, &r->sunFound) != VK_SUCCESS) return fail("sun finder buffer");
        r->f.GetBufferMemoryRequirements(data.device, r->sunFound, &req);
        VkPhysicalDeviceMemoryProperties props{};
        data.get_memory_properties(data.physical_device, &props);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < props.memoryTypeCount && type == UINT32_MAX; ++t)
            if ((req.memoryTypeBits & (1u << t)) && (props.memoryTypes[t].propertyFlags & want) == want) type = t;
        auto a = vk<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        a.allocationSize = req.size; a.memoryTypeIndex = type;
        void* mapped = nullptr;
        if (type == UINT32_MAX || r->f.AllocateMemory(data.device, &a, nullptr, &r->sunFoundMemory) != VK_SUCCESS ||
            r->f.BindBufferMemory(data.device, r->sunFound, r->sunFoundMemory, 0) != VK_SUCCESS ||
            r->f.MapMemory(data.device, r->sunFoundMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return fail("sun finder buffer memory");
        r->sunFoundMapped = static_cast<uint32_t*>(mapped);
    }
    {
        auto b = vk<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        b.size = sizeof(FogParams); b.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (r->f.CreateBuffer(data.device, &b, nullptr, &r->params) != VK_SUCCESS) return fail("params buffer");
        VkMemoryRequirements req{};
        r->f.GetBufferMemoryRequirements(data.device, r->params, &req);
        if (!memory_for(data, r->f, req, r->paramsMemory) || r->f.BindBufferMemory(data.device, r->params, r->paramsMemory, 0) != VK_SUCCESS) return fail("params memory");
    }
    {
        auto i = vk<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        i.imageType = VK_IMAGE_TYPE_2D; i.format = VK_FORMAT_R32G32B32A32_SFLOAT; i.extent = {1, 1, 1}; i.mipLevels = 1; i.arrayLayers = 1;
        i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL; i.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (r->f.CreateImage(data.device, &i, nullptr, &r->noWater) != VK_SUCCESS) return fail("no-water image");
        VkMemoryRequirements req{};
        r->f.GetImageMemoryRequirements(data.device, r->noWater, &req);
        if (!memory_for(data, r->f, req, r->noWaterMemory) || r->f.BindImageMemory(data.device, r->noWater, r->noWaterMemory, 0) != VK_SUCCESS) return fail("no-water memory");
        auto v = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        v.image = r->noWater; v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = i.format; v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (r->f.CreateImageView(data.device, &v, nullptr, &r->noWaterView) != VK_SUCCESS) return fail("no-water view");
    }
    for (auto [sampler, filter] : {std::pair{&r->linear, VK_FILTER_LINEAR}, std::pair{&r->nearest, VK_FILTER_NEAREST}}) {
        auto s = vk<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        s.magFilter = s.minFilter = filter; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; s.maxLod = 0;
        if (r->f.CreateSampler(data.device, &s, nullptr, sampler) != VK_SUCCESS) return fail("sampler");
    }
    {
        auto s = vk<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        s.magFilter = s.minFilter = VK_FILTER_LINEAR; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; s.maxLod = 0;
        if (r->f.CreateSampler(data.device, &s, nullptr, &r->repeat) != VK_SUCCESS) return fail("sampler");
    }
    const auto C = VK_SHADER_STAGE_COMPUTE_BIT, F = VK_SHADER_STAGE_FRAGMENT_BIT, V = VK_SHADER_STAGE_VERTEX_BIT; // V: fog_glare.vert
    if (!set_layout(*r, r->computeSetLayout, {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, C, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr},
                                               {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr}, {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr}, {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr},
                                               {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr}, {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr},
                                               {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr}, {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr},
                                               {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr}, {10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr},
                                               {11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr}, {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr},
                                               {13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr}, {14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr},
                                               {15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr}, {16, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr}}) ||
        !set_layout(*r, r->compositeSetLayout, {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, F | V, nullptr}, {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, F | V, nullptr},
                                                 {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, F, nullptr}, {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, F, nullptr},
                                                 {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, F, nullptr}, {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, F, nullptr},
                                                 {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, F, nullptr}, {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, F, nullptr},
                                                 {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, F, nullptr}, {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, F, nullptr},
                                                 {13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, F, nullptr}}) ||   // 13: floor memory (fog_shade.frag)
        !pipeline_layout(*r, r->computeLayout, r->computeSetLayout) || !pipeline_layout(*r, r->compositeLayout, r->compositeSetLayout))
        return fail("descriptor/pipeline layouts");
    r->inject = compute_pipeline(*r, "fog_inject.comp.spv");
    r->light = compute_pipeline(*r, "fog_light.comp.spv");
    r->integrate = compute_pipeline(*r, "fog_integrate.comp.spv");
    r->memoryClear = compute_pipeline(*r, "fog_memory_clear.comp.spv");
    r->memoryScatter = compute_pipeline(*r, "fog_memory_scatter.comp.spv");
    r->sunFind = compute_pipeline(*r, "fog_sun_find.comp.spv");
    r->noiseBake = compute_pipeline(*r, "fog_noise_bake.comp.spv");
    r->ground = compute_pipeline(*r, "fog_ground.comp.spv");
    r->exposure = compute_pipeline(*r, "fog_exposure.comp.spv");
    if (!r->inject || !r->light || !r->integrate || !r->memoryClear || !r->memoryScatter || !r->sunFind || !r->noiseBake || !r->ground || !r->exposure) return fail("compute pipelines (fog_*.comp.spv in " + depth_shader_dir() + ")");
    if (!render_pass(*r, colorFormat, true, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, r->pass) || !fullscreen_pipeline(*r, r->pass, "fog_composite.frag.spv", true, r->composite, "fog_glare.vert.spv"))
        return fail("composite pipeline (fog_glare.vert.spv, fog_composite.frag.spv)");
    if (!shadow_image(*r, why)) return nullptr;
    if (!cascade_maps(*r, why)) return nullptr;
    if (!rays_images(*r, why)) return nullptr;
    {
        // 2 compute sets, 32 world-depth sets (1 buffer + 9 samplers + the floor memory), 16 game-cascade sets (1 buffer + 5:
        // they never run the composite or the rays / shading passes, which alone read bindings 9..13) - both of the composite layout.
        const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 + 32 + 16}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 22 + 32},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 + 9 * 32 + 5 * 16}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        auto p = vk<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        p.maxSets = 2 + 32 + 16; p.poolSizeCount = 4; p.pPoolSizes = sizes;
        if (r->f.CreateDescriptorPool(data.device, &p, nullptr, &r->pool) != VK_SUCCESS) return fail("descriptor pool");
        for (uint32_t parity = 0; parity < 2; ++parity) {
            auto a = vk<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
            a.descriptorPool = r->pool; a.descriptorSetCount = 1; a.pSetLayouts = &r->computeSetLayout;
            if (r->f.AllocateDescriptorSets(data.device, &a, &r->computeSet[parity]) != VK_SUCCESS) return fail("compute descriptor set");
            VkDescriptorBufferInfo ubo{r->params, 0, sizeof(FogParams)};
            VkDescriptorImageInfo images[16]{{VkSampler{}, r->medium.view, VK_IMAGE_LAYOUT_GENERAL}, {VkSampler{}, r->lighting[parity].view, VK_IMAGE_LAYOUT_GENERAL},
                                            {VkSampler{}, r->integrated.view, VK_IMAGE_LAYOUT_GENERAL}, {r->linear, r->lighting[parity ^ 1].view, VK_IMAGE_LAYOUT_GENERAL},
                                            {r->nearest, r->shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VkSampler{}, r->heightMemory.view, VK_IMAGE_LAYOUT_GENERAL},
                                            {VkSampler{}, r->tagMemory.view, VK_IMAGE_LAYOUT_GENERAL}, {VkSampler{}, r->sunMemory.view, VK_IMAGE_LAYOUT_GENERAL},
                                            {VkSampler{}, r->sunTagMemory.view, VK_IMAGE_LAYOUT_GENERAL}, {VkSampler{}, r->visibilityVolume.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {VkSampler{}, r->picture.view, VK_IMAGE_LAYOUT_GENERAL}, {}, {VkSampler{}, r->noiseLattice.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {r->repeat, r->noiseLattice.view, VK_IMAGE_LAYOUT_GENERAL}, {VkSampler{}, r->floorMemory.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {r->nearest, r->cascadeArrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkDescriptorBufferInfo found{r->sunFound, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w[17]{};
            for (uint32_t i = 0; i < 17; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = r->computeSet[parity]; w[i].dstBinding = i; w[i].descriptorCount = 1;
                w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : (i == 4 || i == 5 || i == 14 || i == 16) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                      i == 12 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (i == 0) w[i].pBufferInfo = &ubo; else if (i == 12) w[i].pBufferInfo = &found; else w[i].pImageInfo = &images[i - 1];
            }
            r->f.UpdateDescriptorSets(data.device, 17, w, 0, nullptr);
        }
    }
    {
        auto p = vk<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; p.queueFamilyIndex = 0;
        if (r->f.CreateCommandPool(data.device, &p, nullptr, &r->commandPool) != VK_SUCCESS) return fail("command pool");
        for (auto& slot : r->compute) {
            auto a = vk<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
            a.commandPool = r->commandPool; a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; a.commandBufferCount = 1;
            if (r->f.AllocateCommandBuffers(data.device, &a, &slot.cmd) != VK_SUCCESS) return fail("command buffer");
            // Layer-created dispatchable object: the loader's dispatch pointer must be installed.
            if (data.set_device_loader_data && data.set_device_loader_data(data.device, slot.cmd) != VK_SUCCESS) return fail("loader data");
            auto fi = vk<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (r->f.CreateFence(data.device, &fi, nullptr, &slot.fence) != VK_SUCCESS) return fail("fence");
        }
    }
    if (timestampBits && data.get_properties) {
        VkPhysicalDeviceProperties props{};
        data.get_properties(data.physical_device, &props);
        r->timestampPeriod = props.limits.timestampPeriod;
        auto q = vk<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        q.queryType = VK_QUERY_TYPE_TIMESTAMP; q.queryCount = computeSlots * computeStamps + compositeSlots * 2;
        if (!props.limits.timestampComputeAndGraphics || r->f.CreateQueryPool(data.device, &q, nullptr, &r->queries) != VK_SUCCESS) r->queries = VkQueryPool{};
    }
    return r;
}

// Our framebuffer/view on the application's color image (cached per image + generation).
VkFramebuffer framebuffer_for(FogResources& r, const Image& color, VkFormat viewFormat, uint32_t width, uint32_t height) {
    for (auto& fb : r.framebuffers) if (fb.image == color.handle && fb.generation == color.generation) return fb.framebuffer;
    if (r.framebuffers.size() >= maxFramebuffers) return VkFramebuffer{};
    FogFramebuffer fb{color.handle, color.generation, VkImageView{}, VkFramebuffer{}};
    auto v = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    v.image = (VkImage)color.handle; v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = viewFormat; v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (r.f.CreateImageView(r.data.device, &v, nullptr, &fb.view) != VK_SUCCESS) return VkFramebuffer{};
    auto f = vk<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    f.renderPass = r.pass; f.attachmentCount = 1; f.pAttachments = &fb.view; f.width = width; f.height = height; f.layers = 1;
    if (r.f.CreateFramebuffer(r.data.device, &f, nullptr, &fb.framebuffer) != VK_SUCCESS) { r.f.DestroyImageView(r.data.device, fb.view, nullptr); return VkFramebuffer{}; }
    r.framebuffers.push_back(fb);
    return fb.framebuffer;
}
// Our DEPTH-aspect sampled view of the scene depth + the composite descriptor set that reads it.
VkDescriptorSet depth_set_for(FogResources& r, const Image& depth, VkImageView waterMask, uint32_t parity) {
    for (auto& b : r.depths) if (b.image == depth.handle && b.generation == depth.generation && b.waterMask == waterMask && b.parity == parity) return b.set;
    if (r.depths.size() >= 32) return VkDescriptorSet{};
    DepthBinding b{depth.handle, depth.generation, waterMask, parity, VkImageView{}, VkDescriptorSet{}};
    auto v = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    v.image = (VkImage)depth.handle; v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = depth.format; v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (r.f.CreateImageView(r.data.device, &v, nullptr, &b.view) != VK_SUCCESS) return VkDescriptorSet{};
    auto a = vk<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    a.descriptorPool = r.pool; a.descriptorSetCount = 1; a.pSetLayouts = &r.compositeSetLayout;
    if (r.f.AllocateDescriptorSets(r.data.device, &a, &b.set) != VK_SUCCESS) { r.f.DestroyImageView(r.data.device, b.view, nullptr); return VkDescriptorSet{}; }
    VkDescriptorBufferInfo ubo{r.params, 0, sizeof(FogParams)};
    VkDescriptorImageInfo images[9]{{r.nearest, b.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}, {r.linear, r.medium.view, VK_IMAGE_LAYOUT_GENERAL},
                                    {r.linear, r.lighting[parity].view, VK_IMAGE_LAYOUT_GENERAL}, {r.linear, r.integrated.view, VK_IMAGE_LAYOUT_GENERAL},
                                    {r.nearest, waterMask ? waterMask : r.noWaterView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                    {r.linear, r.raysMask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {r.linear, r.rays.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                    {r.nearest, r.cascadeArrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {r.nearest, r.shade.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkDescriptorImageInfo floorInfo{VkSampler{}, r.floorMemory.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[11]{};
    for (uint32_t i = 0; i < 11; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = b.set; writes[i].descriptorCount = 1;
        writes[i].dstBinding = i ? 3 + i : 0;
        writes[i].descriptorType = i == 10 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (i == 10) writes[i].pImageInfo = &floorInfo; else if (i) writes[i].pImageInfo = &images[i - 1]; else writes[i].pBufferInfo = &ubo;
    }
    r.f.UpdateDescriptorSets(r.data.device, 11, writes, 0, nullptr);
    r.depths.push_back(b);
    return b.set;
}

// A composite-layout descriptor set whose binding 4 is a depth view of one of the game's cascade maps in the layout the game
// samples it in (its own sets are never rewritten: one per image, generation and layout, up to 16).
VkDescriptorSet cascade_set_for(FogResources& r, const Image& image, VkImageLayout layout) {
    for (auto& c : r.cascadeSources) if (c.image == image.handle && c.generation == image.generation && c.layout == layout) return c.set;
    if (r.cascadeSources.size() >= 16) return VkDescriptorSet{};
    FogResources::CascadeSource c{image.handle, image.generation, layout, VkImageView{}, VkDescriptorSet{}};
    auto v = vk<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    v.image = (VkImage)image.handle; v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = image.format; v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (r.f.CreateImageView(r.data.device, &v, nullptr, &c.view) != VK_SUCCESS) return VkDescriptorSet{};
    auto a = vk<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    a.descriptorPool = r.pool; a.descriptorSetCount = 1; a.pSetLayouts = &r.compositeSetLayout;
    if (r.f.AllocateDescriptorSets(r.data.device, &a, &c.set) != VK_SUCCESS) { r.f.DestroyImageView(r.data.device, c.view, nullptr); return VkDescriptorSet{}; }
    VkDescriptorBufferInfo ubo{r.params, 0, sizeof(FogParams)};
    VkDescriptorImageInfo images[5]{{r.nearest, c.view, layout}, {r.linear, r.medium.view, VK_IMAGE_LAYOUT_GENERAL}, {r.linear, r.lighting[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                    {r.linear, r.integrated.view, VK_IMAGE_LAYOUT_GENERAL}, {r.nearest, r.noWaterView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[6]{};
    for (uint32_t i = 0; i < 6; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = c.set; writes[i].descriptorCount = 1;
        writes[i].dstBinding = i ? 3 + i : 0;
        writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (i) writes[i].pImageInfo = &images[i - 1]; else writes[i].pBufferInfo = &ubo;
    }
    r.f.UpdateDescriptorSets(r.data.device, 6, writes, 0, nullptr);
    r.cascadeSources.push_back(c);
    return c.set;
}

// ---- fog capture: planning and copying -----------------------------------------------------------------
void image_barrier(const FogFns& f, VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);
struct CaptureJob {
    std::string name, format;
    VkImage image{}; VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;   // the layout the image is in when copied (GENERAL: copied in place)
    uint32_t width = 0, height = 0, depth = 1; size_t texel = 0;
    size_t offset = 0;
    VkImageAspectFlags barrierAspect = 0;   // layout transitions (0 = aspect): a depth/stencil image needs both aspects
};
std::shared_ptr<FogCapture> capture_prepare(FogResources& r, const std::shared_ptr<FogResources>& keep, const std::string& dir, const std::string& label,
                                            std::vector<CaptureJob>& jobs, std::string& why) {
    size_t total = 0;
    for (auto& j : jobs) { j.offset = total; total = (total + size_t(j.width) * j.height * j.depth * j.texel + 255) / 256 * 256; }
    if (!total) { why = "nothing to copy"; return nullptr; }
    auto c = std::make_shared<FogCapture>();
    c->f = r.f; c->device = r.data.device; c->keep = keep; c->dir = dir; c->label = label;
    auto b = vk<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    b.size = total; b.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (r.f.CreateBuffer(c->device, &b, nullptr, &c->buffer) != VK_SUCCESS) { why = "readback buffer"; return nullptr; }
    VkMemoryRequirements req{};
    r.f.GetBufferMemoryRequirements(c->device, c->buffer, &req);
    VkPhysicalDeviceMemoryProperties props{};
    r.data.get_memory_properties(r.data.physical_device, &props);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want &&
            (type == UINT32_MAX || (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))) type = i;
    if (type == UINT32_MAX) { why = "no host-visible memory"; return nullptr; }
    auto a = vk<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    a.allocationSize = req.size; a.memoryTypeIndex = type;
    if (r.f.AllocateMemory(c->device, &a, nullptr, &c->memory) != VK_SUCCESS || r.f.BindBufferMemory(c->device, c->buffer, c->memory, 0) != VK_SUCCESS ||
        r.f.MapMemory(c->device, c->memory, 0, VK_WHOLE_SIZE, 0, &c->mapped) != VK_SUCCESS) { why = "readback memory (" + std::to_string(total >> 20) + " MiB)"; return nullptr; }
    auto e = vk<VkEventCreateInfo>(VK_STRUCTURE_TYPE_EVENT_CREATE_INFO);
    if (r.f.CreateEvent(c->device, &e, nullptr, &c->event) != VK_SUCCESS) { why = "event"; return nullptr; }
    for (const auto& j : jobs) c->sections.push_back({j.name, j.format, j.width, j.height, j.depth, j.offset, size_t(j.width) * j.height * j.depth * j.texel});
    return c;
}
// Everything earlier in the command buffer (and the queue) -> transfer; copy; back to the layout it was in.
void capture_copy(const FogFns& f, VkCommandBuffer cmd, const FogCapture& c, const CaptureJob& j) {
    const bool general = j.layout == VK_IMAGE_LAYOUT_GENERAL;
    const VkImageAspectFlags barrierAspect = j.barrierAspect ? j.barrierAspect : j.aspect;
    if (general) {
        auto m = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; m.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
    } else {
        image_barrier(f, cmd, j.image, barrierAspect, j.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    }
    VkBufferImageCopy copy{};
    copy.bufferOffset = j.offset; copy.imageSubresource = {j.aspect, 0, 0, 1}; copy.imageExtent = {j.width, j.height, j.depth};
    f.CmdCopyImageToBuffer(cmd, j.image, general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.buffer, 1, &copy);
    if (!general)
        image_barrier(f, cmd, j.image, barrierAspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, j.layout, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

double ticks_to_ms(const FogResources& r, uint64_t a, uint64_t b) {
    const uint64_t mask = r.timestampBits >= 64 ? ~0ull : ((1ull << r.timestampBits) - 1);
    return double((b - a) & mask) * r.timestampPeriod * 1e-6;
}
void ema(double& value, double sample) { value = value == 0 ? sample : value * .9 + sample * .1; }

// ---- per-frame parameters ------------------------------------------------------------------------------
// game: WoW's light block when it was read this frame (nullptr = not readable). Its direction is used when sun_source = game,
// its ambient colour when ambient_source = game; otherwise (or when unreadable) the settings' values.
FogParams make_params(const FogSettings& s, const fog::Grid& g, const ReconstructionSetup& r, uint32_t debug, float baseHeight, const GameSun* game,
                      const float* seenDisc = nullptr, float sunWeatherScale = 1) {
    const GameSun* sun = s.sunFromGame ? game : nullptr;
    FogParams p{};
    p.grid[0] = float(g.x); p.grid[1] = float(g.y); p.grid[2] = float(g.z); p.grid[3] = g.maxDistance;
    p.dist[0] = g.distribution; p.dist[1] = std::log1p(g.maxDistance / g.distribution); p.dist[2] = s.lightLimit;
    p.proj[0] = r.p00; p.proj[1] = r.p11; p.proj[2] = r.p22; p.proj[3] = r.p23;
    p.viewport[0] = r.viewport.x; p.viewport[1] = r.viewport.y; p.viewport[2] = r.viewport.width; p.viewport[3] = r.viewport.height;
    p.depthRange[0] = r.viewport.minDepth; p.depthRange[1] = r.viewport.maxDepth; p.depthRange[2] = s.maxOpacity;
    p.right[0] = r.right.x; p.right[1] = r.right.y; p.right[2] = r.right.z; p.right[3] = float(r.camera[2]);
    p.up[0] = r.up.x; p.up[1] = r.up.y; p.up[2] = r.up.z;
    p.forward[0] = r.forward.x; p.forward[1] = r.forward.y; p.forward[2] = r.forward.z;
    p.medium[0] = s.medium.baseDensity; p.medium[1] = s.medium.heightFalloff; p.medium[2] = baseHeight; p.medium[3] = s.medium.maxExtinction;
    std::copy_n(s.medium.albedo, 3, p.albedo); p.albedo[3] = s.anisotropy;
    p.phase[0] = s.sunAureole; p.phase[1] = s.sunAureoleG; p.phase[2] = s.sunGlareRadius * 3.14159265f / 180.f;
    p.rays[1] = s.sunRaysLength; p.rays[2] = .97f; p.rays[3] = 8;   // rays[0]: at the composite, with the disc's direction
    std::copy_n(s.medium.emissive, 3, p.emissive); p.emissive[3] = s.skyFog;
    if (game && s.ambientFromGame) for (int i = 0; i < 3; ++i) p.ambient[i] = game->ambient[i] * s.ambientScale;
    else std::copy_n(s.ambient, 3, p.ambient);
    // The disc seen on the screen first (it is what the rays should come from), then the game's sky value, its lighting
    // direction, fog.ini.
    const float* toward = s.sunFromGame && s.sunAtSkyDisc && seenDisc ? seenDisc : !sun ? s.sunDirection : s.sunAtSkyDisc && sun->skyOk ? sun->skyToward : sun->toward;
    std::copy_n(toward, 3, p.sunDir);
    // The sun finder searches along the game light's azimuth (WoW's disc is drawn there), 4 degrees either side, from 3 degrees
    // above the horizon; without the game's light it searches the whole sky.
    if (game) {
        const float h = std::sqrt(game->toward[0] * game->toward[0] + game->toward[1] * game->toward[1]);
        if (h > 1e-3f) { p.sunArc[0] = game->toward[0] / h; p.sunArc[1] = game->toward[1] / h; p.sunArc[2] = std::sin(4.f * 3.14159265f / 180.f); }
    }
    p.sunArc[3] = std::sin(3.f * 3.14159265f / 180.f);
    // A light below the horizon would light the fog from underneath the ground: no directional term then.
    p.sunDir[3] = s.sunEnabled && (!sun || toward[2] > 0) ? 1.f : 0.f;
    if (game && s.sunColorFromGame) for (int i = 0; i < 3; ++i) p.sunColor[i] = game->specular[i] * s.sunColor[0];
    else std::copy_n(s.sunColor, 3, p.sunColor);
    // Rain / snow / sandstorm: the sun is behind clouds, the fog keeps only its ambient light (WoW's own colours do not change).
    for (int i = 0; i < 3; ++i) p.sunColor[i] *= sunWeatherScale;
    p.mode[0] = debug;
    // Noise: world position = noiseOrigin + camera-relative offset; the origin is reduced modulo the noise period in double,
    // so WoW's ~17000-yard coordinates never reach the shader. Wind drifts the pattern slowly along a fixed direction.
    static const auto start = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double wind[3]{.93 * s.noiseWind * seconds, .37 * s.noiseWind * seconds, 0};
    p.noise[0] = s.noiseAmount; p.noise[1] = s.noiseScale;
    static float cachedAmount = -1, cachedInverseMean = 1;
    if (s.noiseAmount != cachedAmount) { cachedAmount = s.noiseAmount; cachedInverseMean = 1.f / std::max(fog::smoke_mean(fog::smoke_threshold(s.noiseAmount)), 1e-3f); }
    p.noise[2] = fog::smoke_threshold(s.noiseAmount); p.noise[3] = cachedInverseMean;
    const double period = fog::noise_period(s.noiseScale);   // the baked lattice repeats: 2 x scale x 64 yards
    for (int i = 0; i < 3; ++i) { double o = std::fmod(r.camera[i] + wind[i], period); if (o < 0) o += period; p.noiseOrigin[i] = float(o); }
    // The finer octaves also rise and cross the wind (1.5x / 0.8x the wind speed): billowing, not a rigid slide. 0 wind = still.
    const double detail[3]{-.37 * .8 * s.noiseWind * seconds, .93 * .8 * s.noiseWind * seconds, 1.5 * s.noiseWind * seconds};
    for (int i = 0; i < 3; ++i) p.noiseDetail[i] = float(std::fmod(detail[i], period) + (detail[i] < 0 ? period : 0.0));
    return p;
}

std::string hex_id(uint64_t v) { std::ostringstream o; o << "0x" << std::hex << v; return o.str(); }
const char* image_kind(const Image* i) { return !i ? "none" : i->swapchain ? "swapchain" : "offscreen"; }

void timeline(FogState& st, const std::string& text) { if (st.timelineFrames) log_line("[PASS-TIMELINE] " + text); }

// Barrier helpers (recorded straight to the next layer).
void image_barrier(const FogFns& f, VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    auto b = vk<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    f.CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// World z of the medium's base: absolute, or offset from the camera height followed with a 6 s time constant (snaps on
// jumps over 80 yards: teleports, zone changes).
float effective_base_height(FogState& st, double cameraZ) {
    const auto& s = st.settings;
    if (s.heightReference == fog::HeightReference::World) return st.lastBaseHeight = s.medium.baseHeight;
    const auto now = std::chrono::steady_clock::now();
    if (!st.haveReference || std::fabs(cameraZ - st.referenceZ) > 80.0) { st.referenceZ = cameraZ; st.haveReference = true; }
    else {
        const double dt = std::min(std::chrono::duration<double>(now - st.referenceTime).count(), 1.0);
        st.referenceZ += (cameraZ - st.referenceZ) * (1.0 - std::exp(-dt / 6.0));
    }
    st.referenceTime = now;
    if (s.heightReference == fog::HeightReference::Ground)   // until the first reading (a few frames): what a third-person camera looks down on
        return st.lastBaseHeight = float(st.groundValid ? st.groundZ : st.referenceZ - 5.0) + s.medium.baseHeight;
    return st.lastBaseHeight = float(st.referenceZ) + s.medium.baseHeight;
}

// ---- the composite (into the application's command buffer, outside any render pass) -------------------
void record_composite(const DeviceData& data, Device& s, Command& c, VkCommandBuffer cmd, const char* trigger) {
    auto& st = fog_state(s);
    // fog.ini before the first composite (fog_present reads it at the end of the frame): the first frames after switching the
    // fog on used the defaults, built their resources, then rebuilt them for the file's preset and lost the memories.
    if (st.settingsPath.empty()) load_settings(st);
    auto pending = st.pending;
    st.pending = {};
    const auto started = std::chrono::steady_clock::now();
    auto skip = [&](const std::string& why) {
        ++st.droppedThisFrame;
        if (st.lastProblem != why) log_line("[FOG] not composited: " + why);
        st.lastProblem = why;
    };
    if (!pending.layoutsKnown) return skip("world color/depth layout unknown at the trigger");
    if (c.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || !(c.usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)) return skip("trigger command buffer not primary one-time-submit");
    if (c.activeQueries || c.conditional || c.transformFeedback) return skip("active query/conditional rendering/transform feedback at the trigger");
    if (auto why = private_pass_restore_problem(s, c); !why.empty()) return skip("application state not restorable: " + why);
    ReconstructionSetup camera;
    std::string why;
    if (!scene_debugger_frame_reconstruction(s, camera, why)) return skip("camera: " + why);
    const auto& set = st.settings;
    const auto grid = fog::grid_for(set.preset, pending.width, pending.height, set.maxDistance, set.distribution);
    const VkFormat colorFormat = pending.colorViewFormat;
    if (!st.resources || st.resources->grid.x != grid.x || st.resources->grid.y != grid.y || st.resources->grid.z != grid.z || st.resources->colorFormat != colorFormat ||
        st.resources->width != pending.width || st.resources->height != pending.height) {
        if (st.resources) st.retired.emplace_back(s.frameSerial, std::move(st.resources));
        st.resources = create_resources(data, grid, colorFormat, pending.width, pending.height, queue_timestamp_bits(data, s, c), why);
        if (!st.resources) return skip("resources: " + why);
        log_line("[FOG] resources " + std::to_string(grid.x) + "x" + std::to_string(grid.y) + "x" + std::to_string(grid.z) + " (" +
                 std::to_string(3 * grid.x * grid.y * grid.z * 8 / 1024) + " KiB volumes) color format " + std::to_string(int(colorFormat)));
    }
    auto& r = *st.resources;
    r.grid = grid;
    const auto framebuffer = framebuffer_for(r, *pending.color, colorFormat, pending.width, pending.height);
    const uint32_t parity = st.nextParity;
    const auto depthSet = depth_set_for(r, *pending.depth, pending.waterMask, parity);
    if (!framebuffer || !depthSet) return skip("framebuffer/depth view for the world targets");

    auto frame = std::make_shared<FogFrame>();
    frame->grid = grid;
    st.lastGrid[0] = grid.x; st.lastGrid[1] = grid.y; st.lastGrid[2] = grid.z;
    frame->parity = parity;
    st.nextParity ^= 1;
    std::copy(std::begin(camera.camera), std::end(camera.camera), frame->camera);
    const GameSun* game = st.gameSun.ok ? &st.gameSun : nullptr;
    st.lastSunFromGame = game && set.sunFromGame;
    st.lastSunFromDisc = st.lastSunFromGame && set.sunAtSkyDisc && game->skyOk;
    st.lastAmbientFromGame = game && set.ambientFromGame;
    st.lastSunColorFromGame = game && set.sunColorFromGame;
    const bool discFresh = st.seenDiscValid && std::chrono::steady_clock::now() - st.seenDiscTime < std::chrono::minutes(20);
    st.lastSunWeatherScale = 1 - set.weatherSun * weather_overcast(st.weather);
    frame->params = make_params(set, grid, camera, camera_hud_input().fogMode ? camera_hud_input().fogMode - 1 : 0, effective_base_height(st, camera.camera[2]), game,
                                discFresh ? st.seenDisc : nullptr, st.lastSunWeatherScale);
    st.lastSunSource = !set.sunFromGame ? "fog.ini" : set.sunAtSkyDisc && discFresh ? "seen disc" : !game ? "fog.ini (game not read)" :
                       set.sunAtSkyDisc && game->skyOk ? "sky address" : "game light";
    std::copy_n(frame->params.ambient, 3, st.lastAmbient);
    std::copy_n(frame->params.sunDir, 3, st.lastSunToward);
    st.lastSunOn = frame->params.sunDir[3] > 0;
    frame->params.mode[1] = pending.waterMask ? 1u : 0u;
    frame->params.depthRange[3] = set.autoExposure && frame->params.mode[0] == 0 ? st.exposure : 1.f;
    // The game's cascades this frame (or a test's): with game_shadows their light is the fog's sun - the direction of the
    // shadows the ground has, taken from the shared depth row in this frame's view space.
    const FogState::GameCascades* cascades = st.cascadesInjected ? &*st.cascadesInjected :
                                             st.cascades.valid && st.cascades.frame == s.frameSerial ? &st.cascades : nullptr;
    if (set.gameShadows && cascades) {
        float view[3];
        if (fog::cascade_light_view(cascades->rows, view)) {
            float world[3];
            for (int i = 0; i < 3; ++i) world[i] = view[0] * frame->params.right[i] + view[1] * frame->params.up[i] + view[2] * frame->params.forward[i];
            const float l = std::sqrt(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]);
            if (l > 1e-6f) {
                for (int i = 0; i < 3; ++i) frame->params.sunDir[i] = world[i] / l;
                frame->params.sunDir[3] = set.sunEnabled && world[2] > 0 ? 1.f : 0.f;
                st.lastSunSource = "game shadows";
                std::copy_n(frame->params.sunDir, 3, st.lastSunToward);
                st.lastSunOn = frame->params.sunDir[3] > 0;
            }
        }
    }
    // sun_glare / sun_rays: around the disc WoW draws - the seen disc while it is fresh (the fog's sun may be the shadows'
    // direction), else the fog's sun when it is up. The composite samples this frame's depth over it for the visible share;
    // the rays only while the fog's sun is on (not at night, weaker in rain).
    if ((set.sunGlare > 0 || set.sunRays > 0) && (discFresh || frame->params.sunDir[3] > 0)) {
        std::copy_n(discFresh ? st.seenDisc : frame->params.sunDir, 3, frame->params.glare);
        frame->params.glare[3] = set.sunGlare;
        if (frame->params.sunDir[3] > 0) frame->params.rays[0] = set.sunRays * st.lastSunWeatherScale;
    }
    // Scene shading: strengths (only in the final view) and the sun in view space - the cascades' light when they are here
    // (the game's shadows fall that way), else the fog's sun.
    if (frame->params.mode[0] == 0 && (set.contactShadows > 0 || set.ambientOcclusion > 0 || set.terrainShadows > 0 || set.pointShadows > 0)) {
        frame->params.shade[0] = set.contactShadows; frame->params.shade[1] = set.ambientOcclusion; frame->params.shade[2] = set.terrainShadows;
        float view[3]{};
        const FogState::GameCascades* shadeCascades = st.cascadesInjected ? &*st.cascadesInjected :
                                                      st.cascades.valid && st.cascades.frame == s.frameSerial ? &st.cascades : nullptr;
        if (!(shadeCascades && fog::cascade_light_view(shadeCascades->rows, view))) {
            const float* d = frame->params.sunDir;
            for (int a = 0; a < 3; ++a) { const float* axis = a == 0 ? frame->params.right : a == 1 ? frame->params.up : frame->params.forward; view[a] = d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2]; }
        }
        std::copy_n(view, 3, frame->params.shadeLight);
        frame->params.shadeLight[3] = set.terrainShadowsFrom;
        if (frame->params.sunDir[3] <= 0) frame->params.shade[0] = frame->params.shade[2] = 0;   // no sun (night): no sun shadows
        // Indoors (the game drew its sun cascades this session, but not for 30 frames: WMO interiors cast none - the Den):
        // no sun there - no contact / terrain sun shadows, and point lights at full weight.
        const bool indoors = !st.cascadesInjected && st.cascadesLastValid && s.frameSerial > st.cascadesLastValid + 30;
        if (indoors) frame->params.shade[0] = frame->params.shade[2] = 0;
        // Point-light shadows: the 8 lights nearest the camera within 100 yd, in this frame's view space; full weight at night,
        // down to 30% with the sun 30+ degrees up (a torch hardly lights anything at noon).
        if (set.pointShadows > 0) {
            std::vector<std::array<float, 3>> world;
            if (st.pointLightsInjected) world = *st.pointLightsInjected;
            else for (const auto& l : game_lights_snapshot()) world.push_back({l.pos[0], l.pos[1], l.pos[2]});
            std::vector<std::pair<double, std::array<float, 3>>> near;
            for (const auto& w : world) {
                const double d[3]{w[0] - frame->camera[0], w[1] - frame->camera[1], w[2] - frame->camera[2]};
                const double dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist > 100) continue;
                std::array<float, 3> v{};
                for (int a = 0; a < 3; ++a) { const float* axis = a == 0 ? frame->params.right : a == 1 ? frame->params.up : frame->params.forward; v[a] = float(d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2]); }
                near.push_back({dist, v});
            }
            std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            const float sunUp = frame->params.sunDir[3] > 0 && !indoors ? std::clamp((frame->params.sunDir[2] - .05f) / .45f, 0.f, 1.f) : 0.f;
            const size_t n = std::min<size_t>(near.size(), 8);
            for (size_t i = 0; i < n; ++i) { std::copy_n(near[i].second.data(), 3, frame->params.pointLights[i]); frame->params.pointLights[i][3] = 1.f - .7f * sunUp; }
            frame->params.pointShadow[0] = n ? set.pointShadows : 0.f;
            frame->params.pointShadow[1] = set.pointShadowRange;
            frame->params.pointShadow[2] = float(n);
            frame->params.pointShadow[3] = set.pointShadowIgnore;
            frame->params.pointShadowLength[0] = set.pointShadowLength;
        }
    }
    st.lastWaterSurface = pending.waterMask != VkImageView{};
    c.fogFrame = frame;
    ++c.fogComposites;

    const auto& f = r.f;
    const VkImage color = (VkImage)pending.color->handle, depth = (VkImage)pending.depth->handle;
    const auto depthAspect = depth_barrier_aspects(pending.depth->format);
    const uint32_t slot = r.nextComposite++ % compositeSlots, query = computeSlots * computeStamps + slot * 2;
    if (r.queries) {
        f.CmdResetQueryPool(cmd, r.queries, query, 2); f.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, r.queries, query);
        r.composites[slot] = {true, false};
        frame->compositeSlot = int(slot);
    }
    // Sun finder: the game's picture before the fog, at a quarter of the resolution, for the next frame's fog_sun_find.comp.
    if (!r.pictureBlitChecked) {
        r.pictureBlitChecked = true;
        VkFormatProperties src{}, dst{};
        if (data.get_format_properties) { data.get_format_properties(data.physical_device, pending.color->format, &src); data.get_format_properties(data.physical_device, VK_FORMAT_R8G8B8A8_UNORM, &dst); }
        r.pictureBlitOk = (pending.color->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (src.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                          (src.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && (dst.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
        if (!r.pictureBlitOk) log_line("[FOG-SUN] the game's picture cannot be blitted (format " + std::to_string(int(pending.color->format)) + "): no sun disc detection");
    }
    if (r.pictureBlitOk && (set.shadows || set.autoExposure)) {   // the sun finder's and the exposure measurement's picture
        image_barrier(f, cmd, r.picture.image, VK_IMAGE_ASPECT_COLOR_BIT, r.pictureInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        image_barrier(f, cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, pending.colorLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(pending.width), int32_t(pending.height), 1};
        blit.dstOffsets[1] = {int32_t(r.pictureWidth), int32_t(r.pictureHeight), 1};
        f.CmdBlitImage(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.picture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        image_barrier(f, cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.colorLayout,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        image_barrier(f, cmd, r.picture.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
        r.pictureInitialized = true;
        frame->pictureWrittenBy = &r;
    }
    // Fog capture (one frame, nothing shown differently): the frame as the game drew it, the result, our view depth, the
    // memories, the volumes and the visibility volume the light pass writes on this frame only (per froxel r = sun, b = sky).
    std::shared_ptr<FogCapture> capture;
    std::vector<CaptureJob> captureBefore, captureAfter;
    if (st.captureStage == 1 && st.fogFramesOn >= 60) {
        const bool full = true;
        st.captureStage = 4;
        const size_t colorTexel = fog_texel_bytes(pending.color->format);
        const std::string colorName = "vk-format-" + std::to_string(int(pending.color->format));
        const bool colorCopyable = colorTexel && (pending.color->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (!colorCopyable) log_line("[FOG-CAPTURE] the game's color image is not copyable (format " + std::to_string(int(pending.color->format)) + ", usage " + hex_id(pending.color->usage) + ")");
        const uint32_t w = pending.width, h = pending.height;
        if (full && colorCopyable) captureBefore.push_back({"color_before", colorName, color, VK_IMAGE_ASPECT_COLOR_BIT, pending.colorLayout, w, h, 1, colorTexel});
        if (colorCopyable) captureAfter.push_back({full ? "color_after" : "visibility_view", colorName, color, VK_IMAGE_ASPECT_COLOR_BIT, pending.colorLayout, w, h, 1, colorTexel});
        if (full) {
            if (set.shadows) captureAfter.push_back({"view_depth", "r32f", r.shadow, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, r.shadowWidth, r.shadowHeight, 1, 4});
            const uint32_t n = FogResources::memorySize;
            captureAfter.push_back({"height_memory", "r32ui-ordered-float", r.heightMemory.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, n, n, 1, 4});
            captureAfter.push_back({"memory_tags", "r32ui", r.tagMemory.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, n, n, 1, 4});
            captureAfter.push_back({"sun_map", "r32ui-ordered-float", r.sunMemory.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, n, n, 1, 4});
            captureAfter.push_back({"floor_memory", "r32ui-ordered-float", r.floorMemory.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, n, n, 1, 4});
            // The game's cascades as the world sampled them (roadmap M10: tools/cascade-check.py pairs them with this frame's
            // light matrices against the scene's depth). The depth aspect copies as 4 bytes a texel.
            if (cascades) for (int k = 0; k < 4; ++k) if (const auto& im = cascades->images[k]) {
                const char* format = im->format == VK_FORMAT_D24_UNORM_S8_UINT || im->format == VK_FORMAT_X8_D24_UNORM_PACK32 ? "d24-x8" :
                                     im->format == VK_FORMAT_D16_UNORM || im->format == VK_FORMAT_D16_UNORM_S8_UINT ? "d16" : "r32f";
                const size_t texel = std::string(format) == "d16" ? 2 : 4;
                CaptureJob job{"game_cascade_" + std::to_string(k), format, (VkImage)im->handle, VK_IMAGE_ASPECT_DEPTH_BIT, cascades->layouts[k], im->extent.width, im->extent.height, 1, texel};
                job.barrierAspect = depth_barrier_aspects(im->format);
                captureAfter.push_back(job);
            }
            captureAfter.push_back({"medium", "rgba16f", r.medium.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, grid.x, grid.y, grid.z, 8});
            captureAfter.push_back({"integrated", "rgba16f", r.integrated.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, grid.x, grid.y, grid.z, 8});
        }
        captureAfter.push_back({"lighting", "rgba16f", r.lighting[parity].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, grid.x, grid.y, grid.z, 8});
        captureAfter.push_back({"visibility", "rgba8", r.visibilityVolume.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, grid.x, grid.y, grid.z, 4});
        std::vector<CaptureJob> all(captureBefore);
        all.insert(all.end(), captureAfter.begin(), captureAfter.end());
        std::string captureWhy;
        capture = capture_prepare(r, st.resources, st.captureDir, "final", all, captureWhy);
        if (capture) {
            for (size_t i = 0; i < captureBefore.size(); ++i) captureBefore[i].offset = all[i].offset;
            for (size_t i = 0; i < captureAfter.size(); ++i) captureAfter[i].offset = all[captureBefore.size() + i].offset;
            frame->capture = capture;
            for (const auto& j : captureBefore) capture_copy(f, cmd, *capture, j);
        } else {
            log_line("[FOG-CAPTURE] skipped: " + captureWhy);
            st.captureNote = "failed: " + captureWhy;
        }
    }
    // Our compute writes (earlier submission) and the params upload -> fragment reads.
    auto memory = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
    image_barrier(f, cmd, depth, depthAspect, pending.depthLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_barrier(f, cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, pending.colorLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    // Sun shadows: this frame's world depth, reduced, for the next frame's light pass (it read the previous content already:
    // the image barrier orders this write after that earlier submission's reads).
    // The game's cascades reduced into our maps (the next frame's light pass tests the fog against them, in this frame's view
    // space: the same shadow camera as the view depth below). Sampled in the layout the world just sampled them in.
    const bool shadeWantsCascades = set.contactShadows > 0 || set.terrainShadows > 0 || set.pointShadows > 0;   // the added sun shadows skip the game's own; point shadows: where the sun reaches
    if ((set.gameShadows || shadeWantsCascades) && cascades) {
        VkDescriptorSet sources[4]{};
        bool ok = true;
        for (int k = 0; k < 4; ++k) ok = ok && cascades->images[k] && (sources[k] = cascade_set_for(r, *cascades->images[k], cascades->layouts[k]));
        if (ok) {
            auto written = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            written.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 1, &written, 0, nullptr, 0, nullptr);
            image_barrier(f, cmd, r.cascadeMaps, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            const uint32_t n = FogResources::cascadeSize;
            const VkViewport viewport{0, 0, float(n), float(n), 0, 1};
            const VkRect2D area{{0, 0}, {n, n}};
            for (int k = 0; k < 4; ++k) {
                auto begin = vk<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
                begin.renderPass = r.cascadePass; begin.framebuffer = r.cascadeFramebuffers[k]; begin.renderArea = area;
                f.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
                f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.cascadePipeline);
                f.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.compositeLayout, 0, 1, &sources[k], 0, nullptr);
                f.CmdSetViewport(cmd, 0, 1, &viewport);
                f.CmdSetScissor(cmd, 0, 1, &area);
                f.CmdDraw(cmd, 3, 1, 0, 0);
                f.CmdEndRenderPass(cmd);   // -> SHADER_READ_ONLY_OPTIMAL
            }
            frame->cascadesWrittenBy = &r;
            std::memcpy(frame->cascadeRows, cascades->rows, sizeof frame->cascadeRows);
            std::memcpy(frame->params.frameCascade, cascades->rows, sizeof frame->params.frameCascade);   // for fog_shade.frag
            frame->params.shade[3] = 1;
        } else st.cascades.problem = "no view / descriptor set for a cascade map";
    }
    if (set.gameShadows) {
        const std::string state = cascades && frame->cascadesWrittenBy ? "found" : st.cascades.problem;
        if (state != st.cascadesLogged || (cascades && s.frameSerial >= st.cascadesNextLog)) {
            st.cascadesLogged = state;
            st.cascadesNextLog = s.frameSerial + 900;
            if (state != "found") log_line("[FOG-CASCADES] the game's sun cascades not used: " + state);
            else {
                const auto& g = *cascades;
                char b[320];
                std::snprintf(b, sizeof b, "[FOG-CASCADES] 4 maps (%ux%u fmt %d) at bindings %u,%u,%u,%u, %.0f/%.0f/%.0f/%.0f yd wide; their light el %.1f az %.1f is the fog's sun; used in %llu frames",
                              g.images[0]->extent.width, g.images[0]->extent.height, int(g.images[0]->format), g.bindings[0], g.bindings[1], g.bindings[2], g.bindings[3],
                              2 / std::sqrt(g.rows[0][0] * g.rows[0][0] + g.rows[0][1] * g.rows[0][1] + g.rows[0][2] * g.rows[0][2]),
                              2 / std::sqrt(g.rows[3][0] * g.rows[3][0] + g.rows[3][1] * g.rows[3][1] + g.rows[3][2] * g.rows[3][2]),
                              2 / std::sqrt(g.rows[6][0] * g.rows[6][0] + g.rows[6][1] * g.rows[6][1] + g.rows[6][2] * g.rows[6][2]),
                              2 / std::sqrt(g.rows[9][0] * g.rows[9][0] + g.rows[9][1] * g.rows[9][1] + g.rows[9][2] * g.rows[9][2]),
                              std::asin(double(std::clamp(frame->params.sunDir[2], -1.f, 1.f))) * 180 / M_PI,
                              std::atan2(double(frame->params.sunDir[1]), double(frame->params.sunDir[0])) * 180 / M_PI, (unsigned long long)st.cascadeFramesUsed);
                log_line(b);
            }
        }
    }
    if (set.shadows || set.autoExposure || set.gameShadows || set.terrainShadows > 0) {   // the view depth: sun shadows / memories (also terrain_shadows' floor), exposure, the cascades' camera
        image_barrier(f, cmd, r.shadow, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        auto shadowBegin = vk<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        shadowBegin.renderPass = r.shadowPass; shadowBegin.framebuffer = r.shadowFramebuffer; shadowBegin.renderArea = {{0, 0}, {r.shadowWidth, r.shadowHeight}};
        f.CmdBeginRenderPass(cmd, &shadowBegin, VK_SUBPASS_CONTENTS_INLINE);
        f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.shadowPipeline);
        f.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.compositeLayout, 0, 1, &depthSet, 0, nullptr);
        const VkViewport shadowViewport{0, 0, float(r.shadowWidth), float(r.shadowHeight), 0, 1};
        const VkRect2D shadowScissor{{0, 0}, {r.shadowWidth, r.shadowHeight}};
        f.CmdSetViewport(cmd, 0, 1, &shadowViewport);
        f.CmdSetScissor(cmd, 0, 1, &shadowScissor);
        f.CmdDraw(cmd, 3, 1, 0, 0);
        f.CmdEndRenderPass(cmd);   // -> SHADER_READ_ONLY_OPTIMAL; the next compute submission's barrier makes the writes visible
        frame->shadowWrittenBy = &r;
    }
    // Sun rays: the mask, then the march over it, from this frame's depth - for the composite right after.
    if (frame->params.rays[0] > 0) {
        const VkViewport raysViewport{0, 0, float(r.raysWidth), float(r.raysHeight), 0, 1};
        const VkRect2D raysArea{{0, 0}, {r.raysWidth, r.raysHeight}};
        for (auto [target, pass, pipeline] : {std::tuple{&r.raysMask, r.raysMaskPass, r.raysMaskPipeline}, std::tuple{&r.rays, r.raysPass, r.raysPipeline}}) {
            image_barrier(f, cmd, target->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            auto raysBegin = vk<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
            raysBegin.renderPass = pass; raysBegin.framebuffer = target->framebuffer; raysBegin.renderArea = raysArea;
            f.CmdBeginRenderPass(cmd, &raysBegin, VK_SUBPASS_CONTENTS_INLINE);
            f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            f.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.compositeLayout, 0, 1, &depthSet, 0, nullptr);
            f.CmdSetViewport(cmd, 0, 1, &raysViewport);
            f.CmdSetScissor(cmd, 0, 1, &raysArea);
            f.CmdDraw(cmd, 3, 1, 0, 0);
            f.CmdEndRenderPass(cmd);   // -> SHADER_READ_ONLY_OPTIMAL
            auto written = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            written.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &written, 0, nullptr, 0, nullptr);
        }
    }
    // Scene shading (contact shadows, ambient occlusion, terrain shadows): from this frame's depth (and the cascades reduced
    // above), for the composite right after. The sun's direction is the cascades' (the game's shadows), else the fog's.
    if (frame->params.shade[0] > 0 || frame->params.shade[1] > 0 || frame->params.shade[2] > 0 || frame->params.pointShadow[0] > 0) {
        const VkViewport shadeViewport{0, 0, float(r.shadeWidth), float(r.shadeHeight), 0, 1};
        const VkRect2D shadeArea{{0, 0}, {r.shadeWidth, r.shadeHeight}};
        image_barrier(f, cmd, r.shade.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        auto shadeBegin = vk<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        shadeBegin.renderPass = r.shadePass; shadeBegin.framebuffer = r.shade.framebuffer; shadeBegin.renderArea = shadeArea;
        f.CmdBeginRenderPass(cmd, &shadeBegin, VK_SUBPASS_CONTENTS_INLINE);
        f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.shadePipeline);
        f.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.compositeLayout, 0, 1, &depthSet, 0, nullptr);
        f.CmdSetViewport(cmd, 0, 1, &shadeViewport);
        f.CmdSetScissor(cmd, 0, 1, &shadeArea);
        f.CmdDraw(cmd, 3, 1, 0, 0);
        f.CmdEndRenderPass(cmd);   // -> SHADER_READ_ONLY_OPTIMAL
        auto written = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        written.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &written, 0, nullptr, 0, nullptr);
    }
    auto begin = vk<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    begin.renderPass = r.pass; begin.framebuffer = framebuffer; begin.renderArea = {{0, 0}, {pending.width, pending.height}};
    f.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.composite);
    f.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.compositeLayout, 0, 1, &depthSet, 0, nullptr);
    const VkViewport viewport{0, 0, float(pending.width), float(pending.height), 0, 1};
    const VkRect2D scissor{{0, 0}, {pending.width, pending.height}};
    f.CmdSetViewport(cmd, 0, 1, &viewport);
    f.CmdSetScissor(cmd, 0, 1, &scissor);
    f.CmdDraw(cmd, 3, 1, 0, 0);
    f.CmdEndRenderPass(cmd);
    image_barrier(f, cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, pending.colorLayout,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    image_barrier(f, cmd, depth, depthAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, pending.depthLayout,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (r.queries) f.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, r.queries, query + 1);
    if (capture) {
        for (const auto& j : captureAfter) capture_copy(f, cmd, *capture, j);
        auto host = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        f.CmdSetEvent(cmd, capture->event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        st.captures.push_back(capture);
        log_line("[FOG-CAPTURE] recorded " + capture->label + " dir=" + capture->dir + " sections=" + std::to_string(capture->sections.size()));
    }
    restore_after_private_pass(data, cmd, c);
    fog_flush_deferred(data, s, c, cmd, "after fog");   // the held-back particles / glows, over the fogged world

    ++st.compositesThisFrame;
    st.lastTrigger = trigger;
    st.lastProblem.clear();
    ema(st.cpuComposite, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    timeline(st, std::string("fog composite recorded trigger=") + trigger + " color=" + hex_id(pending.color->handle) + " worldPasses=" + std::to_string(pending.worldPasses));
}

bool same_image(const std::shared_ptr<Image>& a, const Image* b) { return a && b && a->handle == b->handle && a->generation == b->generation; }
}

// Frame scan "uniforms" context: the camera of the fog's last computed frame (what the scan's frames were drawn with, give or
// take a frame) and the game's light, for tools/shadow-constants.py to test world- and view-space light matrices.
std::string fog_scan_context(Device& s) {
    if (!s.fog || !s.fog->prevValid) return "";
    const auto& st = *s.fog;
    const auto& f = st.prevFrame;
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(9) << "{\"camera\":[" << f.camera[0] << ',' << f.camera[1] << ',' << f.camera[2] << ']';
    auto v = [&](const char* name, const float* x, int n) { o << ",\"" << name << "\":["; for (int i = 0; i < n; ++i) o << (i ? "," : "") << x[i]; o << ']'; };
    v("right", f.params.right, 3); v("up", f.params.up, 3); v("forward", f.params.forward, 3); v("proj", f.params.proj, 4); v("viewport", f.params.viewport, 4);
    v("depthRange", f.params.depthRange, 2); v("fogSunToward", f.params.sunDir, 3);
    o << ",\"gameSunOk\":" << (st.gameSun.ok ? "true" : "false");
    v("gameLightToward", st.gameSun.toward, 3);
    o << ",\"present\":" << s.frameSerial << "}";
    return o.str();
}

FogState& fog_state(Device& s) {
    if (!s.fog) s.fog = std::make_shared<FogState>();
    return *s.fog;
}
const FogSettings& fog_settings_now(Device& s) {
    auto& st = fog_state(s);
    load_settings(st);   // a stat per call; reads the file only when it changed
    return st.settings;
}
bool fog_enabled() { return camera_hud_input().fogMode != 0; }

// The game's sun cascades (roadmap M10), looked for on world draws until found this frame (at most 8 resolutions a frame, only
// pipelines whose fragment shader has four or more depth-compare bindings): the four large square depth images it samples
// with a comparison, ordered by binding, and its vertex constants c53..c64 (DXVK set 2 binding 0, host-coherent ring) - the
// light matrices, checked (fog::cascade_rows_problem) and sorted finest first.
static void observe_cascades(Device& s, FogState& st, const Command& c) {
    auto& g = st.cascades;
    if (g.frame != s.frameSerial) { g.frame = s.frameSerial; g.attempts = 0; g.valid = false; }
    if (g.valid || g.attempts >= 8) return;
    const auto& p = c.pipeline;
    if (!p || !p->fs || !p->vs) return;
    auto [known, fresh] = st.cascadePipelines.try_emplace(p->serial, false);
    if (fresh) {
        std::vector<std::pair<uint32_t, uint32_t>> drefs;
        for (const auto& b : p->fs->bindings) if (b.dref && std::find(drefs.begin(), drefs.end(), std::pair{b.set, b.binding}) == drefs.end()) drefs.push_back({b.set, b.binding});
        known->second = drefs.size() >= 4;
    }
    if (!known->second) return;
    ++g.attempts;
    // The draw as observe() snapshots it: the push words carry DXVK's sampler indices - without them the samplers stay
    // unresolved and the draw counts as incomplete (Shadowglen: "vertex constants: descriptor-unresolved...").
    SubmittedDraw d;
    d.draw.pipeline = p; d.draw.target = c.target; d.draw.dynamic = c.dynamic; d.draw.sets = c.sets; d.draw.vertices = c.vertices; d.draw.index = c.index;
    d.draw.push = c.push; d.draw.dynamicOffsets = c.dynamicOffsets;
    resolve_resources(s, d);
    std::vector<const ResourceUse*> maps;
    for (const auto& r : d.resources) {
        if (!r.staticDref || !r.descriptor.view || !r.descriptor.view->image) continue;
        const auto& im = *r.descriptor.view->image;
        if (im.extent.width < 1024 || im.extent.width != im.extent.height || !(depth_barrier_aspects(im.format) & VK_IMAGE_ASPECT_DEPTH_BIT)) continue;
        if (std::none_of(maps.begin(), maps.end(), [&](const ResourceUse* m) { return m->descriptor.view->image == r.descriptor.view->image; })) maps.push_back(&r);
    }
    std::sort(maps.begin(), maps.end(), [](const ResourceUse* a, const ResourceUse* b) { return std::pair{a->set, a->binding} < std::pair{b->set, b->binding}; });
    if (maps.size() != 4) { g.problem = std::to_string(maps.size()) + " large depth maps sampled with a comparison (want 4)"; return; }
    const ResourceUse* constants = nullptr;
    for (const auto& r : d.resources)
        if (r.set == 2 && r.binding == 0 && (r.descriptor.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || r.descriptor.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)) constants = &r;
    if (!constants) { g.problem = "no vertex constants at set 2 binding 0"; return; }
    size_t used = 0;
    const auto snap = try_capture_uniform(s, d, *constants, used, size_t(64) << 10, true);
    if (!snap->reason.empty()) { g.problem = "vertex constants: " + snap->reason + " (descriptor issues " + std::to_string(d.descriptorIssues) + ")"; return; }
    if (snap->bytes.size() < 65 * 16) { g.problem = "vertex constants end before c64"; return; }
    float rows[12][4];
    std::memcpy(rows, snap->bytes.data() + 53 * 16, sizeof rows);
    if (const char* why = fog::cascade_rows_problem(rows)) { g.problem = std::string("c53..c64: ") + why; return; }
    int order[4];
    fog::cascade_order(rows, order);
    for (int j = 0; j < 4; ++j) {
        const auto* m = maps[order[j]];
        g.images[j] = m->descriptor.view->image; g.layouts[j] = m->descriptor.layout; g.bindings[j] = m->binding;
        std::memcpy(g.rows[3 * j], rows[3 * order[j]], sizeof(float) * 12);
    }
    g.valid = true;
    st.cascadesLastValid = s.frameSerial;
    g.problem.clear();
}

void fog_observe_draw(Device& s, Command& c) {
    if (!fog_enabled() && !(s.fog && s.fog->timelineFrames)) return;
    if (!(c.dynamic.valid & Viewport)) { ++c.fogOtherDraws; return; }
    const auto& v = c.dynamic.viewport;
    if (v.minDepth == 0.f && v.maxDepth > .5f && v.maxDepth < .999f) {
        ++c.fogWorldDraws;
        if (fog_enabled() && s.fog && (s.fog->settings.gameShadows || s.fog->settings.contactShadows > 0 || s.fog->settings.terrainShadows > 0 || s.fog->settings.pointShadows > 0) &&
            !s.fog->cascadesInjected) observe_cascades(s, *s.fog, c);
    }
    else if (v.minDepth >= .99f) ++c.fogSkyDraws;
    else ++c.fogOtherDraws;
}
void fog_pass_begin(Command& c) { c.fogWorldDraws = c.fogSkyDraws = c.fogOtherDraws = 0; }

void fog_pass_end(const DeviceData&, Device& s, Command& c, VkCommandBuffer cmd) {
    if (!s.fog && !fog_enabled()) return;
    auto& st = fog_state(s);
    const Image* color = !c.target.colors.empty() && c.target.colors[0] ? c.target.colors[0]->image.get() : nullptr;
    const Image* depth = c.target.depth ? c.target.depth->image.get() : nullptr;
    if (st.timelineFrames)
        timeline(st, "frame=" + std::to_string(s.frameSerial) + " cmd=" + hex_id(id(cmd)) + " pass=" + std::to_string(st.timelinePass++) + " color=" +
                 (color ? hex_id(color->handle) + "(" + image_kind(color) + " " + std::to_string(color->extent.width) + "x" + std::to_string(color->extent.height) + " fmt" +
                          std::to_string(int(color->format)) + ")" : std::string("none")) +
                 " depth=" + (depth ? hex_id(depth->handle) + (depth->handle == st.sceneDepthImage ? "(SCENE)" : "(other)") : std::string("none")) +
                 " draws world=" + std::to_string(c.fogWorldDraws) + " sky=" + std::to_string(c.fogSkyDraws) + " other=" + std::to_string(c.fogOtherDraws));
    if (!fog_enabled()) return;
    const bool worldTargets = color && depth && depth->handle == st.sceneDepthImage && st.sceneDepthImage;
    if (st.frameTrace.size() < 40)
        st.frameTrace.push_back("pass color=" + (color ? hex_id(color->handle) : std::string("none")) + (depth && depth->handle == st.sceneDepthImage ? " SCENE" : " -") +
                                " world=" + std::to_string(c.fogWorldDraws) + " sky=" + std::to_string(c.fogSkyDraws) + " other=" + std::to_string(c.fogOtherDraws) +
                                (st.pending.active ? " (fog pending)" : ""));
    if (st.pending.active && same_image(st.pending.color, color) && same_image(st.pending.depth, depth) && c.fogWorldDraws == 0 && c.fogOtherDraws > 0) {
        // Something that is not world geometry (a full-depth-range viewport: the UI) was drawn on the world's own targets:
        // the UI shares them. Never fog it. (Sky-only passes are the world continuing: a split between the world and the sky
        // draws used to land here and drop the fog for that frame - flicker.)
        st.pending = {};
        ++st.droppedThisFrame;
        const std::string why = "non-world draws on the world targets (UI shares them) - fog dropped, UI protected";
        if (st.lastProblem != why) log_line("[FOG] " + why);
        st.lastProblem = why;
        if (st.dropsTraced < 12) {
            ++st.dropsTraced;
            std::string trace;
            for (const auto& line : st.frameTrace) trace += "\n    " + line;
            log_line("[FOG-DROP] frame " + std::to_string(s.frameSerial) + " passes so far:" + trace);
        }
        return;
    }
    if (!worldTargets || c.fogWorldDraws + c.fogSkyDraws == 0) return;
    VkImageLayout colorLayout = VK_IMAGE_LAYOUT_UNDEFINED, depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!c.pass && c.target.renderingLayoutsKnown) { colorLayout = c.target.renderingColorLayout; depthLayout = c.target.renderingDepthLayout; }
    else if (c.pass && c.pass->supported) {
        for (size_t i = 0; i < c.attachments.size() && i < c.pass->attachments.size(); ++i) if (auto v = c.attachments[i]) {
            if (v->image.get() == color) colorLayout = c.pass->attachments[i].finalLayout;
            if (v->image.get() == depth) depthLayout = c.pass->attachments[i].finalLayout;
        }
    }
    auto& p = st.pending;
    if (!p.active) { p = {}; p.active = true; p.color = c.target.colors[0]->image; p.depth = c.target.depth->image; p.frame = s.frameSerial; }
    p.colorViewFormat = c.target.colors[0]->format != VK_FORMAT_UNDEFINED ? c.target.colors[0]->format : color->format;
    // The water replay records at this same pass end, just before us: a new mask on these targets belongs to this world.
    if (s.water && s.water->maskSerial != st.waterSerialSeen) {
        st.waterSerialSeen = s.water->maskSerial;
        if (s.water->maskColorImage == color->handle && s.water->maskView) p.waterMask = s.water->maskView;
    }
    p.colorLayout = colorLayout; p.depthLayout = depthLayout;
    p.layoutsKnown = colorLayout != VK_IMAGE_LAYOUT_UNDEFINED && depthLayout != VK_IMAGE_LAYOUT_UNDEFINED;
    p.width = c.target.width ? c.target.width : color->extent.width;
    p.height = c.target.height ? c.target.height : color->extent.height;
    ++p.worldPasses;
    ++st.worldPassesThisFrame;
}

void fog_before_pass_begin(const DeviceData& data, Device& s, Command& c, VkCommandBuffer cmd, const Image* color, const Image* depth) {
    if (!s.fog || !s.fog->pending.active) return;
    auto& p = s.fog->pending;
    if (same_image(p.color, color) && same_image(p.depth, depth)) return; // the world continues (split pass)
    record_composite(data, s, c, cmd, "next pass on other targets");
}

void fog_before_transfer(const DeviceData& data, Device& s, Command& c, VkCommandBuffer cmd, VkImage a, VkImage b, const char* what) {
    if (!s.fog || !s.fog->pending.active) return;
    const auto& p = s.fog->pending;
    auto touches = [&](VkImage x) { return x && ((p.color && id(x) == p.color->handle) || (p.depth && id(x) == p.depth->handle)); };
    if (touches(a) || touches(b)) record_composite(data, s, c, cmd, what);
}

// ---- transparent-aware fog (roadmap milestone 3) ---------------------------------------------------------------
// The composite applies the fog with the scene depth. A blended draw that does not write depth (a particle, a glow, an M2's
// alpha batch) has no depth of its own there: it took the fog of the surface BEHIND it (a torch against the sky was fogged
// as if it were at the end of the volume). Frame scans (docs/draw-classes.md) showed everything else is right already:
// opaque and depth-writing blended draws (foliage, WMO glass) leave their own depth, water has its mask. So only those draws
// are held back from the world pass and issued again, unchanged, right after the composite in the same command buffer (the
// same buffers and descriptor sets are still valid there; across command buffers they may not be). Not fogged by the volume
// then - right for near ones, a little bright for far ones. Layers drawn on the geometry of the draw before them (EQUAL test
// or the same index range: WoW's second material passes) keep their place: they ARE at the depth of that surface.
//
// Water (2026-10-08): the same draws after the selected water in a pass are held too, fog or not. The water composite (at the pass end)
// undoes the game's blend from the picture; an effect already blended over the water there (a fishing pool's swirl) was taken for seabed,
// amplified by 1 / (1 - alpha), and came out a purple ring (session 39664). Held, they are issued right after the water composite
// (discovery_commands.cpp clear_target) or, with transparent_fog, after the fog's.
bool fog_defer_candidate(Device& s, const Command& c, const DrawSemantic& sem, const DrawFacts& f, uint32_t kind, uint32_t count, uint32_t first, int32_t vertexOffset) {
    const bool forFog = s.fog && s.fog->settings.transparentFog && fog_enabled();
    const bool forWater = c.waterSeenInPass && s.water && s.water->mode && s.water->selectedVS;
    if ((!forFog && !forWater) || c.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) return false;
    if (s.water && s.water->selectedVS && c.pipeline && water_draw_shaders(*s.water, *c.pipeline)) return false;   // the water tiles themselves stay in place
    // Over the water the pass is the water's own (its target): the scene-depth identification is not needed there.
    if (!(f.sceneTarget || forWater) || !(sem.evidence & EvWorldRange)) return false;
    if (sem.cls != DrawClass::Transparent && sem.cls != DrawClass::Additive) return false;
    if (!f.depthTestKnown || !f.depthTest || !f.depthWriteKnown || f.depthWrite) return false;
    auto& st = fog_state(s);
    const auto& g = c.lastGeometry;
    const bool sameGeometry = g.valid && g.kind == kind && g.first == first && g.count == count && g.vertexOffset == vertexOffset &&
                              g.index == (kind == 1 ? c.index.buffer : 0) && g.vertex0 == c.vertices[0].buffer && g.vertex0Offset == c.vertices[0].offset;
    if (sameGeometry || (f.compareKnown && f.depthCompare == VK_COMPARE_OP_EQUAL)) { ++st.deferLayers; return false; }
    if (s.isolation || (s.inspector && (s.inspector->armed || s.inspector->highlight))) return false;   // their draw-order tools see the original order
    if (c.fogDeferred.size() >= 512) { ++st.deferRejected["held-draw budget (512)"]; return false; }
    return true;
}
bool fog_defer_draw(Device& s, Command& c, const std::shared_ptr<Draw>& draw) {
    auto& st = fog_state(s);
    if (const char* why = app_draw_replay_problem(s, c, *draw)) { ++st.deferRejected[why]; return false; }
    if (!c.fogDeferred.empty()) {   // one rendering instance re-creates them all: same attachments
        const auto& t = c.fogDeferred.front().draw->target;
        if (t.colors[0] != draw->target.colors[0] || t.depth != draw->target.depth) { ++st.deferRejected["other target than the held draws"]; return false; }
    }
    draw->fogDeferred = true;
    c.fogDeferred.push_back({draw, c.waterRestore});
    ++st.deferred;
    return true;
}
bool fog_deferred_target(const Command& c, uint64_t image) {
    if (c.fogDeferred.empty() || !image) return false;
    const auto& t = c.fogDeferred.front().draw->target;
    return (t.colors[0]->image && t.colors[0]->image->handle == image) || (t.depth->image && t.depth->image->handle == image);
}
void fog_flush_deferred(const DeviceData& data, Device& s, Command& c, VkCommandBuffer cmd, const char* why, bool endOfCommand) {
    if (c.fogDeferred.empty()) return;
    auto& st = fog_state(s);
    const bool restorable = private_pass_restore_problem(s, c).empty();
    if (!restorable && !endOfCommand) {   // issuing them now would leave the application's following draws with our bindings
        if (!c.fogDeferPostponed) { c.fogDeferPostponed = true; ++st.deferPostponed; }
        return;
    }
    auto begin = next<PFN_vkCmdBeginRendering>(data, "vkCmdBeginRendering");
    auto end = next<PFN_vkCmdEndRendering>(data, "vkCmdEndRendering");
    if (!begin || !end) { begin = next<PFN_vkCmdBeginRendering>(data, "vkCmdBeginRenderingKHR"); end = next<PFN_vkCmdEndRendering>(data, "vkCmdEndRenderingKHR"); }
    auto barrier = next<PFN_vkCmdPipelineBarrier>(data, "vkCmdPipelineBarrier");
    const auto& t = c.fogDeferred.front().draw->target;
    const VkPipelineStageFlags attachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags attachmentAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (begin && end && barrier) {
        // The world pass (or the composite) wrote these attachments; nothing changed their layouts since (every barrier or
        // transfer on them flushes first), so the instance begins with the world's own layouts and keeps what is there.
        auto before = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        before.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; before.dstAccessMask = attachmentAccess;
        barrier(cmd, attachmentStages, attachmentStages, 0, 1, &before, 0, nullptr, 0, nullptr);
        auto color = vk<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO), depth = color, stencil = color;
        color.imageView = (VkImageView)t.colors[0]->handle; color.imageLayout = t.renderingColorLayout;
        depth.imageView = (VkImageView)t.depth->handle; depth.imageLayout = t.renderingDepthLayout;
        stencil.imageView = depth.imageView; stencil.imageLayout = t.renderingStencilLayout;
        for (auto* a : {&color, &depth, &stencil}) { a->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a->storeOp = VK_ATTACHMENT_STORE_OP_STORE; }
        auto info = vk<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
        info.renderArea = t.renderingArea; info.layerCount = 1; info.colorAttachmentCount = 1; info.pColorAttachments = &color;
        info.pDepthAttachment = &depth; info.pStencilAttachment = t.renderingStencil ? &stencil : nullptr;
        begin(cmd, &info);
        for (const auto& d : c.fogDeferred) replay_app_draw(data, cmd, *d.draw, d.state);
        end(cmd);
        auto after = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier(cmd, attachmentStages, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
        if (restorable) restore_after_private_pass(data, cmd, c);
        (std::strcmp(why, "after fog") == 0 ? st.deferredAfterFog : std::strcmp(why, "after water") == 0 ? st.deferredAfterWater : st.deferredFallback) += c.fogDeferred.size();
    } else ++st.deferRejected["no vkCmdBeginRendering downstream (draws lost)"];
    c.fogDeferred.clear();
    c.fogDeferPostponed = false;
}

void fog_layout_event(Device& s, const Image& image, const VkImageSubresourceRange& range, VkImageLayout layout, bool known) {
    if (!s.fog || !s.fog->pending.active) return;
    auto& p = s.fog->pending;
    const bool isColor = p.color && p.color->handle == image.handle, isDepth = p.depth && p.depth->handle == image.handle;
    if (!isColor && !isDepth) return;
    const bool whole = range.baseMipLevel == 0 && range.baseArrayLayer == 0 && (range.levelCount == VK_REMAINING_MIP_LEVELS || range.levelCount >= image.mips) &&
                       (range.layerCount == VK_REMAINING_ARRAY_LAYERS || range.layerCount >= image.layers);
    if (!known || !whole) { p.layoutsKnown = false; return; }
    (isColor ? p.colorLayout : p.depthLayout) = layout;
}

// Jitter, history weight and the previous camera for fog_light.comp. History is dropped (weight 0) when it cannot be
// reprojected meaningfully: first frame, the previous volume is not the other parity (a compute was skipped), grid or
// settings changed, or a camera cut (large jump/turn, FOV change). The volume holds no geometry, so moving characters
// cannot ghost it; what history smooths is the jittered sampling of the medium/lighting inside each froxel.
static double halton(uint32_t index, uint32_t base) {
    double f = 1, r = 0;
    for (uint32_t i = index; i; i /= base) { f /= base; r += f * (i % base); }
    return r;
}
static FogParams temporal_params(FogState& st, const FogFrame& frame) {
    FogParams p = frame.params;
    const float weight = st.settings.temporalWeight;
    for (int i = 0; i < 3; ++i) p.jitter[i] = .5f;
    p.jitter[3] = 0;
    st.lastHistoryUsed = false;
    st.lastHistoryWeight = 0;
    float turnDegrees = 0, stepYards = 0;
    if (weight <= 0) { st.lastReset = "temporal off"; return p; }
    const uint32_t n = ++st.jitterIndex % 1024 + 1;
    p.jitter[0] = float(halton(n, 2)); p.jitter[1] = float(halton(n, 3)); p.jitter[2] = float(halton(n, 5));
    const auto& prev = st.prevFrame;
    std::string reset;
    if (!st.prevValid) reset = "no previous frame";
    else if (prev.parity == frame.parity || st.lastComputedParity == frame.parity) reset = "previous volume not available (skipped compute)";
    else if (prev.grid.x != frame.grid.x || prev.grid.y != frame.grid.y || prev.grid.z != frame.grid.z || prev.grid.maxDistance != frame.grid.maxDistance ||
             prev.grid.distribution != frame.grid.distribution) reset = "grid changed";
    else if (st.prevSettingsGeneration != st.settingsGeneration) reset = "settings changed";
    else if (prev.params.mode[0] != frame.params.mode[0]) reset = "debug view changed";
    else {
        const double dx = frame.camera[0] - prev.camera[0], dy = frame.camera[1] - prev.camera[1], dz = frame.camera[2] - prev.camera[2];
        const float cosTurn = p.forward[0] * prev.params.forward[0] + p.forward[1] * prev.params.forward[1] + p.forward[2] * prev.params.forward[2];
        turnDegrees = std::acos(std::clamp(cosTurn, -1.f, 1.f)) * 180.f / 3.14159265f;
        stepYards = float(std::sqrt(dx * dx + dy * dy + dz * dz));
        if (dx * dx + dy * dy + dz * dz > 8.0 * 8.0) reset = "camera cut: moved over 8 yd in a frame";
        else if (cosTurn < std::cos(20.f * 3.14159265f / 180.f)) reset = "camera cut: turned over 20 deg in a frame";
        else if (std::fabs(p.proj[1] - prev.params.proj[1]) > .01f * p.proj[1]) reset = "FOV changed";
        p.prevOffset[0] = float(dx); p.prevOffset[1] = float(dy); p.prevOffset[2] = float(dz);
    }
    if (!reset.empty()) {
        if (reset != st.lastReset) ++st.historyResets;
        st.lastReset = reset;
        return p;
    }
    std::copy_n(prev.params.right, 3, p.prevRight); p.prevRight[3] = prev.params.proj[0];
    std::copy_n(prev.params.up, 3, p.prevUp); p.prevUp[3] = prev.params.proj[1];
    std::copy_n(prev.params.forward, 3, p.prevForward);
    p.jitter[3] = fog::motion_history_weight(weight, st.settings.temporalMoving, turnDegrees, stepYards);
    st.lastHistoryUsed = true;
    st.lastHistoryWeight = p.jitter[3];
    st.lastReset = "none";
    return p;
}

// Shadows for this frame's light pass. The shadow image holds the depth written by the previous computed frame's composite
// (that composite was in the batch submitted right after that frame's compute), so the sun march projects into that frame's
// view and the height memory is fed from it. Returns the memory passes to dispatch: 0 none, 1 clear/decay, 2 also scatter.
static int shadow_params(FogState& st, const FogResources& r, const FogFrame& frame, FogParams& p) {
    p.shadowImage[0] = float(r.shadowWidth); p.shadowImage[1] = float(r.shadowHeight); p.shadowImage[2] = float(FogResources::shadowFactor);
    p.shadowForward[3] = 0; p.shadowRight[3] = 0;
    p.memoryB[0] = p.memoryB[1] = p.memoryB[3] = 0; p.memoryB[2] = FogResources::memorySize;
    p.memoryA[3] = FogResources::memoryCell;
    p.memoryC[0] = 0; p.memoryC[1] = st.settings.shadows ? st.settings.skyLight : 0.f; p.memoryC[3] = st.settings.openSun;
    st.lastShadowProblem.clear();
    // The camera of the previous frame's view depth (also read by the exposure measurement with the shadows off; the light
    // pass marches only when shadowForward.w, set below, is above 0).
    const auto& prev = st.prevFrame;
    const bool haveDepth = st.prevValid && prev.shadowWrittenBy == &r;
    if (haveDepth) {
        std::copy_n(prev.params.right, 3, p.shadowRight); p.shadowRight[3] = prev.params.proj[0];
        std::copy_n(prev.params.up, 3, p.shadowUp); p.shadowUp[3] = prev.params.proj[1];
        std::copy_n(prev.params.forward, 3, p.shadowForward);
        for (int i = 0; i < 3; ++i) p.shadowOffset[i] = float(frame.camera[i] - prev.camera[i]);
        std::copy_n(prev.params.viewport, 4, p.shadowViewport);
    }
    // The memories also run for terrain_shadows alone: its land test reads the floor memory (fog_shade.frag); the fog's own
    // sun shadows (shadowForward.w, the sun map) stay off then.
    if (!st.settings.shadows && !(st.settings.terrainShadows > 0)) { st.lastShadowProblem = "off"; st.memoryValid = false; return 0; }
    // Height memory around this camera (cells of memoryCell yards, the camera's cell and its fractional position in double).
    const double cell = FogResources::memoryCell;
    const double cx = std::floor(frame.camera[0] / cell), cy = std::floor(frame.camera[1] / cell);
    p.memoryB[0] = int32_t(cx); p.memoryB[1] = int32_t(cy);
    p.memoryA[0] = float(frame.camera[0] / cell - cx); p.memoryA[1] = float(frame.camera[1] / cell - cy); p.memoryA[2] = float(frame.camera[2]);
    const auto now = std::chrono::steady_clock::now();
    int flags = 1;
    const double dx = frame.camera[0] - st.memoryCamera[0], dy = frame.camera[1] - st.memoryCamera[1], dz = frame.camera[2] - st.memoryCamera[2];
    if (!st.memoryValid || dx * dx + dy * dy + dz * dz > 200.0 * 200.0) {
        flags |= 2; ++st.memoryClears;
        // Teleported: another zone's sky and ground - found again there (discEpoch drops searches made before the jump).
        if (st.memoryValid) { st.seenDiscValid = false; st.discTrack.reset(); st.groundValid = false; st.groundBasisCells = 0; st.waterValid = false; ++st.discEpoch; }
    }
    else p.memoryC[0] = float(std::min(std::chrono::duration<double>(now - st.memoryTime).count(), .5) * .05);   // sinks .05 yd/s
    if (haveDepth) flags |= 4;
    p.memoryB[3] = flags;
    st.memoryValid = true; st.memoryTime = now;
    std::copy_n(frame.camera, 3, st.memoryCamera);
    if (!st.settings.shadows) st.lastShadowProblem = "off";
    else if (frame.params.sunDir[3] <= 0) st.lastShadowProblem = "no sun";
    else {
        p.shadowForward[3] = st.settings.shadowLength;
        // Sun map basis (double, from the direction this frame lights with): kept within half a degree, else new + cleared.
        double s[3]{frame.params.sunDir[0], frame.params.sunDir[1], frame.params.sunDir[2]};
        const double sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        for (auto& x : s) x /= sl;
        const double* b = st.sunBasis[2];
        int sunFlags = 1;
        if (!st.sunBasisValid || s[0] * b[0] + s[1] * b[1] + s[2] * b[2] < std::cos(.5 * M_PI / 180) || (flags & 2)) {
            // u = horizontal, across the sun's azimuth (s is never vertical in WoW: its elevation stays below ~60 degrees)
            double u[3]{-s[1], s[0], 0};
            const double ul = std::sqrt(u[0] * u[0] + u[1] * u[1]);
            if (ul < 1e-6) { u[0] = 1; u[1] = 0; } else { u[0] /= ul; u[1] /= ul; }
            const double v[3]{s[1] * u[2] - s[2] * u[1], s[2] * u[0] - s[0] * u[2], s[0] * u[1] - s[1] * u[0]};
            for (int i = 0; i < 3; ++i) { st.sunBasis[0][i] = u[i]; st.sunBasis[1][i] = v[i]; st.sunBasis[2][i] = s[i]; }
            st.sunBasisValid = true;
            sunFlags |= 2;
            ++st.sunMapClears;
            ++st.discStats.sunMapClears;
        }
        const double cu = (frame.camera[0] * st.sunBasis[0][0] + frame.camera[1] * st.sunBasis[0][1] + frame.camera[2] * st.sunBasis[0][2]) / cell;
        const double cv = (frame.camera[0] * st.sunBasis[1][0] + frame.camera[1] * st.sunBasis[1][1] + frame.camera[2] * st.sunBasis[1][2]) / cell;
        const double cs = frame.camera[0] * st.sunBasis[2][0] + frame.camera[1] * st.sunBasis[2][1] + frame.camera[2] * st.sunBasis[2][2];
        p.sunMapB[0] = int32_t(std::floor(cu)); p.sunMapB[1] = int32_t(std::floor(cv)); p.sunMapB[2] = FogResources::memorySize;
        p.sunMapA[0] = float(cu - std::floor(cu)); p.sunMapA[1] = float(cv - std::floor(cv)); p.sunMapA[2] = float(cs); p.sunMapA[3] = float(cell);
        for (int i = 0; i < 3; ++i) { p.sunU[i] = float(st.sunBasis[0][i]); p.sunV[i] = float(st.sunBasis[1][i]); p.sunS[i] = float(st.sunBasis[2][i]); }
        if (haveDepth) sunFlags |= 4;
        p.sunMapB[3] = sunFlags;
    }
    if (!haveDepth) st.lastShadowProblem = st.lastShadowProblem.empty() ? "waiting for a depth" : st.lastShadowProblem;
    return haveDepth ? 2 : 1;
}

// The sun finder's counters for one submission: a compact cluster of disc-coloured sky texels is the disc the game draws;
// its centre goes through the camera of the picture that was searched (the previous frame's) to a world direction, onto the
// game light's azimuth arc, and through the temporal filter (fog_math.hpp: DiscTrack) into the fog sun.
static void sun_found(FogState& st, const uint32_t* all, const float (&cam)[4][4], const float (&arc)[4], uint32_t width, uint32_t height, uint64_t frame) {
    // A compact cluster from the white set, else from the warm set (fog_sun_find.comp).
    const uint32_t sky = all[5];
    fog::DiscCluster found;
    const char* kind = nullptr;
    const char* problem = nullptr;
    for (uint32_t base : {0u, 8u}) {
        const uint32_t* w = all + base;
        if (w[0] < 4) continue;                                        // nothing of this colour
        fog::DiscCluster c;
        c.count = w[0]; c.sky = sky; c.width = width; c.height = height;
        c.meanX = double(w[1]) / c.count; c.meanY = double(w[2]) / c.count;
        c.spreadX = std::sqrt(std::max(0.0, double(w[3]) / c.count - c.meanX * c.meanX));
        c.spreadY = std::sqrt(std::max(0.0, double(w[4]) / c.count - c.meanY * c.meanY));
        if (const char* why = fog::disc_cluster_problem(c)) { if (!problem) problem = why; continue; }
        found = c; kind = base ? "warm" : "white";
        break;
    }
    if (!kind) { if (problem) ++st.discStats.rejected[problem]; return; }
    const double px = (found.meanX + .5) * 4, py = (found.meanY + .5) * 4;
    const float* vp = cam[3];
    const double nx = (px - vp[0]) / vp[2] * 2 - 1, ny = (py - vp[1]) / vp[3] * 2 - 1;
    const double rx = nx / cam[0][3], ry = ny / cam[1][3];
    double d[3];
    for (int i = 0; i < 3; ++i) d[i] = cam[0][i] * rx + cam[1][i] * ry + cam[2][i];
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (auto& x : d) x /= l;
    if (d[2] < std::sin(3 * M_PI / 180)) { ++st.discStats.rejected["near the horizon"]; return; }
    const bool whole = fog::disc_whole(found.count, st.settings.sunGlareRadius, cam[0][3], width, 1 / l);   // 1 / l: cos to the view axis
    if (arc[2] > 0) fog::disc_onto_arc(arc, d);
    const auto step = st.discTrack.add(d, whole);
    if (step == fog::DiscTrack::Partial) { ++st.discStats.rejected["part of a disc away from the fog sun"]; return; }
    if (step == fog::DiscTrack::Waiting) { ++st.discStats.waiting; return; }
    if (step == fog::DiscTrack::Moved) ++st.discStats.moved;
    for (int i = 0; i < 3; ++i) st.seenDisc[i] = float(st.discTrack.dir[i]);
    st.seenDiscValid = true;
    st.seenDiscTime = std::chrono::steady_clock::now();
    st.seenDiscPixel[0] = float(px); st.seenDiscPixel[1] = float(py);
    st.seenDiscTexels = found.count;
    ++st.seenDiscCount;
    if (frame >= st.seenDiscNextLog) {
        st.seenDiscNextLog = frame + 900;
        char b[320];
        std::snprintf(b, sizeof b, "[FOG-SUN] %s disc seen at pixel %.0f,%.0f (%u texels%s, spread %.1f/%.1f) -> toward %.4f %.4f %.4f elevation %.2f azimuth %.2f%s",
                      kind, px, py, found.count, whole ? "" : ", part of it", found.spreadX, found.spreadY, double(st.seenDisc[0]), double(st.seenDisc[1]), double(st.seenDisc[2]),
                      std::asin(double(st.seenDisc[2])) * 180 / M_PI, std::atan2(double(st.seenDisc[1]), double(st.seenDisc[0])) * 180 / M_PI,
                      arc[2] > 0 ? " (on the light's arc)" : " (whole sky)");
        auto& n = st.discStats;
        std::string rejected;
        for (const auto& [why, count] : n.rejected) rejected += (rejected.empty() ? "" : ", ") + std::to_string(count) + " " + why;
        log_line(std::string(b) + " | since the last log: fog sun moved " + std::to_string(n.moved) + "x, sightings waiting for confirmation " +
                 std::to_string(n.waiting) + ", rejected " + (rejected.empty() ? "none" : rejected) + ", searches skipped while turning " +
                 std::to_string(n.turning) + ", sun map clears " + std::to_string(n.sunMapClears));
        n = {};
    }
}

// The exposure histogram of one submission (fog_exposure.comp): the key of its bright part, the exposure that brings it to the
// target, and the adapted exposure (~0.4 s to darken, ~1.5 s back - the eye adapts to a glare faster than back to the dark).
static void exposure_found(FogState& st, const uint32_t* bins, uint64_t frame) {
    float key = 0;
    if (!fog::exposure_key(bins, exposureBins, -8.f, .125f, .4f, .98f, key)) return;
    const auto& s = st.settings;
    const float goal = fog::exposure_for(key, s.exposureTarget, s.exposureMin);
    const auto now = std::chrono::steady_clock::now();
    if (!st.exposureValid) st.exposure = goal;
    else {
        const double dt = std::min(std::chrono::duration<double>(now - st.exposureTime).count(), 1.0);
        st.exposure += float((goal - st.exposure) * (1.0 - std::exp(-dt / (goal < st.exposure ? .4 : 1.5))));
    }
    st.exposureValid = true;
    st.exposureTime = now;
    st.exposureKey = key;
    st.exposureGoal = goal;
    ++st.exposureReadings;
    if (frame >= st.exposureNextLog) {
        st.exposureNextLog = frame + 900;
        char b[160];
        std::snprintf(b, sizeof b, "[FOG-EXPOSURE] bright key %.3f (target %.2f) -> exposure goal %.2f, applied %.2f", double(key), double(s.exposureTarget), double(goal), double(st.exposure));
        log_line(b);
    }
}

// The ground histogram of one submission (fog_ground.comp): the area's ground level is its 20th percentile (the land and the
// water surface; canopy tops and roofs seen from above sit higher), followed with a ~3 s time constant. Floor cells under the
// area's water level count as the surface: the seabed a frame without the water mask showed stays in the floor memory
// (Booty Bay, 2026-09-29: the ground sank from -3 to -11 yd and the fog thinned over the sea).
static void ground_found(FogState& st, const uint32_t* found, const uint32_t* water, const double camera[3], uint64_t frame) {
    // The water level: the lowest bin with a fair share of the water surface's pixels (the sea under a river or a waterfall;
    // a pixel's height is never below its surface). Kept while the camera stays within 128 yd of where water was last seen -
    // the frames without the mask see none.
    uint32_t waterPixels = 0;
    for (uint32_t i = 0; i < waterBins; ++i) waterPixels += water[i];
    st.waterPixels = waterPixels;
    const double wx = camera[0] - st.waterSeen[0], wy = camera[1] - st.waterSeen[1];
    if (waterPixels >= 64) {
        for (uint32_t i = 0; i < waterBins; ++i)
            if (uint64_t(water[i]) * 50 >= waterPixels) {
                st.waterZ = camera[2] - 128.0 + i + .5; st.waterValid = true; st.waterSeen[0] = camera[0]; st.waterSeen[1] = camera[1];
                break;
            }
    } else if (st.waterValid && wx * wx + wy * wy > 128.0 * 128.0) st.waterValid = false;
    const double cameraZ = camera[2];
    uint32_t bins[groundBins];
    std::copy_n(found, groundBins, bins);
    if (st.waterValid) {
        const double top = std::floor(st.waterZ - cameraZ + 128.0);
        if (top > 0 && top < groundBins)
            for (uint32_t i = 0; i < uint32_t(top); ++i) { bins[uint32_t(top)] += bins[i]; bins[i] = 0; }
    }
    uint32_t cells = 0;
    for (uint32_t i = 0; i < groundBins; ++i) cells += bins[i];
    st.groundCells = cells;
    float level = 0;
    // The first reading needs a fair part of the area (Shadowglen: from 208 cells right after a reset it was 15 yd too high and
    // took ~10 s to settle); a reading from far more cells than the estimate rests on replaces it at once.
    if (!fog::ground_level(bins, groundBins, -128.f, 1.f, .2f, st.groundValid ? 200 : 1000, level)) return;
    const double reading = cameraZ + level;
    const auto now = std::chrono::steady_clock::now();
    if (!st.groundValid || cells > 3 * st.groundBasisCells) { st.groundZ = reading; st.groundBasisCells = cells; }
    else {
        const double dt = std::min(std::chrono::duration<double>(now - st.groundTime).count(), 1.0);
        st.groundZ += (reading - st.groundZ) * (1.0 - std::exp(-dt / 3.0));
    }
    st.groundValid = true;
    st.groundBasisCells = std::max(st.groundBasisCells, cells);
    st.groundTime = now;
    st.groundReading = float(reading);
    ++st.groundReadings;
    if (frame >= st.groundNextLog) {
        st.groundNextLog = frame + 900;
        char b[260], w[80] = "no water level";
        if (st.waterValid) std::snprintf(w, sizeof w, "water level z %.1f (%u surface pixels this reading)", st.waterZ, waterPixels);
        std::snprintf(b, sizeof b, "[FOG-GROUND] ground z %.1f (reading %.1f from %u floor cells within 128 yd), %s, camera z %.1f, fog base z %.1f",
                      st.groundZ, reading, cells, w, cameraZ, double(st.lastBaseHeight));
        log_line(b);
    }
}

// Fog capture: the parameters the GPU really used (temporal and shadow fields are only final at submit).
static void capture_write_params(const FogState& st, const FogFrame& frame, const FogParams& p) {
    std::ofstream o(std::filesystem::path(frame.capture->dir) / (frame.capture->label + "_params.json"));
    auto a = [&](const char* name, const auto* v, int n, bool last = false) {
        o << "  \"" << name << "\": [";
        for (int i = 0; i < n; ++i) o << (i ? ", " : "") << v[i];
        o << "]" << (last ? "\n" : ",\n");
    };
    o.precision(9);
    o << "{\n  \"label\": \"" << frame.capture->label << "\",\n";
    a("camera", frame.camera, 3);
    const uint32_t grid[3]{frame.grid.x, frame.grid.y, frame.grid.z};
    a("gridSize", grid, 3);
    a("grid", p.grid, 4); a("dist", p.dist, 4); a("proj", p.proj, 4); a("viewport", p.viewport, 4); a("depthRange", p.depthRange, 4);
    a("right", p.right, 4); a("up", p.up, 4); a("forward", p.forward, 4); a("medium", p.medium, 4); a("albedo", p.albedo, 4);
    a("emissive", p.emissive, 4); a("ambient", p.ambient, 4); a("sunDir", p.sunDir, 4); a("sunColor", p.sunColor, 4); a("mode", p.mode, 4);
    a("jitter", p.jitter, 4); a("noise", p.noise, 4); a("noiseOrigin", p.noiseOrigin, 4); a("noiseDetail", p.noiseDetail, 4);
    a("shadowRight", p.shadowRight, 4); a("shadowUp", p.shadowUp, 4); a("shadowForward", p.shadowForward, 4); a("shadowOffset", p.shadowOffset, 4);
    a("shadowViewport", p.shadowViewport, 4); a("shadowImage", p.shadowImage, 4); a("memoryA", p.memoryA, 4); a("memoryB", p.memoryB, 4); a("memoryC", p.memoryC, 4);
    a("sunMapA", p.sunMapA, 4); a("sunMapB", p.sunMapB, 4); a("sunU", p.sunU, 4); a("sunV", p.sunV, 4); a("sunS", p.sunS, 4); a("sunArc", p.sunArc, 4); a("phase", p.phase, 4); a("glare", p.glare, 4); a("rays", p.rays, 4);
    a("cascadeInfo", p.cascadeInfo, 4);
    const auto& g = st.gameSun;
    o << "  \"skyFog\": " << st.settings.skyFog << ", \"maxOpacity\": " << st.settings.maxOpacity << ",\n";
    o << "  \"lightLimit\": " << st.settings.lightLimit << ", \"autoExposure\": " << (st.settings.autoExposure ? "true" : "false") << ", \"exposure\": " << st.exposure
      << ", \"exposureKey\": " << st.exposureKey << ", \"exposureGoal\": " << st.exposureGoal << ",\n";
    {
        const auto& g = st.cascadesInjected ? *st.cascadesInjected : st.cascades;
        o << "  \"gameShadows\": " << (st.settings.gameShadows ? "true" : "false") << ", \"gameShadowFlip\": " << st.settings.gameShadowFlip
          << ", \"gameCascadesFound\": " << (g.valid ? "true" : "false") << ", \"gameCascadesProblem\": \"" << g.problem << "\", \"gameCascadeBindings\": ["
          << g.bindings[0] << "," << g.bindings[1] << "," << g.bindings[2] << "," << g.bindings[3] << "], \"gameCascadeLayouts\": ["
          << int(g.layouts[0]) << "," << int(g.layouts[1]) << "," << int(g.layouts[2]) << "," << int(g.layouts[3]) << "], \"gameCascadesUsedFrames\": " << st.cascadeFramesUsed
          << ", \"cascadesReducedThisFrame\": " << (frame.cascadesWrittenBy ? "true" : "false") << ",\n  \"cascadeRowsThisFrame\": [";
        for (int i = 0; i < 12; ++i) o << (i ? ", " : "") << "[" << frame.cascadeRows[i][0] << ", " << frame.cascadeRows[i][1] << ", " << frame.cascadeRows[i][2] << ", " << frame.cascadeRows[i][3] << "]";
        o << "],\n";
    }
    o << "  \"heightReference\": \"" << fog::height_reference_name(st.settings.heightReference) << "\", \"groundValid\": " << (st.groundValid ? "true" : "false")
      << ", \"groundZ\": " << st.groundZ << ", \"groundReading\": " << st.groundReading << ", \"groundCells\": " << st.groundCells
      << ", \"waterLevelValid\": " << (st.waterValid ? "true" : "false") << ", \"waterLevelZ\": " << st.waterZ << ", \"waterPixels\": " << st.waterPixels << ",\n";
    a("gameLightToward", g.toward, 3); a("gameSkyDiscToward", g.skyToward, 3); a("gameAmbient", g.ambient, 3); a("gameDiffuse", g.diffuse, 3); a("gameSpecular", g.specular, 3);
    o << "  \"gameSunOk\": " << (g.ok ? "true" : "false") << ", \"gameSkyDiscOk\": " << (g.skyOk ? "true" : "false") << ",\n";
    o << "  \"weather\": \"" << weather_name(st.weather.ok ? st.weather.kind : 0) << "\", \"weatherOk\": " << (st.weather.ok ? "true" : "false")
      << ", \"weatherIntensity\": " << st.weather.intensity << ", \"sunWeatherScale\": " << st.lastSunWeatherScale << ",\n";
    {   // the sun colour before the weather took its share (sunColor = this x sunWeatherScale), for tools/fog-whatif.py
        float clear[3];
        if (st.gameSun.ok && st.settings.sunColorFromGame) for (int i = 0; i < 3; ++i) clear[i] = st.gameSun.specular[i] * st.settings.sunColor[0];
        else std::copy_n(st.settings.sunColor, 3, clear);
        a("sunColorClear", clear, 3);
    }
    o << "  \"shadowState\": \"" << (st.lastShadowProblem.empty() ? "marching" : st.lastShadowProblem) << "\", \"historyUsed\": " << (st.lastHistoryUsed ? "true" : "false") << "\n}\n";
}

void fog_before_submit(const DeviceData& data, VkQueue queue, const std::vector<VkCommandBuffer>& commands) {
    std::lock_guard lock(mutex);
    auto& s = device_state(data.device);
    if (!s.fog || !s.fog->resources) return;
    auto& st = *s.fog;
    std::shared_ptr<FogFrame> frame;
    for (auto cmd : commands) if (auto it = s.commands.find(id(cmd)); it != s.commands.end() && it->second.fogComposites && it->second.fogFrame) frame = it->second.fogFrame;
    if (!frame) return;
    const auto started = std::chrono::steady_clock::now();
    auto& r = *st.resources;
    const auto& f = r.f;
    // Composite timestamps: only slots whose command buffer was submitted earlier (their reset has been queued); this batch's
    // own composite counts from the next submit on.
    for (uint32_t i = 0; i < compositeSlots; ++i) if (r.composites[i].pending && r.composites[i].submitted && r.queries) {
        uint64_t t[2]{};
        if (f.GetQueryPoolResults(data.device, r.queries, computeSlots * computeStamps + i * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            ema(st.gpuComposite, ticks_to_ms(r, t[0], t[1]));
            r.composites[i].pending = false;
        }
    }
    if (frame->compositeSlot >= 0 && uint32_t(frame->compositeSlot) < compositeSlots) r.composites[frame->compositeSlot].submitted = true;
    auto& slot = r.compute[r.nextCompute % computeSlots];
    const uint32_t slotIndex = r.nextCompute % computeSlots;
    if (f.GetFenceStatus(data.device, slot.fence) != VK_SUCCESS) { ++st.computeSkipped; return; }
    if (slot.sunPending) {
        if (slot.sunEpoch == st.discEpoch) sun_found(st, r.sunFoundMapped + slotIndex * readbackWords, slot.sunCamera, slot.sunArc, r.pictureWidth, r.pictureHeight, s.frameSerial);
        slot.sunPending = false;
    }
    if (slot.exposurePending) {
        exposure_found(st, r.sunFoundMapped + slotIndex * readbackWords + sunFindWords + groundBins, s.frameSerial);
        slot.exposurePending = false;
    }
    if (slot.groundPending) {
        if (slot.groundEpoch == st.discEpoch)
            ground_found(st, r.sunFoundMapped + slotIndex * readbackWords + sunFindWords, r.sunFoundMapped + slotIndex * readbackWords + waterWords, slot.groundCamera, s.frameSerial);
        slot.groundPending = false;
    }
    // Previous use of this slot is complete: fold its timestamps in.
    if (slot.timestamps && r.queries) {
        uint64_t t[computeStamps]{};
        if (f.GetQueryPoolResults(data.device, r.queries, slotIndex * computeStamps, computeStamps, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            ema(st.gpuMemoryClear, ticks_to_ms(r, t[0], t[1])); ema(st.gpuMemoryScatter, ticks_to_ms(r, t[1], t[2])); ema(st.gpuSunFind, ticks_to_ms(r, t[2], t[3]));
            ema(st.gpuInject, ticks_to_ms(r, t[3], t[4])); ema(st.gpuLight, ticks_to_ms(r, t[4], t[5])); ema(st.gpuIntegrate, ticks_to_ms(r, t[5], t[6]));
        }
    }
    const auto cmd = slot.cmd;
    f.ResetCommandBuffer(cmd, 0);
    auto begin = vk<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (f.BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) { ++st.computeSkipped; return; }
    // Everything earlier on this queue (the previous composite reading the volumes/params) before we overwrite them.
    // Also the previous composite's shadow image writes (color attachment) -> this light pass's reads.
    auto before = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    if (!r.volumesInitialized) {
        for (auto* v : {&r.medium, &r.lighting[0], &r.lighting[1], &r.integrated, &r.visibilityVolume})
            image_barrier(f, cmd, v->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        image_barrier(f, cmd, r.noWater, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        f.CmdClearColorImage(cmd, r.noWater, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &whole);
        image_barrier(f, cmd, r.shadow, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);   // read only once filled
        image_barrier(f, cmd, r.cascadeMaps, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);   // likewise
        for (auto image : {r.raysMask.image, r.rays.image, r.shade.image})   // sampled only after the composite that uses them filled them
            image_barrier(f, cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        // Height memory starts empty (0 = nothing seen; tag 0 never matches a cell of a real window except by the clear pass).
        const VkClearColorValue none{};
        for (auto* v : {&r.heightMemory, &r.tagMemory, &r.sunMemory, &r.sunTagMemory, &r.floorMemory}) {
            image_barrier(f, cmd, v->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            f.CmdClearColorImage(cmd, v->image, VK_IMAGE_LAYOUT_GENERAL, &none, 1, &whole);
        }
        auto cleared = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; cleared.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cleared, 0, nullptr, 0, nullptr);
        st.memoryValid = false;
        st.sunBasisValid = false;
        image_barrier(f, cmd, r.noWater, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        r.volumesInitialized = true;
    }
    FogParams params = temporal_params(st, *frame);
    const int memoryPasses = shadow_params(st, r, *frame, params);
    // The previous frame's reduced cascades, in its view space (the shadow camera, set by shadow_params with its view depth).
    if (st.settings.gameShadows && st.prevValid && st.prevFrame.cascadesWrittenBy == &r && params.shadowRight[3] > 0) {
        std::memcpy(params.cascade, st.prevFrame.cascadeRows, sizeof params.cascade);
        const float* d = params.cascade[2];
        params.cascadeInfo[0] = 1;
        params.cascadeInfo[1] = 1.5f * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);   // 1.5 yd of depth
        params.cascadeInfo[2] = float(FogResources::cascadeSize);
        params.cascadeInfo[3] = st.settings.gameShadowFlip;
        ++st.cascadeFramesUsed;
    }
    params.memoryC[2] = float(slotIndex);   // the sun finder's counter slice
    if (frame->capture) { params.mode[2] = 1; capture_write_params(st, *frame, params); frame->capture.reset(); }   // mode.z: write the visibility volume   // only needed until here (it keeps the resources alive)
    f.CmdUpdateBuffer(cmd, r.params, 0, sizeof(FogParams), &params);
    auto upload = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    upload.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; upload.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &upload, 0, nullptr, 0, nullptr);
    // Stamps: start, after the memory clear, the scatter, the sun finder, inject, light, integrate (a skipped stage costs 0).
    const uint32_t q = slotIndex * computeStamps;
    auto stamp = [&](uint32_t k) { if (r.queries) f.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, r.queries, q + k); };
    if (r.queries) { f.CmdResetQueryPool(cmd, r.queries, q, computeStamps); f.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, r.queries, q); }
    f.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.computeLayout, 0, 1, &r.computeSet[frame->parity & 1], 0, nullptr);
    if (!r.noiseBaked) {   // the smoke lattice, once: written here, only read from now on
        image_barrier(f, cmd, r.noiseLattice.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.noiseBake);
        const uint32_t n = (fog::noiseLatticeCells + 3) / 4;
        f.CmdDispatch(cmd, n, n, n);
        image_barrier(f, cmd, r.noiseLattice.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        r.noiseBaked = true;
    }
    auto between = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    between.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; between.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    const auto gx = (frame->grid.x + 7) / 8, gy = (frame->grid.y + 7) / 8;
    if (memoryPasses) {   // height memory: clear/decay, then raise from the previous frame's depth; the light pass reads it
        auto memory = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT; memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        const uint32_t n = (FogResources::memorySize + 7) / 8;
        f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.memoryClear);
        f.CmdDispatch(cmd, n, n, 2);   // z: height memory, sun map
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
        stamp(1);
        if (memoryPasses > 1) {
            f.CmdFillBuffer(cmd, r.sunFound, (slotIndex * readbackWords + waterWords) * sizeof(uint32_t), waterBins * sizeof(uint32_t), 0);
            auto filled = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; filled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &filled, 0, nullptr, 0, nullptr);
            f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.memoryScatter);
            f.CmdDispatch(cmd, (r.shadowWidth + 7) / 8, (r.shadowHeight + 7) / 8, 1);
            f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
        }
        // Ground level (height_reference = ground): the floor memory around the camera counted by height, read back later.
        if (st.settings.heightReference == fog::HeightReference::Ground) {
            f.CmdFillBuffer(cmd, r.sunFound, (slotIndex * readbackWords + sunFindWords) * sizeof(uint32_t), groundBins * sizeof(uint32_t), 0);
            auto filled = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; filled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &filled, 0, nullptr, 0, nullptr);
            f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.ground);
            f.CmdDispatch(cmd, 256 / 8, 256 / 8, 1);   // 256 x 256 samples, every second cell of +-256
            auto host = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
            slot.groundPending = true;
            slot.groundEpoch = st.discEpoch;
            std::copy_n(frame->camera, 3, slot.groundCamera);
        }
    } else stamp(1);
    stamp(2);
    // Sun finder over the previous frame's picture (its view depth is the shadow image, its camera the shadow camera). Not
    // while the camera turns (over .3 degrees since that picture): a picture and a camera a frame apart would put the disc
    // off by the turn - the disc is found again as soon as the view rests.
    const bool sunFindReady = st.settings.shadows && st.prevValid && st.prevFrame.pictureWrittenBy == &r && params.shadowRight[3] > 0;
    const float turnCos = std::min(params.forward[0] * params.shadowForward[0] + params.forward[1] * params.shadowForward[1] + params.forward[2] * params.shadowForward[2],
                                   params.right[0] * params.shadowRight[0] + params.right[1] * params.shadowRight[1] + params.right[2] * params.shadowRight[2]);
    if (sunFindReady && turnCos < std::cos(.3f * 3.14159265f / 180.f)) ++st.discStats.turning;
    else if (sunFindReady) {
        f.CmdFillBuffer(cmd, r.sunFound, slotIndex * readbackWords * sizeof(uint32_t), sunFindWords * sizeof(uint32_t), 0);
        auto filled = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; filled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &filled, 0, nullptr, 0, nullptr);
        f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.sunFind);
        f.CmdDispatch(cmd, (r.pictureWidth + 7) / 8, (r.pictureHeight + 7) / 8, 1);
        auto host = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        slot.sunPending = true;
        slot.sunEpoch = st.discEpoch;
        std::copy_n(params.shadowRight, 4, slot.sunCamera[0]); std::copy_n(params.shadowUp, 4, slot.sunCamera[1]);
        std::copy_n(params.shadowForward, 4, slot.sunCamera[2]); std::copy_n(params.shadowViewport, 4, slot.sunCamera[3]);
        std::copy_n(params.sunArc, 4, slot.sunArc);
    }
    stamp(3);
    f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.inject);
    f.CmdDispatch(cmd, gx, gy, frame->grid.z);
    f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &between, 0, nullptr, 0, nullptr);
    stamp(4);
    f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.light);
    f.CmdDispatch(cmd, (frame->grid.x + 3) / 4, (frame->grid.y + 3) / 4, (frame->grid.z + 3) / 4);   // fog_light.comp: 4x4x4 groups
    f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &between, 0, nullptr, 0, nullptr);
    stamp(5);
    f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.integrate);
    f.CmdDispatch(cmd, gx, gy, 1);
    // Auto exposure: the previous frame's picture with this frame's fog, counted by luminance and read back later.
    if (st.settings.autoExposure && params.mode[0] == 0 && st.prevValid && st.prevFrame.pictureWrittenBy == &r && params.shadowRight[3] > 0) {
        f.CmdFillBuffer(cmd, r.sunFound, (slotIndex * readbackWords + sunFindWords + groundBins) * sizeof(uint32_t), exposureBins * sizeof(uint32_t), 0);
        auto ready = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        ready.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &ready, 0, nullptr, 0, nullptr);
        f.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.exposure);
        f.CmdDispatch(cmd, (r.pictureWidth + 7) / 8, (r.pictureHeight + 7) / 8, 1);
        auto host = vk<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        slot.exposurePending = true;
    }
    stamp(6);
    if (f.EndCommandBuffer(cmd) != VK_SUCCESS) { ++st.computeSkipped; return; }
    f.ResetFences(data.device, 1, &slot.fence);
    auto submit = vk<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    if (f.QueueSubmit(queue, 1, &submit, slot.fence) != VK_SUCCESS) {
        // Leave the fence signalled so the slot stays usable.
        auto fi = vk<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        f.DestroyFence(data.device, slot.fence, nullptr);
        f.CreateFence(data.device, &fi, nullptr, &slot.fence);
        ++st.computeSkipped;
        return;
    }
    slot.timestamps = r.queries != VkQueryPool{};
    // This frame's volume exists now: it is the next frame's history.
    st.prevFrame = *frame;
    st.prevValid = true;
    st.lastComputedParity = frame->parity;
    st.prevSettingsGeneration = st.settingsGeneration;
    ++r.nextCompute;
    ++st.computeSubmits;
    ema(st.cpuPrelude, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

// The game's sun, read once per present (two small self-memory reads). Logged when it starts or stops working and every
// ~15 s while it works, so a wrong or moved chain shows up in the log without the HUD.
static void read_game_sun(FogState& st, uint64_t frame) {
    st.gameSun = read_wow_sun(self_memory_reader(), wow_sun_layout_from_env());
    ++st.sunReads;
    if (!st.gameSun.ok) ++st.sunFailures;
    const auto& g = st.gameSun;
    if (st.sunLoggedAny && g.ok == st.sunLoggedOk && frame < st.sunNextLog) return;
    st.sunLoggedAny = true; st.sunLoggedOk = g.ok; st.sunNextLog = frame + 900;
    if (!g.ok) { log_line("[FOG-SUN] game sun not read (" + g.failure + "); using fog.ini sun_direction"); return; }
    char b[420];
    std::snprintf(b, sizeof b, "[FOG-SUN] game [0x%llx]=0x%llx toward %.4f %.4f %.4f elevation %.2f azimuth %.2f ambient %.3f %.3f %.3f diffuse %.3f %.3f %.3f specular %.3f %.3f %.3f"
                  " | sky disc %s %.4f %.4f %.4f elevation %.2f azimuth %.2f",
                  (unsigned long long)g.staticPointer, (unsigned long long)g.block, double(g.toward[0]), double(g.toward[1]), double(g.toward[2]),
                  double(g.elevationDeg), double(g.azimuthDeg), double(g.ambient[0]), double(g.ambient[1]), double(g.ambient[2]),
                  double(g.diffuse[0]), double(g.diffuse[1]), double(g.diffuse[2]), double(g.specular[0]), double(g.specular[1]), double(g.specular[2]),
                  g.skyOk ? "ok" : "NOT READ", double(g.skyToward[0]), double(g.skyToward[1]), double(g.skyToward[2]), double(g.skyElevationDeg), double(g.skyAzimuthDeg));
    log_line(b);
}

// WoW's weather (memory_sun.hpp), read every present like the sun; logged when the weather kind or readability changes.
static void read_game_weather(FogState& st) {
    const auto previous = st.weather;
    st.weather = read_wow_weather(self_memory_reader(), wow_weather_layout_from_env());
    const auto& w = st.weather;
    if (st.weatherLogged && w.ok == previous.ok && w.kind == previous.kind) return;
    st.weatherLogged = true;
    char b[200];
    if (!w.ok) std::snprintf(b, sizeof b, "[FOG-WEATHER] not read (%s); the sun is not dimmed by the weather", w.failure.c_str());
    else std::snprintf(b, sizeof b, "[FOG-WEATHER] %s intensity %.3f (target %.3f) -> overcast %.2f, sun x %.2f (weather_sun %.2f)", weather_name(w.kind),
                       double(w.intensity), double(w.target), double(weather_overcast(w)), double(1 - st.settings.weatherSun * weather_overcast(w)),
                       double(st.settings.weatherSun));
    log_line(b);
}

static void fog_capture_arm(FogState& st, const char* why) {
    if (st.captureStage != 0) return;
    std::error_code ec;
    st.captureDir = (fog_capture_root() / ("fog-capture-" + std::to_string(getpid()) + "-" + std::to_string(++st.captureSequence))).string();
    std::filesystem::create_directories(st.captureDir, ec);
    if (ec) { st.captureNote = "cannot create " + st.captureDir; log_line("[FOG-CAPTURE] " + st.captureNote); return; }
    if (!st.settingsPath.empty()) std::filesystem::copy_file(st.settingsPath, std::filesystem::path(st.captureDir) / "fog.ini", std::filesystem::copy_options::overwrite_existing, ec);
    std::ofstream(std::filesystem::path(st.captureDir) / "README.txt") <<
        "Fog capture (docs/fog.md, tools/fog-capture.py, tools/fog-whatif.py). One frame, taken 60 frames after the fog was on.\n"
        "color_before / color_after: the game's color image before and after the fog composite (format in final_sections.json).\n"
        "view_depth: R32F view depth of the world (sky/cleared = 1e6; a water surface NEGATIVE - take abs), what the next frame's shadows march through.\n"
        "height_memory: R32UI order-preserving floats (0 = nothing seen), texel = world cell mod 1024, cells of memoryA[3] yards; memory_tags: the cell each texel holds.\n"
        "medium / lighting / integrated: RGBA16F froxel volumes (grid in final_params.json). visibility: RGBA8 volume, r = sun, b = sky visibility.\n"
        "sun_map: R32UI ordered floats across the sun (sunU/sunV/sunS in the params).\n"
        "*_params.json: the FogParams the GPU used, camera (world, double), game sun; fog.ini: the settings.\n";
    st.captureStage = 1;
    st.captureWaited = 0;
    // The fog view is not saved and starts OFF each launch: a capture of nothing would wait forever (it did, 2026-09-26).
    auto& hud = camera_hud_input();
    st.captureTurnedFogOn = hud.fogMode == 0;
    if (st.captureTurnedFogOn) hud.fogMode = 1;
    st.captureNote = "armed (" + std::string(why) + ")";
    log_line("[FOG-CAPTURE] armed by " + std::string(why) + ": " + st.captureDir);
}

// Fog capture on the host side: the request file, the view switch between the two frames, the read-back.
static void fog_capture_present(FogState& st) {
    auto& hud = camera_hud_input();
    if (++st.presentsSinceCaptureCheck >= 30) {
        st.presentsSinceCaptureCheck = 0;
        std::error_code ec;
        const auto request = fog_capture_root() / "fog-capture-request";
        if (std::filesystem::exists(request, ec)) { std::filesystem::remove(request, ec); fog_capture_arm(st, "request file"); }
    }
    if (st.captureStage == 4) { if (st.captureTurnedFogOn) hud.fogMode = 0; st.captureStage = 0; }
    // No fog composite for ~10 s (no world pass reached it: loading screen, the view switched off): give up and say so.
    if (st.captureStage == 1 && ++st.captureWaited > 2000) {
        if (st.captureTurnedFogOn) hud.fogMode = 0;
        st.captureStage = 0;
        st.captureNote = "gave up: no fog frame was drawn";
        log_line("[FOG-CAPTURE] " + st.captureNote);
    }
    for (auto it = st.captures.begin(); it != st.captures.end();) {
        auto& c = **it;
        const VkResult status = c.f.GetEventStatus(c.device, c.event);
        if (status == VK_EVENT_RESET && ++c.polls < 6000) { ++it; continue; }
        if (status == VK_EVENT_SET) {
            std::ofstream index(std::filesystem::path(c.dir) / (c.label + "_sections.json"));
            index << "[\n";
            size_t bytes = 0;
            for (size_t i = 0; i < c.sections.size(); ++i) {
                const auto& sec = c.sections[i];
                std::ofstream out(std::filesystem::path(c.dir) / (sec.name + ".bin"), std::ios::binary);
                out.write(static_cast<const char*>(c.mapped) + sec.offset, std::streamsize(sec.size));
                bytes += sec.size;
                index << "  {\"name\": \"" << sec.name << "\", \"format\": \"" << sec.format << "\", \"width\": " << sec.width << ", \"height\": " << sec.height
                      << ", \"depth\": " << sec.depth << ", \"bytes\": " << sec.size << "}" << (i + 1 < c.sections.size() ? ",\n" : "\n");
            }
            index << "]\n";
            st.captureNote = "written " + std::filesystem::path(c.dir).filename().string();
            log_line("[FOG-CAPTURE] written " + c.label + " dir=" + c.dir + " bytes=" + std::to_string(bytes));
        } else log_line("[FOG-CAPTURE] abandoned " + c.label + " (event status " + std::to_string(int(status)) + ")");
        c.release();          // the ~100 MB read-back buffer, now
        c.keep.reset();
        it = st.captures.erase(it);
    }
}

void fog_present(Device& s) {
    const bool enabled = fog_enabled();
    // The next frame's first world draw freezes a fresh memory camera, also when the overlay (which normally re-arms
    // this) is not presenting.
    if (enabled && s.sceneDebugger) s.sceneDebugger->world.snapshotPending = true;
    if (!s.fog && !enabled) return;
    auto& st = fog_state(s);
    if (enabled && ++st.presentsSinceSettingsCheck >= 60) { st.presentsSinceSettingsCheck = 0; load_settings(st); }
    if (enabled && st.settingsPath.empty()) load_settings(st);
    if (enabled && (st.settings.sunFromGame || st.settings.ambientFromGame)) read_game_sun(st, s.frameSerial);
    if (enabled) read_game_weather(st);
    st.fogFramesOn = enabled ? st.fogFramesOn + 1 : 0;
    // The fog's own cost in the log every ~15 s while it is on (the HUD shows it too, only with F9).
    if (enabled && st.fogFramesOn % 900 == 450) {
        char b[360];
        std::snprintf(b, sizeof b, "[FOG-PERF] gpu ms: memories %.3f (clear %.3f scatter %.3f sun find %.3f) inject %.3f light %.3f integrate %.3f composite %.3f total %.3f"
                      " | grid %ux%ux%u | cpu ms %.3f | sun from %s | skipped computes %llu",
                      fog_gpu_memories(st), st.gpuMemoryClear, st.gpuMemoryScatter, st.gpuSunFind, st.gpuInject, st.gpuLight, st.gpuIntegrate, st.gpuComposite,
                      fog_gpu_total(st), st.lastGrid[0], st.lastGrid[1], st.lastGrid[2], st.cpuComposite + st.cpuPrelude, st.lastSunSource.c_str(),
                      (unsigned long long)st.computeSkipped);
        log_line(b);
        if (st.settings.transparentFog) {   // milestone 3: what the transparent-aware fog did over these 900 frames, per frame
            auto per = [](uint64_t v) { char t[24]; std::snprintf(t, sizeof t, "%.1f", double(v) / 900.0); return std::string(t); };
            std::string rejected;
            for (const auto& [why, count] : st.deferRejected) rejected += (rejected.empty() ? "" : "; ") + per(count) + " " + why;
            log_line("[FOG-TRANSPARENT] per frame: held back " + per(st.deferred) + " (drawn after the fog " + per(st.deferredAfterFog) + ", after the water " + per(st.deferredAfterWater) + ", by a fallback flush " +
                     per(st.deferredFallback) + ", flush postponed " + per(st.deferPostponed) + "), layers kept in place " + per(st.deferLayers) +
                     " | not held: " + (rejected.empty() ? std::string("none") : rejected));
            st.deferred = st.deferredAfterFog = st.deferredAfterWater = st.deferredFallback = st.deferLayers = st.deferPostponed = 0;
            st.deferRejected.clear();
        }
    }
    st.frameTrace.clear();
    fog_capture_present(st);
    if (st.pending.active) {
        ++st.droppedThisFrame;
        const std::string why = "world not followed by any other use of its targets before present";
        if (st.lastProblem != why) log_line("[FOG] not composited: " + why);
        st.lastProblem = why;
        st.pending = {};
    }
    st.lastComposites = st.compositesThisFrame; st.lastWorldPasses = st.worldPassesThisFrame; st.lastDropped = st.droppedThisFrame;
    st.compositesThisFrame = st.worldPassesThisFrame = st.droppedThisFrame = 0;
    if (st.timelineFrames) { log_line("[PASS-TIMELINE] present frame=" + std::to_string(s.frameSerial)); --st.timelineFrames; st.timelinePass = 0; }
    // Log the pass structure of two frames when the fog is turned on, and again every ~15 s while it stays on.
    if (enabled && s.frameSerial >= st.timelineNextFrame) { st.timelineFrames = 2; st.timelineNextFrame = s.frameSerial + 900; }
    if (!enabled) st.timelineNextFrame = 0;
    while (!st.retired.empty() && s.frameSerial >= st.retired.front().first + retireAfterPresents) st.retired.pop_front();
}

void fog_destroy(Device& s) {
    if (!s.fog) return;
    s.fog->pending = {};
    s.fog->retired.clear();
    s.fog->resources.reset();
}

// ---- F10 page -----------------------------------------------------------------------------------------
namespace {
template<size_t N> float next_step(float current, const float (&steps)[N]) {
    for (float v : steps) if (v > current + 1e-6f) return v;
    return steps[0];
}
std::string number(float v, int decimals) { char b[32]; std::snprintf(b, sizeof b, "%.*f", decimals, double(v)); return b; }
constexpr float densitySteps[]{.005f, .01f, .02f, .04f, .08f}, falloffSteps[]{.02f, .04f, .08f, .15f, .3f}, baseSteps[]{-40, -20, -12, -7, -5, 0, 10, 25};
constexpr float distanceSteps[]{150, 250, 400, 600, 800}, gSteps[]{0, .2f, .3f, .5f, .7f}, albedoSteps[]{.5f, .7f, .9f, 1.f}, ambientSteps[]{.1f, .2f, .3f, .5f, .8f};
constexpr float sunSteps[]{0, .5f, 1, 1.6f, 3, 5, 8, 12, 18}, ambientScaleSteps[]{.5f, .75f, 1, 1.5f, 2, 3}, shadowSteps[]{0, 60, 120, 200}, skySteps[]{0, .25f, .5f, .75f}, openSunSteps[]{.15f, .25f, .5f, 1};
constexpr float temporalSteps[]{0, .8f, .9f, .95f}, noiseSteps[]{0, .15f, .3f, .5f, .8f}, noiseScaleSteps[]{12, 24, 48}, windSteps[]{0, .2f, 1, 3};
enum Row : unsigned { ViewRow, Quality, Density, Falloff, Reference, Base, Distance, Anisotropy, Albedo, Ambient, AmbientSource, Sun, SunSource, Shadows, SkyLight, OpenSun, Temporal, Noise, NoiseScale, Wind, LookShafts, LightGizmo, LightSources, GameFog, CaptureRow, Reset, RowCount };
constexpr float gameFogSteps[]{1, 1.5f, 2, 3, 0};
}
unsigned fog_menu_rows() { return RowCount; }
namespace {
constexpr float softnessSteps[]{1, 1.5f, 2, 3}, contactSteps[]{0, .3f, .5f, .8f}, aoSteps[]{0, .5f, .8f, 1}, terrainSteps[]{0, .25f, .4f, .6f};
constexpr float terrainFromSteps[]{0, 10, 20, 40, 80}, pointShadowSteps[]{0, .3f, .6f, 1}, pointRangeSteps[]{6, 12, 20}, pointIgnoreSteps[]{.5f, 1, 1.5f, 2.5f}, pointLengthSteps[]{2, 5, 10, 20};
constexpr float softShadowSteps[]{0, .5f, 1, 2, 3, 4}, shadowFadeSteps[]{0, .3f, .5f, .8f, 1}, fadeDistanceSteps[]{3, 5, 10, 20, 40}, indoorSteps[]{0, .5f, 1};
std::string strength(float v) { return v <= 0 ? "OFF" : std::to_string(int(v * 100 + .5f)) + "%"; }
// Soft shadows are built into the game's shaders when it creates them (only when the game started with them on); from then
// on the settings are live. The note says which.
std::string soft_shadow_note(bool size) {
    const auto st = discovery::soft_shadow_stats();
    if (!st.latched) return {};
    if (st.refused) return " (DRIVER REFUSED)";
    if (st.active.size <= 0 || !st.shaders) return " (AFTER A GAME RESTART)";
    return size ? " - " + std::to_string(st.shaders) + " SHADERS" : "";
}
}
unsigned shade_menu_rows() { return 13; }
std::string shade_menu_label(Device& d, unsigned row) {
    auto& st = fog_state(d);
    if (st.settingsPath.empty()) load_settings(st);
    const auto& s = st.settings;
    switch (row) {
    case 0: return s.shadowSoftness <= 1 ? "SHADOW EDGES: AS THE GAME" : "SHADOW EDGES: " + number(s.shadowSoftness, 1) + "X SOFTER";
    case 1: return "SOFT SHADOWS: " + (s.softShadows > 0 ? number(s.softShadows, s.softShadows < 1 ? 1 : 0) + "X" : std::string("OFF")) + soft_shadow_note(true);
    case 2: return "SHADOW FADE: " + strength(s.shadowFade) + soft_shadow_note(false);
    case 3: return "FADE DISTANCE: " + number(s.shadowFadeDistance, 0) + " YD FROM THE CASTER";
    case 4: return "CONTACT SHADOWS: " + strength(s.contactShadows);
    case 5: return "AMBIENT OCCLUSION: " + strength(s.ambientOcclusion);
    case 6: return "TERRAIN SHADOWS: " + strength(s.terrainShadows) + (s.terrainShadows > 0 ? " (EXPERIMENTAL)" : "");
    case 7: return s.terrainShadowsFrom <= 0 ? std::string("TERRAIN SHADOWS FROM: THE CAMERA") : "TERRAIN SHADOWS FROM: " + number(s.terrainShadowsFrom, 0) + " YD";
    case 8: return "POINT LIGHT SHADOWS: " + strength(s.pointShadows) + (s.pointShadows > 0 ? " (" + std::to_string(discovery::game_lights_count()) + " LIGHTS KNOWN)" : "");
    case 9: return "POINT SHADOW RANGE: " + number(s.pointShadowRange, 0) + " YD";
    case 10: return "POINT SHADOW IGNORE: " + number(s.pointShadowIgnore, 1) + " YD AROUND THE LIGHT";
    case 11: return "POINT SHADOW LENGTH: " + number(s.pointShadowLength, 0) + " YD BEHIND THE CASTER";
    case 12: return std::string("INDOOR SUN SHADOWS: ") + (s.indoorShadows >= 1 ? "AS THE GAME" : s.indoorShadows <= 0 ? "OFF" : strength(s.indoorShadows));
    default: return {};
    }
}
std::string shade_menu_hint(unsigned row) {
    switch (row) {
    case 0: return "  THE GAME'S OWN SUN SHADOWS, FILTER SPREAD";
    case 1: return "  BLURRIER THE FARTHER FROM THE CASTER";
    case 2: return "  LIGHTER THE FARTHER FROM THE CASTER";
    case 3: return "  WHERE THE FADE IS REACHED";
    case 4: return "  GRASS, STONES, FEET - WHERE THE GAME HAS NO SHADOW";
    case 5: return "  CORNERS, WALL FEET, UNDER TREES";
    case 6: return "  HILLS AND CLIFFS (THE GAME CASTS NONE FROM LAND)";
    case 7: return "  NONE NEAR THE CHARACTER, FADED IN FARTHER";
    case 8: return "  TORCHES, FIRES: CHARACTERS AND THINGS CAST THEM";
    case 9: return "  HOW FAR AROUND A LIGHT";
    case 10: return "  ITS OWN BRAZIER / LANTERN CASTS NOTHING";
    case 11: return "  FADES OUT AWAY FROM WHAT CASTS IT";
    case 12: return "  THE GAME'S SHADOWS IN CAVES AND BUILDINGS";
    default: return {};
    }
}
void shade_menu_click(Device& d, unsigned row) {
    auto& st = fog_state(d);
    if (st.settingsPath.empty()) load_settings(st);
    auto& s = st.settings;
    switch (row) {
    case 0: s.shadowSoftness = next_step(s.shadowSoftness, softnessSteps); break;
    case 1: s.softShadows = next_step(s.softShadows, softShadowSteps); break;
    case 2: s.shadowFade = next_step(s.shadowFade, shadowFadeSteps); break;
    case 3: s.shadowFadeDistance = next_step(s.shadowFadeDistance, fadeDistanceSteps); break;
    case 4: s.contactShadows = next_step(s.contactShadows, contactSteps); break;
    case 5: s.ambientOcclusion = next_step(s.ambientOcclusion, aoSteps); break;
    case 6: s.terrainShadows = next_step(s.terrainShadows, terrainSteps); break;
    case 7: s.terrainShadowsFrom = next_step(s.terrainShadowsFrom, terrainFromSteps); break;
    case 8: s.pointShadows = next_step(s.pointShadows, pointShadowSteps); break;
    case 9: s.pointShadowRange = next_step(s.pointShadowRange, pointRangeSteps); break;
    case 10: s.pointShadowIgnore = next_step(s.pointShadowIgnore, pointIgnoreSteps); break;
    case 11: s.pointShadowLength = next_step(s.pointShadowLength, pointLengthSteps); break;
    case 12: s.indoorShadows = next_step(s.indoorShadows, indoorSteps); break;
    default: return;
    }
    write_settings(st.settingsPath, s);
    std::error_code ec;
    st.settingsTime = std::filesystem::last_write_time(st.settingsPath, ec);   // our own write: no reload needed
    st.settingsNote = "saved";
    ++st.settingsGeneration;
}
std::string fog_menu_label(Device& d, unsigned row) {
    auto& st = fog_state(d);
    if (st.settingsPath.empty()) load_settings(st);
    const auto& s = st.settings;
    static const char* views[]{"OFF", "FINAL", "FROXEL DENSITY", "FROXEL EXTINCTION", "FROXEL LIGHTING", "INTEGRATED SCATTERING", "TRANSMITTANCE", "SUN VISIBILITY"};
    switch (row) {
    case ViewRow: return std::string("VIEW: ") + views[camera_hud_input().fogMode % 8];
    case Quality: return std::string("QUALITY: ") + preset_name(s.preset);
    case Density: return "DENSITY: " + number(s.medium.baseDensity, 3) + " PER YARD";
    case Falloff: return "HEIGHT FALLOFF: " + number(s.medium.heightFalloff, 2) + " HALF EVERY " + number(std::log(2.f) / std::max(s.medium.heightFalloff, 1e-4f), 0) + " YD";
    case Reference: return std::string("HEIGHT REFERENCE: ") + (s.heightReference == fog::HeightReference::Ground ? "GROUND" : s.heightReference == fog::HeightReference::Camera ? "CAMERA" : "WORLD Z");
    case Base: return s.heightReference == fog::HeightReference::World ? "BASE: Z " + number(s.medium.baseHeight, 0) :
                      (s.heightReference == fog::HeightReference::Ground ? "BASE: GROUND " : "BASE: CAMERA ") + number(s.medium.baseHeight, 0) + " YD";
    case Distance: return "MAX DISTANCE: " + number(s.maxDistance, 0) + " YD";
    case Anisotropy: return "FORWARD SCATTER G: " + number(s.anisotropy, 1);
    case Albedo: return "ALBEDO: " + number(s.medium.albedo[1], 1);
    case Ambient: return s.ambientFromGame ? "AMBIENT LIGHT: GAME X" + number(s.ambientScale, 2) : "AMBIENT LIGHT: " + number(s.ambient[1], 1);
    case AmbientSource:
        if (!s.ambientFromGame) return "AMBIENT COLOR: FOG.INI (MANUAL)";
        if (!st.gameSun.ok) return st.sunReads ? "AMBIENT COLOR: GAME - NOT READ, FOG.INI USED" : "AMBIENT COLOR: GAME";
        return "AMBIENT COLOR: GAME " + number(st.gameSun.ambient[0], 2) + " " + number(st.gameSun.ambient[1], 2) + " " + number(st.gameSun.ambient[2], 2);
    case Sun: {
        if (!s.sunEnabled) return "SUN LIGHT: OFF";
        const float scale = 1 - s.weatherSun * weather_overcast(st.weather);   // the weather's share, as the composite applies it
        // "(RAIN 0.47, SUN 0%)": the game's rain intensity, then how much of the sun the fog keeps (fits the 44-character row)
        if (st.weather.ok && st.weather.kind && s.weatherSun > 0)
            return "SUN LIGHT: " + number(s.sunColor[0], 1) + (s.sunColorFromGame ? " X GLOW" : "") + " (" + (st.weather.kind == 3 ? "SAND" : weather_name(st.weather.kind)) +
                   " " + number(st.weather.intensity, 2) + ", SUN " + std::to_string(int(scale * 100 + .5f)) + "%)";
        return "SUN LIGHT: " + number(s.sunColor[0], 1) + (s.sunColorFromGame ? " X GAME GLOW COLOUR" : "");
    }
    case SunSource:
        if (!s.sunFromGame) return "SUN DIRECTION: FOG.INI (MANUAL)";
        if (!st.gameSun.ok) return st.sunReads ? "SUN DIRECTION: GAME - NOT READ, FOG.INI USED" : "SUN DIRECTION: GAME";
        if (s.sunAtSkyDisc && st.gameSun.skyOk) return "SUN DIRECTION: SKY DISC EL " + number(st.gameSun.skyElevationDeg, 1) + " AZ " + number(st.gameSun.skyAzimuthDeg, 0);
        return std::string(s.sunAtSkyDisc ? "SUN DIRECTION: LIGHT (NO DISC) EL " : "SUN DIRECTION: GAME LIGHT EL ") + number(st.gameSun.elevationDeg, 1) + " AZ " + number(st.gameSun.azimuthDeg, 0);
    case Shadows: return s.shadows ? "SUN SHADOWS: " + number(s.shadowLength, 0) + " YD" + (st.lastShadowProblem.empty() || st.lastShadowProblem == "off" ? "" : " (" + st.lastShadowProblem + ")") : "SUN SHADOWS: OFF";
    case SkyLight: return s.skyLight > 0 ? "SKY LIGHT FROM ABOVE: " + std::to_string(int(s.skyLight * 100 + .5f)) + "% OF AMBIENT" : "SKY LIGHT FROM ABOVE: OFF";
    case OpenSun: return s.openSun >= 1 ? "OPEN-SKY SUN: SAME AS UNDER COVER" : "OPEN-SKY SUN: " + std::to_string(int(s.openSun * 100 + .5f)) + "% (SHAFTS UNDER COVER)";
    case Temporal: return s.temporalWeight > 0 ? "TEMPORAL: ON HISTORY " + number(s.temporalWeight, 2) : "TEMPORAL: OFF";
    case Noise: return s.noiseAmount > 0 ? "NOISE: " + number(s.noiseAmount, 2) : "NOISE: OFF";
    case NoiseScale: return "NOISE SCALE: " + number(s.noiseScale, 0) + " YD";
    case Wind: return "NOISE DRIFT: " + number(s.noiseWind, 1) + " YD/S";
    case LookShafts: return "LOOK: FOREST LIGHT SHAFTS (SETS THE ROWS ABOVE)";
    case LightGizmo: return s.lightGizmo ? "LIGHT GIZMO: ON Y=FOG W=DISC C=LIGHT M=ADDR" : "LIGHT GIZMO: OFF";
    case LightSources: return s.lightSources ? "LIGHT SOURCES: ON (" + std::to_string(game_lights_count()) + " POINT LIGHTS)" : "LIGHT SOURCES: OFF";
    case GameFog: {
        const auto seen = game_fog_state();
        const std::string what = s.gameFogDistance == 1 ? "AS THE GAME" : s.gameFogDistance == 0 ? "OFF" : number(s.gameFogDistance, 1) + "X FARTHER";
        return "GAME'S OWN FOG: " + what + (seen.empty() || s.gameFogDistance == 1 ? "" : " (" + seen + ")");
    }
    case CaptureRow: return st.captureStage ? "CAPTURE FOG DATA: IN PROGRESS" : st.captureNote.empty() ? "CAPTURE FOG DATA" : "CAPTURE FOG DATA - " + st.captureNote;
    case Reset: return "RESET TO DEFAULTS";
    default: return {};
    }
}
void fog_menu_click(Device& d, unsigned row) {
    auto& st = fog_state(d);
    if (st.settingsPath.empty()) load_settings(st);
    auto& s = st.settings;
    auto& hud = camera_hud_input();
    switch (row) {
    case ViewRow: hud.fogMode = (hud.fogMode + 1) % CameraHudInput::fogModeCount; return;   // not a saved setting
    case Quality: s.preset = s.preset == fog::Preset::Low ? fog::Preset::Medium : s.preset == fog::Preset::Medium ? fog::Preset::High : fog::Preset::Low; break;
    case Density: s.medium.baseDensity = next_step(s.medium.baseDensity, densitySteps); break;
    case Falloff: s.medium.heightFalloff = next_step(s.medium.heightFalloff, falloffSteps); break;
    case Reference:
        // CAMERA -> WORLD -> GROUND -> CAMERA, keeping the fog where it is right now: the base converted to the new reference.
        if (s.heightReference == fog::HeightReference::Camera) {
            s.heightReference = fog::HeightReference::World;
            s.medium.baseHeight = st.lastBaseHeight;
        } else if (s.heightReference == fog::HeightReference::World) {
            s.heightReference = fog::HeightReference::Ground;
            s.medium.baseHeight = st.groundValid ? float(std::round(st.lastBaseHeight - st.groundZ)) : -7.f;
        } else {
            s.heightReference = fog::HeightReference::Camera;
            s.medium.baseHeight = st.haveReference ? float(std::round(st.lastBaseHeight - st.referenceZ)) : -12.f;
        }
        break;
    case Base: s.medium.baseHeight = s.heightReference != fog::HeightReference::World ? next_step(s.medium.baseHeight, baseSteps) : s.medium.baseHeight + 10.f; break;
    case Distance: s.maxDistance = next_step(s.maxDistance, distanceSteps); break;
    case Anisotropy: s.anisotropy = next_step(s.anisotropy, gSteps); break;
    case Albedo: { const float a = next_step(s.medium.albedo[1], albedoSteps); s.medium.albedo[0] = s.medium.albedo[1] = a; s.medium.albedo[2] = std::min(a * 1.02f, 1.f); break; }
    case Ambient:
        if (s.ambientFromGame) s.ambientScale = next_step(s.ambientScale, ambientScaleSteps);
        else { const float a = next_step(s.ambient[1], ambientSteps); s.ambient[0] = a * .9f; s.ambient[1] = a; s.ambient[2] = a * 1.15f; }
        break;
    case AmbientSource: s.ambientFromGame = !s.ambientFromGame; break;
    case Sun: {
        const float current = s.sunEnabled ? s.sunColor[0] : 0.f, v = next_step(current, sunSteps);
        s.sunEnabled = v > 0;
        if (v > 0) { s.sunColor[0] = v; s.sunColor[1] = v * .94f; s.sunColor[2] = v * .81f; }
        break;
    }
    case SunSource:   // GAME SKY DISC -> GAME LIGHT -> FOG.INI -> GAME SKY DISC
        if (s.sunFromGame && s.sunAtSkyDisc) s.sunAtSkyDisc = false;
        else if (s.sunFromGame) s.sunFromGame = false;
        else { s.sunFromGame = true; s.sunAtSkyDisc = true; }
        break;
    case Shadows: { const float v = next_step(s.shadows ? s.shadowLength : 0.f, shadowSteps); s.shadows = v > 0; if (v > 0) s.shadowLength = v; break; }
    case SkyLight: s.skyLight = next_step(s.skyLight, skySteps); break;
    case OpenSun: s.openSun = next_step(s.openSun, openSunSteps); break;
    case Temporal: s.temporalWeight = next_step(s.temporalWeight, temporalSteps); break;
    case Noise: s.noiseAmount = next_step(s.noiseAmount, noiseSteps); break;
    case NoiseScale: s.noiseScale = next_step(s.noiseScale, noiseScaleSteps); break;
    case Wind: s.noiseWind = next_step(s.noiseWind, windSteps); break;
    case CaptureRow: fog_capture_arm(st, "menu"); return;   // not a setting
    case LightGizmo: s.lightGizmo = !s.lightGizmo; break;
    case LightSources: s.lightSources = !s.lightSources; break;
    case GameFog: {
        size_t i = 0;
        while (i < std::size(gameFogSteps) && gameFogSteps[i] != s.gameFogDistance) ++i;
        s.gameFogDistance = gameFogSteps[(i + 1) % std::size(gameFogSteps)];
        break;
    }
    case LookShafts:
        // Shafts visible from the side need the sun term well above the ambient in the fog, and enough fog to scatter it:
        // density ~.025 at the camera, a strong sun with mild forward scattering, most of the ambient from the (blocked) sky.
        // Chosen from offline re-renders of Teldrassil captures (tools/fog-whatif.py): rays in the game's glow colour. Shaft
        // brightness goes with density x sun, the veil over the background with density alone: half the density of the
        // first version (.04) with twice the sun kept the shafts and gave the forest behind them back (capture 28697-3).
        // Base 7 yd under the area's ground: at a third-person camera ~5 yd up the same density as the first version's camera
        // -12 had, but on the land, not following the character.
        s.medium.baseDensity = .02f; s.medium.heightFalloff = .04f; s.heightReference = fog::HeightReference::Ground; s.medium.baseHeight = -7;
        s.sunEnabled = true; s.sunColor[0] = 12; s.sunColor[1] = 12 * .94f; s.sunColor[2] = 12 * .81f; s.sunColorFromGame = true; s.anisotropy = .35f;
        // Roadmap M5 (tools/fog-shafts.py on 4 captures + an in-game A/B the user approved): density x.6 and sun x1.6 give the
        // shafts +20-25% in forests for +3-9% of overall change.
        s.medium.baseDensity = .012f; s.sunColor[0] = 19.2f; s.sunColor[1] = 19.2f * .94f; s.sunColor[2] = 19.2f * .81f;
        s.skyLight = .85f; s.shadows = true; s.ambientScale = 1; s.ambientFromGame = true; s.noiseAmount = .3f; s.openSun = .25f;
        s.maxOpacity = .6f;   // that density over open water erased the far side of Booty Bay's harbour (capture 20013-5)
        s.lightLimit = .8f; s.autoExposure = true;   // looking into the sun through a canopy gap went white (Shadowglen)
        s.sunAureole = .04f; s.sunAureoleG = .9f;      // light bleeding around what the sun is half behind
        s.sunGlare = .7f;                              // the eye's glare over what the sun is half behind
        s.sunRays = 1.5f;                              // beams through the gaps near the sun (captures 40147-2, 62384-2)
        break;
    case Reset: s = FogSettings{}; break;
    default: return;
    }
    write_settings(st.settingsPath, s);
    std::error_code ec;
    st.settingsTime = std::filesystem::last_write_time(st.settingsPath, ec);   // our own write: no reload needed
    st.settingsNote = "saved";
    ++st.settingsGeneration;
}
}
