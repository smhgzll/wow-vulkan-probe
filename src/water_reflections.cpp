#include "water_reflections.hpp"
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "overlay_settings.hpp"
#include "depth_tracking.hpp"
#include "sync_tracking.hpp"
#include "memory_sun.hpp"
#include "process_memory.hpp"

namespace depth_probe::discovery {
namespace {
template<class T>T raw(const DeviceData& d,const char* n){return reinterpret_cast<T>(d.next_gdpa(d.device,n));}
const WaterReplayFunctions& fns(const DeviceData& d) {
    auto& f=d.waterFns;
    if(!f.resolved) {
        f.setViewport=raw<PFN_vkCmdSetViewport>(d,"vkCmdSetViewport");f.setScissor=raw<PFN_vkCmdSetScissor>(d,"vkCmdSetScissor");
        f.drawIndexed=raw<PFN_vkCmdDrawIndexed>(d,"vkCmdDrawIndexed");f.pushConstants=raw<PFN_vkCmdPushConstants>(d,"vkCmdPushConstants");
        f.bindVertexBuffers=raw<PFN_vkCmdBindVertexBuffers>(d,"vkCmdBindVertexBuffers");f.bindIndexBuffer=raw<PFN_vkCmdBindIndexBuffer>(d,"vkCmdBindIndexBuffer");
        f.bindVertexBuffers2=raw<PFN_vkCmdBindVertexBuffers2>(d,"vkCmdBindVertexBuffers2");
        if(!f.bindVertexBuffers2)f.bindVertexBuffers2=raw<PFN_vkCmdBindVertexBuffers2>(d,"vkCmdBindVertexBuffers2EXT");
        f.bindDescriptorSets=raw<PFN_vkCmdBindDescriptorSets>(d,"vkCmdBindDescriptorSets");f.updateBuffer=raw<PFN_vkCmdUpdateBuffer>(d,"vkCmdUpdateBuffer");
        f.copyImage=raw<PFN_vkCmdCopyImage>(d,"vkCmdCopyImage");f.clearColorImage=raw<PFN_vkCmdClearColorImage>(d,"vkCmdClearColorImage");f.resolved=true;
    }
    return f;
}
void status(Device& s,const std::string& message) {
    auto& w=water_state(s);if(w.status==message)return;w.status=message;
    inspector_diagnostic(s,"water:"+message,"mode="+std::to_string(w.mode));
}
const char* push_problem(const Pipeline& p,const std::array<PushState,2>& push) {
    if(!p.layout)return "no-layout";
    for(const auto& r:p.layout->pushRanges) {
        // DXVK declares ranges for more stages than VS/FS (e.g. ALL_GRAPHICS). Vulkan makes the app push
        // with each range's full stage mask, so its vertex/fragment data is tracked; replay pushes with the
        // union of the overlapping ranges' stages. Only a range with neither stage could not be restored.
        if(!(r.stageFlags&(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT)))return "push-range-no-vertex-or-fragment";
        if(r.offset%4 || r.size%4 || r.offset+r.size>maxPushWords*4)return "push-range-size";
        for(unsigned stage=0;stage<2;++stage)if(r.stageFlags&(stage?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT)) {
            if(push[stage].signature!=p.layout->pushSignature)return "push-signature";
            for(unsigned word=r.offset/4;word<(r.offset+r.size)/4;++word)if(!(push[stage].known&(uint64_t(1)<<word)))return "push-word-unset";
        }
    }
    for(unsigned word=0;word<maxPushWords;++word) {
        VkShaderStageFlags stages=0;for(const auto& r:p.layout->pushRanges)if(word*4>=r.offset && word*4<r.offset+r.size)stages|=r.stageFlags;
        if((stages&VK_SHADER_STAGE_VERTEX_BIT) && (stages&VK_SHADER_STAGE_FRAGMENT_BIT) && push[0].words[word]!=push[1].words[word])return "push-stage-values-differ";
    }
    return nullptr;
}
// Order matches the `index` argument of the DYNAMIC hooks in discovery_commands.cpp.
struct SimpleState {const char* core;const char* ext;};
constexpr std::array<SimpleState,WaterRestore::simpleCount> waterSimpleStates{{
    {"vkCmdSetDepthTestEnable","vkCmdSetDepthTestEnableEXT"},{"vkCmdSetPrimitiveRestartEnable","vkCmdSetPrimitiveRestartEnableEXT"},
    {"vkCmdSetDepthBoundsTestEnable","vkCmdSetDepthBoundsTestEnableEXT"},{"vkCmdSetStencilTestEnable","vkCmdSetStencilTestEnableEXT"},
    {"vkCmdSetDepthWriteEnable","vkCmdSetDepthWriteEnableEXT"},{"vkCmdSetDepthCompareOp","vkCmdSetDepthCompareOpEXT"},
    {"vkCmdSetCullMode","vkCmdSetCullModeEXT"},{"vkCmdSetFrontFace","vkCmdSetFrontFaceEXT"},{"vkCmdSetDepthBiasEnable","vkCmdSetDepthBiasEnableEXT"},
    {"vkCmdSetPrimitiveTopology","vkCmdSetPrimitiveTopologyEXT"},{"vkCmdSetRasterizerDiscardEnable","vkCmdSetRasterizerDiscardEnableEXT"}}};
// Puts back every dynamic state the application had set (each with the core or EXT entry point it used itself).
void restore_dynamic(const DeviceData& data,VkCommandBuffer cmd,const WaterRestore& r) {
    using Word=void(VKAPI_PTR*)(VkCommandBuffer,uint32_t);
    if(r.set&WaterRestore::ViewportCount)if(auto f=raw<PFN_vkCmdSetViewportWithCount>(data,(r.ext&WaterRestore::ViewportCount)?"vkCmdSetViewportWithCountEXT":"vkCmdSetViewportWithCount"))f(cmd,1,&r.viewport);
    if(r.set&WaterRestore::ScissorCount)if(auto f=raw<PFN_vkCmdSetScissorWithCount>(data,(r.ext&WaterRestore::ScissorCount)?"vkCmdSetScissorWithCountEXT":"vkCmdSetScissorWithCount"))f(cmd,1,&r.scissor);
    if(r.set&WaterRestore::Viewport)fns(data).setViewport(cmd,0,1,&r.viewport);
    if(r.set&WaterRestore::Scissor)fns(data).setScissor(cmd,0,1,&r.scissor);
    if(r.set&WaterRestore::Bias)if(auto f=raw<PFN_vkCmdSetDepthBias>(data,"vkCmdSetDepthBias"))f(cmd,r.biasConstant,r.biasClamp,r.biasSlope);
    if(r.set&WaterRestore::BlendConstants)if(auto f=raw<PFN_vkCmdSetBlendConstants>(data,"vkCmdSetBlendConstants"))f(cmd,r.blendConstants);
    if(r.set&WaterRestore::LineWidth)if(auto f=raw<PFN_vkCmdSetLineWidth>(data,"vkCmdSetLineWidth"))f(cmd,r.lineWidth);
    if(r.set&WaterRestore::DepthBounds)if(auto f=raw<PFN_vkCmdSetDepthBounds>(data,"vkCmdSetDepthBounds"))f(cmd,r.depthBoundsLow,r.depthBoundsHigh);
    if(r.set&WaterRestore::LogicOp)if(auto f=raw<PFN_vkCmdSetLogicOpEXT>(data,"vkCmdSetLogicOpEXT"))f(cmd,r.logicOp);
    if(r.set&WaterRestore::BlendEnable)if(auto f=raw<PFN_vkCmdSetColorBlendEnableEXT>(data,"vkCmdSetColorBlendEnableEXT")){VkBool32 v=r.blendEnable;f(cmd,0,1,&v);}
    if(r.set&WaterRestore::BlendEquation)if(auto f=raw<PFN_vkCmdSetColorBlendEquationEXT>(data,"vkCmdSetColorBlendEquationEXT"))f(cmd,0,1,&r.blendEquation);
    if(r.set&WaterRestore::ColorWriteMask)if(auto f=raw<PFN_vkCmdSetColorWriteMaskEXT>(data,"vkCmdSetColorWriteMaskEXT")){VkColorComponentFlags v=r.colorWriteMask;f(cmd,0,1,&v);}
    for(size_t i=0;i<waterSimpleStates.size();++i)if(r.simpleSet&(1u<<i))
        if(auto f=reinterpret_cast<Word>(raw<PFN_vkVoidFunction>(data,(r.ext&(1u<<(16+i)))?waterSimpleStates[i].ext:waterSimpleStates[i].core)))f(cmd,r.simple[i]);
    const auto face=[&](unsigned kind,const char* name,const std::array<uint32_t,2>& values) {
        if(!r.stencilFaces[kind])return;auto f=raw<PFN_vkCmdSetStencilCompareMask>(data,name);if(!f)return;
        for(unsigned i=0;i<2;++i)if(r.stencilFaces[kind]&(1u<<i))f(cmd,VkStencilFaceFlags(1u<<i),values[i]);
    };
    face(0,"vkCmdSetStencilCompareMask",r.stencilCompare);face(1,"vkCmdSetStencilWriteMask",r.stencilWrite);face(2,"vkCmdSetStencilReference",r.stencilReference);
    if(r.stencilFaces[3])if(auto f=raw<PFN_vkCmdSetStencilOp>(data,(r.ext&WaterRestore::StencilOp)?"vkCmdSetStencilOpEXT":"vkCmdSetStencilOp"))
        for(unsigned i=0;i<2;++i)if(r.stencilFaces[3]&(1u<<i))f(cmd,VkStencilFaceFlags(1u<<i),r.stencilOp[i].fail,r.stencilOp[i].pass,r.stencilOp[i].depthFail,r.stencilOp[i].compare);
}
void push_values(const DeviceData& data,VkCommandBuffer cmd,VkPipelineLayout layout,const Layout& l,const std::array<PushState,2>& push) {
    auto fn=fns(data).pushConstants;
    // One call per contiguous run of words with the same stages (a word covered by both a vertex and a fragment range is pushed
    // from the vertex state, as before), instead of one call per word.
    unsigned start=0;VkShaderStageFlags runStages=0;
    auto flush=[&](unsigned end){if(runStages && end>start)fn(cmd,layout,runStages,start*4,(end-start)*4,push[(runStages&VK_SHADER_STAGE_VERTEX_BIT)?0:1].words.data()+start);};
    for(unsigned word=0;word<=maxPushWords;++word) {
        VkShaderStageFlags stages=0;
        if(word<maxPushWords)for(const auto& r:l.pushRanges)if(word*4>=r.offset && word*4<r.offset+r.size)stages|=r.stageFlags;
        const bool sameSource=((stages&VK_SHADER_STAGE_VERTEX_BIT)!=0)==((runStages&VK_SHADER_STAGE_VERTEX_BIT)!=0);
        if(stages!=runStages || !sameSource){flush(word);start=word;runStages=stages;}
    }
}
std::string sets_problem(const Device& s,const Pipeline& p,const std::array<BoundSet,maxSets>& sets) {
    if(!p.layout)return "no-layout";
    if(p.layout->sets.size()>3)return "layout-sets>3";
    for(size_t i=0;i<sets.size();++i)if(sets[i].handle || sets[i].pushed) {
        const auto& b=sets[i];auto live=find_ptr(s.sets,b.handle);const auto name="set"+std::to_string(i)+"-";
        if(i>=p.layout->sets.size())return name+"beyond-layout";
        if(b.pushed)return name+"push-descriptors";
        if(!live)return name+"untracked";
        if(live->generation!=b.generation)return name+"stale";
        if(!b.layout)return name+"layout-unknown";
        if(b.layout->signature!=p.layout->signature)return name+"layout-mismatch";
    }
    return {};
}
void bind_sets(const DeviceData& data,VkCommandBuffer cmd,VkPipelineLayout layout,const std::array<BoundSet,maxSets>& sets) {
    auto fn=fns(data).bindDescriptorSets;
    for(uint32_t i=0;i<sets.size();++i)if(sets[i].handle) {
        auto set=(VkDescriptorSet)sets[i].handle;fn(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,i,1,&set,sets[i].offsets.size(),sets[i].offsets.data());
    }
}
bool live_buffer(const Device& s,const BufferBinding& b) {
    auto live=find_ptr(s.buffers,b.buffer);return live && live->generation==b.generation && b.offset<live->size;
}
// withStride=false binds without the dynamic stride: private replay pipelines carry their strides statically, and setting a stride
// that is static in the bound pipeline before re-binding that same pipeline is not portable. The restore path uses withStride=true
// because the application's own pipeline declares the stride dynamic.
void bind_vertices(const DeviceData& data,VkCommandBuffer cmd,const std::array<BufferBinding,16>& vertices,const BufferBinding& index,VkIndexType type,bool withStride=true) {
    const auto& f=fns(data);auto bind=f.bindVertexBuffers;auto bind2=f.bindVertexBuffers2;
    for(uint32_t i=0;i<vertices.size();++i)if(!vertices[i].buffer && vertices[i].bound) {
        // The application bound VK_NULL_HANDLE here (nullDescriptor is therefore enabled); reads return zero. Do the same.
        VkBuffer none{};VkDeviceSize zero=0,whole=VK_WHOLE_SIZE,stride=vertices[i].stride;
        if(withStride && bind2 && stride)bind2(cmd,i,1,&none,&zero,&whole,&stride);else bind(cmd,i,1,&none,&zero);
    } else if(vertices[i].buffer) {
        const auto& v=vertices[i];auto buffer=(VkBuffer)v.buffer;VkDeviceSize offset=v.offset,size=v.size?v.size:VK_WHOLE_SIZE,stride=v.stride;
        if(withStride && bind2 && v.stride)bind2(cmd,i,1,&buffer,&offset,&size,&stride);else bind(cmd,i,1,&buffer,&offset);
    }
    if(index.buffer)fns(data).bindIndexBuffer(cmd,(VkBuffer)index.buffer,index.offset,type);
}
// Why latest_layout gave up: which layout events exist for the image and what the last one looked like.
std::string layout_history(const Command& c,const Image& image) {
    size_t count=0,lastIndex=0;const Event* last=nullptr;
    for(size_t i=0;i<c.events.size();++i)if(c.events[i].layout.image && c.events[i].layout.image->handle==image.handle && c.events[i].layout.image->generation==image.generation){++count;last=&c.events[i];lastIndex=i;}
    if(!last)return "no-layout-event-in-command";
    const auto& r=last->layout.range;
    return "events="+std::to_string(count)+" last#"+std::to_string(lastIndex)+"/"+std::to_string(c.events.size())+" layout="+std::to_string(int(last->layout.layout))+
           (last->layout.known?" known":" NOT-known(pNext/queue-transfer/concurrent)")+" range=mip"+std::to_string(r.baseMipLevel)+"+"+std::to_string(r.levelCount)+"/layer"+std::to_string(r.baseArrayLayer)+"+"+std::to_string(r.layerCount);
}
VkImageLayout latest_layout(const Command& c,const Image& image) {
    for(auto it=c.events.rbegin();it!=c.events.rend();++it)if(it->layout.image==nullptr)continue;
    else if(it->layout.image->handle==image.handle && it->layout.image->generation==image.generation) {
        const auto& e=it->layout;
        if(!e.known || e.range.baseMipLevel || e.range.baseArrayLayer || (e.range.levelCount!=VK_REMAINING_MIP_LEVELS && e.range.levelCount<image.mips) ||
           (e.range.layerCount!=VK_REMAINING_ARRAY_LAYERS && e.range.layerCount<image.layers))return VK_IMAGE_LAYOUT_UNDEFINED;
        return e.layout;
    }
    // A previous submit's layout alone is insufficient to prove same-queue use.
    return VK_IMAGE_LAYOUT_UNDEFINED;
}
// Layout events for `image` after the draw `d` in this command buffer: none (untouched = true) means the image is still in the layout
// the draw sampled it in - which Vulkan requires to be the layout its descriptor states. Otherwise the last event's layout
// (UNDEFINED when it covered only part of the image or was unknown). A draw not found in the events counts as touched.
VkImageLayout layout_after_draw(const Command& c,const Draw& d,const Image& image,bool& untouched) {
    untouched=false;
    size_t at=c.events.size();
    for(size_t i=c.events.size();i-->0;)if(c.events[i].draw.get()==&d){at=i;break;}
    if(at==c.events.size())return latest_layout(c,image);
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;bool seen=false;
    for(size_t i=at+1;i<c.events.size();++i) {
        const auto& e=c.events[i].layout;
        if(!e.image || e.image->handle!=image.handle || e.image->generation!=image.generation)continue;
        seen=true;
        const bool whole=!e.range.baseMipLevel && !e.range.baseArrayLayer && (e.range.levelCount==VK_REMAINING_MIP_LEVELS || e.range.levelCount>=image.mips) &&
                         (e.range.layerCount==VK_REMAINING_ARRAY_LAYERS || e.range.layerCount>=image.layers);
        layout=e.known && whole?e.layout:VK_IMAGE_LAYOUT_UNDEFINED;
    }
    untouched=!seen;
    return layout;
}
// Input locations a vertex shader reads (Input-class variables with a Location decoration), cached per shader.
// Empty when the module cannot be scanned, which callers treat as "every binding is used".
const std::set<uint32_t>& vertex_input_locations(const Shader& shader) {
    static std::unordered_map<uint64_t,std::set<uint32_t>> cache;
    auto found=cache.find(shader.hash);if(found!=cache.end())return found->second;
    std::set<uint32_t> locations;std::unordered_map<uint32_t,uint32_t> decorated;std::vector<uint32_t> inputs;
    const auto& w=shader.words;bool ok=w.size()>5 && w[0]==0x07230203;
    for(size_t i=5;ok && i<w.size();) {
        const uint32_t count=w[i]>>16,op=w[i]&65535;
        if(!count || count>w.size()-i){ok=false;break;}
        if(op==71 && count==4 && w[i+2]==30)decorated[w[i+1]]=w[i+3];            // OpDecorate <id> Location <n>
        if(op==59 && count>=4 && w[i+3]==1)inputs.push_back(w[i+2]);               // OpVariable <type> <id> Input
        i+=count;
    }
    if(ok)for(auto id:inputs)if(decorated.count(id))locations.insert(decorated[id]);
    return cache.emplace(shader.hash,std::move(locations)).first->second;
}
// Vertex bindings that carry at least one attribute the vertex shader reads. A binding no attribute reads may stay unbound.
std::set<uint32_t> used_vertex_bindings(const Pipeline& p) {
    std::set<uint32_t> out;const std::set<uint32_t>* locations=p.vs?&vertex_input_locations(*p.vs):nullptr;
    if(!locations || locations->empty()){for(const auto& b:p.water->bindings)out.insert(b.binding);return out;}
    for(const auto& a:p.water->attributes)if(locations->count(a.location))out.insert(a.binding);
    return out;
}
// The sea is ~170 draws that differ only in their vertex/index ranges. Once one of them passed resources_ready, another that is identical in
// everything that verdict depends on (pipeline, target, dynamic state, descriptor sets, push constants, vertex/index buffer identity) needs only
// its own buffers checked for liveness, which is what makes the replay cost per draw a few microseconds instead of a full re-validation.
bool same_dynamic(const Dynamic& x,const Dynamic& y) {
    return x.valid==y.valid && std::memcmp(&x.viewport,&y.viewport,sizeof(VkViewport))==0 && std::memcmp(&x.scissor,&y.scissor,sizeof(VkRect2D))==0 &&
           x.depthTest==y.depthTest && x.depthWrite==y.depthWrite && x.compare==y.compare && x.cull==y.cull && x.topology==y.topology && x.rasterDiscard==y.rasterDiscard &&
           x.blend==y.blend && x.frontFace==y.frontFace && x.biasEnable==y.biasEnable && x.biasConstant==y.biasConstant && x.biasClamp==y.biasClamp && x.biasSlope==y.biasSlope &&
           x.stencil==y.stencil && x.depthBounds==y.depthBounds && x.restart==y.restart && x.blendSrc==y.blendSrc && x.blendDst==y.blendDst;
}
bool same_water_state(const Draw& a,const Draw& b) {
    if(a.pipeline!=b.pipeline || a.kind!=b.kind || a.indexType!=b.indexType || !b.target.known || !a.target.known || !b.target.depth || b.target.depth!=a.target.depth)return false;
    if(a.target.colors.size()!=1 || b.target.colors.size()!=1 || a.target.colors[0]!=b.target.colors[0])return false;
    if(b.unsupported || b.skipped || b.waterReplayUnsafe || !b.count || b.instances!=1 || b.firstInstance || !same_dynamic(a.dynamic,b.dynamic))return false;
    for(size_t i=0;i<a.sets.size();++i) {
        const auto& x=a.sets[i];const auto& y=b.sets[i];
        if(x.handle!=y.handle || x.generation!=y.generation || x.pushed!=y.pushed || x.layout!=y.layout || x.offsets!=y.offsets)return false;
    }
    if(a.dynamicOffsets!=b.dynamicOffsets)return false;
    for(size_t k=0;k<a.push.size();++k)if(a.push[k].known!=b.push[k].known || a.push[k].signature!=b.push[k].signature || a.push[k].words!=b.push[k].words)return false;
    for(size_t i=0;i<a.vertices.size();++i)
        if(a.vertices[i].buffer!=b.vertices[i].buffer || a.vertices[i].generation!=b.vertices[i].generation || a.vertices[i].stride!=b.vertices[i].stride || a.vertices[i].bound!=b.vertices[i].bound)return false;
    return a.index.buffer==b.index.buffer && a.index.generation==b.index.generation;
}
// The per-draw part of resources_ready: this draw's own vertex and index ranges must still lie inside live buffers.
bool draw_buffers_live(const Device& s,const Draw& d) {
    for(const auto& v:d.vertices)if(v.buffer && !live_buffer(s,v))return false;
    return d.kind!=1 || live_buffer(s,d.index);
}
bool resources_ready(const Device& s,const Command& c,const Draw& d,bool water,SubmittedDraw& resolved,std::string* why=nullptr) {
    // Structural failures return at once; the rest are collected so one report names them all.
    std::string all;bool ok=true;
    auto no=[&](const std::string& reason){ok=false;if(why && all.size()<600)all+=(all.empty()?"":",")+reason;return !why;};
    auto stop=[&](const std::string& reason){no(reason);if(why)*why=all;return false;};
    if(!d.pipeline)return stop("no-pipeline");
    std::string pipelineWhy;if(!water_pipeline_check(*d.pipeline,why?&pipelineWhy:nullptr))return stop("pipeline{"+pipelineWhy+"}");
    if(d.unsupported)if(no(std::string("draw-unsupported:")+d.unsupportedReason))return false;
    if(d.skipped)if(no("draw-isolated-by-inspector"))return false;
    if(d.waterReplayUnsafe)if(no("query-conditional-or-xfb"))return false;
    if(d.kind>1)if(no("indirect-draw"))return false;
    if(!d.count)if(no("empty-draw"))return false;
    if(d.instances!=1 || d.firstInstance)if(no("instanced-draw"))return false;
    if(!d.target.known)return stop("target-unknown");
    if(!d.target.depth)return stop("no-depth-attachment");
    if(d.target.colors.size()!=1)return stop("color-attachments:"+std::to_string(d.target.colors.size()));
    if(auto problem=push_problem(*d.pipeline,d.push))if(no(problem))return false;
    if(auto problem=sets_problem(s,*d.pipeline,d.sets);!problem.empty())if(no(problem))return false;
    const auto e=effective_state(d);const uint32_t need=Viewport|Scissor|DepthTest|DepthWrite|Compare|Cull|Topology|Discard|Blend|FrontFace|BiasEnable|Stencil;
    if((e.valid&need)!=need)if(no("state-never-set:0x"+hex(need&~e.valid)))return false;
    if(e.stencil)if(no("stencil-test"))return false;
    if(e.rasterDiscard)if(no("rasterizer-discard"))return false;
    if(!e.depthTest)if(no("depth-test-off"))return false;
    if(e.compare!=VK_COMPARE_OP_LESS && e.compare!=VK_COMPARE_OP_LESS_OR_EQUAL)if(no("depth-compare:"+std::to_string(e.compare)))return false;
    if(e.topology<VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || e.topology>VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)if(no("topology:"+std::to_string(e.topology)))return false;
    if(!std::isfinite(e.viewport.height) || e.viewport.width<=0 || !e.viewport.height)if(no("viewport"))return false;
    if(d.pipeline->dynamic.count(VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE)) {
        if(!(e.valid&DepthBoundsEnable) && no("depth-bounds-enable-never-set"))return false;
        if((e.valid&DepthBoundsEnable) && e.depthBounds && no("depth-bounds-on"))return false;
    }
    else if(d.pipeline->water->depthBounds)if(no("depth-bounds-on"))return false;
    if(d.pipeline->dynamic.count(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE) && !(e.valid&Restart))if(no("restart-enable-never-set"))return false;
    const auto usedBindings=used_vertex_bindings(*d.pipeline);
    for(const auto& b:d.pipeline->water->bindings) {
        if(b.binding>=16){if(no("vertex-binding>=16"))return false;continue;}
        const bool nullBound=d.vertices[b.binding].bound && !d.vertices[b.binding].buffer;
        if(usedBindings.count(b.binding) && !nullBound && d.pipeline->dynamic.count(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE) && !d.vertices[b.binding].stride)if(no("vertex-stride-never-set:"+std::to_string(b.binding)))return false;
    }
    if(water && !e.blend)if(no("water-not-blended"))return false;
    if(water && e.depthWrite)if(no("water-writes-depth"))return false;
    if(!water && (e.blend || !e.depthWrite || (d.pipeline->colorMask&7)!=7))if(no("not-opaque-depth-writing"))return false;
    for(const auto& binding:d.pipeline->water->bindings) {
        if(!usedBindings.count(binding.binding))continue;
        const bool nullBound=binding.binding<d.vertices.size() && d.vertices[binding.binding].bound && !d.vertices[binding.binding].buffer;
        if(!nullBound && (binding.binding>=d.vertices.size() || !live_buffer(s,d.vertices[binding.binding])))
            if(no("vertex-buffer-"+std::to_string(binding.binding)+"-not-live"))return false;
    }
    if(d.kind==1 && !live_buffer(s,d.index))if(no("index-buffer-not-live"))return false;
    if(d.kind==1 && d.indexType!=VK_INDEX_TYPE_UINT16 && d.indexType!=VK_INDEX_TYPE_UINT32)if(no("index-type"))return false;
    resolved={};resolved.draw=d;resolve_resources(s,resolved);
    // Scene draws replayed for the planar image re-bind the application's own descriptor sets, so a sampled binding the layer could not
    // resolve (never written, or a null descriptor) reads exactly what it read originally. Images it CAN resolve are still checked below.
    const uint32_t tolerated=water?0u:uint32_t(SamplerIndexUnknown|MissingSampledBinding);
    const bool descriptorsOk=water?coverage_resources_supported(resolved)
        :((resolved.descriptorIssues&~tolerated)==0 && std::none_of(resolved.resources.begin(),resolved.resources.end(),[](const ResourceUse& r){return r.updateAfterBind && !(r.samplerIndexed && r.descriptor.type==VK_DESCRIPTOR_TYPE_SAMPLER && r.descriptor.known && r.descriptor.sampler);}));
    if(!descriptorsOk) {
        size_t unresolvedUab=0;for(const auto& r:resolved.resources)if(r.updateAfterBind && !(r.samplerIndexed && r.descriptor.type==VK_DESCRIPTOR_TYPE_SAMPLER && r.descriptor.known && r.descriptor.sampler))++unresolvedUab;
        if(no("descriptors-unresolved[issues=0x"+hex(resolved.descriptorIssues)+" unresolvedUpdateAfterBind="+std::to_string(unresolvedUab)+"]"))return false;
    }
    // Replay happens at pass end. Reject attachments sampled by the draw and
    // layouts changed since the original draw; never invent a transition for it.
    for(const auto& r:resolved.resources)if(r.descriptor.view && r.descriptor.view->image) {
        const auto& im=*r.descriptor.view->image;const auto where="set"+std::to_string(r.set)+"/binding"+std::to_string(r.binding);
        if(im.handle==d.target.colors[0]->image->handle || im.handle==d.target.depth->image->handle)if(no("samples-render-target:"+where))return false;
        auto live=find_ptr(s.images,im.handle);if(!live || live->generation!=im.generation)if(no("image-dead:"+where))return false;
        // Not transitioned since the draw sampled it (the replay runs later in the same command buffer): it is in the layout the
        // application's descriptor states. The layer's global layout record is not needed for that - and must not be: it is wiped
        // whenever a submit is truncated or holds an unrecorded command buffer (accept_submit), and static textures such as the water's
        // animated ones are never transitioned again after their upload, so the water stayed blocked for the rest of the session
        // (2026-09-29, Booty Bay open sea: image-layout-unobserved:set1/binding0,1 after ~5000 working passes).
        bool untouched=false;
        auto layout=layout_after_draw(c,d,im,untouched);
        if(untouched)layout=r.descriptor.layout;
        else if(layout==VK_IMAGE_LAYOUT_UNDEFINED)if(no("image-layout-unobserved:"+where))return false;
        if(layout!=r.descriptor.layout)if(no("image-layout-changed:"+where))return false;
        if(layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && layout!=VK_IMAGE_LAYOUT_GENERAL)if(no("image-layout-unsupported:"+where+"="+std::to_string(layout)))return false;
    }
    if(why)*why=all;
    return ok;
}
// The layer adds SAMPLED to eligible depth images itself, after the application's
// create info was recorded, so Image::usage alone under-reports; the depth registry
// knows the usage the image was really created with.
bool depth_sampled(VkDevice device,const Image& im) {
    if(im.usage&VK_IMAGE_USAGE_SAMPLED_BIT)return true;
    std::lock_guard lock(g_state_mutex);
    auto found=g_depth_candidates.find({device,image_handle_u64(reinterpret_cast<VkImage>(im.handle))});
    return found!=g_depth_candidates.end() && found->second.sampledReady;
}
const char* target_problem(VkDevice device,const Target& t) {
    if(!t.known)return "target-unknown";
    if(t.colors.size()!=1 || !t.colors[0])return "not-one-color-attachment";
    if(!t.depth)return "no-depth-attachment";
    if(!t.width || !t.height || t.width>4096 || t.height>2160)return "target-extent";
    for(auto v:{t.colors[0],t.depth}) {
        if(!v->image)return "view-without-image";
        if(v->type!=VK_IMAGE_VIEW_TYPE_2D || v->range.baseMipLevel || v->range.baseArrayLayer || v->range.levelCount!=1 || v->range.layerCount!=1)return "view-not-single-2d";
        const auto& im=*v->image;
        if(im.samples!=1)return "image-multisampled";
        if(im.layers!=1 || im.mips!=1 || im.type!=VK_IMAGE_TYPE_2D)return "image-layers-mips-type";
        if(im.sharing!=VK_SHARING_MODE_EXCLUSIVE)return "image-concurrent";
        if(im.flags&~(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT))return "image-flags";
        if(im.extent.width!=t.width || im.extent.height!=t.height)return "image-extent-differs-from-render-area";
    }
    if(!(t.colors[0]->image->usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT))return "color-image-no-transfer-src";
    if(!depth_sampled(device,*t.depth->image))return "depth-image-not-sampled";
    return nullptr;
}
void transition(WaterBatch& b,VkCommandBuffer cmd,VkImage image,VkImageAspectFlags aspect,VkImageLayout oldLayout,VkImageLayout newLayout) {
    VkImageMemoryBarrier v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;v.srcAccessMask=oldLayout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    v.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;v.oldLayout=oldLayout;v.newLayout=newLayout;v.srcQueueFamilyIndex=v.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    v.image=image;v.subresourceRange={aspect,0,1,0,1};b.f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&v);
}
// The water colour's mip chain, after the mask pass wrote level 0 (and left it SHADER_READ_ONLY): each level a linear 2:1 blit of the one above
// (a 2x2 box), so the composite can read the game's water colour with its texture averaged away. Ends with every level SHADER_READ_ONLY.
void water_color_mips(WaterBatch& b,VkCommandBuffer cmd) {
    const auto& im=b.waterColor;if(im.levels<2)return;
    auto blit=raw<PFN_vkCmdBlitImage>(b.data,"vkCmdBlitImage");
    auto barrier=[&](uint32_t level,uint32_t count,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;v.srcAccessMask=src;v.dstAccessMask=dst;v.oldLayout=from;v.newLayout=to;
        v.srcQueueFamilyIndex=v.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;v.image=im.image;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,level,count,0,1};
        b.f.cmdPipelineBarrier(cmd,srcStage,dstStage,0,0,nullptr,0,nullptr,1,&v);
    };
    barrier(0,1,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(1,im.levels-1,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    int32_t w=int32_t(b.width),h=int32_t(b.height);
    for(uint32_t level=1;level<im.levels;++level) {
        const int32_t nw=std::max(w/2,1),nh=std::max(h/2,1);
        VkImageBlit r{};r.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level-1,0,1};r.srcOffsets[1]={w,h,1};r.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,1};r.dstOffsets[1]={nw,nh,1};
        blit(cmd,im.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,im.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&r,VK_FILTER_LINEAR);
        barrier(level,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        w=nw;h=nh;
    }
    barrier(0,im.levels,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}
void begin_pass(WaterBatch& b,VkCommandBuffer cmd,VkRenderPass pass,VkFramebuffer fb,uint32_t w,uint32_t h,bool depth,uint32_t colorCount=1) {
    VkClearValue clear[3]{};clear[colorCount].depthStencil.depth=1;
    VkRenderPassBeginInfo info{};info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;info.renderPass=pass;info.framebuffer=fb;info.renderArea.extent={w,h};info.clearValueCount=colorCount+(depth?1:0);info.pClearValues=clear;
    b.f.cmdBeginRenderPass(cmd,&info,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0,0,float(w),float(h),0,1};VkRect2D scissor{{0,0},{w,h}};
    fns(b.data).setViewport(cmd,0,1,&viewport);fns(b.data).setScissor(cmd,0,1,&scissor);
}
// Consecutive replayed draws that only differ in their vertex/index ranges (the sea tiles) skip everything they share: descriptor sets,
// push constants, pipeline, viewport and scissor stay bound from the previous draw. Each pass starts with a fresh state.
struct ReplayState {const Draw* last=nullptr;VkPipeline pipeline{};};
void replay(WaterBatch& b,VkDescriptorSet cameraSet,VkCommandBuffer cmd,const Draw& d,VkPipeline pipeline,ReplayState& state) {
    const auto& f=fns(b.data);
    auto layout=d.pipeline->layout->water->layout;
    if(state.last && state.pipeline==pipeline && same_water_state(*state.last,d)) {
        const auto& previous=*state.last;bool rangesChanged=d.kind==1?d.index.offset!=previous.index.offset:false;
        for(size_t i=0;i<d.vertices.size() && !rangesChanged;++i)rangesChanged=d.vertices[i].offset!=previous.vertices[i].offset || d.vertices[i].size!=previous.vertices[i].size;
        if(rangesChanged)bind_vertices(b.data,cmd,d.vertices,d.kind==1?d.index:BufferBinding{},d.indexType,false);
    } else {
        const auto e=effective_state(d);
        bind_sets(b.data,cmd,layout,d.sets);b.f.cmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,1,&cameraSet,0,nullptr);
        bind_vertices(b.data,cmd,d.vertices,d.kind==1?d.index:BufferBinding{},d.indexType,false);push_values(b.data,cmd,layout,*d.pipeline->layout,d.push);
        b.f.cmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);f.setViewport(cmd,0,1,&e.viewport);f.setScissor(cmd,0,1,&e.scissor);
    }
    if(d.kind==1)f.drawIndexed(cmd,d.count,d.instances,d.first,d.vertexOffset,d.firstInstance);
    else b.f.cmdDraw(cmd,d.count,d.instances,d.first,d.firstInstance);
    state.last=&d;state.pipeline=pipeline;
}
}
const char* water_push_problem(const Pipeline& p,const std::array<PushState,2>& push){return push_problem(p,push);}
VkImageLayout layout_since_draw(const Command& c,const Draw& d,const Image& image,bool& untouched){return layout_after_draw(c,d,image,untouched);}
const char* water_target_problem(VkDevice device,const Target& target){return target_problem(device,target);}
// Seconds since the layer first asked, wrapped so float precision stays fine; drives the wave animation.
bool water_frozen() {
    static const bool frozen=[]{const char* e=std::getenv("DEPTH_PROBE_WATER_FREEZE");return e && e[0]=='1';}(); // tests compare pixels exactly
    return frozen;
}
// The sun for the MODERN water (params.sun / sunColor): the game's light direction and its glow colour, read from the game's memory at most
// every 100 ms (two small reads, as the fog does). The light direction, not the "sky address": in the fog captures the disc tracked on the
// screen sat 2 degrees above the light direction (23634-1: 22.6 vs 20.7), the sky address 30 degrees higher (INFERRED from those captures).
// Weather dims it: medium rain hides the sun. No sun (w = 0) below the horizon or when nothing can be read and no ini fallback is set.
void water_update_sun(WaterState& w) {
    if(water_frozen()){w.sun[3]=0;return;}   // exact-pixel tests: no sun unless a test sets one
    const auto now=std::chrono::steady_clock::now();
    if(w.sunRead.time_since_epoch().count() && now-w.sunRead<std::chrono::milliseconds(100))return;
    w.sunRead=now;
    const auto sun=read_wow_sun(self_memory_reader(),wow_sun_layout_from_env());
    float overcast=0;
    if(const auto weather=read_wow_weather(self_memory_reader(),wow_weather_layout_from_env());weather.ok)overcast=weather_overcast(weather);
    if(sun.ok && sun.toward[2]>-.035f) {
        for(int c=0;c<3;++c){w.sun[c]=sun.toward[c];w.sunColor[c]=sun.specular[c]*(1.f-overcast);}
        w.sun[3]=1;
    } else if(w.sunFallback) {
        const float e=w.sunFallbackDeg[0]*3.14159265f/180.f,a=w.sunFallbackDeg[1]*3.14159265f/180.f;
        w.sun[0]=std::cos(e)*std::cos(a);w.sun[1]=std::cos(e)*std::sin(a);w.sun[2]=std::sin(e);w.sun[3]=2;
        for(int c=0;c<3;++c)w.sunColor[c]=1.f-overcast;
    } else w.sun[3]=0;
}
double water_time_seconds() {
    if(water_frozen())return 0.;
    static const auto start=std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
}
float water_time() {
    if(water_frozen())return 0.f;
    static const auto start=std::chrono::steady_clock::now();
    return float(std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(),3600.0));
}
void water_advance_foam_clock(WaterState& w,std::chrono::steady_clock::time_point now) {
    if(water_frozen())w.foamClock=0;
    else{
        const double dt=w.foamClockLast.time_since_epoch().count()?std::chrono::duration<double>(now-w.foamClockLast).count():0.;
        w.foamClock=std::fmod(w.foamClock+std::clamp(dt,0.,.25)*w.look.foamSpeed,86400.);   // a stall is not a jump; the speed only scales what is added
    }
    w.foamClockLast=now;
}
void water_look_cycle(WaterLook& look,size_t option) {
    if(option>=waterLookOptionCount)return;
    const auto& o=waterLookOptions[option];float& value=look.*o.value;
    unsigned nearest=0;for(unsigned i=1;i<o.count;++i)if(std::abs(o.steps[i]-value)<std::abs(o.steps[nearest]-value))nearest=i;
    value=o.steps[(nearest+1)%o.count];
}
void water_look_cycle_quality(WaterLook& look){look.quality=(look.quality+1)%waterQualityCount;}
std::string water_look_label(const WaterLook& look,size_t option) {
    if(option>=waterLookOptionCount)return {};
    const auto& o=waterLookOptions[option];return std::string(o.label)+": "+std::to_string(int(look.*o.value*100.f+.5f))+"%";
}
std::string water_settings_text(const WaterState& w) {
    std::ostringstream out;out.precision(6);
    out<<"# Water reflections (saved automatically from the Water Reflections menu)\n"
       <<"profile="<<(w.profileSaved&&w.selectedVS?1:0)<<"\nmode="<<w.mode<<"\nstrength="<<w.strength<<"\nlayers="<<w.layers<<"\ntone="<<w.tone<<"\n"
       <<"quality="<<w.look.quality<<"\nstyle="<<w.style<<"\n";
    for(const auto& o:waterLookOptions)out<<o.key<<"="<<w.look.*o.value<<"\n";
    out<<"# Gerstner waves: waveN=direction (deg, + wind_direction),wavelength (yd),amplitude (yd),steepness (0..1 orbit shape),speed (x physical)\n"
       <<"wind_direction="<<w.waves.windDeg<<"\nwave_speed="<<w.waves.speed<<"\n"
       <<"# MODERN: absorption per yard r,g,b (x clarity scale; default pure water); sun_fallback=elevation,azimuth (degrees) or off\n"
       <<"absorption="<<w.absorption[0]<<","<<w.absorption[1]<<","<<w.absorption[2]<<"\n"
       <<"sun_fallback="<<(w.sunFallback?std::to_string(w.sunFallbackDeg[0])+","+std::to_string(w.sunFallbackDeg[1]):std::string("off"))<<"\n";
    for(unsigned i=0;i<water::maxWaves;++i){const auto& v=w.waves.waves[i];out<<"wave"<<i+1<<"="<<v.directionDeg<<","<<v.wavelength<<","<<v.amplitude<<","<<v.steepness<<","<<v.speed<<"\n";}
    return out.str();
}
bool water_settings_parse(WaterState& w,const std::string& text) {
    bool ok=true;std::istringstream in(text);std::string line;
    auto number=[&](const std::string& value,double low,double high,double& out){
        char* end=nullptr;const double v=std::strtod(value.c_str(),&end);
        if(value.empty() || end==value.c_str() || *end || !std::isfinite(v) || v<low || v>high){ok=false;return false;}
        out=v;return true;
    };
    while(std::getline(in,line)) {
        if(line.empty() || line[0]=='#')continue;
        const auto eq=line.find('=');if(eq==std::string::npos)continue;
        const std::string key=line.substr(0,eq),value=line.substr(eq+1);double v=0;
        if(key=="profile"){if(number(value,0,1,v)){w.profileSaved=v>.5;if(w.profileSaved){w.selectedVS=wowWaterVS;w.selectedFS=wowWaterFS;w.selectedSignature=0;w.depthGeneration=0;}}}
        else if(key=="mode"){if(number(value,0,2,v))w.mode=uint32_t(v);}
        else if(key=="strength"){if(number(value,.05,1.5,v))w.strength=float(v);}
        else if(key=="layers"){if(number(value,0,waterLayersAll,v))w.layers=uint32_t(v);}
        else if(key=="tone"){if(number(value,0,waterToneCount-1,v))w.tone=uint32_t(v);}
        else if(key=="quality"){if(number(value,0,waterQualityCount-1,v))w.look.quality=uint32_t(v);}
        else if(key=="style"){if(number(value,0,1,v))w.style=uint32_t(v);}
        else if(key=="absorption"||key=="sun_fallback") {
            // comma-separated numbers; the setting changes only when all of them are valid ("off" turns the fallback sun off)
            if(key=="sun_fallback" && value=="off"){w.sunFallback=false;continue;}
            const unsigned n=key=="absorption"?3:2;double f[3]{};std::string rest=value;bool all=true;
            for(unsigned i=0;i<n && all;++i){const auto comma=rest.find(',');all=number(rest.substr(0,comma),key=="absorption"?0.:(i?-360.:-90.),key=="absorption"?20.:(i?360.:90.),f[i]);
                rest=comma==std::string::npos?std::string():rest.substr(comma+1);if(i+1<n && comma==std::string::npos){all=false;ok=false;}}
            if(all && key=="absorption")for(unsigned i=0;i<3;++i)w.absorption[i]=float(f[i]);
            if(all && key=="sun_fallback"){w.sunFallback=true;w.sunFallbackDeg[0]=float(f[0]);w.sunFallbackDeg[1]=float(f[1]);}
        }
        else if(key=="wind_direction"){if(number(value,-360,360,v))w.waves.windDeg=float(v);}
        else if(key=="wave_speed"){if(number(value,0,4,v))w.waves.speed=float(v);}
        else if(key.size()==5 && key.compare(0,4,"wave")==0 && key[4]>='1' && key[4]<char('0'+water::maxWaves+1)) {
            // five comma-separated numbers; the slot changes only when all of them are valid
            double f[5];std::string rest=value;bool all=true;
            const double low[5]{-360,0,0,0,0},high[5]{360,500,5,1,4};
            for(int i=0;i<5 && all;++i){const auto comma=rest.find(',');const std::string part=rest.substr(0,comma);all=number(part,low[i],high[i],f[i]);rest=comma==std::string::npos?std::string():rest.substr(comma+1);if(i<4 && comma==std::string::npos){all=false;ok=false;}}
            if(all)w.waves.waves[size_t(key[4]-'1')]={float(f[0]),float(f[1]),float(f[2]),float(f[3]),float(f[4])};
        }
        else {
            for(const auto& o:waterLookOptions)if(key==o.key){
                double low=o.steps[0],high=o.steps[0];for(unsigned i=1;i<o.count;++i){low=std::min<double>(low,o.steps[i]);high=std::max<double>(high,o.steps[i]);}
                if(number(value,low-1e-6,high+1e-6,v))w.look.*o.value=float(v);   // steps are floats: a saved boundary value must not fall outside its own range
            }
        }
    }
    return ok;
}
std::string water_settings_path() {
    if(const char* e=std::getenv("DEPTH_PROBE_WATER_SETTINGS");e && *e)return e;
    return (std::filesystem::path(settings_config_path()).parent_path()/"water.ini").string();
}
void water_settings_load(WaterState& w) {
    const auto path=water_settings_path();std::ifstream in(path);
    if(!in) {
        // First run (no water.ini at the default path): reflections on with the known WoW water shaders - every custom effect starts on
        // (2026-10-08). An explicit DEPTH_PROBE_WATER_SETTINGS path (tests) keeps the off defaults.
        const char* e=std::getenv("DEPTH_PROBE_WATER_SETTINGS");
        if(!(e && *e)){w.profileSaved=true;w.selectedVS=wowWaterVS;w.selectedFS=wowWaterFS;w.selectedSignature=0;w.depthGeneration=0;w.mode=1;
            log_line("[WATER-SETTINGS] no "+path+": first run, SSR on with the WoW water profile");}
        return;
    }
    std::stringstream text;text<<in.rdbuf();
    const bool ok=water_settings_parse(w,text.str());
    if(w.profileSaved && w.selectedVS)w.status="Saved water profile restored";
    log_line("[WATER-SETTINGS] loaded "+path+(ok?"":" (some values invalid, kept defaults)")+" profile="+std::to_string(w.profileSaved)+" mode="+std::to_string(w.mode)+" strength="+std::to_string(w.strength)+" layers="+std::to_string(w.layers)+" tone="+std::to_string(w.tone));
}
void water_settings_save(const WaterState& w) {
    const auto path=std::filesystem::path(water_settings_path());std::error_code ec;
    if(path.has_parent_path())std::filesystem::create_directories(path.parent_path(),ec);
    const auto temporary=path.string()+".tmp";
    {std::ofstream out(temporary,std::ios::trunc);out<<water_settings_text(w);if(!out){log_line("[WATER-SETTINGS] cannot write "+temporary);return;}}
    std::filesystem::rename(temporary,path,ec);
    if(ec)log_line("[WATER-SETTINGS] cannot replace "+path.string()+": "+ec.message());
}
WaterState& water_state(Device& s){if(!s.water){s.water=std::make_shared<WaterState>();water_settings_load(*s.water);}return *s.water;}
void capture_water_recipe(Device& s,Pipeline& p,const VkGraphicsPipelineCreateInfo& info) {
    if(!inspector_enabled())return;
    auto recipe=std::make_shared<WaterRecipe>();p.water=recipe;
    auto& r=*recipe;const auto* libs=chain<VkPipelineLibraryCreateInfoKHR>(info.pNext,VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    const auto* subset=chain<VkGraphicsPipelineLibraryCreateInfoEXT>(info.pNext,VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
    const uint32_t own=subset?subset->flags:(libs && libs->libraryCount)?0:15;
    // Every rejection is named; "recipe-unsupported" alone hid the real cause once.
    auto reject=[&](const std::string& reason){r.supported=false;if(r.why.size()<400)r.why+=(r.why.empty()?"":";")+reason;};
    if(libs)for(uint32_t i=0;i<libs->libraryCount;++i) {
        auto lib=find_ptr(s.pipelines,id(libs->pLibraries[i]));if(!lib || !lib->water){reject("library-without-recipe");continue;}
        auto& x=*lib->water;if(!x.supported)reject("library{"+x.why+"}");
        if(x.vertexInput){r.vertexInput=true;r.bindings=x.bindings;r.attributes=x.attributes;r.restart=x.restart;}
        if(x.raster){r.raster=true;r.rasterInfo=x.rasterInfo;r.clipExtension=x.clipExtension;r.depthClip=x.depthClip;r.vs=x.vs;}
        if(x.fragment){r.fragment=true;r.fs=x.fs;r.depthBounds=x.depthBounds;}r.output|=x.output;
    }
    if(own&1) {
        r.vertexInput=true;const auto* v=info.pVertexInputState;
        if(!v)reject("no-vertex-input-state");
        else {
            // Only the attribute-divisor structure is tolerated: replay is limited to one
            // instance starting at 0, where any divisor yields instance index 0.
            for(auto* n=static_cast<const VkBaseInStructure*>(v->pNext);n;n=n->pNext)
                if(uint32_t(n->sType)!=1000190001u && uint32_t(n->sType)!=1000525000u)reject("vertex-input-pnext:"+std::to_string(int(n->sType)));
            if(v->vertexBindingDescriptionCount>16)reject("vertex-bindings>16");
            if(v->vertexAttributeDescriptionCount>32)reject("vertex-attributes>32");
        }
        if(!info.pInputAssemblyState)reject("no-input-assembly-state");
        else if(info.pInputAssemblyState->pNext)reject("input-assembly-pnext");
        if(v && v->vertexBindingDescriptionCount<=16 && v->vertexAttributeDescriptionCount<=32 && info.pInputAssemblyState) {
            if(v->vertexBindingDescriptionCount)r.bindings.assign(v->pVertexBindingDescriptions,v->pVertexBindingDescriptions+v->vertexBindingDescriptionCount);
            if(v->vertexAttributeDescriptionCount)r.attributes.assign(v->pVertexAttributeDescriptions,v->pVertexAttributeDescriptions+v->vertexAttributeDescriptionCount);
            r.restart=info.pInputAssemblyState->primitiveRestartEnable;
        }
    }
    if(own&2) {
        r.raster=true;if(!info.pRasterizationState)reject("no-rasterization-state");
        else {r.rasterInfo=*info.pRasterizationState;r.rasterInfo.pNext=nullptr;
            if(r.rasterInfo.polygonMode!=VK_POLYGON_MODE_FILL)reject("polygon-mode:"+std::to_string(int(r.rasterInfo.polygonMode)));
            // depthClampEnable is kept: private pipelines copy rasterInfo verbatim and the app already enabled the feature.
            for(auto* n=static_cast<const VkBaseInStructure*>(info.pRasterizationState->pNext);n;n=n->pNext)
                if(n->sType==VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT){r.clipExtension=true;r.depthClip=reinterpret_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(n)->depthClipEnable;}
                else reject("raster-pnext:"+std::to_string(int(n->sType)));
        }
    }
    if(own&4){r.fragment=true;if(info.pDepthStencilState){r.depthBounds=info.pDepthStencilState->depthBoundsTestEnable;if(info.pDepthStencilState->pNext)reject("depth-stencil-pnext");}}
    if(own&8)r.output=true;
    for(uint32_t i=0;(own&6) && i<info.stageCount;++i) {
        const auto& stage=info.pStages[i];auto& out=stage.stage==VK_SHADER_STAGE_VERTEX_BIT?r.vs:r.fs;
        if(stage.flags)reject("stage-flags:"+hex(stage.flags));
        if(stage.stage!=VK_SHADER_STAGE_VERTEX_BIT && stage.stage!=VK_SHADER_STAGE_FRAGMENT_BIT)reject("stage:"+std::to_string(int(stage.stage)));
        if(auto spec=stage.pSpecializationInfo) {
            if(spec->mapEntryCount>256 || spec->dataSize>4096){reject("specialization:"+std::to_string(spec->mapEntryCount)+"/"+std::to_string(spec->dataSize));continue;}
            if(spec->mapEntryCount)out.entries.assign(spec->pMapEntries,spec->pMapEntries+spec->mapEntryCount);
            if(spec->dataSize)out.bytes.assign(static_cast<const uint8_t*>(spec->pData),static_cast<const uint8_t*>(spec->pData)+spec->dataSize);
        }
    }
}
bool water_pipeline_check(const Pipeline& p,std::string* why) {
    // Without `why` this is the per-draw fast path and stops at the first failure.
    // With it, every failed condition is listed, so a pipeline the game really
    // creates (for example DXVK's library-linked ones) can be diagnosed from one
    // log line instead of a single opaque "unsupported".
    bool ok=true;
    auto reject=[&](const std::string& reason){ok=false;if(why)why->append(why->empty()?"":",").append(reason);return !why;};
    if(!p.water){if(reject("no-recipe"))return false;}
    else {
        if(!p.water->supported && reject("recipe-unsupported{"+p.water->why+"}"))return false;
        if(!p.water->vertexInput && reject("no-vertex-input-part"))return false;
        if(!p.water->raster && reject("no-raster-part"))return false;
        if(!p.water->fragment && reject("no-fragment-part"))return false;
        if(!p.water->output && reject("no-output-part"))return false;
    }
    if(!p.layout){if(reject("no-layout"))return false;}
    else {
        if(!p.layout->supported) {
            if(reject("layout-untracked{"+p.layout->diagnostic+"}"))return false;
            if(why)for(size_t i=0;i<p.layout->sets.size();++i)if(!p.layout->sets[i] || !p.layout->sets[i]->supported)
                why->append(",set"+std::to_string(i)+"{"+(p.layout->sets[i]?p.layout->sets[i]->diagnostic:"unknown")+"}");
        }
        if(!p.layout->water && reject("no-private-water-layout"))return false;
    }
    if((!p.vs || !p.fs) && reject("missing-vs-or-fs"))return false;
    if(p.vs && !p.vs->replaySafe && reject("vs-not-replay-safe"))return false;
    if(p.fs && !p.fs->replaySafe && reject("fs-not-replay-safe"))return false;
    if(!p.replaySubset && reject("replay-subset-rejected{"+p.queryStatus+"}"))return false;
    if(p.parts!=15 && reject("pipeline-parts:"+std::to_string(p.parts)))return false;
    if(p.samples!=1 && reject("samples:"+std::to_string(p.samples)))return false;
    if(p.logicOp && reject("logic-op"))return false;
    if(p.alphaToCoverage && reject("alpha-to-coverage"))return false;
    for(auto d:p.dynamic)switch(d) {
        case VK_DYNAMIC_STATE_VIEWPORT:case VK_DYNAMIC_STATE_SCISSOR:case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
        case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:case VK_DYNAMIC_STATE_CULL_MODE:
        case VK_DYNAMIC_STATE_FRONT_FACE:case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:case VK_DYNAMIC_STATE_DEPTH_BIAS:
        case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS:case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
        case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:case VK_DYNAMIC_STATE_STENCIL_REFERENCE:case VK_DYNAMIC_STATE_STENCIL_OP:
        case VK_DYNAMIC_STATE_LOGIC_OP_EXT:case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:break;
        default:if(reject("dynamic-state:"+std::to_string(int64_t(d))))return false;
    }
    return ok;
}
bool water_pipeline_eligible(const Pipeline& p){return water_pipeline_check(p,nullptr);}
void select_water_surface(Device& s,Inspector& in) {
    auto& w=water_state(s);
    if(!in.selected || !in.selected->draw.pipeline){status(s,"Inspect water first");return;}
    const auto& p=*in.selected->draw.pipeline;
    if(!p.vs || !p.fs || p.vs->hash!=wowWaterVS || p.fs->hash!=wowWaterFS){status(s,"Selected shader is not the supported water");return;}
    std::string why;
    if(!water_pipeline_check(p,&why)) {
        std::string dynamic;for(auto d:p.dynamic)dynamic+=(dynamic.empty()?"":",")+std::to_string(int64_t(d));
        // Selection is a user action, so one line per press is not log spam.
        log_line("[WATER-PIPELINE-REJECT] serial="+std::to_string(p.serial)+" vs="+hex(p.vs->hash)+" fs="+hex(p.fs->hash)+" reasons="+why+
                 " dynamic=["+dynamic+"] parts="+std::to_string(p.parts)+" samples="+std::to_string(p.samples)+" layoutSets="+std::to_string(p.layout?p.layout->sets.size():0));
        status(s,"Water pipeline unsupported - see log");return;
    }
    {
        // The geometry buffers decide whether plane extraction can read them; log
        // their real usage now, before any pass is recorded.
        const auto& d=in.selected->draw;auto describe=[&](const BufferBinding& b){auto live=find_ptr(s.buffers,b.buffer);
            return live?"usage=0x"+hex(live->usage)+" size="+std::to_string(live->size)+" offset="+std::to_string(b.offset):std::string("unknown");};
        const auto attribute=p.water?std::find_if(p.water->attributes.begin(),p.water->attributes.end(),[](const auto& a){return a.location==0;}):std::vector<VkVertexInputAttributeDescription>::iterator{};
        std::string position="none";
        if(p.water && attribute!=p.water->attributes.end())position="binding="+std::to_string(attribute->binding)+" format="+std::to_string(int(attribute->format))+" offset="+std::to_string(attribute->offset)+
            (attribute->binding<16?" buffer["+describe(d.vertices[attribute->binding])+"]":"");
        log_line("[WATER-BUFFERS] serial="+std::to_string(p.serial)+" kind="+std::to_string(d.kind)+" count="+std::to_string(d.count)+" position{"+position+"} index["+(d.kind==1?describe(d.index):"direct")+
                 "] needsStorageBufferUsage=0x"+hex(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    }
    // The profile is the shader pair, not this frame's depth image: it stays valid across scene changes and is restored at the next launch.
    w.selectedVS=p.vs->hash;w.selectedFS=p.fs->hash;w.selectedSignature=0;w.depthGeneration=0;w.profileSaved=true;water_settings_save(w);
    status(s,w.mode?"Water profile saved - reflections on":"Water profile saved - choose SSR or planar");
}
void destroy_water_source(Device& s,uint64_t image) {
    if(!s.water)return;
    for(auto& weak:s.water->batches)if(auto b=weak.lock();b && (b->sourceColor==image || b->sourceDepth==image)) {
        for(auto& slotWeak:s.water->slots)if(auto slot=slotWeak.lock();slot && slot->batch==b)slot->release();
        b->release();
    }
}
void destroy_water_device(Device& s) {
    if(s.water)water_capture_release_all(*s.water);
    if(s.water)for(auto& weak:s.water->layouts)if(auto l=weak.lock())l->release();
    if(s.water)for(auto& weak:s.water->slots)if(auto slot=weak.lock())slot->release();
    if(s.water)for(auto& weak:s.water->batches)if(auto b=weak.lock())b->release();
    for(auto& [h,l]:s.layouts){(void)h;if(l->water)l->water->release();}
    for(auto& [h,p]:s.pipelines){(void)h;if(p->layout && p->layout->water)p->layout->water->release();}
}
namespace {
void log_water_perf(WaterState& w) {
    auto& p=w.perf;if(p.gpuSamples==0 && p.cpuSamples==0)return;
    const double cpu=p.cpuSamples?p.cpuSeconds*1000./p.cpuSamples:0;const double n=p.gpuSamples?double(p.gpuSamples):1;
    log_line("[WATER-PERF] cpu record avg "+std::to_string(cpu)+" ms (n="+std::to_string(p.cpuSamples)+") | gpu avg "+std::to_string(p.gpuMs[4]/n)+" ms total = mask+planar "+std::to_string(p.gpuMs[0]/n)+
             " + sceneCopy "+std::to_string(p.gpuMs[1]/n)+" + skyMemory "+std::to_string(p.gpuMs[2]/n)+" + composite "+std::to_string(p.gpuMs[3]/n)+" (n="+std::to_string(p.gpuSamples)+
             ") | waterDraws="+std::to_string(p.waterDraws)+" reflectedDraws="+std::to_string(p.reflectedDraws)+" heldOverWater="+std::to_string(p.heldOverWater)+" mode="+std::to_string(w.mode)+" layers="+std::to_string(w.layers));
    p={};
}
// Adds this call's CPU time to the running average; logs every 300 samples.
struct WaterCpuTimer {
    WaterState& w;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    explicit WaterCpuTimer(WaterState& state):w(state){}
    ~WaterCpuTimer(){
        w.perf.cpuSeconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        if(++w.perf.cpuSamples>=300)log_water_perf(w);
    }
};
// Valid timestamp bits of the queue family this command buffer's pool belongs to (0 = timestamps unusable there).
uint32_t timestamp_bits(const DeviceData& data,const Device& s,const Command& c) {
    const auto pool=s.commandPools.find(c.pool);
    if(pool==s.commandPools.end() || (pool->second.flags&VK_COMMAND_POOL_CREATE_PROTECTED_BIT) || !data.get_queue_properties)return 0;
    uint32_t count=0;data.get_queue_properties(data.physical_device,&count,nullptr);
    if(!count || count>64 || pool->second.queueFamilyIndex>=count)return 0;
    std::vector<VkQueueFamilyProperties> properties(count);data.get_queue_properties(data.physical_device,&count,properties.data());
    const auto& family=properties[pool->second.queueFamilyIndex];
    return (family.queueFlags&VK_QUEUE_GRAPHICS_BIT)?family.timestampValidBits:0;
}
// The previous execution of this slot's command buffer is over (it was reset to be re-recorded): fold its five timestamps into the averages.
void collect_timestamps(WaterState& w,WaterSlot& slot) {
    if(!slot.queries || !slot.timestampsPending)return;
    slot.timestampsPending=false;
    struct Result {uint64_t ticks,available;} values[5]{};
    auto& f=slot.batch->f;
    if(f.getQueryPoolResults(slot.batch->data.device,slot.queries,0,5,sizeof(values),values,sizeof(Result),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)!=VK_SUCCESS)return;
    for(const auto& v:values)if(!v.available)return;
    const uint64_t mask=slot.timestampBits>=64?UINT64_MAX:(uint64_t(1)<<slot.timestampBits)-1;
    auto ms=[&](int from,int to){return double((values[to].ticks-values[from].ticks)&mask)*slot.timestampPeriod/1e6;};
    w.perf.gpuMs[0]+=ms(0,1);w.perf.gpuMs[1]+=ms(1,2);w.perf.gpuMs[2]+=ms(2,3);w.perf.gpuMs[3]+=ms(3,4);w.perf.gpuMs[4]+=ms(0,4);
    ++w.perf.gpuSamples;
}
}
uint64_t water_transform_signature(Shader& shader) {
    if(shader.waterSignatureDone)return shader.waterSignature;
    shader.waterSignatureDone=true;
    const auto& w=shader.words;if(w.size()<5)return 0;
    std::unordered_map<uint32_t,uint32_t> constants,sets,bindings;std::vector<uint32_t> uniforms;
    for(size_t i=5;i<w.size();) {
        const uint32_t op=w[i]&0xffff,n=w[i]>>16;if(!n || i+n>w.size())return 0;
        if(op==71 && n>=4){if(w[i+2]==34)sets[w[i+1]]=w[i+3];else if(w[i+2]==33)bindings[w[i+1]]=w[i+3];}   // OpDecorate DescriptorSet/Binding
        else if(op==59 && n>=4 && w[i+3]==2)uniforms.push_back(w[i+2]);                                    // OpVariable Uniform
        else if(op==43 && n>=4)constants[w[i+2]]=w[i+3];                                                     // OpConstant
        i+=n;
    }
    uint32_t buffer=0;
    for(auto u:uniforms)if(sets.count(u) && sets[u]==2 && bindings.count(u) && bindings[u]==0)buffer=u;
    if(!buffer)return 0;
    uint64_t h=14695981039346656037ull;int reads=0;
    for(size_t i=5;i<w.size() && reads<8;) {
        const uint32_t op=w[i]&0xffff,n=w[i]>>16;
        if((op==65 || op==66) && n>=4 && w[i+3]==buffer) {   // OpAccessChain / OpInBoundsAccessChain into the constant buffer
            for(uint32_t k=4;k<n;++k){auto c=constants.find(w[i+k]);if(c==constants.end())return 0;h=(h^c->second)*1099511628211ull;}
            h=(h^0xffffffffull)*1099511628211ull;++reads;
        }
        i+=n;
    }
    if(reads<8)return 0;
    shader.waterSignature=h;return h;
}
bool water_draw_shaders(WaterState& w,const Pipeline& p) {
    if(!w.selectedVS || !p.vs || !p.fs || p.fs->hash!=w.selectedFS)return false;
    if(p.vs->hash==w.selectedVS){if(!w.selectedSignature)w.selectedSignature=water_transform_signature(*p.vs);return true;}
    return w.selectedSignature && water_transform_signature(*p.vs)==w.selectedSignature;
}
std::string private_pass_restore_problem(const Device& s,const Command& c) {
    if(!c.pipeline) {
        for(const auto& b:c.sets)if(b.handle || b.pushed)return "descriptor-sets-bound-without-pipeline";
        return {};
    }
    if(auto why=push_problem(*c.pipeline,c.push))return std::string("push[")+why+"]";
    if(auto why=sets_problem(s,*c.pipeline,c.sets);!why.empty())return "sets["+why+"]";
    return {};
}
void restore_after_private_pass(const DeviceData& data,VkCommandBuffer cmd,const Command& c) {
    restore_dynamic(data,cmd,c.waterRestore);
    bind_vertices(data,cmd,c.vertices,c.index,c.indexType);
    if(!c.pipeline)return;
    bind_sets(data,cmd,(VkPipelineLayout)c.pipeline->layoutHandle,c.sets);
    push_values(data,cmd,(VkPipelineLayout)c.pipeline->layoutHandle,*c.pipeline->layout,c.push);
    raw<PFN_vkCmdBindPipeline>(data,"vkCmdBindPipeline")(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkPipeline)c.pipeline->handle);
}
uint32_t queue_timestamp_bits(const DeviceData& data,const Device& s,const Command& c){return timestamp_bits(data,s,c);}
const char* app_draw_replay_problem(const Device& s,const Command& c,const Draw& d) {
    if(c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY || !(c.usage&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))return "command-not-primary-one-time";
    if(c.unsupported || c.truncated || c.activeQueries || c.conditional || c.transformFeedback)return "command-state";
    if(c.pass)return "render-pass-object";   // only dynamic rendering instances are re-begun
    if(!d.pipeline || !d.pipeline->layout || !d.pipeline->layout->supported)return "pipeline-layout";
    const auto& p=*d.pipeline;
    if(p.parts!=15 || p.samples!=1 || p.logicOp || p.alphaToCoverage)return "pipeline-parts-samples-logic";
    if(!p.vs || !p.fs || !p.vs->replaySafe || !p.fs->replaySafe)return "shaders-not-replay-safe";
    for(auto state:p.dynamic)switch(state) {   // the dynamic states WaterRestore records (restore_dynamic)
        case VK_DYNAMIC_STATE_VIEWPORT:case VK_DYNAMIC_STATE_SCISSOR:case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
        case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:case VK_DYNAMIC_STATE_CULL_MODE:
        case VK_DYNAMIC_STATE_FRONT_FACE:case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:case VK_DYNAMIC_STATE_DEPTH_BIAS:
        case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS:case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
        case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:case VK_DYNAMIC_STATE_STENCIL_REFERENCE:case VK_DYNAMIC_STATE_STENCIL_OP:
        case VK_DYNAMIC_STATE_LOGIC_OP_EXT:case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
        case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT:case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT:case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT:case VK_DYNAMIC_STATE_LINE_WIDTH:break;
        default:return "dynamic-state";
    }
    if(push_problem(p,d.push))return "push-constants";
    if(!sets_problem(s,p,d.sets).empty())return "descriptor-sets";
    if(d.unsupported || d.kind>1 || !d.count)return "draw-kind";
    if(!draw_buffers_live(s,d))return "buffers";
    if(!d.target.known || d.target.colors.size()!=1 || !d.target.colors[0] || !d.target.depth || !d.target.renderingLayoutsKnown || d.target.renderingLayers!=1)return "target";
    return nullptr;
}
void replay_app_draw(const DeviceData& data,VkCommandBuffer cmd,const Draw& d,const WaterRestore& state) {
    const auto& p=*d.pipeline;const auto layout=(VkPipelineLayout)p.layoutHandle;
    // Dynamic state first, the pipeline after it: state set before a bind stays for what the pipeline makes dynamic, while state set after
    // a bind must not touch what it has static (VUID-vkCmdDraw-None-08608: a WITH_COUNT viewport is restored as both viewport states).
    restore_dynamic(data,cmd,state);
    raw<PFN_vkCmdBindPipeline>(data,"vkCmdBindPipeline")(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkPipeline)p.handle);
    bind_sets(data,cmd,layout,d.sets);
    push_values(data,cmd,layout,*p.layout,d.push);
    bind_vertices(data,cmd,d.vertices,d.kind==1?d.index:BufferBinding{},d.indexType);
    if(d.kind==1)fns(data).drawIndexed(cmd,d.count,d.instances,d.first,d.vertexOffset,d.firstInstance);
    else raw<PFN_vkCmdDraw>(data,"vkCmdDraw")(cmd,d.count,d.instances,d.first,d.firstInstance);
}
void record_water_reflections(const DeviceData& data,Device& s,Command& c,VkCommandBuffer cmd) {
    if(s.water && s.water->capture)water_capture_poll(data,s); // finishes a pending capture even when the mode was switched off
    const bool perfOff=g_perf_water_off.load(std::memory_order_relaxed);   // PERFORMANCE TEST: measure the frame without the water replay
    if(s.water && s.water->selectedVS)g_water_hint.store((perfOff?0u:s.water->mode)|(s.water->layers<<4)|(s.water->look.quality<<12),std::memory_order_relaxed);
    if(perfOff || !s.water || !s.water->mode || !s.water->selectedVS)return;
    auto& w=*s.water;
    if(data.sync && !data.sync->singleGraphicsQueue){status(s,"Reflections require one graphics queue");return;}
    bool waterSeen=false;
    for(size_t i=c.waterPassStart;i<c.events.size();++i)if(auto draw=c.events[i].draw;draw && draw->pipeline && water_draw_shaders(w,*draw->pipeline) && effective_state(*draw).blend)waterSeen=true;
    if(!waterSeen)return;
    water_capture_poll_request(w);
    WaterCpuTimer cpuTimer(w);
    // Independent gates are evaluated together and reported in one message, so a
    // real game session reveals every blocker at once instead of one per attempt.
    std::vector<std::string> problems;
    auto problem=[&](const std::string& text){problems.push_back(text);};
    {
        std::string bad;auto add=[&](bool on,const std::string& text){if(on)bad+=(bad.empty()?"":",")+text;};
        add(c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY,"secondary");add(!(c.usage&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT),"not-one-time-submit");
        add(c.unsupported,std::string("unsupported:")+c.unsupportedReason);add(c.truncated,"event-budget-exceeded");
        add(c.activeQueries,"active-query");add(c.conditional,"conditional-rendering");add(c.transformFeedback,"transform-feedback");
        if(!bad.empty())problem("command["+bad+"]");
    }
    const char* targetWhy=target_problem(data.device,c.target);if(targetWhy)problem(std::string("source-image[")+targetWhy+"]");
    if(!c.pipeline)problem("no-pipeline-at-pass-end");
    else {
        if(auto why=push_problem(*c.pipeline,c.push))problem(std::string("restore-bindings[")+why+"]");
        if(auto why=sets_problem(s,*c.pipeline,c.sets);!why.empty())problem("restore-bindings["+why+"]");
    }
    if(c.pass) {
        if(!c.pass->supported || c.pass->subpasses.size()!=1)problem("render-pass[multi-subpass-or-unknown]");
        else if(!targetWhy)for(size_t i=0;i<c.attachments.size() && i<c.pass->attachments.size();++i)if(c.attachments[i] &&
            (c.attachments[i]->image==c.target.colors[0]->image || c.attachments[i]->image==c.target.depth->image) && c.pass->attachments[i].storeOp!=VK_ATTACHMENT_STORE_OP_STORE){problem("render-pass[source-attachments-discarded]");break;}
    }
    std::shared_ptr<Draw> selected;SubmittedDraw resolved;std::string firstWhy;
    for(size_t i=c.waterPassStart;i<c.events.size();++i)if(auto d=c.events[i].draw;d && d->pipeline && water_draw_shaders(w,*d->pipeline)) {
        std::string why;if(resources_ready(s,c,*d,true,resolved,&why)){selected=d;break;}
        if(firstWhy.empty())firstWhy=why;
    }
    if(!selected)problem("water-draw["+firstWhy+"]");
    // A shader with side effects elsewhere in this pass could have changed the
    // earlier draw's buffers or descriptor-backed images before this replay.
    for(size_t i=c.waterPassStart;i<c.events.size();++i)if(c.events[i].secondary) {problem("pass-side-effects[executes-secondary-commands]");break;}
        else if(const auto& x=c.events[i].draw;x && x->kind<6 && (!x->pipeline || !x->pipeline->vs || !x->pipeline->fs || !x->pipeline->vs->replaySafe || !x->pipeline->fs->replaySafe)) {
            problem("pass-side-effects[draw "+std::to_string(i-c.waterPassStart)+" vs="+hex(x->pipeline&&x->pipeline->vs?x->pipeline->vs->hash:0)+" fs="+hex(x->pipeline&&x->pipeline->fs?x->pipeline->fs->hash:0)+" not replay-safe]");break;
        }
    if(!problems.empty()) {
        std::string joined;for(const auto& text:problems)joined+=(joined.empty()?"":"; ")+text;
        status(s,"Water replay blocked: "+joined);return;
    }
    auto& d=*selected;auto& p=*d.pipeline;auto e=effective_state(d);
    problems.clear();
    if(s.inspector && w.depthGeneration && (w.depthGeneration!=s.inspector->depthGeneration || d.target.depth->image->handle!=s.inspector->depthImage))problem("scene-depth-changed-select-water-again");
    auto colorLayout=latest_layout(c,*d.target.colors[0]->image),depthLayout=latest_layout(c,*d.target.depth->image);
    // A rendering instance keeps the layouts it began with; that is more direct than scanning barrier events.
    if(!c.pass && c.target.renderingLayoutsKnown){colorLayout=c.target.renderingColorLayout;depthLayout=c.target.renderingDepthLayout;}
    if(colorLayout==VK_IMAGE_LAYOUT_UNDEFINED)problem("source-color-layout-unknown["+layout_history(c,*d.target.colors[0]->image)+"]");
    if(depthLayout==VK_IMAGE_LAYOUT_UNDEFINED)problem("source-depth-layout-unknown["+layout_history(c,*d.target.depth->image)+"]");
    VkPhysicalDeviceProperties properties{};if(!data.get_properties)return;data.get_properties(data.physical_device,&properties);
    const auto attribute=std::find_if(p.water->attributes.begin(),p.water->attributes.end(),[](const auto& a){return a.location==0;});
    const bool haveAttribute=attribute!=p.water->attributes.end() && attribute->binding<16;
    if(attribute==p.water->attributes.end())problem("geometry[no-vertex-attribute-at-location-0]");
    else if(attribute->binding>=16)problem("geometry[position-binding>=16]");
    else if(attribute->format!=VK_FORMAT_R32G32B32_SFLOAT)problem("geometry[position-format="+std::to_string(int(attribute->format))+"-need-float3]");
    const auto binding=haveAttribute?std::find_if(p.water->bindings.begin(),p.water->bindings.end(),[&](const auto& b){return b.binding==attribute->binding;}):p.water->bindings.end();
    const uint32_t stride=binding==p.water->bindings.end()?0:p.dynamic.count(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE)?d.vertices[attribute->binding].stride:binding->stride;
    if(haveAttribute) {
        if(binding==p.water->bindings.end())problem("geometry[no-binding-for-position]");
        else if(binding->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX)problem("geometry[position-binding-is-per-instance]");
        if(!stride || stride%4 || attribute->offset%4 || d.count>8192 || !d.count)
            problem("geometry[stride="+std::to_string(stride)+" offset="+std::to_string(attribute->offset)+" vertices="+std::to_string(d.count)+"]");
    }
    const BufferBinding noBuffer{};const auto& vb=haveAttribute?d.vertices[attribute->binding]:noBuffer;
    std::shared_ptr<Buffer> vertex,index;
    if(haveAttribute){vertex=find_ptr(s.buffers,vb.buffer);index=d.kind==1?find_ptr(s.buffers,d.index.buffer):vertex;}
    if(haveAttribute && (!vertex || !index || !(vertex->usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !(index->usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || vertex->size>properties.limits.maxStorageBufferRange ||
                         index->size>properties.limits.maxStorageBufferRange || vertex->size>UINT32_MAX || index->size>UINT32_MAX || index->size%4)) {
        auto describe=[](const std::shared_ptr<Buffer>& b){return b?"usage=0x"+hex(b->usage)+" size="+std::to_string(b->size):std::string("dead");};
        problem("buffers[need-storage-usage vertex "+describe(vertex)+", index "+describe(index)+"]");
    }
    if(haveAttribute && (vb.offset+attribute->offset>UINT32_MAX || (vb.offset+attribute->offset)%4))problem("geometry[vertex-offset-unaligned-or-too-large]");
    const uint32_t indexBytes=d.kind==1?(d.indexType==VK_INDEX_TYPE_UINT16?2:4):0;
    if(index && indexBytes && (d.index.offset>UINT32_MAX || d.first>UINT32_MAX-d.count || d.index.offset+(uint64_t(d.first)+d.count)*indexBytes>index->size))problem("geometry[index-range-exceeds-buffer]");
    VkDescriptorBufferInfo camera{},uniformWindow{};
    for(const auto& r:resolved.resources)if(r.set==2 && r.binding==0 && !r.element && r.descriptor.buffer) {
        uint64_t dynamic=0;auto buffer=find_ptr(s.buffers,r.descriptor.buffer);
        if(!buffer || buffer->generation!=r.descriptor.bufferGeneration || !uniform_dynamic_offset(d,r,dynamic))continue;
        auto offset=r.descriptor.offset+dynamic+256;
        if(offset%properties.limits.minUniformBufferOffsetAlignment || offset>buffer->size || 144>buffer->size-offset || (r.descriptor.range!=VK_WHOLE_SIZE && r.descriptor.range<400))continue;
        camera={(VkBuffer)buffer->handle,offset,144};
        const uint64_t base=r.descriptor.offset+dynamic;
        if(buffer->usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT)uniformWindow={(VkBuffer)buffer->handle,base,std::min<uint64_t>(r.descriptor.range==VK_WHOLE_SIZE?buffer->size-base:r.descriptor.range,4096)};
    }
    if(!camera.buffer)problem("camera-ubo[set2/binding0 window 256..400 unavailable]");
    if(!problems.empty()) {
        std::string joined;for(const auto& text:problems)joined+=(joined.empty()?"":"; ")+text;
        status(s,"Water replay blocked: "+joined);return;
    }
    WaterParams params{};params.viewport[0]=e.viewport.x;params.viewport[1]=e.viewport.y;params.viewport[2]=e.viewport.width;params.viewport[3]=e.viewport.height;
    params.depthRange[0]=e.viewport.minDepth;params.depthRange[1]=e.viewport.maxDepth;
    params.vertex[0]=uint32_t(vb.offset+attribute->offset);params.vertex[1]=stride;params.vertex[2]=uint32_t(vertex->size);params.vertex[3]=12;
    params.indices[0]=d.kind==1?uint32_t(d.index.offset):0;params.indices[1]=indexBytes;params.indices[2]=d.first;params.indices[3]=d.count;
    params.draw[0]=d.vertexOffset;params.draw[1]=p.dynamic.count(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)?e.restart:p.water->restart;params.options[0]=float(w.mode);params.options[1]=w.strength;params.options[2]=float(w.debug);params.options[3]=water_time();
    {const auto& tone=waterTones[w.tone<waterToneCount?w.tone:0];for(int c=0;c<3;++c)params.tone[c]=tone.color[c];params.tone[3]=tone.amount;}
    {   // Which factors the game blended the water with: the composite undoes that blend to recover the seabed (0 = a blend it cannot undo).
        const bool known=(e.valid&BlendEquation)!=0;float mode=1;   // unknown: assume ordinary alpha blending, the D3D water default
        if(known && e.blendSrc==VK_BLEND_FACTOR_SRC_ALPHA && e.blendDst==VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)mode=1;
        else if(known && e.blendSrc==VK_BLEND_FACTOR_ONE && e.blendDst==VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)mode=2;
        else if(known)mode=0;
        params.extra[1]=mode;
        static uint32_t logged=UINT32_MAX;const uint32_t key=known?(e.blendSrc<<8|e.blendDst|1u<<20):0u;
        if(key!=logged){logged=key;log_line("[WATER-BLEND] water draw blend factors src="+std::string(known?std::to_string(e.blendSrc):"unknown")+" dst="+std::string(known?std::to_string(e.blendDst):"unknown")+" -> mode "+std::to_string(int(mode))+(known?"":" (assumed alpha blending)"));}
    }
    water_advance_foam_clock(w,std::chrono::steady_clock::now());
    params.foam[0]=w.look.foamAmount;params.foam[1]=w.look.foamSize;params.foam[2]=float(w.foamClock);params.foam[3]=w.look.foamSoftness;params.extra[0]=w.look.gameOpacity;
    params.fx[0]=w.look.clarity;params.fx[1]=w.look.caustics;params.fx[2]=w.look.glitter;params.fx[3]=w.look.whitecaps;
    params.look[0]=w.look.foamPulse;params.look[1]=w.look.foamReach;params.look[2]=w.look.detail;params.look[3]=float(w.look.quality);
    water_update_sun(w);
    params.modern[0]=float(w.style);params.modern[1]=w.look.roughness;params.modern[2]=w.look.mediumNormals;params.modern[3]=w.look.microNormals;
    std::memcpy(params.sun,w.sun,sizeof params.sun);
    for(int c=0;c<3;++c){params.sunColor[c]=w.sunColor[c];params.body[c]=w.absorption[c];}
    params.sunColor[3]=w.look.glitter;params.body[3]=w.look.subsurface;
    {
        const auto waves=water::pack_waves(w.waves,w.look.waveHeight,w.look.waveSteepness,water_time_seconds());
        std::memcpy(params.waveA,waves.a,sizeof waves.a);std::memcpy(params.waveB,waves.b,sizeof waves.b);std::memcpy(params.surface,waves.surface,sizeof waves.surface);
    }
    VkDescriptorBufferInfo vertexInfo{(VkBuffer)vb.buffer,0,vertex->size},indexInfo{(VkBuffer)index->handle,0,index->size};
    std::shared_ptr<WaterSlot> slot;
    for(auto& x:c.waterSlots)if(x && x->alive && !x->recorded && water_batch_matches(*x->batch,d)){slot=x;break;}
    if(!slot) {
        // The heavy resources are shared by every command buffer that draws into the same game images; only the small slot is per command buffer.
        std::shared_ptr<WaterBatch> batch;size_t liveBatches=0,liveSlots=0;
        for(auto it=w.batches.begin();it!=w.batches.end();)if(it->expired())it=w.batches.erase(it);else{if(auto x=it->lock();x->alive){++liveBatches;if(water_batch_matches(*x,d))batch=x;}++it;}
        for(auto it=w.slots.begin();it!=w.slots.end();)if(it->expired())it=w.slots.erase(it);else{if(auto x=it->lock();x->alive)++liveSlots;++it;}
        if(!batch) {
            if(liveBatches>=3){status(s,"Three reflection resource sets already live");return;}
            std::string why;batch=create_water_batch(data,d,w.mode,params,camera,vertexInfo,indexInfo,&why);
            if(!batch){status(s,"Reflection resource creation failed: "+why);return;}
            w.batches.push_back(batch);
        }
        if(liveSlots>=64){status(s,"Too many command buffers use reflections");return;}
        std::string why;slot=create_water_slot(batch,d,camera,vertexInfo,indexInfo,&why);
        if(!slot){status(s,"Reflection descriptor creation failed: "+why);return;}
        c.waterSlots.push_back(slot);w.slots.push_back(slot);
    }
    auto& b=*slot->batch;
    if(!slot->queries && slot->timestampBits==0 && b.f.createQueryPool && b.f.cmdResetQueryPool && b.f.cmdWriteTimestamp && b.f.getQueryPoolResults && properties.limits.timestampComputeAndGraphics) {
        slot->timestampBits=timestamp_bits(data,s,c);slot->timestampPeriod=properties.limits.timestampPeriod;
        if(slot->timestampBits && slot->timestampPeriod>0.f) {
            VkQueryPoolCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=5;
            if(b.f.createQueryPool(data.device,&qi,nullptr,&slot->queries)!=VK_SUCCESS)slot->queries={};
        }
    }
    collect_timestamps(w,*slot);
    auto stamp=[&](uint32_t index){if(slot->queries)b.f.cmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,slot->queries,index);};
    if(slot->queries){b.f.cmdResetQueryPool(cmd,slot->queries,0,5);slot->timestampsPending=true;}
    stamp(0);    // The command reset that makes this batch reusable also proves completion of
    // its previous execution; descriptors may now refer to this frame's buffers.
    VkWriteDescriptorSet updates[3]{};VkDescriptorBufferInfo infos[]{camera,vertexInfo,indexInfo};
    for(uint32_t i=0;i<3;++i){updates[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;updates[i].dstSet=slot->resolveSet;updates[i].dstBinding=i;updates[i].descriptorCount=1;updates[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;updates[i].pBufferInfo=&infos[i];}
    b.f.updateDescriptorSets(data.device,3,updates,0,nullptr);
    std::string pipelineWhy;auto maskPipeline=water_draw_pipeline(b,d,false,&pipelineWhy);if(!maskPipeline){status(s,"Water mask pipeline failed: "+pipelineWhy);return;}
    // The sea is many tiles (one draw each). Only the first tile validates the plane (height/camera on the GPU); every tile that can be
    // replayed is drawn into the mask, and the composite drops any pixel whose surface is not on that plane, so a tile at another height
    // cannot receive a wrong reflection.
    std::vector<std::pair<std::shared_ptr<Draw>,VkPipeline>> waterDraws{{selected,maskPipeline}};size_t skippedWater=0;
    for(size_t i=c.waterPassStart;i<c.events.size() && waterDraws.size()<512;++i)if(auto other=c.events[i].draw;other && other!=selected && other->pipeline && water_draw_shaders(w,*other->pipeline)) {
        if(same_water_state(*selected,*other) && draw_buffers_live(s,*other)){waterDraws.emplace_back(other,maskPipeline);continue;}
        SubmittedDraw scratch;VkPipeline pipeline{};
        if(resources_ready(s,c,*other,true,scratch,nullptr))pipeline=water_draw_pipeline(b,*other,false,nullptr);
        if(pipeline)waterDraws.emplace_back(other,pipeline);else ++skippedWater;
    }
    std::vector<std::pair<std::shared_ptr<Draw>,VkPipeline>> reflected;
    if(w.mode==2)for(size_t i=c.waterPassStart;i<c.events.size();++i)if(auto other=c.events[i].draw;other && other!=selected && other->pipeline && other->target.depth && other->target.depth->image==d.target.depth->image && reflected.size()<512) {
        SubmittedDraw resources;if(resources_ready(s,c,*other,false,resources))if(auto pipeline=water_draw_pipeline(b,*other,true,nullptr))reflected.emplace_back(other,pipeline);
    }
    if(w.mode==2 && reflected.empty()){status(s,"No replayable opaque draws in this pass");return;}
    slot->recorded=true;
    VkMemoryBarrier memory{};memory.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;memory.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;memory.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&memory,0,nullptr,0,nullptr);
    if(b.detail.uploads<waterDetailUploadExecutions || b.modern.uploads<waterDetailUploadExecutions)water_upload_detail(b,cmd);
    params.draw[2]=b.skyReady?0:(water_frozen()?2:1);params.draw[3]=int(w.layers);   // sky memory needs initialising / which water layers are on
    fns(data).updateBuffer(cmd,slot->params,0,sizeof(params),&params);
    memory.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT;
    b.f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&memory,0,nullptr,0,nullptr);
    begin_pass(b,cmd,b.cameraPass,b.cameraFB,waterCameraWidth,1,false);b.f.cmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,b.cameraPipeline);
    b.f.cmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,b.resolveLayout,0,1,&slot->resolveSet,0,nullptr);b.f.cmdDraw(cmd,3,1,0,0);b.f.cmdEndRenderPass(cmd);
    begin_pass(b,cmd,b.maskPass,b.maskFB,b.width,b.height,true,2);
    {ReplayState replayState;for(const auto& item:waterDraws)replay(b,slot->cameraSet,cmd,*item.first,item.second,replayState);}
    b.f.cmdEndRenderPass(cmd);
    water_color_mips(b,cmd);
    begin_pass(b,cmd,b.planarPass,b.planarFB,b.width,b.height,true);
    {ReplayState replayState;for(auto& item:reflected)replay(b,slot->cameraSet,cmd,*item.first,item.second,replayState);}
    b.f.cmdEndRenderPass(cmd);
    stamp(1);
    transition(b,cmd,(VkImage)b.sourceColor,VK_IMAGE_ASPECT_COLOR_BIT,colorLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(b,cmd,b.scene.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={b.width,b.height,1};
    fns(data).copyImage(cmd,(VkImage)b.sourceColor,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b.scene.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    transition(b,cmd,b.scene.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(b,cmd,(VkImage)b.sourceColor,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const auto aspect=depth_barrier_aspects(d.target.depth->format);
    transition(b,cmd,(VkImage)b.sourceDepth,aspect,depthLayout,VK_IMAGE_LAYOUT_GENERAL);
    stamp(2);
    // Sky memory: fold what the screen shows of the sky into the persistent map (first use: initialise it), before the composite reads it.
    begin_pass(b,cmd,b.skyReady?b.skyPass:b.skyInitPass,b.skyFB,waterSkyWidth,waterSkyHeight,false);b.f.cmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,b.skyPipeline);
    b.f.cmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,b.compositeLayout,0,1,&slot->compositeSet,0,nullptr);b.f.cmdDraw(cmd,3,1,0,0);b.f.cmdEndRenderPass(cmd);
    b.skyReady=true;
    stamp(3);
    begin_pass(b,cmd,b.compositePass,b.compositeFB,b.width,b.height,false);b.f.cmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,b.compositePipeline);
    b.f.cmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,b.compositeLayout,0,1,&slot->compositeSet,0,nullptr);b.f.cmdDraw(cmd,3,1,0,0);b.f.cmdEndRenderPass(cmd);
    stamp(4);
    w.perf.waterDraws=uint32_t(waterDraws.size());w.perf.reflectedDraws=uint32_t(reflected.size());
    if(water_capture_wanted(w)) {
        // Source color is COLOR_ATTACHMENT_OPTIMAL and source depth GENERAL right here; both are handed back exactly as found.
        std::vector<WaterCaptureDraw> passDraws;
        for(size_t i=c.waterPassStart;i<c.events.size();++i)if(auto other=c.events[i].draw;other && other->pipeline && other->kind<6) {
            WaterCaptureDraw item{other,"skipped",""};
            if(other==selected)item.role="water-plane";
            else if(std::any_of(waterDraws.begin(),waterDraws.end(),[&](const auto& x){return x.first==other;}))item.role="water";
            else if(std::any_of(reflected.begin(),reflected.end(),[&](const auto& x){return x.first==other;}))item.role="reflected";
            else if(w.mode==2){SubmittedDraw tmp;std::string why;resources_ready(s,c,*other,false,tmp,&why);item.why=why.empty()?"not-selected-for-replay":why;}
            else item.why="not evaluated (SSR mode replays nothing)";
            passDraws.push_back(std::move(item));
        }
        const uint64_t vertexBytes=std::min<uint64_t>(vertex->size>vb.offset?vertex->size-vb.offset:0,262144),indexOffset=d.kind==1?d.index.offset+uint64_t(d.first)*indexBytes:0;
        WaterCaptureInput input{b,d,params,passDraws,(VkImage)b.sourceColor,(VkImage)b.sourceDepth,d.target.colors[0]->format,d.target.depth->format,colorLayout,depthLayout,
            (VkBuffer)vb.buffer,d.kind==1?(VkBuffer)index->handle:VkBuffer{},uniformWindow.buffer,vb.offset,vertexBytes,indexOffset,d.kind==1?uint64_t(d.count)*indexBytes:0,uniformWindow.offset,uniformWindow.range,
            attribute->binding,attribute->offset,uint32_t(attribute->format),stride,bool(d.target.depth->image->usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)};
        if(!(vertex->usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT))input.vertexBuffer=VK_NULL_HANDLE;
        if(d.kind==1 && !(index->usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT))input.indexBuffer=VK_NULL_HANDLE;
        water_capture_record(data,s,cmd,input);
    }
    transition(b,cmd,(VkImage)b.sourceDepth,aspect,VK_IMAGE_LAYOUT_GENERAL,depthLayout);
    transition(b,cmd,(VkImage)b.sourceColor,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,colorLayout);
    // Restore state after private static pipelines, then restore the original
    // pipeline so its static/dynamic state precedence remains Vulkan-correct.
    restore_dynamic(data,cmd,c.waterRestore);
    bind_vertices(data,cmd,c.vertices,c.index,c.indexType);
    bind_sets(data,cmd,(VkPipelineLayout)c.pipeline->layoutHandle,c.sets);
    push_values(data,cmd,(VkPipelineLayout)c.pipeline->layoutHandle,*c.pipeline->layout,c.push);
    b.f.cmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkPipeline)c.pipeline->handle);
    ++w.maskSerial;w.maskColorImage=b.sourceColor;w.maskView=b.mask.view;
    ++w.passes;w.draws+=reflected.size();status(s,w.mode==1?"SSR recorded - GPU validates water plane":"Planar recorded - same-pass opaque scene");
    if(w.passes<=4 || w.passes%600==0)log_line("[WATER-REFLECTION] mode="+std::to_string(w.mode)+" debug="+std::to_string(w.debug)+" passes="+std::to_string(w.passes)+" waterDraws="+std::to_string(waterDraws.size())+" skippedWaterDraws="+std::to_string(skippedWater)+" reflectedDraws="+std::to_string(reflected.size())+" extent="+std::to_string(b.width)+"x"+std::to_string(b.height)+" gpuPlaneValidation=required");
}

}
