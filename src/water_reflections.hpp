#pragma once
#include <chrono>
#include <set>
#include "surface_inspector.hpp"
#include "overlay_internal.hpp"
#include "water_surface.hpp"

namespace depth_probe::discovery {
// Width of the private camera texture (texels): see water_camera.frag for the layout.
constexpr uint32_t waterCameraWidth=20;
// Sky memory map size (texels); water_sky.frag has the same constants.
constexpr uint32_t waterSkyWidth=128,waterSkyHeight=64;
// Water layers (a bit mask in WaterState::layers / WaterParams::draw[3]).
constexpr uint32_t waterLayerReflection=1,waterLayerRefraction=2,waterLayerDepthTint=4,waterLayerFoam=8,waterLayersAll=15;
constexpr uint64_t wowWaterVS=0x2fce10e0177c7507ull, wowWaterFS=0x6e0413111ae30ee9ull;
struct WaterLayout {
    DeviceData data;VkDescriptorSetLayout cameraSet{},emptySet{};VkPipelineLayout layout{};
    ~WaterLayout();void release();
};
struct WaterStage {std::vector<VkSpecializationMapEntry> entries;std::vector<uint8_t> bytes;};
struct WaterRecipe {
    bool supported=true,vertexInput=false,raster=false,fragment=false,output=false;
    std::string why; // every reason `supported` was cleared
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    bool restart=false,depthBounds=false,clipExtension=false;VkBool32 depthClip=VK_TRUE;
    VkPipelineRasterizationStateCreateInfo rasterInfo{};
    WaterStage vs,fs;
};
// view: level 0 (what a render pass writes); mips: every level (sampled), only for images created with a mip chain (levels > 1).
struct WaterImage {VkImage image{};VkImageView view{};VkDeviceMemory memory{};bool transferSrc=false;VkImageView mips{};uint32_t levels=1;};
// Mip levels of the water colour: the full chain (the image clamps it to 1x1; 1920x1080 has 11). The MODERN composite reads the game's colour
// of its water at the level whose texels cover ~6 yards of water: the game's texture averaged away, its zone tint and broad light kept. Seen
// from close above, one texel must span most of the screen, so the chain is not cut short.
constexpr uint32_t waterColorLevels=16;
// Everything heavy that one set of game images needs (about 90 MB at 1080p): private images, render passes, fixed pipelines and the
// cache of replay pipelines. Shared by every command buffer that draws into the same game color/depth images: each execution is
// self-contained (it writes, then reads, all of it) and starts with an ALL_COMMANDS barrier on the single graphics queue, so two
// executions can never overlap.
struct WaterBatch {
    DeviceData data;DepthOverlayFns f;
    uint32_t width=0,height=0;VkFormat colorFormat=VK_FORMAT_UNDEFINED;
    uint64_t sourceColor=0,sourceDepth=0,sourceColorGeneration=0,sourceDepthGeneration=0;
    VkImageView colorView{},depthView{};VkSampler sampler{};
    WaterImage camera,mask,maskDepth,planar,planarDepth,scene;
    // the water shader's own output (rgb, alpha), second attachment of the mask pass: lets the composite undo the game's blend; its mip chain
    // (built after the mask pass, water_color_mips) gives MODERN the game's water colour without the game's texture. tintSampler: trilinear.
    WaterImage waterColor;VkSampler tintSampler{};
    // Sky memory: persists across frames (only ever touched inside self-contained, serialized executions).
    WaterImage sky;VkSampler skySampler{};VkRenderPass skyInitPass{},skyPass{};VkFramebuffer skyFB{};VkPipeline skyPipeline{};bool skyReady=false;
    // Tileable water textures, 3-layer arrays uploaded from a staging buffer that lives as long as the batch, by the first few executions (any
    // of them may run first; each is self-contained). `detail`: water_detail.bin (tools/generate-water-textures.py; CLASSIC, 256^2);
    // `modern`: water_modern.bin (tools/generate-modern-water-textures.py; MODERN ripple normals, foam, chromatic caustics).
    struct Textures {
        VkImage image{};VkImageView view{};VkDeviceMemory memory{};VkSampler sampler{};
        VkBuffer staging{};VkDeviceMemory stagingMemory{};std::vector<VkBufferImageCopy> copies;
        uint32_t size=1,levels=1,uploads=0;bool fromFile=false;
    };
    Textures detail,modern;
    VkDescriptorSetLayout resolveSetLayout{},compositeSetLayout{};
    VkPipelineLayout resolveLayout{},compositeLayout{};
    VkRenderPass cameraPass{},maskPass{},planarPass{},compositePass{};
    VkFramebuffer cameraFB{},maskFB{},planarFB{},compositeFB{};
    VkPipeline cameraPipeline{},compositePipeline{};
    std::map<std::array<uint64_t,26>,VkPipeline> pipelines;
    std::vector<std::shared_ptr<WaterLayout>> layouts;
    bool alive=true;
    ~WaterBatch();void release();
};
// What one command buffer owns: descriptor sets and the params buffer (a few hundred bytes). Descriptors cannot be rewritten while
// an earlier execution may still use them, so they stay per command buffer; the batch behind them is shared.
struct WaterSlot {
    std::shared_ptr<WaterBatch> batch;
    VkBuffer params{};VkDeviceMemory paramsMemory{};VkDescriptorPool pool{};
    VkDescriptorSet resolveSet{},compositeSet{},cameraSet{};
    VkQueryPool queries{};uint32_t timestampBits=0;float timestampPeriod=0;bool timestampsPending=false; // 5 timestamps per execution
    bool alive=true,recorded=false; // recorded: from recording until the command buffer is reset
    ~WaterSlot();void release();
};
// Measured cost of the water work (logged as [WATER-PERF]): CPU time inside record_water_reflections and GPU time per stage from
// timestamp queries (read one execution later, never waited for).
struct WaterPerf {
    double cpuSeconds=0;uint64_t cpuSamples=0;
    double gpuMs[5]{};uint64_t gpuSamples=0; // camera+mask+planar, scene copy, sky memory, composite, total
    uint32_t waterDraws=0,reflectedDraws=0;
    uint64_t heldOverWater=0;   // blended effects held back over the water and issued after its composite (fog_defer_candidate)
};
// Look settings from the "Water details" menu page, saved with the other water settings. Each value cycles through a short list of steps.
struct WaterLook {
    float foamAmount=1.f,foamSize=1.f,foamSpeed=1.f,foamSoftness=.5f,foamPulse=.7f,foamReach=1.f,detail=.6f,gameOpacity=1.f,
          clarity=.6f,caustics=.8f,glitter=1.f,whitecaps=.5f,   // seabed visibility in shallow water, light patterns on it, sun sparkle, open-sea foam // gameOpacity: how much of the game's own water texture stays
          waveHeight=1.f,waveSteepness=1.f,                     // x the configured Gerstner waves' amplitudes / orbit shapes (WaterState::waves)
          // MODERN style: GGX roughness (TUNING), medium / micro ripple normal scales (1 = rms slopes ~.07 / ~.08, together about the calm-wind
          // Cox-Munk .12: MEASURED reference, TUNING scale), subsurface light through the wave crests toward the sun (TUNING).
          roughness=.06f,mediumNormals=1.f,microNormals=1.f,subsurface=1.f;
    uint32_t quality=1; // 0 low, 1 medium, 2 high: how far and how finely the screen-space reflection march looks
    bool operator==(const WaterLook&) const=default;
};
struct WaterLookOption {const char* key;const char* label;float WaterLook::* value;float steps[5];unsigned count;};
inline constexpr WaterLookOption waterLookOptions[]{
    {"foam_amount","FOAM AMOUNT",&WaterLook::foamAmount,{0.f,.5f,1.f,1.5f,2.f},5},
    {"foam_size","FOAM PATCH SIZE",&WaterLook::foamSize,{.5f,.75f,1.f,1.5f,2.5f},5},
    {"foam_speed","FOAM SPEED",&WaterLook::foamSpeed,{0.f,.5f,1.f,1.5f,2.5f},5},
    {"foam_softness","FOAM SOFTNESS",&WaterLook::foamSoftness,{0.f,.25f,.5f,.75f,1.f},5},
    {"foam_pulse","FOAM COMES AND GOES",&WaterLook::foamPulse,{0.f,.35f,.7f,1.f,1.f},4},
    {"foam_reach","FOAM SHORE REACH",&WaterLook::foamReach,{.4f,.7f,1.f,1.6f,2.5f},5},
    {"detail","WATER TEXTURE DETAIL",&WaterLook::detail,{0.f,.3f,.6f,1.f,1.5f},5},
    {"game_opacity","GAME WATER TEXTURE OPACITY",&WaterLook::gameOpacity,{0.f,.25f,.5f,.75f,1.f},5},
    {"clarity","SHALLOW WATER CLARITY",&WaterLook::clarity,{0.f,.3f,.6f,.85f,1.f},5},
    {"caustics","SEABED CAUSTICS",&WaterLook::caustics,{0.f,.4f,.8f,1.2f,1.6f},5},
    {"glitter","SUN GLITTER",&WaterLook::glitter,{0.f,.5f,1.f,1.5f,2.f},5},
    {"whitecaps","OPEN SEA WHITECAPS",&WaterLook::whitecaps,{0.f,.25f,.5f,.75f,1.f},5},
    {"wave_height","WAVE HEIGHT",&WaterLook::waveHeight,{0.f,.5f,1.f,2.f,4.f},5},
    {"wave_steepness","WAVE STEEPNESS",&WaterLook::waveSteepness,{0.f,.5f,1.f,1.f,1.f},3},
    {"roughness","ROUGHNESS (MODERN)",&WaterLook::roughness,{.03f,.06f,.1f,.16f,.25f},5},
    {"medium_normals","MEDIUM RIPPLES (MODERN)",&WaterLook::mediumNormals,{0.f,.5f,1.f,1.5f,2.f},5},
    {"micro_normals","MICRO RIPPLES (MODERN)",&WaterLook::microNormals,{0.f,.5f,1.f,1.5f,2.f},5},
    {"subsurface","SUBSURFACE LIGHT (MODERN)",&WaterLook::subsurface,{0.f,.5f,1.f,1.5f,2.f},5}};
constexpr size_t waterLookOptionCount=sizeof(waterLookOptions)/sizeof(waterLookOptions[0]);
constexpr uint32_t waterQualityCount=3;
inline constexpr const char* waterQualityNames[waterQualityCount]{"LOW","MEDIUM","HIGH"};
void water_look_cycle(WaterLook&,size_t option);       // next step of one option (wraps)
void water_look_cycle_quality(WaterLook&);
std::string water_look_label(const WaterLook&,size_t option); // "FOAM AMOUNT: 100%"
// Debug views (menu VIEW, WaterState::debug, params.options.z): each isolates one component of the composite.
// 8.. isolate MODERN components (they are computed by the modern path whatever the style).
inline constexpr const char* waterDebugViewNames[]{"FINAL","MASK","NORMAL","WATER DEPTH","REFLECTION","BASE SURFACE","DISPLACEMENT","LARGE WAVES",
    "MEDIUM NORMALS","MICRO NORMALS","FINAL NORMAL","FRESNEL","ROUGHNESS","SSR CONFIDENCE","PLANAR WEIGHT","ENVIRONMENT WEIGHT","SUN GLINTS",
    "WATER THICKNESS","ABSORPTION","REFRACTION","CAUSTICS","WATER TINT"};
constexpr uint32_t waterDebugViewCount=sizeof(waterDebugViewNames)/sizeof(waterDebugViewNames[0]);
struct WaterState {
    uint32_t mode=0,debug=0,layers=waterLayersAll,tone=1;float strength=.70f;
    // The latest water mask recorded (view-space water surface position per pixel, w = alpha; SHADER_READ_ONLY after the
    // water passes), for the volumetric fog to stop at the water surface. maskSerial increments per recorded mask.
    uint64_t maskSerial=0,maskColorImage=0;VkImageView maskView{};
    WaterLook look;
    water::WaveSettings waves;   // the Gerstner waves (water.ini wave1..wave5, wind_direction, wave_speed); scaled by look.waveHeight/Steepness
    // A/B: 1 MODERN (layered surface, Fresnel, roughness, sun, absorption), 0 CLASSIC (the look before 2026-09-29, kept bit-exact).
    uint32_t style=1;
    // MODERN body: absorption per yard of water along the view ray, before the clarity scale. Default: pure water (Pope & Fry 1997,
    // 0.34 / 0.06 / 0.009 per metre at 650 / 550 / 450 nm, in yards): MEASURED physics, scaled by `clarity` (TUNING).
    float absorption[3]{.31f,.055f,.008f};
    // The sun for specular light and subsurface light: the game's light direction (memory_sun.hpp); when it cannot be read, the ini fallback
    // `sun_fallback=elevation,azimuth` (degrees) if given, else no sun term. Read at most every 100 ms.
    bool sunFallback=false;float sunFallbackDeg[2]{30.f,45.f};
    std::chrono::steady_clock::time_point sunRead{};float sun[4]{},sunColor[3]{};
    // Foam animation clock: seconds multiplied by the foam speed and ACCUMULATED, so changing the speed changes the pace, not the pattern.
    double foamClock=0;std::chrono::steady_clock::time_point foamClockLast{};
    // The chosen water profile (shader pair) is kept across scene changes and saved with the other settings, then restored at launch.
    bool profileSaved=false;
    WaterPerf perf;
    uint64_t selectedVS=0,selectedFS=0,depthGeneration=0;
    uint64_t selectedSignature=0;   // water_transform_signature of the selected VS, learned the first time a draw uses it
    uint64_t passes=0,draws=0,skipped=0;
    std::string status="Select the known WoW water candidate";
    std::vector<std::weak_ptr<WaterBatch>> batches;std::vector<std::weak_ptr<WaterSlot>> slots;
    std::vector<std::weak_ptr<WaterLayout>> layouts;
    // Debug capture: armed from the menu (or automatically once per mode/view with DEPTH_PROBE_WATER_CAPTURE=1).
    bool captureArmed=false;uint32_t captureCount=0,requestPolls=0;std::set<uint32_t> capturedViews;
    std::shared_ptr<struct WaterCapture> capture;std::vector<std::shared_ptr<struct WaterCapture>> abandoned;
};
// First eight reads of the constant buffer (set 2 / binding 0), in order, as constant index chains: the vertex transform
// (c0..c3 projection, c5..c8 view) DXVK emits for the water VS. 0 when the module does not have that shape.
uint64_t water_transform_signature(Shader&);
// Is this draw's shader pair the selected water? The selected pair, or a VS variant that shares the water fragment shader
// and the exact same transform prologue (WoW draws parts of the sea with such variants; they differ only after it).
bool water_draw_shaders(WaterState&,const Pipeline&);
struct alignas(16) WaterParams {
    float viewport[4]{},depthRange[4]{};
    uint32_t vertex[4]{}; // byte base, stride, buffer bytes, attribute bytes
    uint32_t indices[4]{}; // byte base, index type (0/2/4), first, count
    int32_t draw[4]{}; // baseVertex, restart, reserved
    float options[4]{}; // mode, strength, debug, time (seconds)
    float tone[4]{}; // water tone rgb, amount
    float foam[4]{}; // foam amount, patch size, foam clock (seconds, already scaled by the speed), softness
    float look[4]{}; // foam come-and-go strength, shore reach, texture detail, reflection quality (0..2)
    float extra[4]{}; // opacity of the game's own water texture (1 = untouched), water blend mode (0 unknown, 1 alpha, 2 premultiplied), reserved
    float fx[4]{}; // shallow clarity, seabed caustics, sun glitter, open-sea whitecaps
    float waveA[water::maxWaves][4]{},waveB[water::maxWaves][4]{},surface[4]{}; // water::PackedWaves (shaders/water_surface.glsl)
    float modern[4]{};   // style (0 classic, 1 modern), GGX roughness, medium ripple scale, micro ripple scale
    float sun[4]{};      // toward the sun (world), source (0 none, 1 the game's light, 2 the ini fallback)
    float sunColor[4]{}; // rgb: the game's sun (glow) colour, w: sun specular strength (glitter x (1 - weather overcast))
    float body[4]{};     // absorption per yard rgb (x the clarity scale), subsurface strength
};
static_assert(sizeof(WaterParams)==416);
// Records the upload of the water textures into `cmd` (transition, copy, transition back to shader-read). Called for the first few executions.
constexpr uint32_t waterDetailUploadExecutions=8;
void water_upload_detail(WaterBatch&,VkCommandBuffer);
void water_advance_foam_clock(WaterState&,std::chrono::steady_clock::time_point now); // adds (now - last) * foamSpeed, at most .25 s per call
WaterState& water_state(Device&);
float water_time();
double water_time_seconds(); // unwrapped, for the wave phases (0 when frozen)
void water_update_sun(WaterState&); // WaterState::sun / sunColor for the MODERN style (the game's light, else the ini fallback, else none)
bool water_frozen(); // DEPTH_PROBE_WATER_FREEZE=1: fixed wave time and a black default sky, for exact pixel tests
// ---- debug capture (water_capture.cpp) ----
// Water tone presets (menu: TONE). Applied luma-preserving under the depth-tint layer and as a flavor on the reflection.
struct WaterTone {const char* name;float color[3];float amount;};
inline constexpr WaterTone waterTones[]{{"NATURAL",{0.f,0.f,0.f},0.f},{"TROPICAL",{.10f,.72f,.75f},.55f},{"DEEP BLUE",{.06f,.24f,.60f},.60f},{"EMERALD",{.08f,.58f,.36f},.55f},{"GOLDEN",{.80f,.58f,.30f},.45f}};
constexpr uint32_t waterToneCount=sizeof(waterTones)/sizeof(waterTones[0]);
// Persistent settings (water.ini next to the overlay settings, or DEPTH_PROBE_WATER_SETTINGS): profile, mode, strength, layers, tone.
std::string water_settings_text(const WaterState&);
bool water_settings_parse(WaterState&,const std::string& text); // false when a value was invalid (it keeps its previous value)
std::string water_settings_path();
void water_settings_load(WaterState&);
void water_settings_save(const WaterState&);
struct WaterCaptureDraw {std::shared_ptr<Draw> draw;std::string role,why;};
struct WaterCaptureInput {
    WaterBatch& batch;const Draw& water;const WaterParams& params;const std::vector<WaterCaptureDraw>& passDraws;
    VkImage sourceColor,sourceDepth;VkFormat colorFormat,depthFormat;VkImageLayout colorLayout,depthLayout;
    VkBuffer vertexBuffer,indexBuffer,uniformBuffer;
    uint64_t vertexOffset,vertexBytes,indexOffset,indexBytes,uniformOffset,uniformBytes;
    uint32_t attributeBinding,attributeOffset,attributeFormat,stride;
    bool depthCopyable; // the game created its depth image with TRANSFER_SRC
};
bool water_capture_wanted(const WaterState&);
void water_capture_poll_request(WaterState&);
// Records the copies at the point where the source color is COLOR_ATTACHMENT_OPTIMAL (after the composite) and the
// source depth is GENERAL; finished asynchronously by water_capture_poll once the GPU has executed them.
void water_capture_record(const DeviceData&,Device&,VkCommandBuffer,const WaterCaptureInput&);
void water_capture_poll(const DeviceData&,Device&);
void water_capture_release_all(WaterState&);
void create_water_layout(const DeviceData&,Layout&,const VkPipelineLayoutCreateInfo&);
void capture_water_recipe(Device&,Pipeline&,const VkGraphicsPipelineCreateInfo&);
// Extra image is a private 16x1 GPU-resolved camera/plane texture at set 3.
std::vector<uint32_t> reflect_vertex_spirv(const std::vector<uint32_t>&,const std::string& entry,std::string& reason);
std::vector<uint32_t> water_fragment_spirv(const std::vector<uint32_t>&,const std::string& entry,std::string& reason);
bool water_pipeline_eligible(const Pipeline&);
// With `reasons` non-null lists every failed condition (comma separated); with
// null it is the same check and stops at the first one.
bool water_pipeline_check(const Pipeline&,std::string* reasons);
// nullptr when the render target can be copied/sampled by the reflection passes, else the reason.
const char* water_target_problem(VkDevice,const Target&);
// nullptr when the pipeline's push constants can be re-pushed after the replay, else the reason.
const char* water_push_problem(const Pipeline&,const std::array<PushState,2>&);
void record_water_reflections(const DeviceData&,Device&,Command&,VkCommandBuffer);
// Layout events for `image` after the draw `d` in this command buffer: `untouched` when there are none (the image is then still in the
// layout the draw sampled it in, which its descriptor states); else the last one's layout (UNDEFINED if partial or unknown).
VkImageLayout layout_since_draw(const Command&,const Draw& d,const Image& image,bool& untouched);
void destroy_water_source(Device&,uint64_t image);
void destroy_water_device(Device&);
void select_water_surface(Device&,Inspector&);
// Heavy shared resources for the game images of `draw`. The last four parameters are not used here (they belong to the slot).
std::shared_ptr<WaterBatch> create_water_batch(const DeviceData&,const Draw&,uint32_t mode,const WaterParams&,
    const VkDescriptorBufferInfo& camera,const VkDescriptorBufferInfo& vertices,const VkDescriptorBufferInfo& indices,std::string* why=nullptr);
bool water_batch_matches(const WaterBatch&,const Draw&);
std::shared_ptr<WaterSlot> create_water_slot(const std::shared_ptr<WaterBatch>&,const Draw&,const VkDescriptorBufferInfo& camera,
    const VkDescriptorBufferInfo& vertices,const VkDescriptorBufferInfo& indices,std::string* why=nullptr);
VkPipeline water_draw_pipeline(WaterBatch&,const Draw&,bool reflected,std::string* why=nullptr);
}
