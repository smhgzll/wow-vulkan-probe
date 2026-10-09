#include "volumetric_fog.hpp"
#include "water_reflections.hpp"
#include <atomic>
#include <array>
#include <cstring>
#include "discovery_internal.hpp"
#include "surface_inspector.hpp"
#include "inspector_uniform_gpu.hpp"
#include "draw_probe.hpp"
#include "sync_tracking.hpp"
#include "game_soft_shadows.hpp"
#include <filesystem>

// These are the existing protected entry points. Wrappers forward through them.
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage*);
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice,VkImage,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView*);
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice,VkSwapchainKHR,uint32_t*,VkImage*);
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice,VkSwapchainKHR,const VkAllocationCallbacks*);
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*);
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer,const VkDependencyInfo*);
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue,const VkPresentInfoKHR*);
}
namespace depth_probe {
void invalidate_hook_cache(){discovery::invalidate_downstream_cache();}
bool discovery_enabled() {
    static const bool enabled=[] {const char* p=std::getenv("DEPTH_PROBE_PIPELINE_EXPLORER");const char* i=std::getenv("DEPTH_PROBE_SURFACE_INSPECTOR");return (p && !std::strcmp(p,"1")) || (i && !std::strcmp(i,"1"));}();
    return enabled;
}
}
namespace depth_probe::discovery {
std::mutex mutex;
std::unordered_map<VkDevice,Device> devices;
std::string pnext_types(const void* pointer) {
    std::string result="[";unsigned count=0;
    for(auto* n=static_cast<const VkBaseInStructure*>(pointer);n && count<32;n=n->pNext,++count) {
        if(count)result+=",";
        result+=std::to_string(n->sType);
    }
    return result+"]";
}
void inspector_diagnostic(Device& s,const std::string& key,const std::string& detail) {
    if(inspector_enabled() && s.diagnosticKeys.size()<64 && s.diagnosticKeys.insert(key).second)
        log_line("[INSPECTOR-DIAGNOSTIC] "+key+" "+detail);
}
std::shared_ptr<Shader> retain_shader(Device& s,const uint32_t* words,size_t size) {
    if(!words || size<20 || size%4 || size>4*1024*1024)return {};
    const auto hash=hash_bytes(words,size);
    if(auto found=find_ptr(s.shaderSnapshots,hash)) {
        if(found->words.size()*4==size && !std::memcmp(found->words.data(),words,size))return found;
        s.limits=true;return {}; // Never equate different bytes on a hash collision.
    }
    if(s.shaderSnapshots.size()>=maxPipelines || s.shaderBytes+size>64*1024*1024){s.limits=true;return {};}
    auto value=std::make_shared<Shader>(inspect_spirv(words,size));if(!value->valid)return {};
    s.shaderBytes+=size;s.shaderSnapshots[hash]=value;return value;
}
Device& device_state(VkDevice device) {
    auto [it,created]=devices.try_emplace(device);
    if(created) {
        if(const char* p=std::getenv("DEPTH_PROBE_DISCOVERY_CAPTURE_FRAMES"))it->second.requestedFrames=std::clamp(std::atoi(p),0,int(maxCaptureFrames));
        log_line("[DISCOVERY-READY] observational explorer; categories=unknown; bounded submit/present intervals; isolation=off");
        if(inspector_enabled())log_line("[INSPECTOR-DIAGNOSTICS-READY] revision=3 sampler-indices=push-constants clone-reasons=1 descriptor-paths=1 automatic-failure-capture=1");
    }
    return it->second;
}
namespace {std::atomic<uint64_t> downstreamGeneration{1};}
void invalidate_downstream_cache(){downstreamGeneration.fetch_add(1,std::memory_order_relaxed);}
PFN_vkVoidFunction cached_downstream(const DeviceData& data,const char* name) {
    struct Entry {VkDevice device=nullptr;PFN_vkGetDeviceProcAddr source=nullptr;uint64_t generation=0;PFN_vkVoidFunction function=nullptr;char name[48]{};};
    static thread_local std::array<Entry,512> cache;
    // The name is normally a string literal, so its address spreads well; the stored copy is compared anyway, which keeps a temporary's
    // recycled address from ever returning the wrong function.
    const auto hash=(reinterpret_cast<uintptr_t>(name)>>3)^(reinterpret_cast<uintptr_t>(data.device)>>5);
    auto& entry=cache[hash%cache.size()];
    const auto generation=downstreamGeneration.load(std::memory_order_relaxed);
    if(entry.generation==generation && entry.device==data.device && entry.source==data.next_gdpa && entry.function && std::strncmp(entry.name,name,sizeof(entry.name))==0)return entry.function;
    const auto function=downstream(data,name);
    if(function && std::strlen(name)<sizeof(entry.name)){entry.device=data.device;entry.source=data.next_gdpa;entry.generation=generation;entry.function=function;std::strcpy(entry.name,name);}
    return function;
}
PFN_vkVoidFunction downstream(const DeviceData& data,const char* name) {
    // Keeping these calls on their established path preserves candidate
    // tracking, acquire provenance, and the original submit/present protocol.
#define EXISTING(api) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(::api)
    EXISTING(vkCmdPipelineBarrier);EXISTING(vkCmdPipelineBarrier2);
    EXISTING(vkCreateImage);EXISTING(vkDestroyImage);EXISTING(vkCreateImageView);
    EXISTING(vkGetSwapchainImagesKHR);EXISTING(vkDestroySwapchainKHR);EXISTING(vkQueuePresentKHR);
#undef EXISTING
    if(auto hook=sync_hook(name))return hook;
    if(auto hook=probe_hook(name))return hook;
    return data.next_gdpa(data.device,name);
}
namespace {
// Soft shadows (game_soft_shadows.hpp): the settings for this game run, taken when the game builds its first shader.
SoftShadowSettings soft_shadow_settings(VkDevice d) {
    return soft_shadows_latch([d]{std::lock_guard lock(mutex);const auto& f=fog_settings_now(device_state(d));return SoftShadowSettings{f.softShadows,f.shadowFade,f.indoorShadows};});
}
// A shader's create info with the soft-shadow code in place of the game's (its own storage), or the game's own.
struct SoftShadowModule {
    VkShaderModuleCreateInfo copy{};std::shared_ptr<const SoftShadowShader> code;
    const VkShaderModuleCreateInfo* operator()(VkDevice d,const VkShaderModuleCreateInfo* info) {
        if(!info || !(code=soft_shadow_code(info->pCode,info->codeSize,soft_shadow_settings(d))))return info;
        copy=*info;copy.pCode=code->code.data();copy.codeSize=code->code.size()*4;return &copy;
    }
};
VKAPI_ATTR VkResult VKAPI_CALL shader(VkDevice d,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks* a,VkShaderModule* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    SoftShadowModule soft;const auto* patched=soft(d,info);
    auto result=next<PFN_vkCreateShaderModule>(*data,"vkCreateShaderModule")(d,patched,a,out);
    if(result<0 && patched!=info){soft_shadows_refused(result);result=next<PFN_vkCreateShaderModule>(*data,"vkCreateShaderModule")(d,info,a,out);}
    if(result==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);
        if(s.shaders.size()<maxPipelines)s.shaders[id(*out)]=retain_shader(s,info->pCode,info->codeSize);else s.limits=true;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroyShader(VkDevice d,VkShaderModule h,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);auto& s=device_state(d);s.shaders.erase(id(h));}
    // The cumulative shader-byte budget also covers snapshots retained by
    // pipelines/captures after a module is destroyed.
    next<PFN_vkDestroyShaderModule>(*data,"vkDestroyShaderModule")(d,h,a);
}
std::string identifier_key(uint32_t count,const uint8_t* bytes) {
    if(!bytes || !count || count>VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT)return {};
    return std::string(reinterpret_cast<const char*>(bytes),count);
}
void remember_identifier(Device& s,const VkShaderModuleIdentifierEXT& identifier,std::shared_ptr<Shader> shader) {
    const auto key=identifier_key(identifier.identifierSize,identifier.identifier);if(key.empty() || !shader)return;
    if(s.shaderIdentifiers.size()<maxPipelines || s.shaderIdentifiers.count(key))s.shaderIdentifiers[key]=std::move(shader);else s.limits=true;
}
VKAPI_ATTR void VKAPI_CALL moduleIdentifier(VkDevice device,VkShaderModule module,VkShaderModuleIdentifierEXT* result) {
    auto data=get_device_data(device);if(!data)return;
    next<PFN_vkGetShaderModuleIdentifierEXT>(*data,"vkGetShaderModuleIdentifierEXT")(device,module,result);
    std::lock_guard lock(mutex);auto& s=device_state(device);remember_identifier(s,*result,find_ptr(s.shaders,id(module)));
}
VKAPI_ATTR void VKAPI_CALL createInfoIdentifier(VkDevice device,const VkShaderModuleCreateInfo* info,VkShaderModuleIdentifierEXT* result) {
    auto data=get_device_data(device);if(!data)return;
    // The identifier of the code the driver will really get: DXVK looks pipelines up by it before compiling from inline code.
    SoftShadowModule soft;
    next<PFN_vkGetShaderModuleCreateInfoIdentifierEXT>(*data,"vkGetShaderModuleCreateInfoIdentifierEXT")(device,soft(device,info),result);
    std::lock_guard lock(mutex);auto& s=device_state(device);remember_identifier(s,*result,retain_shader(s,info->pCode,info->codeSize));
}
std::shared_ptr<Shader> stage_shader(Device& s,const VkPipelineShaderStageCreateInfo& stage,std::string& source) {
    std::shared_ptr<Shader> value;
    if(stage.module){value=find_ptr(s.shaders,id(stage.module));source=value?"module":"untracked-module";}
    else if(auto* inlineCode=chain<VkShaderModuleCreateInfo>(stage.pNext,VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)) {
        value=retain_shader(s,inlineCode->pCode,inlineCode->codeSize);source=value?"inline-spirv":"inline-invalid-or-budget";
    } else if(auto* identifier=chain<VkPipelineShaderStageModuleIdentifierCreateInfoEXT>(stage.pNext,VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)) {
        auto it=s.shaderIdentifiers.find(identifier_key(identifier->identifierSize,identifier->pIdentifier));
        if(it!=s.shaderIdentifiers.end())value=it->second;
        source=value?"identifier-snapshot":"identifier-bytes-unobserved";
    } else source="no-module-or-inline-code";
    if(++s.shaderSources[source]==1)log_line("[SHADER-IDENTITY-PATH] source="+source+" hash="+hex(value?value->hash:0));
    return value;
}
std::shared_ptr<Pipeline> pipeline_record(Device& s,VkPipeline handle,const VkGraphicsPipelineCreateInfo& info) {
    auto p=std::make_shared<Pipeline>();p->handle=id(handle);p->serial=++s.serial;
    const auto* library=chain<VkGraphicsPipelineLibraryCreateInfoEXT>(info.pNext,VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
    const auto* links=chain<VkPipelineLibraryCreateInfoKHR>(info.pNext,VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    const auto* flags2=chain<VkPipelineCreateFlags2CreateInfoKHR>(info.pNext,VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
    const uint64_t flags=flags2?flags2->flags:info.flags;
    const uint32_t own=library?library->flags:((flags&VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || (links && links->libraryCount))?0u:15u;
    p->parts=own;
    p->hash=14695981039346656037ull;p->layoutHandle=id(info.layout);p->layout=find_ptr(s.layouts,id(info.layout));
    p->fingerprintComplete=!(own&6) || (p->layout && p->layout->supported);
    bool simple=true;
    if(links) {
        std::vector<uint64_t> hashes;
        for(uint32_t i=0;i<links->libraryCount;++i) {
            auto l=find_ptr(s.pipelines,id(links->pLibraries[i]));
            if(!l){p->fingerprintComplete=false;continue;}
            hashes.push_back(l->hash);p->parts|=l->parts;p->fingerprintComplete&=l->fingerprintComplete;
            simple&=l->isolationSubsetSafe;p->replaySubset&=l->replaySubset;
            p->dynamic.insert(l->dynamic.begin(),l->dynamic.end());
            if(l->parts&1)p->topology=l->topology;
            if(l->parts&2) {
                p->vs=l->vs;p->vsEntry=l->vsEntry;p->vsSource=l->vsSource;p->cull=l->cull;p->frontFace=l->frontFace;p->rasterDiscard=l->rasterDiscard;
                p->depthBiasEnable=l->depthBiasEnable;p->biasConstant=l->biasConstant;p->biasClamp=l->biasClamp;p->biasSlope=l->biasSlope;
                p->viewport=l->viewport;p->scissor=l->scissor;
            }
            if(l->parts&4){p->fs=l->fs;p->fsEntry=l->fsEntry;p->fsSource=l->fsSource;p->depthTest=l->depthTest;p->depthWrite=l->depthWrite;p->compare=l->compare;p->stencil=l->stencil;p->alphaTest=l->alphaTest;}
            if(l->parts&8){p->blendSrc=l->blendSrc;p->blendDst=l->blendDst;p->blendKnown=l->blendKnown;p->blend=l->blend;p->colorMask=l->colorMask;p->samples=l->samples;p->logicOp=l->logicOp;p->alphaToCoverage=l->alphaToCoverage;}
            if(!p->layout && l->layout){p->layout=l->layout;p->layoutHandle=l->layoutHandle;}
        }
        std::sort(hashes.begin(),hashes.end());for(auto h:hashes)hash_value(p->hash,h);
    }
    hash_value(p->hash,p->layout?p->layout->signature:0);hash_value(p->hash,flags);hash_value(p->hash,info.subpass);
    std::vector<std::pair<uint32_t,uint64_t>> stages;
    for(uint32_t i=0;(own&6) && i<info.stageCount;++i) {
        const auto& stage=info.pStages[i];std::string source;auto shader=stage_shader(s,stage,source);
        uint64_t h=shader?shader->hash:0;
        if(!shader || !shader->valid)p->fingerprintComplete=false;
        if(stage.pName)h=hash_bytes(stage.pName,std::strlen(stage.pName),h);
        if(stage.pSpecializationInfo) {
            const auto& spec=*stage.pSpecializationInfo;
            for(uint32_t j=0;j<spec.mapEntryCount;++j) {
                const auto& e=spec.pMapEntries[j];hash_value(h,e.constantID);hash_value(h,e.size);
                if(e.offset<=spec.dataSize && e.size<=spec.dataSize-e.offset)h=hash_bytes(static_cast<const char*>(spec.pData)+e.offset,e.size,h);
                else p->fingerprintComplete=false;
            }
        }
        for(auto* n=static_cast<const VkBaseInStructure*>(stage.pNext);n;n=n->pNext)
            if(n->sType!=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && n->sType!=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)p->fingerprintComplete=false;
        if(stage.stage==VK_SHADER_STAGE_VERTEX_BIT){p->vs=shader;p->vsEntry=stage.pName?stage.pName:"";p->vsSource=source;}
        else if(stage.stage==VK_SHADER_STAGE_FRAGMENT_BIT){p->fs=shader;p->fsEntry=stage.pName?stage.pName:"";p->fsSource=source;p->alphaTest=dxvk_alpha_test_state(shader.get(),stage.pSpecializationInfo);}
        else simple=false;
        stages.emplace_back(stage.stage,h);
    }
    std::sort(stages.begin(),stages.end());for(auto [stage,h]:stages){hash_value(p->hash,stage);hash_value(p->hash,h);}
    if(own && info.pDynamicState)for(uint32_t i=0;i<info.pDynamicState->dynamicStateCount;++i)p->dynamic.insert(info.pDynamicState->pDynamicStates[i]);
    for(auto value:p->dynamic)hash_value(p->hash,value);
    p->dynamicMask=dynamic_state_mask(p->dynamic);p->dynamicMaskReady=true;
    if(auto* v=info.pDepthStencilState;(own&4) && v) {
        p->depthTest=v->depthTestEnable;p->depthWrite=v->depthWriteEnable;p->compare=v->depthCompareOp;p->stencil=v->stencilTestEnable;
        for(auto value:std::initializer_list<uint32_t>{v->depthBoundsTestEnable,v->front.failOp,v->front.passOp,v->front.depthFailOp,v->front.compareOp,
                       v->front.compareMask,v->front.writeMask,v->back.failOp,v->back.passOp,v->back.depthFailOp,v->back.compareOp,
                       v->back.compareMask,v->back.writeMask})hash_value(p->hash,uint32_t(value));
        if(v->pNext)p->fingerprintComplete=false;
    }
    if(auto* r=info.pRasterizationState;(own&2) && r) {
        p->cull=r->cullMode;p->frontFace=r->frontFace;p->rasterDiscard=r->rasterizerDiscardEnable;
        p->depthBiasEnable=r->depthBiasEnable;p->biasConstant=r->depthBiasConstantFactor;p->biasSlope=r->depthBiasSlopeFactor;p->biasClamp=r->depthBiasClamp;
        hash_value(p->hash,r->polygonMode);hash_value(p->hash,r->depthClampEnable);
        p->hash=hash_bytes(&r->depthBiasConstantFactor,4*sizeof(float),p->hash);
        for(auto* n=static_cast<const VkBaseInStructure*>(r->pNext);n;n=n->pNext) {
            if(n->sType==VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT) {
                const auto& clip=*reinterpret_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(n);
                hash_value(p->hash,n->sType);hash_value(p->hash,clip.flags);hash_value(p->hash,clip.depthClipEnable);
                if(clip.flags)p->fingerprintComplete=false;
            } else p->fingerprintComplete=false;
        }
    } else if(own&2)simple=false;
    if((own&1) && info.pInputAssemblyState){p->topology=info.pInputAssemblyState->topology;hash_value(p->hash,info.pInputAssemblyState->primitiveRestartEnable);}
    if(auto* v=info.pVertexInputState;(own&1) && v && !p->dynamic.count(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT)) {
        for(uint32_t i=0;i<v->vertexBindingDescriptionCount;++i){auto& b=v->pVertexBindingDescriptions[i];hash_value(p->hash,b.binding);hash_value(p->hash,b.stride);hash_value(p->hash,b.inputRate);}
        for(uint32_t i=0;i<v->vertexAttributeDescriptionCount;++i){auto& b=v->pVertexAttributeDescriptions[i];hash_value(p->hash,b.location);hash_value(p->hash,b.binding);hash_value(p->hash,b.format);hash_value(p->hash,b.offset);}
        if(v->pNext)p->fingerprintComplete=false;
    }
    if(auto* v=info.pViewportState;(own&2) && v) {
        if(!p->dynamic.count(VK_DYNAMIC_STATE_VIEWPORT) && !p->dynamic.count(VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) && v->viewportCount==1 && v->pViewports)p->viewport=v->pViewports[0];
        if(!p->dynamic.count(VK_DYNAMIC_STATE_SCISSOR) && !p->dynamic.count(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) && v->scissorCount==1 && v->pScissors)p->scissor=v->pScissors[0];
        hash_value(p->hash,v->viewportCount);hash_value(p->hash,v->scissorCount);
        if(v->viewportCount>1 || v->scissorCount>1)simple=false;
    }
    if(auto* v=info.pColorBlendState;(own&8) && v) {
        p->logicOp=v->logicOpEnable;
        hash_value(p->hash,v->logicOpEnable);hash_value(p->hash,v->logicOp);hash_value(p->hash,v->attachmentCount);
        const bool attachmentStateDynamic=p->dynamic.count(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT) && p->dynamic.count(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT) && p->dynamic.count(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
        if(attachmentStateDynamic || (v->attachmentCount && !v->pAttachments))p->fingerprintComplete=false;
        for(uint32_t i=0;!attachmentStateDynamic && v->pAttachments && i<v->attachmentCount;++i) {
            auto& a=v->pAttachments[i];p->blend|=a.blendEnable;p->colorMask|=a.colorWriteMask;
            if(a.blendEnable && !p->blendKnown){p->blendSrc=a.srcColorBlendFactor;p->blendDst=a.dstColorBlendFactor;p->blendKnown=true;}
            for(uint32_t x:{a.blendEnable,uint32_t(a.srcColorBlendFactor),uint32_t(a.dstColorBlendFactor),uint32_t(a.colorBlendOp),uint32_t(a.srcAlphaBlendFactor),uint32_t(a.dstAlphaBlendFactor),uint32_t(a.alphaBlendOp),a.colorWriteMask})hash_value(p->hash,x);
        }
        p->hash=hash_bytes(v->blendConstants,sizeof(v->blendConstants),p->hash);
        if(v->pNext)p->fingerprintComplete=false;
    }
    if(auto* v=info.pMultisampleState;((own&8) || ((own&4) && info.renderPass)) && v){p->alphaToCoverage=v->alphaToCoverageEnable;p->samples=v->rasterizationSamples;hash_value(p->hash,v->rasterizationSamples);hash_value(p->hash,v->sampleShadingEnable);hash_value(p->hash,v->alphaToCoverageEnable);if(v->pNext)p->fingerprintComplete=false;}
    if((own&14) && info.renderPass) {
        auto pass=find_ptr(s.passes,id(info.renderPass));
        if(pass){for(auto& a:pass->attachments){hash_value(p->hash,a.format);hash_value(p->hash,a.samples);}}
        else p->fingerprintComplete=false;
    }
    for(auto* n=static_cast<const VkBaseInStructure*>(info.pNext);n;n=n->pNext) {
        if(n->sType==VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            const auto* r=reinterpret_cast<const VkPipelineRenderingCreateInfo*>(n);
            if(own&14)hash_value(p->hash,r->viewMask);
            if(own&4){hash_value(p->hash,r->depthAttachmentFormat);hash_value(p->hash,r->stencilAttachmentFormat);}
            if(own&8)for(uint32_t i=0;i<r->colorAttachmentCount;++i)hash_value(p->hash,r->pColorAttachmentFormats[i]);
            if(r->viewMask)simple=false;
        } else if(n->sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT &&
                  n->sType!=VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR &&
                  n->sType!=VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO && n->sType!=VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)p->fingerprintComplete=false;
    }
    for(uint32_t value:{p->depthTest,p->depthWrite,p->compare,p->blend,p->cull,p->frontFace,p->topology,p->depthBiasEnable,p->stencil,p->rasterDiscard})hash_value(p->hash,value);
    p->isolationSubsetSafe=simple;
    p->isolationSafe=simple && !(flags&VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) && p->parts==15 && p->layout && p->layout->supported && p->vs && p->fs && p->vs->isolationSafe && p->fs->isolationSafe && p->fingerprintComplete && !p->stencil;
    capture_water_recipe(s,*p,info);
    return p;
}
VKAPI_ATTR VkResult VKAPI_CALL graphics(VkDevice d,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkAllocationCallbacks* a,VkPipeline* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto create=next<PFN_vkCreateGraphicsPipelines>(*data,"vkCreateGraphicsPipelines");
    const auto softSettings=soft_shadow_settings(d);
    SoftShadowPipelines soft(count,infos,softSettings);
    auto r=create(d,cache,count,soft.infos()?soft.infos():infos,a,out);
    if(r<0 && soft.infos()) {   // the driver refused the patched code: the game's own, from now on
        soft_shadows_refused(r);
        for(uint32_t i=0;i<count;++i)if(out[i]){next<PFN_vkDestroyPipeline>(*data,"vkDestroyPipeline")(d,out[i],a);out[i]=VK_NULL_HANDLE;}
        r=create(d,cache,count,infos,a,out);
    }
    std::lock_guard lock(mutex);auto& s=device_state(d);
    for(uint32_t i=0;i<count;++i)if(out[i]) {
        if(s.pipelines.size()<maxPipelines){auto p=pipeline_record(s,out[i],infos[i]);
            if(p->fs)if(auto patched=soft_shadow_code(p->fs->words.data(),p->fs->words.size()*4,softSettings);patched && patched->live){p->softShadowSet=int32_t(patched->set);p->softShadowBinding=patched->binding;}
            if(inspector_enabled())create_inspector_clones(*data,s,*p,infos[i]);
            s.pipelines[id(out[i])]=std::move(p);}else s.limits=true;
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice d,VkPipeline h,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);auto& s=device_state(d);auto p=find_ptr(s.pipelines,id(h));if(p && p->inspector)p->inspector->release();s.pipelines.erase(id(h));}
    next<PFN_vkDestroyPipeline>(*data,"vkDestroyPipeline")(d,h,a);
}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q,uint32_t count,const VkSubmitInfo* infos,VkFence fence) {
    auto data=get_device_data(reinterpret_cast<VkDevice>(q));if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    Frame observed;observed.queue=id(q);
    for(uint32_t i=0;i<count;++i) {
        auto frame=submitted_frame(data->device,q,infos[i].commandBufferCount,infos[i].pCommandBuffers);
        for(auto& batch:frame.uniformBatches)if(std::find(observed.uniformBatches.begin(),observed.uniformBatches.end(),batch)==observed.uniformBatches.end())observed.uniformBatches.push_back(std::move(batch));
        observed.missingCommands+=frame.missingCommands;observed.truncated|=frame.truncated;
        for(auto& event:frame.layouts)if(observed.layouts.size()<maxEvents)observed.layouts.push_back(std::move(event));else observed.truncated=true;
        for(auto& draw:frame.draws)if(observed.draws.size()<maxFrameDraws)observed.draws.push_back(std::move(draw));else observed.truncated=true;
    }
    {std::vector<VkCommandBuffer> all;for(uint32_t i=0;i<count;++i)all.insert(all.end(),infos[i].pCommandBuffers,infos[i].pCommandBuffers+infos[i].commandBufferCount);
     fog_before_submit(*data,q,all);}   // the fog volume must be computed before this batch composites it
    auto uniformBatches=begin_uniform_submit(observed);
    auto result=next<PFN_vkQueueSubmit>(*data,"vkQueueSubmit")(q,count,infos,fence);
    end_uniform_submit(uniformBatches,result);
    if(result==VK_SUCCESS)accept_submit(data->device,q,std::move(observed));
    return result;
}
VkResult submit2_impl(VkQueue q,uint32_t count,const VkSubmitInfo2* infos,VkFence fence,const char* name) {
    auto data=get_device_data(reinterpret_cast<VkDevice>(q));if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    Frame observed;observed.queue=id(q);
    for(uint32_t i=0;i<count;++i) {
        std::vector<VkCommandBuffer> commands;
        for(uint32_t j=0;j<infos[i].commandBufferInfoCount;++j)commands.push_back(infos[i].pCommandBufferInfos[j].commandBuffer);
        auto frame=submitted_frame(data->device,q,commands.size(),commands.data());
        for(auto& batch:frame.uniformBatches)if(std::find(observed.uniformBatches.begin(),observed.uniformBatches.end(),batch)==observed.uniformBatches.end())observed.uniformBatches.push_back(std::move(batch));
        observed.missingCommands+=frame.missingCommands;observed.truncated|=frame.truncated;
        for(auto& event:frame.layouts)if(observed.layouts.size()<maxEvents)observed.layouts.push_back(std::move(event));else observed.truncated=true;
        for(auto& draw:frame.draws)if(observed.draws.size()<maxFrameDraws)observed.draws.push_back(std::move(draw));else observed.truncated=true;
    }
    {std::vector<VkCommandBuffer> all;for(uint32_t i=0;i<count;++i)for(uint32_t j=0;j<infos[i].commandBufferInfoCount;++j)all.push_back(infos[i].pCommandBufferInfos[j].commandBuffer);
     fog_before_submit(*data,q,all);}
    auto uniformBatches=begin_uniform_submit(observed);
    auto result=next<PFN_vkQueueSubmit2>(*data,name)(q,count,infos,fence);
    end_uniform_submit(uniformBatches,result);
    if(result==VK_SUCCESS)accept_submit(data->device,q,std::move(observed));
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL submit2(VkQueue q,uint32_t n,const VkSubmitInfo2* i,VkFence f){return submit2_impl(q,n,i,f,"vkQueueSubmit2");}
VKAPI_ATTR VkResult VKAPI_CALL submit2KHR(VkQueue q,uint32_t n,const VkSubmitInfo2* i,VkFence f){return submit2_impl(q,n,i,f,"vkQueueSubmit2KHR");}
VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue q,const VkPresentInfoKHR* info) {
    auto data=get_device_data(reinterpret_cast<VkDevice>(q));if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    {std::lock_guard lock(mutex);fog_present(device_state(data->device));}
    inspector_present_begin(data->device,q,info);
    const auto result=next<PFN_vkQueuePresentKHR>(*data,"vkQueuePresentKHR")(q,info);
    finish_present(data->device,q,result);return result;
}
}
}
namespace depth_probe {
void destroy_device_discovery(VkDevice device) {
    if(!discovery_enabled())return;
    std::lock_guard lock(discovery::mutex);auto it=discovery::devices.find(device);
    if(it!=discovery::devices.end()){
        discovery::destroy_water_device(it->second);
        discovery::release_gpu_uniforms(it->second);
        discovery::fog_destroy(it->second);
        for(auto& [h,p]:it->second.pipelines){(void)h;if(p->inspector)p->inspector->release();}
        for(auto& weak:it->second.inspectorQueries)if(auto q=weak.lock())q->release();
        discovery::devices.erase(it);
    }
}
PFN_vkVoidFunction discovery_hook(const char* name) {
    if(!discovery_enabled())return nullptr;
    using namespace discovery;
#define HOOK(api,fn) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(fn)
    HOOK(vkCreateShaderModule,shader);HOOK(vkDestroyShaderModule,destroyShader);
    HOOK(vkGetShaderModuleIdentifierEXT,moduleIdentifier);HOOK(vkGetShaderModuleCreateInfoIdentifierEXT,createInfoIdentifier);
    HOOK(vkCreateGraphicsPipelines,graphics);HOOK(vkDestroyPipeline,destroyPipeline);
    HOOK(vkQueueSubmit,submit);HOOK(vkQueueSubmit2,submit2);HOOK(vkQueueSubmit2KHR,submit2KHR);HOOK(vkQueuePresentKHR,present);
#undef HOOK
    if(auto hook=resource_hook(name))return hook;
    if(inspector_enabled())if(auto hook=memory_hook(name))return hook;
    return command_hook(name);
}
}
