#include "surface_inspector.hpp"
#include "inspector_uniform_gpu.hpp"

namespace depth_probe {
bool inspector_enabled() {static const bool enabled=[] {const char* p=std::getenv("DEPTH_PROBE_SURFACE_INSPECTOR");return p && !std::strcmp(p,"1");}();return enabled;}
}
namespace depth_probe::discovery {
namespace {template<class T> T raw(const DeviceData& d,const char* name){return reinterpret_cast<T>(d.next_gdpa(d.device,name));}}
Inspector& inspector_state(Device& s,VkDevice device) {
    if(!s.inspector){s.inspector=std::make_shared<Inspector>();s.inspector->device=device;}
    return *s.inspector;
}
void PickQueries::poll() {
    if(!used || !pool)return;
    results(device,pool,0,used,used*sizeof(values[0]),values.data(),sizeof(values[0]),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
}
// These clones retain the original shader, layout and raster state. The query
// variant disables all attachment writes; the tint variant changes only RGB
// scaling of a single opaque original draw. No application create-info is edited.
void create_inspector_clones(const DeviceData& data,Device& s,Pipeline& p,const VkGraphicsPipelineCreateInfo& original) {
    auto ci=original;
    auto flags2=chain<VkPipelineCreateFlags2CreateInfoKHR>(ci.pNext,VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
    const uint64_t flags=flags2?flags2->flags:ci.flags;
    const auto* library=chain<VkGraphicsPipelineLibraryCreateInfoEXT>(ci.pNext,VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
    const auto* links=chain<VkPipelineLibraryCreateInfoKHR>(ci.pNext,VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    const auto* rendering=chain<VkPipelineRenderingCreateInfo>(ci.pNext,VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    const uint32_t own=library?library->flags:((flags&VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || (links && links->libraryCount))?0u:15u;
    p.cloneInfo="flags="+hex(flags)+" own="+std::to_string(own)+" parts="+std::to_string(p.parts)+
        " top="+pnext_types(ci.pNext)+" raster="+pnext_types((own&2)&&ci.pRasterizationState?ci.pRasterizationState->pNext:nullptr)+
        " viewport="+pnext_types((own&2)&&ci.pViewportState?ci.pViewportState->pNext:nullptr)+
        " depth="+pnext_types((own&4)&&ci.pDepthStencilState?ci.pDepthStencilState->pNext:nullptr)+
        " blend="+pnext_types((own&8)&&ci.pColorBlendState?ci.pColorBlendState->pNext:nullptr);
    for(uint32_t i=0;(own&6)&&i<ci.stageCount;++i)p.cloneInfo+=" stage"+std::to_string(i)+"="+pnext_types(ci.pStages[i].pNext);
    std::vector<std::string> reasons;
    auto reject=[&](const std::string& reason){if(reasons.size()<24)reasons.push_back(reason);};
    auto report=[&] {
        inspector_diagnostic(s,"clone-query:"+p.queryStatus,"pipeline="+std::to_string(p.serial)+" "+p.cloneInfo+" tint="+p.tintStatus);
    };
    if(s.pipelines.size()>=1024){p.queryStatus=p.tintStatus="budget:live-pipelines>=1024";report();return;}
    if(!p.replaySubset)reject("linked-library-subset-rejected");
    // Extensions whose attachment/coverage semantics are not reconstructed are
    // explicitly excluded. They still have observational discovery metadata.
    for(auto* n=static_cast<const VkBaseInStructure*>(ci.pNext);n;n=n->pNext)
        if(n->sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT && n->sType!=VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR &&
           n->sType!=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && n->sType!=VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO &&
           n->sType!=VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)reject("pipeline-pnext:"+std::to_string(n->sType));
    if(flags & ~(uint64_t(VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)|VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT|VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT|
                 VK_PIPELINE_CREATE_DERIVATIVE_BIT|VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT|VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT|
                 VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT|VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT))reject("pipeline-flags:"+hex(flags));
    if(own&2) {
        if(!ci.pRasterizationState || !ci.pViewportState)reject("missing-raster-or-viewport-state");
        // Preserve static depth clipping exactly. DXVK uses this pNext even
        // for ordinary triangles; rejecting every raster pNext excluded them.
        if(ci.pRasterizationState)for(auto* n=static_cast<const VkBaseInStructure*>(ci.pRasterizationState->pNext);n;n=n->pNext) {
            if(n->sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT)
                reject("raster-pnext:"+std::to_string(n->sType));
            else if(reinterpret_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(n)->flags)
                reject("depth-clip-flags");
        }
        if(ci.pViewportState && (ci.pViewportState->pNext || ci.pViewportState->viewportCount>1 || ci.pViewportState->scissorCount>1))reject("viewport-state");
    }
    if((own&4) && ci.pDepthStencilState && ci.pDepthStencilState->pNext)reject("depth-pnext");
    if((own&8) && (!ci.pColorBlendState || ci.pColorBlendState->pNext || ci.pColorBlendState->attachmentCount!=1 || !ci.pColorBlendState->pAttachments || ci.pColorBlendState->logicOpEnable))reject("color-attachment-state");
    if(ci.pMultisampleState && ((own&8)||((own&4)&&ci.renderPass)) && (ci.pMultisampleState->pNext || ci.pMultisampleState->rasterizationSamples!=VK_SAMPLE_COUNT_1_BIT))reject("multisample-state");
    if(rendering && rendering->viewMask)reject("multiview");
    for(auto state:p.dynamic) {
        switch(state) {
        case VK_DYNAMIC_STATE_VIEWPORT:case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:case VK_DYNAMIC_STATE_SCISSOR:case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
        case VK_DYNAMIC_STATE_DEPTH_BIAS:case VK_DYNAMIC_STATE_BLEND_CONSTANTS:case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
        case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
        case VK_DYNAMIC_STATE_CULL_MODE:case VK_DYNAMIC_STATE_FRONT_FACE:case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
        case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:case VK_DYNAMIC_STATE_STENCIL_OP:
        case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
        case VK_DYNAMIC_STATE_LOGIC_OP_EXT:case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:break;
        default:reject("dynamic-state:"+std::to_string(state));
        }
    }
    for(uint32_t i=0;(own&6)&&i<ci.stageCount;++i)if(ci.pStages[i].stage!=VK_SHADER_STAGE_VERTEX_BIT && ci.pStages[i].stage!=VK_SHADER_STAGE_FRAGMENT_BIT)reject("shader-stage:"+std::to_string(ci.pStages[i].stage));
    for(uint32_t i=0;(own&6)&&i<ci.stageCount;++i)for(auto* n=static_cast<const VkBaseInStructure*>(ci.pStages[i].pNext);n;n=n->pNext)
        if(n->sType!=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && n->sType!=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)reject("shader-pnext:"+std::to_string(n->sType));
    p.replaySubset=reasons.empty();
    if(!p.replaySubset) {
        p.queryStatus="rejected";for(const auto& reason:reasons)p.queryStatus+=";"+reason;
        p.tintStatus=p.queryStatus;report();return;
    }
    // Identifier-only stages require FAIL_ON_PIPELINE_COMPILE_REQUIRED. Private
    // variants instead use retained SPIR-V in ordinary temporary modules, so a
    // clone cache miss is legal and never changes the application's cache call.
    struct Modules {
        VkDevice device{};PFN_vkDestroyShaderModule destroy{};std::vector<VkShaderModule> handles;
        ~Modules(){for(auto h:handles)destroy(device,h,nullptr);}
    } modules{data.device,raw<PFN_vkDestroyShaderModule>(data,"vkDestroyShaderModule"),{}};
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for(uint32_t i=0;(own&6)&&i<ci.stageCount;++i) {
        auto stage=ci.pStages[i];auto shader=stage.stage==VK_SHADER_STAGE_VERTEX_BIT?p.vs:p.fs;
        if(!shader || shader->words.empty()){p.queryStatus=p.tintStatus="missing-shader-snapshot";report();return;}
        VkShaderModuleCreateInfo module{};module.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;module.codeSize=shader->words.size()*4;module.pCode=shader->words.data();
        VkShaderModule handle{};auto moduleResult=raw<PFN_vkCreateShaderModule>(data,"vkCreateShaderModule")(data.device,&module,nullptr,&handle);
        if(moduleResult!=VK_SUCCESS){p.queryStatus=p.tintStatus="shader-module-result:"+std::to_string(moduleResult);report();return;}
        modules.handles.push_back(handle);stage.module=handle;stage.pNext=nullptr;stages.push_back(stage);
    }
    if(own&6){ci.stageCount=stages.size();ci.pStages=stages.data();}
    auto clone=std::make_shared<InspectorPipeline>();clone->device=data.device;clone->destroy=raw<PFN_vkDestroyPipeline>(data,"vkDestroyPipeline");
    clone->dynamicScissorCount=p.dynamic.count(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);
    auto create=raw<PFN_vkCreateGraphicsPipelines>(data,"vkCreateGraphicsPipelines");
    for(unsigned variant=0;variant<2;++variant) {
        // Tinting must not disturb dynamic blend constants in the original draw.
        auto& status=variant?p.tintStatus:p.queryStatus;
        if(variant && p.dynamic.count(VK_DYNAMIC_STATE_BLEND_CONSTANTS)){status="dynamic-blend-constants";continue;}
        VkGraphicsPipelineCreateInfo v=ci;v.pNext=nullptr;v.basePipelineHandle=VK_NULL_HANDLE;v.basePipelineIndex=-1;
        auto append=[&](auto& value){value.pNext=v.pNext;v.pNext=&value;};
        VkGraphicsPipelineLibraryCreateInfoEXT lib{};if(library){lib=*library;append(lib);}
        VkPipelineRenderingCreateInfo render{};if(rendering){render=*rendering;append(render);}
        VkPipelineLibraryCreateInfoKHR link{};std::vector<VkPipeline> linked;
        if(links) {
            bool ready=true;
            for(uint32_t i=0;i<links->libraryCount;++i) {
                auto l=find_ptr(s.pipelines,id(links->pLibraries[i]));auto h=l&&l->inspector?(variant?l->inspector->tint:l->inspector->query):VK_NULL_HANDLE;
                if(!h){status="linked-clone-missing:"+std::to_string(l?l->serial:0)+":"+
                    (l?(variant?l->tintStatus:l->queryStatus).substr(0,240):"untracked-library");ready=false;break;}linked.push_back(h);
            }
            if(!ready)continue;
            link=*links;link.pLibraries=linked.data();append(link);
        }
        const auto newFlags=flags&~uint64_t(VK_PIPELINE_CREATE_DERIVATIVE_BIT|VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT|VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);
        v.flags=VkPipelineCreateFlags(newFlags);
        VkPipelineCreateFlags2CreateInfoKHR f2{};if(flags2){f2=*flags2;f2.flags=newFlags;append(f2);}
        VkPipelineDepthStencilStateCreateInfo ds{};if((own&4)&&ci.pDepthStencilState){ds=*ci.pDepthStencilState;if(!variant)ds.depthWriteEnable=VK_FALSE;v.pDepthStencilState=&ds;}
        VkPipelineColorBlendStateCreateInfo cb{};VkPipelineColorBlendAttachmentState attachment{};
        if((own&8)&&ci.pColorBlendState) {
            cb=*ci.pColorBlendState;attachment=cb.pAttachments[0];cb.pAttachments=&attachment;v.pColorBlendState=&cb;
            if(!variant)attachment.colorWriteMask=0;
            else {
                attachment.blendEnable=VK_TRUE;attachment.srcColorBlendFactor=VK_BLEND_FACTOR_CONSTANT_COLOR;attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ZERO;attachment.colorBlendOp=VK_BLEND_OP_ADD;
                attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;attachment.alphaBlendOp=VK_BLEND_OP_ADD;
                cb.blendConstants[0]=1;cb.blendConstants[1]=1;cb.blendConstants[2]=.35f;cb.blendConstants[3]=1;
            }
        }
        std::vector<VkDynamicState> dynamics;
        if(ci.pDynamicState)for(uint32_t i=0;i<ci.pDynamicState->dynamicStateCount;++i) {
            auto state=ci.pDynamicState->pDynamicStates[i];
            if(!variant && state==VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE)continue;
            dynamics.push_back(state);
        }
        if(!variant && (own&2) && !p.dynamic.count(VK_DYNAMIC_STATE_SCISSOR) && !clone->dynamicScissorCount)dynamics.push_back(VK_DYNAMIC_STATE_SCISSOR);
        VkPipelineDynamicStateCreateInfo dyn{};dyn.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;dyn.dynamicStateCount=dynamics.size();dyn.pDynamicStates=dynamics.data();v.pDynamicState=&dyn;
        VkPipeline out{};auto result=create(data.device,VK_NULL_HANDLE,1,&v,nullptr,&out);
        status=result==VK_SUCCESS?"ready":"pipeline-create-result:"+std::to_string(result);
        if(result==VK_SUCCESS){if(variant)clone->tint=out;else clone->query=out;}else if(out)clone->destroy(data.device,out,nullptr);
    }
    if(clone->query || clone->tint)p.inspector=std::move(clone);
    report();
}
void inspector_begin_command(const DeviceData& data,Device& s,Command& c,VkCommandBuffer cmd) {
    if(!inspector_enabled() || !s.inspector || !s.inspector->armed || s.inspector->pools>=16 || c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY || !(c.usage&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))return;
    std::erase_if(s.inspectorQueries,[](auto& weak){return weak.expired();});if(s.inspectorQueries.size()>=32)return;
    auto create=raw<PFN_vkCreateQueryPool>(data,"vkCreateQueryPool");auto reset=raw<PFN_vkCmdResetQueryPool>(data,"vkCmdResetQueryPool");
    if(!create || !reset)return;
    auto q=std::make_shared<PickQueries>();q->device=data.device;q->destroy=raw<PFN_vkDestroyQueryPool>(data,"vkDestroyQueryPool");q->results=raw<PFN_vkGetQueryPoolResults>(data,"vkGetQueryPoolResults");q->ticket=s.inspector->ticket;
    if(!q->destroy || !q->results)return;
    VkQueryPoolCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;info.queryType=VK_QUERY_TYPE_OCCLUSION;info.queryCount=q->values.size();
    if(create(data.device,&info,nullptr,&q->pool)!=VK_SUCCESS)return;
    s.inspectorQueries.push_back(q);
    reset(cmd,q->pool,0,info.queryCount);c.pick=std::move(q);++s.inspector->pools;
}
static bool same_depth(const Draw& d,const Inspector& in) {
    auto v=d.target.depth;return v && v->image && v->image->handle==in.depthImage;
}
static bool contains(const VkRect2D& r,uint32_t x,uint32_t y) {
    return int64_t(x)>=r.offset.x && int64_t(y)>=r.offset.y && int64_t(x)<int64_t(r.offset.x)+r.extent.width && int64_t(y)<int64_t(r.offset.y)+r.extent.height;
}
static bool safe_draw(const Command& c,const Draw& d) {
    auto p=d.pipeline;if(!p || !p->inspector || !p->inspector->query || !p->vs || !p->fs || !p->vs->replaySafe || !p->fs->replaySafe)return false;
    const auto e=effective_state(d);
    if(p->fs->earlyTests && p->alphaToCoverage)return false;
    return c.level==VK_COMMAND_BUFFER_LEVEL_PRIMARY && (c.usage&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) &&
        !c.activeQueries && !c.conditional && !c.transformFeedback && !d.unsupported && !d.skipped && d.kind<=1 &&
        d.target.known && d.target.colors.size()==1 && d.target.width && d.target.height && p->samples==1 && !p->logicOp &&
        (e.valid&(Scissor|Stencil|DepthWrite|Blend))==(Scissor|Stencil|DepthWrite|Blend) && !e.stencil;
}
void inspector_observe_draw(const DeviceData& data,Device& s,Command& c,Draw& d,VkCommandBuffer cmd) {
    if(!s.inspector || !s.inspector->armed || !same_depth(d,*s.inspector))return;
    auto& in=*s.inspector;d.pickTicket=in.ticket;
    if(!c.pick || c.pick->ticket!=in.ticket || c.pick->used>=c.pick->values.size() || !safe_draw(c,d))return;
    SubmittedDraw resources;resources.draw=d;resolve_resources(s,resources);
    if(!coverage_resources_supported(resources))return;
    const auto e=effective_state(d);if(!contains(e.scissor,in.x,in.y)){d.query=UINT32_MAX-1;return;}
    VkRect2D scissor{{int32_t(in.x),int32_t(in.y)},{1,1}};
    auto bind=raw<PFN_vkCmdBindPipeline>(data,"vkCmdBindPipeline");
    auto setScissor=raw<PFN_vkCmdSetScissor>(data,"vkCmdSetScissor");
    auto setCount=raw<PFN_vkCmdSetScissorWithCount>(data,"vkCmdSetScissorWithCount");if(!setCount)setCount=raw<PFN_vkCmdSetScissorWithCount>(data,"vkCmdSetScissorWithCountEXT");
    auto setDepth=raw<PFN_vkCmdSetDepthWriteEnable>(data,"vkCmdSetDepthWriteEnable");if(!setDepth)setDepth=raw<PFN_vkCmdSetDepthWriteEnable>(data,"vkCmdSetDepthWriteEnableEXT");
    if(d.pipeline->inspector->dynamicScissorCount && !setCount)return;
    if(d.pipeline->dynamic.count(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE) && !setDepth)return;
    auto begin=raw<PFN_vkCmdBeginQuery>(data,"vkCmdBeginQuery");auto end=raw<PFN_vkCmdEndQuery>(data,"vkCmdEndQuery");if(!bind || !setScissor || !begin || !end)return;
    d.pick=c.pick;d.query=c.pick->used++;
    bind(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,d.pipeline->inspector->query);
    if(d.pipeline->inspector->dynamicScissorCount)setCount(cmd,1,&scissor);else setScissor(cmd,0,1,&scissor);
    // BEFORE the original: LESS/coplanar rejection and shader discard stay exact.
    begin(cmd,c.pick->pool,d.query,0);
    if(d.kind==0)raw<PFN_vkCmdDraw>(data,"vkCmdDraw")(cmd,d.count,d.instances,d.first,d.firstInstance);
    else raw<PFN_vkCmdDrawIndexed>(data,"vkCmdDrawIndexed")(cmd,d.count,d.instances,d.first,d.vertexOffset,d.firstInstance);
    end(cmd,c.pick->pool,d.query);
    bind(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkPipeline)d.pipeline->handle);
    if(d.pipeline->inspector->dynamicScissorCount)setCount(cmd,1,&e.scissor);
    else if(d.pipeline->dynamic.count(VK_DYNAMIC_STATE_SCISSOR))setScissor(cmd,0,1,&e.scissor);
    if(d.pipeline->dynamic.count(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE))setDepth(cmd,e.depthWrite);
    schedule_gpu_uniforms(data,s,c,d,resources.resources);
}
uint64_t texture_identity(const View& v) {
    if(!v.image)return 0;
    const auto& im=*v.image;uint64_t h=14695981039346656037ull;
    // Session/generation identity, deliberately NOT a cross-session content hash.
    for(auto x:{im.generation,uint64_t(im.format),uint64_t(im.extent.width),uint64_t(im.extent.height),uint64_t(im.mips),uint64_t(im.layers),uint64_t(v.format),uint64_t(v.range.baseMipLevel),uint64_t(v.range.levelCount),uint64_t(v.range.baseArrayLayer),uint64_t(v.range.layerCount)})hash_value(h,x);
    h=hash_bytes(&v.components,sizeof(v.components),h);return h;
}
uint64_t mesh_identity(const Draw& d) {
    uint64_t h=14695981039346656037ull;
    for(const auto& b:d.vertices)for(auto x:{b.generation,b.offset,b.size,uint64_t(b.stride)})hash_value(h,x);
    for(auto x:{d.index.generation,d.index.offset,uint64_t(d.indexType),uint64_t(d.kind),uint64_t(d.count),uint64_t(d.instances),uint64_t(d.first),uint64_t(uint32_t(d.vertexOffset)),uint64_t(d.firstInstance)})hash_value(h,x);
    return h;
}
uint64_t material_identity(const SubmittedDraw& d) {
    if(!d.draw.pipeline || !d.draw.pipeline->fs || d.descriptorsIncomplete)return 0;
    uint64_t h=d.draw.pipeline->fs->hash;const auto e=effective_state(d.draw);
    for(auto x:{e.depthTest,e.depthWrite,e.compare,e.blend,e.cull,e.topology})hash_value(h,x);
    for(const auto& r:d.resources) {
        if(!r.descriptor.known || unresolved_update_after_bind(r))return 0;
        hash_value(h,r.set);hash_value(h,r.binding);hash_value(h,r.element);
        if(r.descriptor.view)hash_value(h,texture_identity(*r.descriptor.view));
        if(r.descriptor.sampler){hash_value(h,r.descriptor.sampler->signature);hash_value(h,r.descriptor.sampler->generation);}
    }
    return h;
}
VkPipeline inspector_tint(const DeviceData&,Device& s,const Draw& d) {
    if(!s.inspector || !s.inspector->highlight || !s.inspector->selected)return VK_NULL_HANDLE;
    const auto& selected=*s.inspector->selected;
    if(!s.inspector->materialIdentity)return VK_NULL_HANDLE;
    // Match a recording position AND resources/mesh. This intentionally refuses
    // reordered draws; a pipeline-wide filter would tint unrelated objects.
    if(!d.pipeline || !d.pipeline->inspector || !d.pipeline->inspector->tint || d.pipeline->serial!=selected.draw.pipeline->serial ||
       d.ordinal!=selected.draw.ordinal || mesh_identity(d)!=s.inspector->meshIdentity || !same_depth(d,*s.inspector))return VK_NULL_HANDLE;
    SubmittedDraw now;now.draw=d;resolve_resources(s,now);
    if(material_identity(now)!=s.inspector->materialIdentity || now.descriptorsIncomplete)return VK_NULL_HANDLE;
    if(now.resources.size()!=selected.resources.size())return VK_NULL_HANDLE;
    for(size_t set=0;set<maxSets;++set)if(d.sets[set].offsets!=selected.draw.sets[set].offsets)return VK_NULL_HANDLE;
    for(size_t i=0;i<now.resources.size();++i)if(now.resources[i].descriptor.buffer) {
        if(i>=selected.resources.size())return VK_NULL_HANDLE;
        auto& a=now.resources[i].descriptor;auto& b=selected.resources[i].descriptor;
        if(a.buffer!=b.buffer || a.offset!=b.offset || a.range!=b.range)return VK_NULL_HANDLE;
    }
    const auto e=effective_state(d);
    if(d.unsupported || d.kind>1 || !(e.valid&Blend) || e.blend || !d.pipeline->vs || !d.pipeline->fs || !d.pipeline->vs->replaySafe || !d.pipeline->fs->replaySafe)return VK_NULL_HANDLE;
    return d.pipeline->inspector->tint;
}
const char* inspector_selection_scope(const Inspector& in) {
    return in.geometryCandidate?"depth-tested-geometry-candidate":in.finalVisible?"visible-swapchain-pixel":"selected-scene-depth-only";
}
static void describe_selection(Inspector& in,uint64_t frame) {
    const auto& d=*in.selected;in.drawIdentity=14695981039346656037ull;
    for(auto value:{frame,d.queue,d.submit,d.draw.command,d.draw.commandGeneration,d.draw.ordinal})hash_value(in.drawIdentity,value);
    in.meshIdentity=mesh_identity(d.draw);in.materialIdentity=material_identity(d);
    const auto e=effective_state(d.draw);
    in.highlight=in.materialIdentity && (e.valid&Blend) && !e.blend &&
        d.draw.pipeline->inspector && d.draw.pipeline->inspector->tint;
    in.reason=in.geometryCandidate?"Geometry candidate - visible owner UNKNOWN":
        in.finalVisible?"Visible swapchain pixel proven by GPU query":"Scene hit - final visible draw UNKNOWN";
    auto_export_uniforms(in);
    log_line("[SURFACE-PICK] ticket="+std::to_string(in.ticket)+" pixel="+std::to_string(in.x)+","+std::to_string(in.y)+" draw="+hex(in.drawIdentity)+" pipeline="+std::to_string(d.draw.pipeline->serial)+" vs="+hex(d.draw.pipeline->vs?d.draw.pipeline->vs->hash:0)+" fs="+hex(d.draw.pipeline->fs?d.draw.pipeline->fs->hash:0)+" scope="+inspector_selection_scope(in)+" highlight="+std::to_string(in.highlight));
}
bool select_geometry_candidate(Inspector& in,uint32_t index) {
    if(index>=in.geometryCandidates.size())return false;
    in.candidateIndex=index;in.selected=in.geometryCandidates[index];
    in.geometryCandidate=true;in.finalVisible=false;in.texture=0;in.preview=false;
    in.previewStatus="Preview not requested";in.previewDiagnostic.clear();
    describe_selection(in,in.candidateFrame);return true;
}
static bool resolve_geometry(Inspector& in) {
    // Query values prove coverage when the ORIGINAL draw executed. Retaining
    // that history across later clears/copies/compositing is intentional here.
    // It does not prove post-compositor UV correspondence or final visibility.
    std::vector<SubmittedDraw> hits;uint32_t ignored=0;bool limited=false;
    if(!in.frame.truncated && !in.frame.missingCommands)for(const auto& submitted:in.frame.draws) {
        const auto& d=submitted.draw;if(!same_depth(d,in) || d.kind>1 || d.skipped || d.unsupported || !d.pipeline)continue;
        const auto e=effective_state(d);
        if((e.valid&Scissor) && !contains(e.scissor,in.x,in.y))continue;
        if((e.valid&(DepthTest|Compare))!=(DepthTest|Compare))continue;
        if(!e.depthTest || e.compare==VK_COMPARE_OP_ALWAYS || e.compare==VK_COMPARE_OP_NEVER){++ignored;continue;}
        if(d.pickTicket!=in.ticket || !d.pick || d.query>=d.pick->used || !coverage_resources_supported(submitted))continue;
        const auto result=d.pick->values[d.query];if(!result[1])return false;
        if(!result[0])continue;
        if(hits.size()==64){hits.erase(hits.begin());limited=true;}
        hits.push_back(submitted);hits.back().draw.pick.reset(); // retain CPU metadata, not a query pool
    }
    std::reverse(hits.begin(),hits.end()); // latest recorded coverage first
    in.geometryCandidates=std::move(hits);in.candidateFrame=in.frame.number;in.candidateLimit=limited;
    in.geometryCandidate=false;in.finalVisible=false;in.selected.reset();in.highlight=false;in.completedTicket=in.ticket;
    log_line("[SURFACE-GEOMETRY-CANDIDATES] ticket="+std::to_string(in.ticket)+" hits="+std::to_string(in.geometryCandidates.size())+
        " ignoredNoDepth="+std::to_string(ignored)+" limit="+std::to_string(limited)+" order=latest-recorded-first visibility=unknown");
    if(!in.geometryCandidates.empty()) {
        // Prefer the latest opaque hit. Other covered geometry (e.g. water) is
        // available explicitly; opaque preference is not a material classifier.
        uint32_t preferred=0;
        for(uint32_t i=0;i<in.geometryCandidates.size();++i) {
            const auto& d=in.geometryCandidates[i].draw;const auto e=effective_state(d);
            if((e.valid&Blend) && !e.blend && (d.pipeline->colorMask&7)==7){preferred=i;break;}
        }
        return select_geometry_candidate(in,preferred);
    }
    in.drawIdentity=in.meshIdentity=in.materialIdentity=0;
    in.reason=in.frame.truncated || in.frame.missingCommands?"Unknown: incomplete scene recording":"Unknown: no GPU-covered depth-tested geometry";
    log_line("[SURFACE-PICK-UNKNOWN] ticket="+std::to_string(in.ticket)+" reason="+in.reason+" scope=depth-tested-geometry-candidate");
    return true;
}
bool resolve_pick(Inspector& in) {
    if(in.geometryMode)return resolve_geometry(in);
    in.geometryCandidates.clear();in.geometryCandidate=false;
    std::optional<SubmittedDraw> winner;bool uncertain=false;size_t order=0,winnerOrder=0;
    for(auto& submitted:in.frame.draws) {
        ++order;
        const auto& d=submitted.draw;
        if(d.kind==7 || (!d.target.known && d.unsupported)){winner.reset();uncertain=true;continue;}
        if(!same_depth(d,in))continue;
        const auto e=effective_state(d);
        if((e.valid&Scissor) && !contains(e.scissor,in.x,in.y))continue;
        if(d.pickTicket!=in.ticket || d.query==UINT32_MAX || d.kind>1 || d.skipped) {winner.reset();uncertain=true;continue;}
        if(d.query==UINT32_MAX-1)continue;
        if(!d.pick || d.query>=d.pick->used){winner.reset();uncertain=true;continue;}
        const auto result=d.pick->values[d.query];if(!result[1])return false;
        if(!result[0])continue;
        // Blending gives multiple contributors. Only an opaque full RGB write
        // can replace earlier ambiguity with a unique visible scene draw.
        if(!coverage_resources_supported(submitted) || !(e.valid&Blend) || e.blend || !d.pipeline || (d.pipeline->colorMask&7)!=7) {winner.reset();uncertain=true;continue;}
        winner=submitted;winnerOrder=order;uncertain=false;
    }
    if(in.frame.truncated || in.frame.missingCommands){winner.reset();uncertain=true;}
    in.finalVisible=false;
    if(winner && in.presentImage && winner->draw.target.colors.size()==1) {
        const auto& color=winner->draw.target.colors[0];in.finalVisible=color && color->image && color->image->handle==in.presentImage;
        for(size_t i=winnerOrder;in.finalVisible && i<in.frame.draws.size();++i) {
            const auto& later=in.frame.draws[i].draw;const auto e=effective_state(later);
            if((e.valid&Scissor) && !contains(e.scissor,in.x,in.y))continue;
            if(!later.target.known){in.finalVisible=false;break;}
            if(same_depth(later,in))continue; // all later scene coverage was resolved above
            for(auto& target:later.target.colors)if(!target || !target->image || target->image->handle==in.presentImage)in.finalVisible=false;
        }
    }
    in.selected=std::move(winner);in.completedTicket=in.ticket;
    in.highlight=false;
    if(in.selected) {
        describe_selection(in,in.frame.number);
    } else {
        in.drawIdentity=in.meshIdentity=in.materialIdentity=0;
        uint32_t scene=0,queried=0,stale=0,noClone=0,unsafeShader=0;
        for(const auto& submitted:in.frame.draws) {
            const auto& d=submitted.draw;if(!same_depth(d,in) || d.kind>5)continue;
            ++scene;queried+=bool(d.pick);stale+=d.pickTicket!=in.ticket;
            const auto p=d.pipeline;
            noClone+=!p || !p->inspector || !p->inspector->query;
            unsafeShader+=!p || !p->vs || !p->fs || !p->vs->replaySafe || !p->fs->replaySafe;
        }
        in.reason=uncertain?"Unknown: unsupported or ambiguous coverage":"Unknown: no covered scene surface";
        if(scene && !queried)in.reason=stale?"Unknown: scene recorded before click":"Unknown: no replayable scene draw";
        log_line("[SURFACE-PICK-UNKNOWN] ticket="+std::to_string(in.ticket)+" reason="+in.reason+
            " sceneDraws="+std::to_string(scene)+" queried="+std::to_string(queried)+" preRecorded="+std::to_string(stale)+
            " noClone="+std::to_string(noClone)+" unsafeShader="+std::to_string(unsafeShader)+" truncated="+std::to_string(in.frame.truncated));
    }
    return true;
}
void inspector_poll(Device& s,Inspector& in) {
    if(!in.staged)return;
    std::set<PickQueries*> seen;for(auto& d:in.frame.draws)if(d.draw.pick && seen.insert(d.draw.pick.get()).second)d.draw.pick->poll();
    if(!poll_gpu_uniforms(in.frame)) {
        if(++in.age<=120)return;
        for(auto& record:in.frame.draws)for(auto& read:record.draw.gpuUniforms)if(read->snapshot->reason=="gpu-readback-pending")read->snapshot->reason="gpu-readback-timeout";
    }
    if(resolve_pick(in)) {
        if(!in.selected && !s.inspectorFailureCaptured) {
            s.inspectorFailureCaptured=true;
            std::set<uint64_t> reported;
            for(const auto& record:in.frame.draws) {
                const auto& d=record.draw;auto p=d.pipeline;
                if(!same_depth(d,in) || !p || !reported.insert(p->serial).second || reported.size()>8)continue;
                log_line("[SURFACE-PICK-BLOCKER] ticket="+std::to_string(in.ticket)+" pipeline="+std::to_string(p->serial)+
                    " query="+p->queryStatus+" command="+d.unsupportedReason+" descriptorApi="+d.descriptorApi+
                    " descriptorIssues="+std::to_string(record.descriptorIssues)+" resources="+std::to_string(record.resources.size())+" "+p->cloneInfo);
            }
            auto capture=std::make_shared<Capture>();capture->sequence=++s.captureSerial;capture->device=id(in.device);
            capture->pickTicket=in.ticket;capture->pickX=in.x;capture->pickY=in.y;capture->frames.push_back(std::move(in.frame));
            log_line("[INSPECTOR-AUTO-CAPTURE] ticket="+std::to_string(in.ticket)+" capture="+std::to_string(capture->sequence)+" reason=first-unresolved-pick");
            export_capture(std::move(capture));
        }
        in.staged=false;in.frame={};
    }
    else if(++in.age>120){in.staged=false;in.frame={};in.selected.reset();in.reason="Unknown: GPU query timed out";}
}
}
namespace depth_probe {
using namespace discovery;
void inspector_request_pick(VkDevice device,uint32_t x,uint32_t y) {
    if(!inspector_enabled())return;
    std::lock_guard lock(mutex);auto& in=inspector_state(device_state(device),device);
    if(x>=in.width || y>=in.height)return;
    if(!in.depthImage){in.reason="Cannot pick: scene depth is not ready";in.page=2;log_line("[SURFACE-PICK-UNAVAILABLE] scene depth is not ready");return;}
    ++in.ticket;if(!in.ticket)++in.ticket;in.x=x;in.y=y;in.armed=true;in.staged=false;in.captureDepth=false;in.depthReady=false;in.highlight=false;in.preview=false;
    in.uniformCaptureBytes=in.uniformCaptureRecords=0;
    in.uniformGpuBytes=in.uniformGpuRecords=0;
    in.autoUniformExports.clear();
    in.selected.reset();in.geometryCandidates.clear();in.geometryCandidate=in.finalVisible=false;
    in.frame={};in.texture=0;in.pools=in.age=0;in.reason="Waiting for next recorded scene frame";in.page=2;
    log_line("[SURFACE-PICK-REQUEST] ticket="+std::to_string(in.ticket)+" pixel="+std::to_string(x)+","+std::to_string(y));
}
void inspector_present_begin(VkDevice device,VkQueue queue,const VkPresentInfoKHR* info) {
    if(!inspector_enabled())return;
    uint64_t presentImage=0;
    if(info->swapchainCount==1 && info->pImageIndices) {
        std::lock_guard stateLock(g_state_mutex);auto found=g_swapchains.find({device,id(info->pSwapchains[0])});
        if(found!=g_swapchains.end() && info->pImageIndices[0]<found->second.images.size())presentImage=id(found->second.images[info->pImageIndices[0]]);
    }
    std::lock_guard lock(mutex);auto& s=device_state(device);if(!s.inspector)return;auto& in=*s.inspector;in.presentImage=presentImage;
    if(info->swapchainCount!=1 || info->pSwapchains[0]!=in.swapchain){in.armed=false;in.reason="Unknown: present target changed";return;}
    // Every present, armed or not: the water replay proves "sampled image layout observed on this same queue" with it,
    // and after launch (water selection restored from settings) no pick may ever arm the Inspector.
    in.queue=id(queue);
    inspector_poll(s,in);
    if(!in.armed)return;
    auto it=s.pending.find(id(queue));
    // DXVK may already have recorded/submitted the next present before the UI
    // processes the click. Do not consume the request on that stale interval.
    bool fresh=false,stale=false;
    if(it!=s.pending.end())for(const auto& submitted:it->second.draws) {
        const auto& d=submitted.draw;if(!same_depth(d,in) || d.kind>5)continue;
        if(d.pickTicket==in.ticket)fresh=true;else stale=true;
    }
    if((!fresh || stale) && ++in.age<=8) {
        if(in.age==1)log_line("[SURFACE-PICK-WAIT] ticket="+std::to_string(in.ticket)+" reason=scene-recorded-before-click");
        in.pools=0;return;
    }
    if(it!=s.pending.end())in.frame=it->second;else in.frame={};
    in.frame.number=s.frameSerial+1;in.armed=false;in.staged=true;in.captureDepth=true;in.age=0;
}
void inspector_set_highlight(VkDevice device,bool enabled) {std::lock_guard lock(mutex);auto& in=inspector_state(device_state(device),device);in.highlight=enabled && bool(in.selected);}
void inspector_cancel(VkDevice device) {
    std::lock_guard lock(mutex);auto found=devices.find(device);if(found==devices.end() || !found->second.inspector)return;
    auto& in=*found->second.inspector;in.armed=in.staged=in.captureDepth=in.depthReady=in.highlight=in.preview=in.finalVisible=false;
    in.selected.reset();in.geometryCandidates.clear();in.geometryCandidate=false;in.frame={};in.completedTicket=0;in.page=0;in.reason="Renderer retired - inspect again";
}
}
