#include "surface_inspector.hpp"
#include "scene_debugger.hpp"
#include <fstream>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <locale>
#include <thread>
#include <fcntl.h>
#include <cmath>

namespace depth_probe::discovery {
Dynamic effective_state(const Draw& draw){return effective_state(draw.pipeline.get(),draw.dynamic);}
uint32_t dynamic_state_mask(const std::set<VkDynamicState>& dynamic) {
    static const std::pair<VkDynamicState,uint32_t> map[]{{VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,Stencil},{VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,DepthTest},
        {VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,DepthWrite},{VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,Compare},{VK_DYNAMIC_STATE_CULL_MODE,Cull},
        {VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,Topology},{VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,Discard},{VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,Blend},
        {VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,ColorMask},{VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,BlendEquation},{VK_DYNAMIC_STATE_FRONT_FACE,FrontFace},
        {VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,BiasEnable},{VK_DYNAMIC_STATE_VIEWPORT,Viewport},{VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,Viewport},
        {VK_DYNAMIC_STATE_SCISSOR,Scissor},{VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,Scissor},{VK_DYNAMIC_STATE_DEPTH_BIAS,Bias}};
    uint32_t mask=0;
    for(auto [state,bit]:map)if(dynamic.count(state))mask|=bit;
    return mask;
}
Dynamic effective_state(const Pipeline* pipeline,Dynamic v) {
    if(!pipeline)return v;const auto& p=*pipeline;
    const uint32_t dynamicMask=p.dynamicMaskReady?p.dynamicMask:dynamic_state_mask(p.dynamic);
    auto isDynamic=[&](uint32_t bit){return (dynamicMask&bit)!=0;};
    auto scalar=[&](VkDynamicState,uint32_t bit,uint32_t initial,uint32_t& value){if(!isDynamic(bit)){value=initial;v.valid|=bit;}};
    scalar(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,Stencil,p.stencil,v.stencil);
    scalar(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,DepthTest,p.depthTest,v.depthTest);
    scalar(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,DepthWrite,p.depthWrite,v.depthWrite);
    scalar(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,Compare,p.compare,v.compare);
    scalar(VK_DYNAMIC_STATE_CULL_MODE,Cull,p.cull,v.cull);
    scalar(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,Topology,p.topology,v.topology);
    scalar(VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,Discard,p.rasterDiscard,v.rasterDiscard);
    scalar(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,Blend,p.blend,v.blend);
    scalar(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,ColorMask,p.colorMask,v.colorMask);
    if(!isDynamic(BlendEquation) && p.blendKnown){v.blendSrc=p.blendSrc;v.blendDst=p.blendDst;v.valid|=BlendEquation;}
    scalar(VK_DYNAMIC_STATE_FRONT_FACE,FrontFace,p.frontFace,v.frontFace);
    scalar(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,BiasEnable,p.depthBiasEnable,v.biasEnable);
    if(!isDynamic(Viewport)){v.viewport=p.viewport;v.valid|=Viewport;}
    if(!isDynamic(Scissor)){v.scissor=p.scissor;v.valid|=Scissor;}
    if(!isDynamic(Bias)){v.biasConstant=p.biasConstant;v.biasClamp=p.biasClamp;v.biasSlope=p.biasSlope;v.valid|=Bias;}
    return v;
}
void resolve_resources(const Device& s,SubmittedDraw& out) {
    const auto& p=out.draw.pipeline;
    for(uint32_t number=0;number<maxSets;++number) {
        const auto& bound=out.draw.sets[number];if(!bound.handle && !bound.pushed)continue;
        auto set=bound.pushed?bound.pushed:find_ptr(s.sets,bound.handle);
        uint32_t issues=0;
        if(!set)issues|=MissingSet;
        else if(!bound.pushed && set->generation!=bound.generation)issues|=StaleSet;
        if(!bound.layout)issues|=MissingBoundLayout;
        if(!p || !p->layout)issues|=MissingPipelineLayout;
        else if(bound.layout && bound.layout->signature!=p->layout->signature)issues|=LayoutMismatch;
        if(issues){out.descriptorIssues|=issues;out.descriptorsIncomplete=true;continue;}
        out.descriptorsIncomplete|=set->incomplete;
        if(set->incomplete)out.descriptorIssues|=IncompleteSet;
        for(const auto& [key,descriptor]:set->descriptors) {
            if(descriptor.type==VK_DESCRIPTOR_TYPE_SAMPLER)continue; // resolve only shader-indexed slots below
            if(out.resources.size()>=maxResourceUses){out.descriptorsIncomplete=true;out.descriptorIssues|=ResourceLimit;break;}
            ResourceUse r;r.set=number;r.binding=key.first;r.element=key.second;r.descriptor=descriptor;
            if(set->layout && set->layout->bindings.count(key.first))r.updateAfterBind=set->layout->bindings.at(key.first).flags&VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            for(auto shader:{p->vs,p->fs})if(shader)for(const auto& b:shader->bindings)if(b.set==number && b.binding==key.first){r.staticSample|=b.sampled;r.staticDref|=b.dref;}
            out.resources.push_back(std::move(r));
        }
        resolve_sampler_resources(s,out,number,*set);
    }
    if(p)for(auto shader:{p->vs,p->fs})if(shader) {
        if(!shader->samplerAccessesComplete){out.descriptorsIncomplete=true;out.descriptorIssues|=SamplerIndexUnknown;}
        for(const auto& a:shader->samplers)if(a.set>=maxSets || (!out.draw.sets[a.set].handle && !out.draw.sets[a.set].pushed)) {
            out.descriptorsIncomplete=true;out.descriptorIssues|=MissingSet;
        }
    }
    if(p)for(auto shader:{p->vs,p->fs})if(shader)for(auto& binding:shader->bindings)if(binding.sampled) {
        if(std::none_of(out.resources.begin(),out.resources.end(),[&](auto& r){return r.set==binding.set && r.binding==binding.binding && r.descriptor.known;})){out.descriptorsIncomplete=true;out.descriptorIssues|=MissingSampledBinding;}
    }
}
namespace {
uint32_t target_kind(const Target& t) {
    if(!t.known)return 0;
    if(t.colors.empty() && t.depth)return 3;
    for(auto& v:t.colors)if(v && v->image && v->image->swapchain)return 1;
    return t.colors.empty()?0:2;
}
void summarize(Device& s,const Frame& f) {
    for(auto& p:s.latest){p.draws=0;p.skipped=0;p.first=p.last=0;}
    uint32_t order=0;
    for(const auto& draw:f.draws) {
        ++order;auto p=draw.draw.pipeline;if(!p)continue;
        auto it=std::find_if(s.latest.begin(),s.latest.end(),[&](auto& v){return v.pipeline->serial==p->serial;});
        if(it==s.latest.end()) {
            if(s.latest.size()>=512){s.limits=true;continue;}
            s.latest.push_back({});it=std::prev(s.latest.end());it->pipeline=p;
        }
        const auto& d=draw.draw;
        uint32_t write=(d.dynamic.valid&DepthWrite)?d.dynamic.depthWrite:UINT32_MAX;
        uint32_t blend=(d.dynamic.valid&Blend)?d.dynamic.blend:UINT32_MAX;
        if(!it->draws){it->target=d.target;it->targetKind=target_kind(d.target);it->depthWrite=write;it->blend=blend;it->depthBindings=it->compareSamplers=0;it->first=order;it->incomplete=false;}
        else {if(it->depthWrite!=write)it->depthWrite=UINT32_MAX;if(it->blend!=blend)it->blend=UINT32_MAX;if(it->targetKind!=target_kind(d.target))it->targetKind=4;}
        ++it->draws;it->skipped+=d.skipped;it->last=order;
        it->incomplete|=d.unsupported || draw.descriptorsIncomplete || f.truncated || f.missingCommands;
        uint32_t depths=0,comparisons=0;
        for(auto& r:draw.resources) {
            if(r.descriptor.view && (r.descriptor.view->range.aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT))++depths;
            if(r.descriptor.sampler && r.descriptor.sampler->compare)++comparisons;
        }
        it->depthBindings=std::max(it->depthBindings,depths);it->compareSamplers=std::max(it->compareSamplers,comparisons);
    }
    std::sort(s.latest.begin(),s.latest.end(),[](auto& a,auto& b){return a.pipeline->serial<b.pipeline->serial;});
    if(!s.selected && !s.latest.empty())s.selected=s.latest.front().pipeline->serial;
}
std::string quote(const std::string& text) {
    std::ostringstream s;s<<'"';for(unsigned char c:text) {
        if(c=='"' || c=='\\')s<<'\\'<<c;
        else if(c<32)s<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;
        else s<<c;
    }s<<'"';return s.str();
}
void view_json(std::ostream& o,const std::shared_ptr<View>& view) {
    if(!view){o<<"null";return;}
    o<<"{\"textureIdentity\":"<<quote(hex(texture_identity(*view)))<<",\"identityScope\":\"device-session-allocation\",\"view\":"<<quote(hex(view->handle))<<",\"viewGeneration\":"<<view->generation<<",\"viewFormat\":"<<view->format
     <<",\"aspect\":"<<view->range.aspectMask<<",\"baseMip\":"<<view->range.baseMipLevel<<",\"mipCount\":"<<view->range.levelCount
     <<",\"baseLayer\":"<<view->range.baseArrayLayer<<",\"layerCount\":"<<view->range.layerCount;
    if(auto image=view->image)o<<",\"image\":"<<quote(hex(image->handle))<<",\"imageGeneration\":"<<image->generation
        <<",\"format\":"<<image->format<<",\"width\":"<<image->extent.width<<",\"height\":"<<image->extent.height
        <<",\"layers\":"<<image->layers<<",\"mips\":"<<image->mips<<",\"samples\":"<<image->samples<<",\"usageAtCreate\":"<<image->usage<<",\"swapchain\":"<<image->swapchain;
    o<<'}';
}
void scalar_json(std::ostream& o,const char* name,uint32_t value,bool valid) {
    o<<','<<quote(name)<<':';if(valid)o<<value;else o<<"null";
}
void finite_json(std::ostream& o,float value) {if(std::isfinite(value))o<<value;else o<<"null";}
void pipeline_json(std::ostream& o,const Pipeline& p) {
    o<<"{\"id\":"<<p.serial<<",\"handle\":"<<quote(hex(p.handle))<<",\"hash\":"<<quote(hex(p.hash))
     <<",\"vs\":"<<quote(hex(p.vs?p.vs->hash:0))<<",\"fs\":"<<quote(hex(p.fs?p.fs->hash:0))
     <<",\"vsEntry\":"<<quote(p.vsEntry)<<",\"fsEntry\":"<<quote(p.fsEntry)<<",\"layout\":"<<quote(hex(p.layoutHandle))
     <<",\"cloneInfo\":"<<quote(p.cloneInfo)<<",\"queryStatus\":"<<quote(p.queryStatus)<<",\"tintStatus\":"<<quote(p.tintStatus)
     <<",\"layoutInfo\":"<<quote(p.layout?p.layout->diagnostic:"missing")
     <<",\"vsSource\":"<<quote(p.vsSource)<<",\"fsSource\":"<<quote(p.fsSource)
     <<",\"layoutSignature\":"<<quote(hex(p.layout?p.layout->signature:0))<<",\"fingerprintComplete\":"<<p.fingerprintComplete
     <<",\"isolationSafe\":"<<p.isolationSafe<<",\"category\":\"unknown\",\"status\":\"PROTOTYPE\",\"depthTestStatic\":"<<p.depthTest
     <<",\"depthWriteStatic\":"<<p.depthWrite<<",\"compareStatic\":"<<p.compare<<",\"blend\":"<<p.blend
     <<",\"cullStatic\":"<<p.cull<<",\"frontFaceStatic\":"<<p.frontFace<<",\"topologyStatic\":"<<p.topology
     <<",\"stencil\":"<<p.stencil<<",\"depthBiasEnableStatic\":"<<p.depthBiasEnable
     <<",\"blendKnown\":"<<p.blendKnown<<",\"blendSrc\":"<<p.blendSrc<<",\"blendDst\":"<<p.blendDst<<",\"colorMaskStatic\":"<<p.colorMask<<",\"parts\":"<<p.parts
     <<",\"alphaTest\":"<<int(p.alphaTest.state)<<",\"alphaCompare\":"<<int(p.alphaTest.compare)<<",\"alphaTestSource\":"<<quote(p.alphaTest.source)<<",\"dynamicStates\":[";
    bool comma=false;for(auto v:p.dynamic){if(comma)o<<',';comma=true;o<<v;}o<<"]}";
}
bool write_exclusive(const std::filesystem::path& path,const void* bytes,size_t size) {
    int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return false;
    const char* p=static_cast<const char*>(bytes);bool ok=true;
    while(size){ssize_t n=::write(fd,p,size);if(n<0 && errno==EINTR)continue;if(n<=0){ok=false;break;}p+=n;size-=size_t(n);}
    if(::close(fd)!=0)ok=false;
    return ok;
}
struct Exporter {
    std::mutex lock;std::condition_variable cv;std::deque<std::shared_ptr<const Capture>> jobs;
    bool stopping=false,busy=false;std::thread worker;
    Exporter():worker([this]{run();}){}
    ~Exporter(){{std::lock_guard guard(lock);stopping=true;}cv.notify_all();worker.join();}
    void run() {
        for(;;) {
            std::shared_ptr<const Capture> job;
            {std::unique_lock guard(lock);cv.wait(guard,[&]{return stopping || !jobs.empty();});if(jobs.empty() && stopping)return;job=std::move(jobs.front());jobs.pop_front();busy=true;}
            try {
                const char* env=std::getenv("DEPTH_PROBE_DISCOVERY_DIR");
                std::filesystem::path directory=env && *env?env:"/tmp/depth-probe-discovery-"+std::to_string(getpid());
                if(!job->directory.empty())directory=job->directory;
                std::filesystem::create_directories(directory/"shaders");
                std::map<uint64_t,std::shared_ptr<Shader>> shaders;
                for(auto& frame:job->frames)for(auto& draw:frame.draws)if(auto p=draw.draw.pipeline)for(auto shader:{p->vs,p->fs})if(shader)shaders[shader->hash]=shader;
                for(auto& [hash,shader]:shaders)if(!shader->words.empty())write_exclusive(directory/"shaders"/(hex(hash)+".spv"),shader->words.data(),shader->words.size()*4);
                if(job->uniforms || job->lights) {
                    std::filesystem::create_directories(directory/"uniforms");
                    for(auto& frame:job->frames)for(auto& draw:frame.draws)for(auto& u:draw.uniforms)if(u && !u->bytes.empty())
                        write_exclusive(directory/"uniforms"/scan_uniform_file(frame,draw,*u),u->bytes.data(),u->bytes.size());
                }
                const auto path=directory/("capture-"+std::to_string(job->sequence)+"-"+hex(job->device)+".json");
                const auto json=capture_json(*job);
                if(write_exclusive(path,json.data(),json.size())) {
                    log_line("[DISCOVERY-CAPTURE] status=PROTOTYPE frames="+std::to_string(job->frames.size())+" path="+path.string()+" semanticCategories=draw_semantics");
                    if(!job->directory.empty()) {   // written then renamed: a reader never sees an empty DONE
                        const std::string done=path.filename().string()+"\n";std::error_code ec;
                        if(write_exclusive(directory/"DONE.tmp",done.data(),done.size()))std::filesystem::rename(directory/"DONE.tmp",directory/"DONE",ec);
                    }
                }
                else log_line("[DISCOVERY-EXPORT-FAILED] output exists or cannot be written: "+path.string());
            } catch(const std::exception& e){log_line(std::string("[DISCOVERY-EXPORT-FAILED] ")+e.what());}
            {std::lock_guard guard(lock);busy=false;}cv.notify_all();
        }
    }
};
Exporter& exporter(){static Exporter value;return value;}
}
bool format_has_depth(VkFormat f) {
    return f==VK_FORMAT_D16_UNORM || f==VK_FORMAT_X8_D24_UNORM_PACK32 || f==VK_FORMAT_D32_SFLOAT || f==VK_FORMAT_D16_UNORM_S8_UINT ||
           f==VK_FORMAT_D24_UNORM_S8_UINT || f==VK_FORMAT_D32_SFLOAT_S8_UINT;
}
std::string scan_uniform_file(const Frame& frame,const SubmittedDraw& draw,const UniformSnapshot& u) {
    return "f"+std::to_string(frame.number)+"-d"+std::to_string(draw.order)+"-s"+std::to_string(u.set)+"-b"+std::to_string(u.binding)+"-e"+std::to_string(u.element)+".bin";
}
// Frame scan "uniforms" (tools/shadow-constants.py, tools/cascade-check.py): the uniform buffers of draws that sample a large
// square depth image with a comparison - the game's sun shadow cascades - and of the draws that render INTO one (the shadow
// passes: their own light matrices), copied host-side at submit (DXVK's D3D9 constants live in host-coherent memory).
// Bounded: 64 receiving + 48 shadow-pass draws, 6 per pipeline and depth target, 8 MiB per capture.
// Frame scan "lights" (tools/light-constants.py): the uniform buffers of every draw on the scene's depth target - the world's
// models, terrain and objects, whose shaders get the game's lights as constants. Bounded: 12 per pipeline, 900 draws, 64 MiB.
void capture_scan_light_uniforms(Device& s,SubmittedDraw& draw,const std::shared_ptr<Pipeline>& p) {
    const auto& depth=draw.draw.target.depth;
    if(!depth || !depth->image || !s.capture->sceneDepthImage || depth->image->handle!=s.capture->sceneDepthImage)return;
    if(s.scanLightDraws>=900 || s.scanUniformPerPipeline[p->serial]>=12)return;
    ++s.scanUniformPerPipeline[p->serial];++s.scanLightDraws;
    for(const auto& r:draw.resources) {
        if(r.descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && r.descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)continue;
        bool declared=false;
        for(auto shader:{p->vs,p->fs})if(shader)for(const auto& b:shader->bindings)declared |= b.storage==2 && b.set==r.set && b.binding==r.binding;
        if(declared)draw.uniforms.push_back(try_capture_uniform(s,draw,r,s.scanUniformBytes,size_t(64)<<20,true));
    }
}
void capture_scan_uniforms(Device& s,SubmittedDraw& draw) {
    if(!s.capture || (!s.capture->uniforms && !s.capture->lights))return;
    auto p=draw.draw.pipeline;if(!p)return;
    if(s.capture->lights){capture_scan_light_uniforms(s,draw,p);return;}
    auto bigSquareDepth=[](const Image& im){return im.extent.width>=1024 && im.extent.width==im.extent.height && format_has_depth(im.format);};
    bool shadow=false;
    for(const auto& r:draw.resources)if(r.staticDref && r.descriptor.view && r.descriptor.view->image)shadow |= bigSquareDepth(*r.descriptor.view->image);
    const auto& depth=draw.draw.target.depth;
    const bool shadowPass=!shadow && depth && depth->image && bigSquareDepth(*depth->image);
    if(!shadow && !shadowPass)return;
    if(shadow ? s.scanUniformDraws>=64 : s.scanUniformShadowPassDraws>=48)return;
    const uint64_t key=p->serial*31+(shadowPass?depth->image->handle:0);
    if(s.scanUniformPerPipeline[key]>=6)return;
    ++s.scanUniformPerPipeline[key];++(shadow ? s.scanUniformDraws : s.scanUniformShadowPassDraws);
    for(const auto& r:draw.resources) {
        if(r.descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && r.descriptor.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)continue;
        bool declared=false;
        for(auto shader:{p->vs,p->fs})if(shader)for(const auto& b:shader->bindings)declared |= b.storage==2 && b.set==r.set && b.binding==r.binding;
        if(declared)draw.uniforms.push_back(try_capture_uniform(s,draw,r,s.scanUniformBytes,size_t(8)<<20,true));
    }
}
Frame submitted_frame(VkDevice device,VkQueue queue,uint32_t count,const VkCommandBuffer* commands) {
    std::lock_guard lock(mutex);auto& s=device_state(device);Frame frame;frame.queue=id(queue);
    const uint64_t submit=++s.submitSerial;
    const bool wantDraws=draw_records_wanted(s);   // layout events below are always needed; the per-draw copy and descriptor resolution only when consumed
    std::set<uint64_t> stack;
    std::function<void(uint64_t,uint64_t,const Target*,unsigned)> visit;
    visit=[&](uint64_t handle,uint64_t generation,const Target* inherited,unsigned level) {
        auto found=s.commands.find(handle);
        if(found==s.commands.end() || !found->second.executable || (generation && generation!=found->second.generation) || level>8 || stack.count(handle)){++frame.missingCommands;return;}
        const auto& c=found->second;frame.truncated|=c.truncated;stack.insert(handle);
        for(const auto& batch:c.uniformBatches)if(std::find(frame.uniformBatches.begin(),frame.uniformBatches.end(),batch)==frame.uniformBatches.end())frame.uniformBatches.push_back(batch);
        for(const auto& e:c.events) {
            if(frame.draws.size()>=maxFrameDraws){frame.truncated=true;break;}
            if(e.secondary){visit(e.secondary,e.generation,e.draw?&e.draw->target:nullptr,level+1);continue;}
            if(e.layout.image) {if(frame.layouts.size()<maxEvents)frame.layouts.push_back(e.layout);else frame.truncated=true;}
            if(!e.draw || !wantDraws)continue;
            SubmittedDraw result;result.draw=*e.draw;
            if(result.draw.target.inherited && inherited)result.draw.target=*inherited;
            result.draw.dynamic=effective_state(result.draw);result.queue=id(queue);result.submit=submit;result.order=frame.draws.size()+1;
            resolve_resources(s,result);capture_pick_uniforms(s,result);capture_scene_debugger_uniforms(s,result);capture_scan_uniforms(s,result);frame.draws.push_back(std::move(result));
        }
        stack.erase(handle);
    };
    for(uint32_t i=0;i<count;++i)visit(id(commands[i]),0,nullptr,0);
    return frame;
}
void accept_submit(VkDevice device,VkQueue queue,Frame&& frame) {
    std::lock_guard lock(mutex);auto& s=device_state(device);
    if(s.pending.size()>=4 && !s.pending.count(id(queue))){s.limits=true;return;}
    if(frame.missingCommands || frame.truncated) {
        for(auto& [h,im]:s.images){(void)h;im->layoutKnown=false;for(auto& mip:im->mipLayouts)mip.known=false;}
        // Why the record was dropped (first few times per device): the water replay no longer depends on it, other readers do.
        if(s.layoutResets++<8)log_line("[LAYOUT-RESET] submit "+std::to_string(s.submitSerial)+" truncated="+std::to_string(frame.truncated)+" missingCommands="+
                                         std::to_string(frame.missingCommands)+" draws="+std::to_string(frame.draws.size())+" layoutEvents="+std::to_string(frame.layouts.size())+
                                         ": every image's layout record dropped");
    }
    else for(auto& event:frame.layouts)if(event.image) {
        auto& im=*event.image;const auto& r=event.range;
        const bool whole=r.baseMipLevel==0 && (r.levelCount==im.mips || r.levelCount==VK_REMAINING_MIP_LEVELS) &&
            r.baseArrayLayer==0 && (r.layerCount==im.layers || r.layerCount==VK_REMAINING_ARRAY_LAYERS);
        if(im.layers==1 && im.mips<=32 && r.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT && r.baseArrayLayer==0 && r.layerCount) {
            if(im.mipLayouts.empty())im.mipLayouts.resize(im.mips,{im.currentLayout,im.layoutQueue,im.layoutKnown});
            const uint64_t end=r.levelCount==VK_REMAINING_MIP_LEVELS?im.mips:uint64_t(r.baseMipLevel)+r.levelCount;
            for(uint32_t mip=r.baseMipLevel;mip<std::min(uint64_t(im.mips),end);++mip)im.mipLayouts[mip]={event.layout,id(queue),event.known};
        }
        im.currentLayout=event.layout;im.layoutKnown=event.known && whole;im.layoutQueue=id(queue);
    }
    auto& pending=s.pending[id(queue)];pending.queue=id(queue);pending.truncated|=frame.truncated;pending.missingCommands+=frame.missingCommands;
    for(auto& draw:frame.draws) {
        revalidate_sampler_resources(s,draw);
        if(pending.draws.size()>=maxFrameDraws){pending.truncated=true;break;}
        draw.order=pending.draws.size()+1;pending.draws.push_back(std::move(draw));
    }
}
void finish_present(VkDevice device,VkQueue queue,VkResult result) {
    std::shared_ptr<const Capture> ready;
    {
        std::lock_guard lock(mutex);auto& s=device_state(device);
        auto& pending=s.pending[id(queue)];pending.number=++s.frameSerial;pending.queue=id(queue);pending.presentResult=result;
        summarize(s,pending);
        draw_classes_present(s);
        if(s.isolation && (s.isolationFrames==0 || --s.isolationFrames==0 || std::chrono::steady_clock::now()>s.isolationDeadline)) {
            s.isolation=0;s.isolationTargets.clear();log_line("[PIPELINE-ISOLATION] expired; new recordings forward unchanged");
        }
        if(s.requestedFrames && !s.capture) {
            s.capture=std::make_shared<Capture>();s.capture->sequence=++s.captureSerial;s.capture->device=id(device);
            s.capture->directory=std::move(s.scanDirectory);s.scanDirectory.clear();
            s.capture->sceneDepthImage=s.sceneDebugger?s.sceneDebugger->sceneDepthImage:0;
            s.capture->uniforms=s.scanUniforms;s.scanUniforms=false;s.scanUniformBytes=0;s.scanUniformDraws=0;s.scanUniformShadowPassDraws=0;s.scanUniformPerPipeline.clear();
            s.capture->lights=s.scanLights;s.scanLights=false;s.scanLightDraws=0;
            log_line("[DISCOVERY-CAPTURE-BEGIN] frames="+std::to_string(s.requestedFrames)+" same-queue submit intervals; first interval may be partial");
        }
        if(s.capture) {
            size_t count=0;for(auto& f:s.capture->frames)count+=f.draws.size();
            if(count+pending.draws.size()>maxCaptureDraws) {pending.draws.resize(maxCaptureDraws-count);pending.truncated=true;s.requestedFrames=1;}
            if(s.capture->uniforms || s.capture->lights)pending.context=fog_scan_context(s);
            s.capture->frames.push_back(std::move(pending));
            if(--s.requestedFrames==0){ready=std::move(s.capture);s.capture.reset();}
        }
        s.pending.erase(id(queue));
        // Frame scan (tools/frame-scan.py), checked after this present's capture handling so the capture starts with the next frame: $DEPTH_PROBE_EXPORT_DIR/frame-scan-request (optional content: frame count, default 3)
        // captures that many whole frames of draws with their state, classes and shaders into frame-scan-<pid>-<n>/.
        if(++s.presentsSinceScanCheck>=30) {
            s.presentsSinceScanCheck=0;
            const char* root=std::getenv("DEPTH_PROBE_EXPORT_DIR");const char* home=std::getenv("HOME");
            const std::filesystem::path base=root && *root?std::filesystem::path(root):home && *home?std::filesystem::path(home):std::filesystem::path("/tmp");
            std::error_code ec;const auto request=base/"frame-scan-request";
            if(std::filesystem::exists(request,ec)) {
                // Content: an optional frame count and the word "uniforms" (the shadow-sampling draws' constants too) or "lights"
                // (the constants of every draw on the scene's depth target).
                unsigned frames=3;bool uniforms=false,lights=false;
                {std::ifstream in(request);std::string word;while(in>>word){if(word=="uniforms")uniforms=true;else if(word=="lights")lights=true;else if(auto n=std::strtoul(word.c_str(),nullptr,10))frames=unsigned(n);}}
                std::filesystem::remove(request,ec);
                if(!s.capture && !s.requestedFrames) {
                    s.requestedFrames=std::clamp(frames,1u,uint32_t(maxCaptureFrames));
                    s.scanDirectory=(base/("frame-scan-"+std::to_string(getpid())+"-"+std::to_string(++s.scanSequence))).string();
                    s.scanUniforms=uniforms;s.scanLights=lights;
                    log_line("[FRAME-SCAN] request: "+std::to_string(s.requestedFrames)+" frames"+(lights?" with every world draw's constants":uniforms?" with shadow-sampling constants":"")+" -> "+s.scanDirectory);
                } else log_line("[FRAME-SCAN] request ignored: a capture is already running");
            }
        }
    }
    if(ready)export_capture(std::move(ready));
}
void export_capture(std::shared_ptr<const Capture> capture) {
    auto& e=exporter();{std::lock_guard lock(e.lock);if(e.jobs.size()>=1){log_line("[DISCOVERY-EXPORT-FAILED] bounded writer queue busy; capture dropped");return;}e.jobs.push_back(std::move(capture));}e.cv.notify_all();
}
std::string capture_json(const Capture& capture) {
    std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(9);
    std::map<uint64_t,std::shared_ptr<Pipeline>> pipelines;std::map<uint64_t,std::shared_ptr<Shader>> shaders;
    for(auto& f:capture.frames)for(auto& draw:f.draws)if(auto p=draw.draw.pipeline){pipelines[p->serial]=p;for(auto shader:{p->vs,p->fs})if(shader)shaders[shader->hash]=shader;}
    o<<"{\"schema\":1,\"status\":\"PROTOTYPE\",\"device\":"<<quote(hex(capture.device))<<",\"capture\":"<<capture.sequence
     <<",\"diagnosticRevision\":2,\"pickTicket\":"<<capture.pickTicket<<",\"pickPixel\":["<<capture.pickX<<','<<capture.pickY<<']'
     <<",\"ordering\":\"same-queue host submit order between present calls; not GPU visibility proof\""
     <<",\"sceneDepthImage\":"<<quote(hex(capture.sceneDepthImage))<<",\"pipelines\":[";
    bool comma=false;for(auto& [serial,p]:pipelines){(void)serial;if(comma)o<<',';comma=true;pipeline_json(o,*p);}o<<"],\"shaders\":[";
    comma=false;for(auto& [hash,s]:shaders) {
        if(comma)o<<',';
        comma=true;o<<"{\"hash\":"<<quote(hex(hash))<<",\"file\":"<<quote("shaders/"+hex(hash)+".spv")
            <<",\"staticSamples\":"<<s->samples<<",\"staticDrefs\":"<<s->drefs<<",\"staticGathers\":"<<s->gathers<<",\"staticFetches\":"<<s->fetches
            <<",\"samplerAccessesComplete\":"<<s->samplerAccessesComplete<<",\"samplerAccessCount\":"<<s->samplers.size()
            <<",\"replaySafe\":"<<s->replaySafe<<",\"earlyTests\":"<<s->earlyTests
            <<",\"unresolvedSamples\":"<<s->unresolvedSamples<<",\"isolationSafe\":"<<s->isolationSafe
            <<",\"discards\":"<<s->discards<<",\"dxvkAlphaTest\":"<<s->dxvkAlphaTest.found<<",\"bindings\":[";
        bool sep=false;for(auto& b:s->bindings){if(sep)o<<',';sep=true;o<<"{\"set\":"<<b.set<<",\"binding\":"<<b.binding<<",\"storageClass\":"<<b.storage<<",\"sampled\":"<<b.sampled<<",\"dref\":"<<b.dref<<'}';}o<<"]}";
    }o<<"],\"frames\":[";comma=false;
    for(const auto& f:capture.frames) {
        if(comma)o<<',';
        comma=true;o<<"{\"number\":"<<f.number<<",\"queue\":"<<quote(hex(f.queue))<<",\"presentResult\":"<<f.presentResult
          <<",\"truncated\":"<<f.truncated<<",\"missingCommands\":"<<f.missingCommands<<",\"context\":"<<(f.context.empty()?std::string("null"):f.context)<<",\"draws\":[";
        bool sep=false;for(const auto& record:f.draws) {
            if(sep)o<<',';
            sep=true;const auto& d=record.draw;const auto& v=d.dynamic;
            o<<"{\"pipeline\":"<<(d.pipeline?d.pipeline->serial:0)<<",\"order\":"<<record.order<<",\"submit\":"<<record.submit<<",\"command\":"<<quote(hex(d.command))
             <<",\"commandGeneration\":"<<d.commandGeneration<<",\"meshCandidate\":"<<quote(hex(mesh_identity(d)))<<",\"materialCandidate\":"<<quote(hex(material_identity(record)))
             <<",\"recordOrdinal\":"<<d.ordinal<<",\"pass\":"<<d.passOrdinal<<",\"kind\":"<<d.kind<<",\"count\":"<<d.count<<",\"instances\":"<<d.instances
             <<",\"first\":"<<d.first<<",\"vertexOffset\":"<<d.vertexOffset<<",\"firstInstance\":"<<d.firstInstance
             <<",\"skipped\":"<<d.skipped<<",\"unsupportedState\":"<<d.unsupported<<",\"descriptorsIncomplete\":"<<record.descriptorsIncomplete
             <<",\"unsupportedReason\":"<<quote(d.unsupportedReason)<<",\"descriptorApi\":"<<quote(d.descriptorApi)<<",\"descriptorIssues\":"<<record.descriptorIssues
             <<",\"semantic\":{\"class\":"<<quote(draw_class_name(d.semantic.cls))<<",\"confidence\":"<<quote(draw_confidence_name(d.semantic.confidence))
             <<",\"reason\":"<<quote(d.semantic.reason)<<",\"evidence\":"<<quote(draw_evidence_text(d.semantic.evidence))
             <<",\"alphaRef\":"<<(d.push[1].known>>6&1?std::to_string(d.push[1].words[6]):std::string("null"))<<'}';   // D3DRS_ALPHAREF (render_state push word 6)
            {   // the pipeline's static state merged with the dynamic state at the draw (null = not known)
                const auto e=effective_state(d);
                o<<",\"effective\":{\"known\":"<<e.valid;
                scalar_json(o,"depthTest",e.depthTest,e.valid&DepthTest);scalar_json(o,"depthWrite",e.depthWrite,e.valid&DepthWrite);scalar_json(o,"compare",e.compare,e.valid&Compare);
                scalar_json(o,"blend",e.blend,e.valid&Blend);scalar_json(o,"blendSrc",e.blendSrc,e.valid&BlendEquation);scalar_json(o,"blendDst",e.blendDst,e.valid&BlendEquation);
                scalar_json(o,"colorMask",e.colorMask,e.valid&ColorMask);scalar_json(o,"topology",e.topology,e.valid&Topology);scalar_json(o,"cull",e.cull,e.valid&Cull);
                o<<",\"depthRange\":";if(e.valid&Viewport){o<<'[';finite_json(o,e.viewport.minDepth);o<<',';finite_json(o,e.viewport.maxDepth);o<<']';}else o<<"null";
                o<<'}';
            }
            scalar_json(o,"depthTest",v.depthTest,v.valid&DepthTest);scalar_json(o,"depthWrite",v.depthWrite,v.valid&DepthWrite);scalar_json(o,"compare",v.compare,v.valid&Compare);
            scalar_json(o,"cull",v.cull,v.valid&Cull);scalar_json(o,"topology",v.topology,v.valid&Topology);scalar_json(o,"rasterDiscard",v.rasterDiscard,v.valid&Discard);
            scalar_json(o,"blend",v.blend,v.valid&Blend);scalar_json(o,"frontFace",v.frontFace,v.valid&FrontFace);scalar_json(o,"biasEnable",v.biasEnable,v.valid&BiasEnable);
            scalar_json(o,"colorMask",v.colorMask,v.valid&ColorMask);scalar_json(o,"blendSrc",v.blendSrc,v.valid&BlendEquation);scalar_json(o,"blendDst",v.blendDst,v.valid&BlendEquation);
            o<<",\"viewportKnown\":"<<bool(v.valid&Viewport)<<",\"viewport\":[";
            bool first=true;for(float x:{v.viewport.x,v.viewport.y,v.viewport.width,v.viewport.height,v.viewport.minDepth,v.viewport.maxDepth}){if(!first)o<<',';first=false;finite_json(o,x);}o<<']';
            o<<",\"scissorKnown\":"<<bool(v.valid&Scissor)<<",\"scissor\":["<<v.scissor.offset.x<<','<<v.scissor.offset.y<<','<<v.scissor.extent.width<<','<<v.scissor.extent.height<<']';
            o<<",\"rasterBiasKnown\":"<<bool(v.valid&Bias)<<",\"rasterBias\":[";finite_json(o,v.biasConstant);o<<',';finite_json(o,v.biasClamp);o<<',';finite_json(o,v.biasSlope);o<<']';
            const auto& t=d.target;o<<",\"target\":{\"known\":"<<t.known<<",\"role\":"<<target_kind(t)<<",\"renderPass\":"<<quote(hex(t.renderPass))<<",\"framebuffer\":"<<quote(hex(t.framebuffer))
                <<",\"subpass\":"<<t.subpass<<",\"width\":"<<t.width<<",\"height\":"<<t.height<<",\"clearDepth\":";
            if(t.clearKnown)finite_json(o,t.clearDepth);else o<<"null";
            o<<",\"depth\":";view_json(o,t.depth);o<<",\"colors\":[";bool color=false;for(auto& c:t.colors){if(color)o<<',';color=true;view_json(o,c);}o<<"]},\"resources\":[";
            bool resource=false;for(auto& r:record.resources) {
                if(resource)o<<',';
                resource=true;const auto& desc=r.descriptor;
                o<<"{\"set\":"<<r.set<<",\"binding\":"<<r.binding<<",\"element\":"<<r.element<<",\"type\":"<<desc.type<<",\"known\":"<<desc.known
                 <<",\"staticSample\":"<<r.staticSample<<",\"staticDref\":"<<r.staticDref<<",\"samplerIndexed\":"<<r.samplerIndexed<<",\"updateAfterBind\":"<<r.updateAfterBind<<",\"layout\":"<<desc.layout<<",\"view\":";view_json(o,desc.view);
                o<<",\"sampler\":";if(auto sampler=desc.sampler) {
                    o<<"{\"handle\":"<<quote(hex(sampler->handle))<<",\"compareEnable\":"<<sampler->compare<<",\"compareOp\":"<<sampler->op
                     <<",\"addressU\":"<<sampler->address[0]<<",\"addressV\":"<<sampler->address[1]<<",\"addressW\":"<<sampler->address[2]
                     <<",\"minFilter\":"<<sampler->min<<",\"magFilter\":"<<sampler->mag<<",\"mipmapMode\":"<<sampler->mipmap<<",\"mipLodBias\":";finite_json(o,sampler->lodBias);o<<'}';
                }else o<<"null";
                o<<",\"buffer\":"<<quote(hex(desc.buffer))<<",\"offset\":"<<desc.offset<<",\"range\":"<<desc.range<<'}';
            }
            o<<"],\"vertexBuffers\":[";bool vertex=false;
            for(size_t index=0;index<d.vertices.size();++index)if(auto& b=d.vertices[index];b.buffer){if(vertex)o<<',';vertex=true;o<<"{\"binding\":"<<index<<",\"buffer\":"<<quote(hex(b.buffer))<<",\"generation\":"<<b.generation<<",\"offset\":"<<b.offset<<",\"size\":"<<b.size<<",\"stride\":"<<b.stride<<'}';}
            o<<"],\"indexBuffer\":{\"buffer\":"<<quote(hex(d.index.buffer))<<",\"generation\":"<<d.index.generation<<",\"offset\":"<<d.index.offset<<",\"type\":"<<d.indexType<<"},\"setDynamicOffsets\":[";
            for(size_t set=0;set<maxSets;++set){if(set)o<<',';o<<'[';for(size_t i=0;i<d.sets[set].offsets.size();++i){if(i)o<<',';o<<d.sets[set].offsets[i];}o<<']';}
            o<<"],\"boundLayouts\":[";bool boundComma=false;
            for(size_t set=0;set<maxSets;++set) {
                const auto& b=d.sets[set];if(!b.handle && !b.pushed)continue;
                if(boundComma)o<<',';
                boundComma=true;
                o<<"{\"set\":"<<set<<",\"handle\":"<<quote(hex(b.handle))<<",\"generation\":"<<b.generation
                 <<",\"pushed\":"<<bool(b.pushed)<<",\"signature\":"<<quote(hex(b.layout?b.layout->signature:0))
                 <<",\"info\":"<<quote(b.layout?b.layout->diagnostic:"missing")<<'}';
            }
            o<<"],\"lastBindDynamicOffsets\":[";for(size_t i=0;i<d.dynamicOffsets.size();++i){if(i)o<<',';o<<d.dynamicOffsets[i];}o<<']';
            if(!record.uniforms.empty()) {
                o<<",\"uniforms\":[";bool uc=false;
                for(auto& u:record.uniforms)if(u){if(uc)o<<',';uc=true;o<<"{\"set\":"<<u->set<<",\"binding\":"<<u->binding<<",\"element\":"<<u->element<<",\"offset\":"<<u->offset
                    <<",\"range\":"<<u->range<<",\"usage\":"<<u->bufferUsage<<",\"reason\":"<<quote(u->reason)<<",\"file\":"<<quote(u->bytes.empty()?std::string():"uniforms/"+scan_uniform_file(f,record,*u))<<'}';}
                o<<']';
            }
            o<<'}';
        }o<<"]}";
    }o<<"]}\n";return o.str();
}
}
namespace depth_probe {
void request_discovery_capture(VkDevice device,uint32_t frames) {
    if(!discovery_enabled())return;
    std::lock_guard lock(discovery::mutex);auto& s=discovery::device_state(device);
    if(!s.capture)s.requestedFrames=std::clamp(frames,1u,uint32_t(discovery::maxCaptureFrames));
}
void flush_discovery_exports() {
    auto& e=discovery::exporter();std::unique_lock lock(e.lock);e.cv.wait(lock,[&]{return e.jobs.empty() && !e.busy;});
}
void stop_discovery_isolation() {
    if(!discovery_enabled())return;
    std::lock_guard lock(discovery::mutex);for(auto& [d,s]:discovery::devices){(void)d;s.isolation=0;s.isolationTargets.clear();}
}
}
