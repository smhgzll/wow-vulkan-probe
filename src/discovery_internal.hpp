#pragma once
#include "pipeline_discovery.hpp"
#include "draw_classifier.hpp"
#include <array>
#include <map>
#include <set>
#include <chrono>
#include <type_traits>
#include <functional>

namespace depth_probe::discovery {
constexpr size_t maxObjects=16384, maxPipelines=8192, maxCommands=4096;
constexpr size_t maxEvents=32768, maxCommandEvents=8192, maxFrameDraws=8192, maxCaptureDraws=16384;
constexpr size_t maxResourceUses=32;
constexpr size_t maxSamplerDescriptors=4096, maxPushWords=64;
constexpr size_t maxDescriptors=256, maxSets=8, maxCaptureFrames=16;
template<class T> uint64_t id(T h) {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<uintptr_t>(h);
    else return uint64_t(h);
}
uint64_t hash_bytes(const void*, size_t, uint64_t seed=14695981039346656037ull);
void hash_value(uint64_t&, uint64_t);
std::string hex(uint64_t);
template<class T> const T* chain(const void* p, VkStructureType type) {
    for(auto* n=static_cast<const VkBaseInStructure*>(p);n;n=n->pNext)
        if(n->sType==type)return reinterpret_cast<const T*>(n);
    return nullptr;
}
PFN_vkVoidFunction downstream(const DeviceData&, const char*);
// downstream() walks string comparisons and then the loader's proc-address lookup; DXVK makes tens of thousands of hooked calls per
// frame, so the result is cached per thread (device, name). Any device create/destroy invalidates every cache.
PFN_vkVoidFunction cached_downstream(const DeviceData&, const char*);
void invalidate_downstream_cache();
template<class T> T next(const DeviceData& d,const char* name){return reinterpret_cast<T>(cached_downstream(d,name));}

struct ShaderBinding { uint32_t set=UINT32_MAX,binding=UINT32_MAX,storage=UINT32_MAX; bool sampled=false,dref=false; };
struct SamplerIndex {bool known=false,fromPush=false;uint32_t value=0,shift=0,mask=UINT32_MAX;};
struct SamplerAccess {uint32_t set=UINT32_MAX,binding=UINT32_MAX;SamplerIndex index;};
// DXVK's D3D9 alpha test inside a translated fragment shader: OpSwitch over the compare op, taken from bits
// [offset, offset+bits) of spec constant `wordSpecId` when spec constant `useSpecId` selects the specialised path, else from
// DXVK's spec-state UBO (fast-linked pipeline libraries). Every WoW fragment shader has it (and a demote), so a discard alone
// says nothing about cutout; the compare op from the pipeline's specialization data does (ALWAYS = alpha test off).
struct DxvkAlphaTest {
    bool found=false,specWhenNonZero=true;
    uint32_t useSpecId=0,wordSpecId=0,offset=0,bits=0,useDefault=0,wordDefault=0;
    // The fast-link side: the uniform word the select takes instead of the spec word (DXVK's spec_state UBO), when it is a load
    // through constant indices - its set, binding and byte offset (roadmap M4: read per draw at record time).
    bool uboKnown=false;uint32_t uboSet=0,uboBinding=0,uboOffset=0;
};
struct Shader {
    uint64_t hash=0;
    std::vector<uint32_t> words;
    std::vector<ShaderBinding> bindings;
    std::vector<SamplerAccess> samplers;bool samplerAccessesComplete=true;
    uint32_t samples=0,drefs=0,gathers=0,fetches=0,unresolvedSamples=0;
    bool valid=false, isolationSafe=false, replaySafe=false, earlyTests=false;
    uint64_t waterSignature=0;bool waterSignatureDone=false;   // water_transform_signature() cache
    bool discards=false;        // OpKill / OpTerminateInvocation / OpDemoteToHelperInvocation anywhere
    DxvkAlphaTest dxvkAlphaTest;
};
Shader inspect_spirv(const uint32_t*,size_t);
// Finds the DxvkAlphaTest pattern (called by inspect_spirv; exposed for tests).
DxvkAlphaTest find_dxvk_alpha_test(const uint32_t* words,size_t wordCount);
// The alpha test a pipeline's fragment stage was specialised with: 1 on, 0 off (compare op ALWAYS), -1 unknown (no pattern,
// or the fast-link path that reads DXVK's spec UBO per draw). `source` says which.
struct AlphaTestState {int8_t state=-1;uint8_t compare=0xff;const char* source="no fragment shader";};
AlphaTestState dxvk_alpha_test_state(const Shader*,const VkSpecializationInfo*);
void inspect_sampler_accesses(Shader&);
struct Image {
    uint64_t handle=0,generation=0;
    VkFormat format=VK_FORMAT_UNDEFINED; VkExtent3D extent{};
    VkImageUsageFlags usage=0; uint32_t layers=0,mips=0,samples=0;
    bool swapchain=false;
    VkSharingMode sharing=VK_SHARING_MODE_EXCLUSIVE;VkImageType type=VK_IMAGE_TYPE_2D;VkImageCreateFlags flags=0;
    VkImageLayout currentLayout=VK_IMAGE_LAYOUT_UNDEFINED;uint64_t layoutQueue=0;bool layoutKnown=false;
    struct MipLayout {VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;uint64_t queue=0;bool known=false;};
    std::vector<MipLayout> mipLayouts; // bounded to 32 for single-layer images
    bool previewCompatible=true;
};
struct View {
    uint64_t handle=0,generation=0;
    std::shared_ptr<Image> image;
    VkImageSubresourceRange range{};VkFormat format=VK_FORMAT_UNDEFINED;
    VkImageViewType type=VK_IMAGE_VIEW_TYPE_2D;VkComponentMapping components{};
    bool previewCompatible=true;
};
struct Sampler {
    uint64_t handle=0,generation=0,signature=0;
    uint32_t compare=0,op=0,min=0,mag=0,mipmap=0,address[3]{};
    float lodBias=0,minLod=0,maxLod=0;
};
struct Binding {
    uint32_t binding=0,count=0;VkDescriptorType type=VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkShaderStageFlags stages=0;VkDescriptorBindingFlags flags=0;
    std::vector<std::shared_ptr<Sampler>> immutable;
};
struct SetLayout { uint64_t signature=0;bool supported=true,largeSamplerArray=false;std::map<uint32_t,Binding> bindings;uint32_t flags=0;std::string diagnostic; };
struct WaterLayout;struct WaterRecipe;struct WaterState;struct WaterBatch;struct WaterSlot;
struct Layout { uint64_t signature=0,pushSignature=0;bool supported=true;std::vector<std::shared_ptr<SetLayout>> sets;uint32_t flags=0;std::string diagnostic;
    std::vector<VkPushConstantRange> pushRanges;std::shared_ptr<WaterLayout> water;
};
struct Descriptor {
    VkDescriptorType type=VK_DESCRIPTOR_TYPE_MAX_ENUM;
    std::shared_ptr<View> view;std::shared_ptr<Sampler> sampler;
    uint64_t buffer=0,offset=0,range=0,bufferGeneration=0;
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    bool known=false;
};
struct Set {
    uint64_t handle=0,generation=0,pool=0;std::shared_ptr<SetLayout> layout;
    // Flat (binding,arrayElement) map, bounded by maxDescriptors per set.
    std::map<std::pair<uint32_t,uint32_t>,Descriptor> descriptors;
    bool incomplete=false;
};
struct DescriptorTemplate {
    VkDescriptorUpdateTemplateType type=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    VkPipelineBindPoint point=VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint64_t layout=0;uint32_t set=0;
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
    bool supported=true;
};
struct Subpass { std::vector<uint32_t> colors;uint32_t depth=VK_ATTACHMENT_UNUSED; };
struct Pass {
    std::vector<VkAttachmentDescription> attachments;std::vector<Subpass> subpasses;
    bool supported=true;
};
struct Framebuffer {
    std::vector<std::shared_ptr<View>> views;uint32_t width=0,height=0,layers=0;
    bool imageless=false;
};
struct Target {
    std::vector<std::shared_ptr<View>> colors;
    std::shared_ptr<View> depth;
    uint64_t renderPass=0,framebuffer=0;
    uint32_t subpass=0,width=0,height=0;
    bool known=false, inherited=false;
    bool clearKnown=false;float clearDepth=0;
    // Dynamic rendering cannot change an attachment's layout inside the instance, so these are also the layouts at its end.
    VkImageLayout renderingColorLayout=VK_IMAGE_LAYOUT_UNDEFINED,renderingDepthLayout=VK_IMAGE_LAYOUT_UNDEFINED;bool renderingLayoutsKnown=false;
    // The rest of the rendering instance, so a private instance on the same attachments can be begun again (deferred draws).
    bool renderingStencil=false;VkImageLayout renderingStencilLayout=VK_IMAGE_LAYOUT_UNDEFINED;VkRect2D renderingArea{};uint32_t renderingLayers=0;
};
struct InspectorPipeline;
struct PickQueries;
struct Inspector;
struct SceneDebugger;
struct GpuUniformRead;
struct GpuUniformBatch;
struct FogState;struct FogFrame;struct DrawClassStats;
std::string fog_scan_context(Device&);
struct Frame;struct SubmittedDraw;struct UniformSnapshot;
std::string scan_uniform_file(const Frame&,const SubmittedDraw&,const UniformSnapshot&);   // discovery_capture.cpp: frame scan "uniforms" file name   // volumetric_fog.cpp: the fog's camera + the game's light as a JSON object ("" without the fog)
struct BufferBinding {uint64_t buffer=0,generation=0,offset=0,size=0;uint32_t stride=0;bool bound=false;}; // bound: a bind call named this binding (buffer 0 = VK_NULL_HANDLE)
struct Memory {
    uint64_t handle=0,generation=0,size=0,mapOffset=0,mapSize=0;
    VkMemoryPropertyFlags properties=0;
    const uint8_t* mapped=nullptr;
    // Conservatively sticky for the allocation lifetime. Never read memory
    // that may alias a GPU-writable resource, even after that resource dies.
    bool compatible=true,gpuWritable=false;
};
struct Buffer {
    uint64_t handle=0,generation=0,size=0;VkBufferUsageFlags usage=0;
    uint64_t memory=0,memoryGeneration=0,memoryOffset=0;
    bool captureCompatible=true;
    bool transferPatched=false;
};
struct Pipeline {
    std::shared_ptr<WaterRecipe> water;
    uint64_t handle=0,serial=0,hash=0,layoutHandle=0;
    std::shared_ptr<Layout> layout;
    std::shared_ptr<Shader> vs,fs;
    std::string vsEntry,fsEntry;
    std::string vsSource="absent",fsSource="absent";
    uint32_t depthTest=0,depthWrite=0,compare=0,blend=0,cull=0,frontFace=0,topology=0;
    uint32_t blendSrc=0,blendDst=0;bool blendKnown=false;   // color blend factors of attachment 0 (first attachment with blending enabled)
    uint32_t depthBiasEnable=0,stencil=0,rasterDiscard=0;
    float biasConstant=0,biasClamp=0,biasSlope=0;
    VkViewport viewport{};VkRect2D scissor{};
    std::set<VkDynamicState> dynamic;
    uint32_t dynamicMask=0;bool dynamicMaskReady=false;   // DynamicBits of the states in `dynamic` (pipeline_record), for effective_state
    bool fingerprintComplete=true,isolationSafe=false,isolationSubsetSafe=true;
    int32_t softShadowSet=-1;uint32_t softShadowBinding=0;   // its fragment shader has soft shadows reading live settings there (game_soft_shadows.hpp)
    uint32_t parts=15;
    uint32_t samples=1,colorMask=0,logicOp=0,alphaToCoverage=0;bool replaySubset=true;
    AlphaTestState alphaTest;   // from the fragment stage's specialization (draw_semantics: opaque vs alpha-tested); not hashed
    std::shared_ptr<InspectorPipeline> inspector;
    std::string cloneInfo,queryStatus="not-attempted",tintStatus="not-attempted";
};
struct Dynamic {
    VkViewport viewport{};VkRect2D scissor{};
    uint32_t depthTest=0,depthWrite=0,compare=0,cull=0,topology=0,rasterDiscard=0,blend=0,frontFace=0,biasEnable=0;
    uint32_t blendSrc=0,blendDst=0;
    float biasConstant=0,biasClamp=0,biasSlope=0;
    uint32_t stencil=0,depthBounds=0,restart=0;
    uint32_t colorMask=15;   // attachment 0 (vkCmdSetColorWriteMaskEXT, or the pipeline's)
    uint32_t valid=0;
};
enum DynamicBits : uint32_t {Viewport=1,Scissor=2,DepthTest=4,DepthWrite=8,Compare=16,Cull=32,Topology=64,Bias=128,Discard=256,Blend=512,FrontFace=1024,BiasEnable=2048,Stencil=4096,DepthBoundsEnable=8192,Restart=16384,BlendEquation=32768,ColorMask=65536};
struct BoundSet { uint64_t handle=0,generation=0;std::shared_ptr<Layout> layout;std::shared_ptr<Set> pushed;std::vector<uint32_t> offsets{}; };
struct PushState {std::array<uint32_t,maxPushWords> words{};uint64_t known=0,signature=0;};
struct Draw {
    std::shared_ptr<Pipeline> pipeline;Target target;Dynamic dynamic;
    std::array<BoundSet,maxSets> sets{};
    std::vector<uint32_t> dynamicOffsets;
    std::array<PushState,2> push{};
    uint64_t command=0,ordinal=0,commandGeneration=0;
    std::array<BufferBinding,16> vertices{};BufferBinding index;VkIndexType indexType=VK_INDEX_TYPE_UINT16;
    std::shared_ptr<PickQueries> pick;uint32_t query=UINT32_MAX;uint64_t pickTicket=0;
    uint32_t count=0,instances=0,first=0,firstInstance=0;
    int32_t vertexOffset=0;
    // 0 direct, 1 indexed, 2 indirect, 3 indexed indirect, 4/5 count-buffer indirect.
    uint32_t kind=0;
    bool skipped=false,unsupported=false,waterReplayUnsafe=false;
    const char* unsupportedReason="none";
    const char* descriptorApi="none";
    std::vector<std::shared_ptr<GpuUniformRead>> gpuUniforms;
    DrawSemantic semantic;   // draw_classifier.cpp, at record time (UNKNOWN when classification is off)
    uint32_t passOrdinal=0;  // which render pass instance of its command buffer it was recorded in (1 = first)
    bool fogDeferred=false;  // held back by the transparent-aware fog and issued after the fog composite
};
struct LayoutEvent {std::shared_ptr<Image> image;VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;bool known=false;VkImageSubresourceRange range{};};
struct Event {std::shared_ptr<Draw> draw; uint64_t secondary=0,generation=0;LayoutEvent layout;};
// The last value the application set for each dynamic state, replayed after a private water pass so the application's own draws carry
// on with the state they expect. Plain data: recording a state is a few stores (the hooks run tens of thousands of times per frame).
struct WaterRestore {
    // Simple states that take one 32-bit value; `simple` is indexed by the order of waterSimpleStates in water_reflections.cpp.
    static constexpr size_t simpleCount=11;
    enum Bit : uint32_t {Viewport=1,Scissor=2,ViewportCount=4,ScissorCount=8,Bias=16,BlendConstants=32,LineWidth=64,DepthBounds=128,LogicOp=256,
        StencilCompare=512,StencilWrite=1024,StencilReference=2048,StencilOp=4096,BlendEnable=8192,BlendEquation=16384,ColorWriteMask=32768};
    uint32_t set=0,ext=0;                  // ext: the application used the EXT entry point (bit = Bit value, simple states use 1<<(16+index))
    uint32_t simpleSet=0;
    std::array<uint32_t,simpleCount> simple{};
    VkViewport viewport{};VkRect2D scissor{};
    float biasConstant=0,biasClamp=0,biasSlope=0,blendConstants[4]{},lineWidth=1,depthBoundsLow=0,depthBoundsHigh=1;
    VkLogicOp logicOp{};
    uint32_t blendEnable=0;VkColorBlendEquationEXT blendEquation{};VkColorComponentFlags colorWriteMask=15;   // attachment 0 (DXVK draws to one)
    uint8_t stencilFaces[4]{};             // per Bit kind (compare, write, reference, op): faces (1 front, 2 back) that were set
    std::array<uint32_t,2> stencilCompare{},stencilWrite{},stencilReference{};
    struct Op{VkStencilOp fail{},pass{},depthFail{};VkCompareOp compare{};};
    std::array<Op,2> stencilOp{};
};
struct Command {
    std::vector<std::shared_ptr<WaterSlot>> waterSlots;
    // waterSeenInPass: this pass has drawn the selected water (blended effects after it are held back over it: fog_defer_candidate)
    size_t waterPassStart=0;uint32_t passDraws=0;bool waterSeenInPass=false;   // draws recorded since the pass began (counted even when not snapshotted)
    uint32_t passOrdinal=0;                          // render pass / rendering instances begun in this command buffer (frame scan)
    WaterRestore waterRestore;
    uint64_t pool=0,generation=0,ordinal=0;VkCommandBufferLevel level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBufferUsageFlags usage=0;
    std::shared_ptr<Pipeline> pipeline;Target target;Dynamic dynamic;
    std::shared_ptr<Pass> pass;std::vector<std::shared_ptr<View>> attachments;
    std::array<BoundSet,maxSets> sets{};std::vector<uint32_t> dynamicOffsets;
    std::array<PushState,2> push{};
    std::vector<Event> events;
    std::array<BufferBinding,16> vertices{};BufferBinding index;VkIndexType indexType=VK_INDEX_TYPE_UINT16;
    std::shared_ptr<PickQueries> pick;
    std::shared_ptr<GpuUniformBatch> uniformPass;
    std::vector<std::shared_ptr<GpuUniformBatch>> uniformBatches;
    uint32_t activeQueries=0;
    bool recording=false,executable=false,truncated=false,unsupported=false,conditional=false,transformFeedback=false;
    const char* unsupportedReason="none";
    const char* descriptorApi="none";
    // Volumetric fog (volumetric_fog.cpp): draw classes of the current pass (reset at pass begin), and the
    // composites recorded into this command buffer with the parameters the compute prelude must produce.
    uint32_t fogWorldDraws=0,fogSkyDraws=0,fogOtherDraws=0,fogComposites=0;
    std::shared_ptr<FogFrame> fogFrame;
    // Transparent-aware fog (volumetric_fog.cpp fog_defer_draw): blended draws without depth write held back from the world
    // pass and issued again after the fog composite in this command buffer (or before anything else touches their target).
    struct DeferredDraw {std::shared_ptr<Draw> draw;WaterRestore state;};
    std::vector<DeferredDraw> fogDeferred;bool fogDeferPostponed=false;
    struct Geometry {uint64_t index=0,vertex0=0,vertex0Offset=0;uint32_t kind=0,first=0,count=0;int32_t vertexOffset=0;bool valid=false;} lastGeometry;   // the previous draw's
};
inline void mark_unsupported(Command& c,const char* reason) {
    if(!c.unsupported)c.unsupportedReason=reason;
    c.unsupported=true;
}
struct ResourceUse {
    uint32_t set=0,binding=0,element=0;Descriptor descriptor;
    bool staticSample=false,staticDref=false,updateAfterBind=false,samplerIndexed=false;
};
enum DescriptorIssues : uint32_t {MissingSet=1,StaleSet=2,MissingBoundLayout=4,MissingPipelineLayout=8,LayoutMismatch=16,IncompleteSet=32,MissingSampledBinding=64,ResourceLimit=128,SamplerIndexUnknown=256,SamplerUnavailable=512};
struct UniformSnapshot {
    uint32_t set=0,binding=0,element=0;
    uint64_t buffer=0,bufferGeneration=0,memoryGeneration=0,offset=0,range=0,submit=0,ticket=0;
    std::string reason;
    std::string captureMoment="host-before-submit";
    uint32_t bufferUsage=0;
    std::vector<uint8_t> bytes;
};
struct SubmittedDraw {Draw draw;std::vector<ResourceUse> resources;uint64_t queue=0,submit=0,order=0;bool descriptorsIncomplete=false;uint32_t descriptorIssues=0;
    std::vector<std::shared_ptr<const UniformSnapshot>> uniforms;
};
struct Frame {
    uint64_t number=0,queue=0;int32_t presentResult=0;
    std::vector<SubmittedDraw> draws;
    std::vector<LayoutEvent> layouts;
    // Submit bookkeeping must survive draw/capture truncation.
    std::vector<std::shared_ptr<GpuUniformBatch>> uniformBatches;
    std::string context;         // frame scan: the fog's camera and the game's light at this present (JSON object, empty = none)
    uint32_t missingCommands=0;
    bool truncated=false;
};
struct PipelineSummary {
    std::shared_ptr<Pipeline> pipeline;
    uint32_t draws=0,skipped=0,depthWrite=UINT32_MAX,blend=0,targetKind=0;
    Target target;uint32_t depthBindings=0,compareSamplers=0;
    uint32_t first=0,last=0;
    bool incomplete=false;
};
struct Capture {uint64_t sequence=0,device=0,pickTicket=0;uint32_t pickX=0,pickY=0;std::vector<Frame> frames;
    bool uniforms=false;         // frame scan "uniforms": the constants of draws that sample a large depth image with a comparison (shadow maps)
    bool lights=false;           // frame scan "lights": the constants of every draw on the scene's depth target (light research, tools/light-constants.py)
    std::string directory;       // frame scan: its own output folder (else DEPTH_PROBE_DISCOVERY_DIR / /tmp), finished by a DONE file
    uint64_t sceneDepthImage=0;  // the scene's depth image when the capture began (draw_classifier's "scene target")
};
struct Device {
    std::shared_ptr<WaterState> water;
    std::unordered_map<uint64_t,std::shared_ptr<Shader>> shaders;
    std::unordered_map<uint64_t,std::shared_ptr<Shader>> shaderSnapshots;
    std::unordered_map<std::string,std::shared_ptr<Shader>> shaderIdentifiers;
    std::unordered_map<uint64_t,std::shared_ptr<Pipeline>> pipelines;
    std::unordered_map<uint64_t,std::shared_ptr<Image>> images;
    std::unordered_map<uint64_t,std::shared_ptr<Buffer>> buffers;
    std::unordered_map<uint64_t,std::shared_ptr<Memory>> memories;
    bool memoryTrackingIncomplete=false;
    std::unordered_map<uint64_t,std::shared_ptr<View>> views;
    std::unordered_map<uint64_t,std::shared_ptr<Sampler>> samplers;
    std::unordered_map<uint64_t,std::shared_ptr<SetLayout>> setLayouts;
    std::unordered_map<uint64_t,std::shared_ptr<Layout>> layouts;
    std::unordered_map<uint64_t,std::shared_ptr<Set>> sets;
    std::unordered_map<uint64_t,DescriptorTemplate> templates;
    std::unordered_map<uint64_t,std::shared_ptr<Pass>> passes;
    std::unordered_map<uint64_t,Framebuffer> framebuffers;
    std::unordered_map<uint64_t,Command> commands;
    std::unordered_map<uint64_t,VkCommandPoolCreateInfo> commandPools;
    std::vector<std::weak_ptr<GpuUniformBatch>> uniformBatches;
    std::unordered_map<uint64_t,Frame> pending;
    std::vector<PipelineSummary> latest;
    std::shared_ptr<Capture> capture;
    uint64_t serial=0,submitSerial=0,frameSerial=0,captureSerial=0;
    uint32_t layoutResets=0;   // submits that dropped every image's layout record ([LAYOUT-RESET], logged the first 8 times)
    uint64_t selected=0;
    uint32_t requestedFrames=0,isolation=0,isolationFrames=0;
    uint64_t explorerUntil=0;   // frameSerial until which the Pipeline Explorer page counts as open (it lists the latest frame's draws)
    std::string scanDirectory;uint64_t scanSequence=0;uint32_t presentsSinceScanCheck=0;   // frame scan request (tools/frame-scan.py)
    bool scanUniforms=false;size_t scanUniformBytes=0;uint32_t scanUniformDraws=0,scanUniformShadowPassDraws=0;std::unordered_map<uint64_t,uint32_t> scanUniformPerPipeline;   // frame scan "uniforms"
    bool scanLights=false;uint32_t scanLightDraws=0;   // frame scan "lights"
    std::chrono::steady_clock::time_point isolationDeadline{};
    std::set<uint64_t> isolationTargets;
    size_t eventCount=0,shaderBytes=0;
    bool limits=false;
    std::map<std::string,uint64_t> shaderSources;
    std::shared_ptr<Inspector> inspector;
    std::vector<std::weak_ptr<PickQueries>> inspectorQueries;
    std::set<std::string> diagnosticKeys;
    bool inspectorFailureCaptured=false;
    std::shared_ptr<SceneDebugger> sceneDebugger;
    std::shared_ptr<FogState> fog;
    std::shared_ptr<DrawClassStats> drawClasses;   // draw_classifier.cpp
};
extern std::mutex mutex;
extern std::unordered_map<VkDevice,Device> devices;
Device& device_state(VkDevice);
std::string pnext_types(const void*);
void inspector_diagnostic(Device&,const std::string& key,const std::string& detail);
std::shared_ptr<Shader> retain_shader(Device&,const uint32_t*,size_t);
template<class T> auto find_ptr(const std::unordered_map<uint64_t,std::shared_ptr<T>>& map,uint64_t handle) {
    auto it=map.find(handle);return it==map.end()?std::shared_ptr<T>{}:it->second;
}
void set_target(Command&,uint32_t subpass);
void write_descriptors(Device&,Set&,uint32_t,const VkWriteDescriptorSet*);
void apply_template(Device&,Set&,const DescriptorTemplate&,const void*);
PFN_vkVoidFunction resource_hook(const char*);
PFN_vkVoidFunction memory_hook(const char*);
// Called under discovery::mutex, before forwarding the application's submit.
void capture_pick_uniforms(Device&,SubmittedDraw&);
// Shared validated-copy helper behind capture_pick_uniforms; also used by
// capture_scene_debugger_uniforms (scene_debugger.cpp) with its own budget
// counter. See the definition in inspector_memory.cpp for what it checks.
std::shared_ptr<UniformSnapshot> try_capture_uniform(Device&,const SubmittedDraw&,const ResourceUse&,size_t& budgetUsed,size_t budgetMax,bool allowWritableUsage=false);
struct Command;
// One 4-byte word of the uniform buffer bound at (set, binding, element 0) for the command's next draw, from host-coherent
// mapped memory (DXVK's spec_state / constant rings: the CPU wrote it before recording the draw). No draw snapshot, no
// descriptor walk - cheap enough for the record path (roadmap M4: the fast-link alpha test). False when not resolvable.
bool read_bound_uniform_word(const Device&,const Command&,uint32_t set,uint32_t binding,uint32_t byteOffset,uint32_t& value);
bool read_bound_uniform_bytes(const Device&,const Command&,uint32_t set,uint32_t binding,uint32_t byteOffset,uint32_t size,void* out);   // host-coherent mapped UBOs only
// The bound uniform buffer's window in host-coherent mapped memory (bytes: its readable/writable length), or nullptr.
uint8_t* bound_uniform_span(const Device&,const Command&,uint32_t set,uint32_t binding,uint64_t& bytes);
void capture_scene_debugger_uniforms(Device&,SubmittedDraw&);
// Camera HUD/gizmo/recording/reconstruction or the Camera page: the light
// per-draw hooks below run (world viewport, water readback), no draw copies.
bool scene_debugger_active(const Device&);
// Only while the Scene Debugger's Camera page is open: opts every draw into
// the full-record/resolve path Inspector armed/highlight uses (see
// draw_records_wanted) for generic per-shader matrix discovery. Costs several
// ms per frame at WoW's 2000+ draws, so nothing else turns it on.
bool scene_debugger_full_capture(const Device&);
// Record time, for draws that are NOT otherwise snapshotted: reads the world
// viewport/depth target straight from the command state, and says whether
// this draw must be snapshotted anyway (the water draw, for its UBO readback).
bool scene_debugger_peek_draw(Device&,const Command&);
// Called at RECORD time (from observe(), alongside inspector_observe_draw)
// for every draw while scene_debugger_active() - not just when a pick is
// armed. Recognizes the known WoW water shader specifically and schedules a
// GPU-side readback of its set2/binding0 buffer via
// schedule_gpu_uniform_read, since that buffer is expected to not be
// host-mappable in real WoW (see scene_debugger.cpp's WaterCameraAnchor).
// Cheap no-op for every other draw/shader.
void scene_debugger_observe_draw(const DeviceData&,Device&,Command&,const Draw&,VkCommandBuffer);
// Polls any in-flight water-anchor GPU readback and, once the copy has
// completed, derives the camera from it. Call once per frame (cheap: no-op
// unless a readback was actually scheduled).
void scene_debugger_poll(Device&);
bool uniform_dynamic_offset(const Draw&,const ResourceUse&,uint64_t&);
// After a private render pass recorded into the application's command buffer (water, fog): whether the
// application's graphics state can be put back exactly (empty = yes), and doing it.
std::string private_pass_restore_problem(const Device&,const Command&);
void restore_after_private_pass(const DeviceData&,VkCommandBuffer,const Command&);
// Deferred application draws (volumetric_fog.cpp, transparent-aware fog): whether a draw being recorded can be issued again
// later in the same command buffer with the application's own pipeline and exactly its state (null = yes), and doing it.
const char* app_draw_replay_problem(const Device&,const Command&,const Draw&);
void replay_app_draw(const DeviceData&,VkCommandBuffer,const Draw&,const WaterRestore&);
uint32_t queue_timestamp_bits(const DeviceData&,const Device&,const Command&);
PFN_vkVoidFunction command_hook(const char*);
Dynamic effective_state(const Draw&);
Dynamic effective_state(const Pipeline*,Dynamic);   // the pipeline's static state where it is not dynamic
uint32_t dynamic_state_mask(const std::set<VkDynamicState>&);   // the DynamicBits those dynamic states cover
void resolve_resources(const Device&,SubmittedDraw&);
void resolve_sampler_resources(const Device&,SubmittedDraw&,uint32_t,const Set&);
void revalidate_sampler_resources(const Device&,SubmittedDraw&);
bool unresolved_update_after_bind(const ResourceUse&);
bool coverage_resources_supported(const SubmittedDraw&);
Frame submitted_frame(VkDevice,VkQueue,uint32_t,const VkCommandBuffer*);
// Per-draw snapshots (recording) and per-draw resource resolution (submit) cost microseconds each and WoW records ~1700 draws per frame, so they are
// made only when something consumes them: an Inspector pick or highlight, an explorer capture/isolation/page, the Scene Debugger's Camera page, or the water replay of that draw.
bool draw_records_wanted(const Device&);
bool water_wants_draw(const Device&,const Pipeline*);
void accept_submit(VkDevice,VkQueue,Frame&&);
void finish_present(VkDevice,VkQueue,VkResult);
void export_capture(std::shared_ptr<const Capture>);
std::string capture_json(const Capture&);
void populate_explorer(VkDevice,OverlayUniform&);
bool explorer_input(uint32_t page,uint32_t control,int direction,bool activate);
}
