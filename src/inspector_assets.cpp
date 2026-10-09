#include "surface_inspector.hpp"
#include "overlay_settings.hpp"
#include "matrix_analyzer.hpp"
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <thread>
#include <functional>
#include <cmath>
#include <iomanip>

namespace depth_probe::discovery {
TextureSource inspector_texture_source(const Device& s,const ResourceUse& r,uint64_t queue) {
    TextureSource out;auto fail=[&](const char* why){out.reason=why;return out;};
    if(!r.descriptor.known || !r.descriptor.view || !r.descriptor.view->image)return fail("Texture metadata unavailable");
    if(r.updateAfterBind)return fail("Image descriptor can change after binding");
    const auto& v=*r.descriptor.view;auto im=find_ptr(s.images,v.image->handle);
    if(!im || im->generation!=v.image->generation)return fail("Texture allocation no longer alive");
    if(!im->previewCompatible || !v.previewCompatible)return fail("Unsupported image/view create chain");
    if(im->flags&~VkImageCreateFlags(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT))return fail("Unsupported image create flags");
    if(im->type!=VK_IMAGE_TYPE_2D || im->samples!=1 || im->layers!=1 || v.type!=VK_IMAGE_VIEW_TYPE_2D || v.range.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT || v.range.baseArrayLayer!=0)return fail("Requires single-layer 2D color texture");
    if(!(im->usage&VK_IMAGE_USAGE_SAMPLED_BIT))return fail("Texture was not created for sampling");
    if(im->sharing!=VK_SHARING_MODE_EXCLUSIVE)return fail("Concurrent queue texture unsupported");
    if(v.range.baseMipLevel>=im->mips || v.range.baseMipLevel>=32 || !v.range.levelCount || !v.range.layerCount)return fail("Invalid mip or layer range");
    Image::MipLayout layout{im->currentLayout,im->layoutQueue,im->layoutKnown};
    if(v.range.baseMipLevel<im->mipLayouts.size())layout=im->mipLayouts[v.range.baseMipLevel];
    if(!layout.known)return fail("Selected mip layout has not been observed");
    if(!queue || layout.queue!=queue)return fail("Selected mip belongs to another queue");
    if(layout.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && layout.layout!=VK_IMAGE_LAYOUT_GENERAL && layout.layout!=VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL)return fail("Selected mip is not in a readable layout");
    switch(v.format) {
    case VK_FORMAT_R8G8B8A8_SRGB:case VK_FORMAT_B8G8R8A8_SRGB:case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:case VK_FORMAT_BC2_SRGB_BLOCK:case VK_FORMAT_BC3_SRGB_BLOCK:case VK_FORMAT_BC7_SRGB_BLOCK:out.conversion=1;break;
    case VK_FORMAT_R8_SNORM:case VK_FORMAT_BC4_SNORM_BLOCK:out.conversion=2;break;
    case VK_FORMAT_R8G8_SNORM:case VK_FORMAT_BC5_SNORM_BLOCK:out.conversion=6;break;
    case VK_FORMAT_R8G8B8A8_SNORM:out.conversion=30;break;
    case VK_FORMAT_R8_UNORM:case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_R16_SFLOAT:case VK_FORMAT_R16G16_SFLOAT:case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:case VK_FORMAT_R32G32_SFLOAT:case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:case VK_FORMAT_BC2_UNORM_BLOCK:case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:case VK_FORMAT_BC5_UNORM_BLOCK:case VK_FORMAT_BC7_UNORM_BLOCK:break;
    default:return fail("Unsupported sampled format (integer/depth/etc)");
    }
    out.width=std::max(1u,im->extent.width>>v.range.baseMipLevel);out.height=std::max(1u,im->extent.height>>v.range.baseMipLevel);
    out.layout=layout.layout;out.view=r.descriptor.view;return out;
}
namespace {
std::string quote(const std::string& s) {
    std::string out="\"";const char* digits="0123456789abcdef";
    for(unsigned char c:s)if(c=='"' || c=='\\'){out+='\\';out+=char(c);}else if(c<32){out+="\\u00";out+=digits[c>>4];out+=digits[c&15];}else out+=char(c);
    return out+'"';
}
std::string json_num(float v) {
    if(!std::isfinite(v))return "null";
    std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(9)<<v;return o.str();
}
std::string json_mat4(const float m[16]) {
    std::string out="[";for(int i=0;i<16;++i){if(i)out+=',';out+=json_num(m[i]);}return out+']';
}
// Mathematical-structure-only matrix candidates for a captured buffer
// snapshot; see matrix_analyzer.hpp. No temporal/cross-draw evidence yet -
// that is a separate, heavier feature to add on top of this export.
std::string matrix_candidates_json(const std::vector<uint8_t>& bytes) {
    std::ostringstream o;o<<'[';bool comma=false;
    for(const auto& c:find_matrix_candidates(bytes,4)) {
        if(comma)o<<',';
        comma=true;
        float transposed[16];transpose4x4(c.raw,transposed);
        float inverse[16];const bool invertible=invert4x4(c.raw,inverse);
        o<<"{\"offset\":"<<c.offset<<",\"classification\":"<<quote(c.classification)<<",\"confidence\":"<<json_num(c.confidence)
         <<",\"determinant\":"<<json_num(c.determinant)<<",\"rowOrthonormal\":"<<(c.rowOrthonormal?"true":"false")
         <<",\"colOrthonormal\":"<<(c.colOrthonormal?"true":"false")<<",\"orthonormalError\":"<<json_num(c.orthonormalError)
         <<",\"lastRowAffine\":"<<(c.lastRowAffine?"true":"false")<<",\"lastColAffine\":"<<(c.lastColAffine?"true":"false")
         <<",\"divideRow\":"<<c.divideRow<<",\"divideCol\":"<<c.divideCol
         <<",\"raw\":"<<json_mat4(c.raw)<<",\"transposed\":"<<json_mat4(transposed)<<",\"inverse\":"<<(invertible?json_mat4(inverse):"null")
         <<",\"reasons\":[";
        for(size_t i=0;i<c.reasons.size();++i){if(i)o<<',';o<<quote(c.reasons[i]);}
        o<<"]}";
    }
    return o.str()+']';
}
// Snapshot under the discovery mutex. The worker must not inspect live maps or
// infer camera matrices / material semantics from descriptor binding numbers.
std::string draw_state(const Inspector& in) {
    const auto& record=*in.selected;const auto& d=record.draw;const auto e=effective_state(d);
    const auto settings=overlay_settings();std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(9);
    auto scalar=[](float value){if(!std::isfinite(value))return std::string("null");std::ostringstream n;n.imbue(std::locale::classic());n<<std::setprecision(9)<<value;return n.str();};
    o<<"{\n  \"schema\":1,\"scope\":"<<quote(inspector_selection_scope(in))
     <<",\"draw\":"<<quote(hex(in.drawIdentity))<<",\"pipeline\":"<<(d.pipeline?d.pipeline->serial:0)
     <<",\"pixel\":["<<in.x<<','<<in.y<<"],\"drawKind\":"<<d.kind<<",\"count\":"<<d.count<<",\"instances\":"<<d.instances
     <<",\"first\":"<<d.first<<",\"vertexOffset\":"<<d.vertexOffset<<",\"firstInstance\":"<<d.firstInstance
     <<",\n  \"effectiveState\":{\"validBits\":"<<e.valid<<",\"depthTest\":"<<e.depthTest<<",\"depthWrite\":"<<e.depthWrite
     <<",\"depthCompare\":"<<e.compare<<",\"blend\":"<<e.blend<<",\"cull\":"<<e.cull<<",\"topology\":"<<e.topology
     <<",\"colorWriteMask\":"<<(d.pipeline?d.pipeline->colorMask:0)<<"},\n"
     <<"  \"semantic\":{\"class\":"<<quote(draw_class_name(d.semantic.cls))<<",\"confidence\":"<<quote(draw_confidence_name(d.semantic.confidence))
     <<",\"reason\":"<<quote(d.semantic.reason)<<",\"evidence\":"<<quote(draw_evidence_text(d.semantic.evidence))
     <<",\"alphaTest\":"<<(d.pipeline?int(d.pipeline->alphaTest.state):-1)<<",\"alphaTestSource\":"<<quote(d.pipeline?d.pipeline->alphaTest.source:"no pipeline")<<"},\n  \"viewport\":["<<scalar(e.viewport.x)<<','<<scalar(e.viewport.y)<<','<<scalar(e.viewport.width)<<','<<scalar(e.viewport.height)<<','<<scalar(e.viewport.minDepth)<<','<<scalar(e.viewport.maxDepth)
     <<"],\"scissor\":["<<e.scissor.offset.x<<','<<e.scissor.offset.y<<','<<e.scissor.extent.width<<','<<e.scissor.extent.height<<"],\n";
    o<<"  \"sceneDepthSample\":{\"ready\":"<<(in.depthReady?"true":"false")<<",\"raw\":"<<scalar(in.depth)
     <<",\"meaning\":\"sampled scene depth; not proof of the selected surface depth\"},\n"
     <<"  \"manualProjection\":{\"mode\":"<<settings.projectionMode<<",\"depthConvention\":"<<settings.depthConvention
     <<",\"near\":"<<settings.nearPlane<<",\"far\":"<<settings.farPlane<<",\"verticalFov\":"<<settings.verticalFov
     <<",\"orthoHeight\":"<<settings.orthoHeight<<",\"verifiedGameCamera\":false},\n"
     <<"  \"reflectionInputs\":{\"waterIdentity\":\"unverified candidate\",\"waterMask\":\"unavailable\",\"waterDepth\":\"unavailable\",\"waterNormal\":\"unavailable\",\"waterPlane\":\"unavailable\",\"cameraMatrices\":\"unavailable\",\"mirroredScenePass\":\"not implemented\"},\n"
     <<"  \"bufferContents\":\"see buffers.json: per-binding host-before-submit or GPU render-pass-end snapshots; matrix semantics unverified\",\n  \"bufferBindings\":[";
    bool comma=false;
    for(const auto& r:record.resources)if(r.descriptor.buffer) {
        if(comma)o<<',';
        comma=true;
        o<<"{\"set\":"<<r.set<<",\"binding\":"<<r.binding<<",\"element\":"<<r.element<<",\"type\":"<<r.descriptor.type
         <<",\"buffer\":"<<quote(hex(r.descriptor.buffer))<<",\"descriptorOffset\":"<<r.descriptor.offset<<",\"range\":"<<r.descriptor.range<<'}';
    }
    o<<"],\n  \"setDynamicOffsets\":[";
    for(size_t set=0;set<d.sets.size();++set) {
        if(set)o<<',';
        o<<'[';for(size_t i=0;i<d.sets[set].offsets.size();++i){if(i)o<<',';o<<d.sets[set].offsets[i];}o<<']';
    }
    o<<"],\n  \"pushConstants\":[";
    for(size_t stage=0;stage<d.push.size();++stage) {
        if(stage)o<<',';
        o<<"{\"stage\":"<<quote(stage?"fragment":"vertex")<<",\"words\":[";
        for(size_t word=0;word<d.push[stage].words.size();++word){if(word)o<<',';if(d.push[stage].known&(uint64_t(1)<<word))o<<d.push[stage].words[word];else o<<"null";}
        o<<"]}";
    }
    o<<"],\n  \"target\":{\"known\":"<<(d.target.known?"true":"false")<<",\"width\":"<<d.target.width<<",\"height\":"<<d.target.height
     <<",\"depthImage\":"<<quote(d.target.depth && d.target.depth->image?hex(d.target.depth->image->handle):"unknown")<<"}\n}\n";
    return o.str();
}
bool write_file(const std::filesystem::path& path,const void* data,size_t size) {
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return false;
    const auto* p=static_cast<const uint8_t*>(data);bool ok=true;
    while(size){auto n=write(fd,p,size);if(n<0 && errno==EINTR)continue;if(n<=0){ok=false;break;}p+=n;size-=size_t(n);}
    if(close(fd))ok=false;
    if(!ok)std::filesystem::remove(path);
    return ok;
}
void manifest(const std::shared_ptr<AssetExport>& job) {
    std::string directory,json;
    {
        std::lock_guard lock(job->lock);directory=job->directory;if(directory.empty())return;
        const bool pending=std::any_of(job->textures.begin(),job->textures.end(),[](const auto& t){return t.status=="queued" || t.status=="gpu" || t.status=="writing";});
        std::ostringstream o;o<<"{\n  \"schema\":1,\"draw\":"<<quote(job->draw)<<",\"scope\":"<<quote(job->scope)<<",\"status\":"<<quote(job->failed?"failed":!job->ready?"preparing":pending?"exporting":"complete")
            <<",\n  \"captureMoment\":\"export-time texels, not the original pick frame\",\n  \"textureSemantics\":\"unknown: bindings are not diffuse/normal labels\",\n  \"bitmapEncoding\":\"BGRA8 BMP V4, top-down, alpha preserved; view swizzle applied; SRGB re-encoded, SNORM channels mapped to [0,1], float channels clamped\",\n  \"shaders\":[";
        unsigned stage=0;for(auto shader:{job->vs,job->fs}) {
            if(stage)o<<',';
            o<<"{\"stage\":"<<quote(stage?"fragment":"vertex")<<",\"hash\":"<<quote(shader?hex(shader->hash):"unknown")<<",\"entry\":"<<quote(stage?job->fsEntry:job->vsEntry)
                <<",\"file\":"<<quote(shader && !shader->words.empty()?(stage?"fs-":"vs-")+hex(shader->hash)+".spv":"")<<"}";++stage;
        }

        o<<"],\n  \"textures\":[";bool comma=false;
        for(const auto& t:job->textures) {
            if(comma)o<<',';
            comma=true;auto v=t.resource.descriptor.view;
            o<<"\n    {\"set\":"<<t.resource.set<<",\"binding\":"<<t.resource.binding<<",\"element\":"<<t.resource.element<<",\"role\":\"unknown\",\"textureId\":"<<quote(v?hex(texture_identity(*v)):"unknown")
                <<",\"imageUsageAtCreate\":"<<(v && v->image?v->image->usage:0)<<",\"renderTargetCapable\":"<<(v && v->image && (v->image->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)?"true":"false")
                <<",\"format\":"<<(v?v->format:0)<<",\"baseMip\":"<<(v?v->range.baseMipLevel:0)<<",\"baseLayer\":"<<(v?v->range.baseArrayLayer:0)<<",\"width\":"<<t.width<<",\"height\":"<<t.height<<",\"file\":"<<quote(t.file)<<",\"status\":"<<quote(t.status)<<",\"reason\":"<<quote(t.reason)<<"}";
        }
        o<<"\n  ]\n}\n";json=o.str();
    }
    auto path=std::filesystem::path(directory);std::error_code ec;std::filesystem::remove(path/"manifest.tmp",ec);
    if(!write_file(path/"manifest.tmp",json.data(),json.size()))throw std::runtime_error("cannot write manifest");
    std::filesystem::rename(path/"manifest.tmp",path/"manifest.json");
}
struct Writer {
    struct Task {std::function<void()> run;size_t bytes;};
    std::mutex mutex;std::condition_variable cv;std::deque<Task> tasks;size_t bytes=0;bool stopping=false,busy=false;std::thread thread;
    Writer():thread([this]{for(;;){Task task;{std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping || !tasks.empty();});if(tasks.empty() && stopping)return;task=std::move(tasks.front());tasks.pop_front();bytes-=task.bytes;busy=true;}task.run();{std::lock_guard lock(mutex);busy=false;}cv.notify_all();}}){}
    ~Writer(){{std::lock_guard lock(mutex);stopping=true;}cv.notify_all();thread.join();}
    bool add(std::function<void()> run,size_t n=0){std::lock_guard lock(mutex);if(tasks.size()>=64 || bytes+n>maxTextureExportBytes)return false;tasks.push_back({std::move(run),n});bytes+=n;cv.notify_all();return true;}
};
Writer& writer(){static Writer w;return w;}
void failed(const std::shared_ptr<AssetExport>& job,const std::string& why){std::lock_guard lock(job->lock);job->failed=true;job->message="Export failed: "+why;log_line("[INSPECTOR-EXPORT-FAILED] "+why);}
void update_manifest(const std::shared_ptr<AssetExport>& job){try{manifest(job);}catch(const std::exception& e){failed(job,e.what());}}
void write_uniforms(const std::filesystem::path& path,const AssetExport& job) {
    std::ostringstream o;o<<"{\"schema\":1,\"draw\":"<<quote(job.draw)
        <<",\"captureMoment\":\"see per-buffer captureMoment; GPU copy is render-pass-end, not proof of original draw values\",\"cameraSemanticsVerified\":false,\"buffers\":[";
    bool comma=false;size_t saved=0;
    for(const auto& u:job.uniforms) {
        if(comma)o<<',';
        comma=true;
        const auto file=u->bytes.empty()?std::string{}:"uniform-s"+std::to_string(u->set)+"-b"+std::to_string(u->binding)+"-e"+std::to_string(u->element)+".bin";
        if(!file.empty()){if(!write_file(path/file,u->bytes.data(),u->bytes.size()))throw std::runtime_error("cannot write uniform snapshot");++saved;}
        o<<"{\"set\":"<<u->set<<",\"binding\":"<<u->binding<<",\"element\":"<<u->element
         <<",\"buffer\":"<<quote(hex(u->buffer))<<",\"bufferGeneration\":"<<u->bufferGeneration<<",\"memoryGeneration\":"<<u->memoryGeneration
         <<",\"effectiveOffset\":"<<u->offset<<",\"range\":"<<u->range<<",\"submit\":"<<u->submit<<",\"ticket\":"<<u->ticket
         <<",\"bufferUsage\":"<<u->bufferUsage<<",\"captureMoment\":"<<quote(u->captureMoment)
         <<",\"file\":"<<quote(file)<<",\"status\":"<<quote(file.empty()?"unavailable":"saved")<<",\"reason\":"<<quote(u->reason)
         <<",\"hash\":"<<quote(u->bytes.empty()?"":hex(hash_bytes(u->bytes.data(),u->bytes.size())))
         <<",\"matrixCandidates\":"<<matrix_candidates_json(u->bytes)<<'}';
    }
    o<<"],\"saved\":"<<saved<<",\"note\":\"Empty list means no eligible uniform observation (or record budget exceeded). Files start at the effective descriptor offset, not the allocation start."
       " matrixCandidates are mathematical-structure-only heuristics scored from this single snapshot (see confidence and reasons per candidate) - not a proof of camera/view/projection identity.\"}\n";
    const auto json=o.str();if(!write_file(path/"buffers.json",json.data(),json.size()))throw std::runtime_error("cannot write uniform manifest");
    log_line("[INSPECTOR-UNIFORM-EXPORT] draw="+job.draw+" saved="+std::to_string(saved)+" observed="+std::to_string(job.uniforms.size())+" directory="+path.string());
}
void initialize(const std::shared_ptr<AssetExport>& job) {
    try {
        const char* env=std::getenv("DEPTH_PROBE_EXPORT_DIR");std::filesystem::path root;
        if(env && *env)root=env;
        else if(const char* xdg=std::getenv("XDG_DATA_HOME");xdg && *xdg)root=std::filesystem::path(xdg)/"vulkan-depth-probe/exports";
        else if(const char* home=std::getenv("HOME");home && *home)root=std::filesystem::path(home)/".local/share/vulkan-depth-probe/exports";
        else root="/tmp/depth-probe-inspector-exports";
        std::filesystem::create_directories(root);auto pattern=(root/("selection-"+std::to_string(getpid())+"-XXXXXX")).string();std::vector<char> path(pattern.begin(),pattern.end());path.push_back(0);
        if(!mkdtemp(path.data()))throw std::runtime_error("cannot create export directory");
        {std::lock_guard lock(job->lock);job->directory=path.data();}
        if(!job->drawState.empty() && !write_file(std::filesystem::path(path.data())/"draw-state.json",job->drawState.data(),job->drawState.size()))throw std::runtime_error("cannot write draw state snapshot");
        write_uniforms(path.data(),*job);
        unsigned stage=0;for(auto shader:{job->vs,job->fs}) {
            if(shader && !shader->words.empty()) {auto file=(stage?"fs-":"vs-")+hex(shader->hash)+".spv";if(!write_file(std::filesystem::path(path.data())/file,shader->words.data(),shader->words.size()*4))throw std::runtime_error("cannot write shader snapshot");}++stage;
        }
        const std::string readme="Graphics Inspector asset export\n\nTextures: 32-bit BMP V4, original selected view mip dimensions, alpha preserved.\nBC compression is decoded by the GPU; files are not raw compressed image data.\nSRGB is re-encoded after sampling; signed normalized channels map [-1,1] to [0,1].\nFloat values outside [0,1] are clamped for bitmap output. The view swizzle is applied.\nTexture roles (diffuse, normal, etc.) are unknown; names use descriptor bindings.\nSee manifest.json for each saved or unavailable resource and its source format.\n\nShaders: original observed SPIR-V snapshots, not reconstructed GLSL/HLSL.\nThe files alone are not a complete Vulkan pipeline or a replacement package.\n";
        if(!write_file(std::filesystem::path(path.data())/"README.txt",readme.data(),readme.size()))throw std::runtime_error("cannot write export README");
        {std::lock_guard lock(job->lock);job->ready=true;job->message="Export ready";}
        manifest(job);log_line("[INSPECTOR-EXPORT-BEGIN] directory="+std::string(path.data())+" textures="+std::to_string(job->textures.size()));
    } catch(const std::exception& e){failed(job,e.what());}
}
}
bool asset_export_busy(const std::shared_ptr<AssetExport>& job) {
    if(!job)return false;
    std::lock_guard lock(job->lock);if(job->failed)return false;if(!job->ready)return true;
    return std::any_of(job->textures.begin(),job->textures.end(),[](const auto& t){return t.status=="queued" || t.status=="gpu" || t.status=="writing";});
}
std::string asset_export_status(const std::shared_ptr<AssetExport>& job) {
    if(!job)return "No export requested";
    std::lock_guard lock(job->lock);if(job->failed || !job->ready)return job->message;
    size_t saved=0,skipped=0;for(const auto& t:job->textures){saved+=t.status=="saved";skipped+=t.status=="unavailable";}
    if(job->textures.empty())return job->vs || job->fs?"Shader snapshots saved":"No exportable assets in this selection";
    return "BMP saved "+std::to_string(saved)+" / "+std::to_string(job->textures.size())+" skipped "+std::to_string(skipped);
}
void request_asset_export(Inspector& in,bool all,bool shadersOnly) {
    if(!in.selected || asset_export_busy(in.assetExport))return;
    auto job=std::make_shared<AssetExport>();job->draw=hex(in.drawIdentity);job->scope=inspector_selection_scope(in);
    job->drawState=draw_state(in);job->uniforms=in.selected->uniforms;
    const auto p=in.selected->draw.pipeline;
    if(p && (all || shadersOnly)){job->vs=p->vs;job->fs=p->fs;job->vsEntry=p->vsEntry;job->fsEntry=p->fsEntry;}
    size_t index=0;
    if(!shadersOnly)for(const auto& r:in.selected->resources)if(r.descriptor.view && r.descriptor.view->image) {
        if(all || index==in.texture) {
            TextureAsset t;t.resource=r;t.file="texture-s"+std::to_string(r.set)+"-b"+std::to_string(r.binding)+"-e"+std::to_string(r.element)+"-"+hex(texture_identity(*r.descriptor.view))+".bmp";job->textures.push_back(std::move(t));
        }++index;
    }
    size_t bytes=0;for(const auto& u:job->uniforms)bytes+=u->bytes.size();
    in.assetExport=job;if(!writer().add([job]{initialize(job);},bytes))failed(job,"writer busy");
}
void auto_export_uniforms(Inspector& in) {
    const auto enabled=std::getenv("DEPTH_PROBE_INSPECTOR_AUTO_CONSTANTS");
    if(!enabled || std::strcmp(enabled,"1") || !in.selected || in.autoUniformExports.size()>=4 || in.autoUniformExports.count(in.drawIdentity))return;
    auto job=std::make_shared<AssetExport>();job->draw=hex(in.drawIdentity);job->scope=inspector_selection_scope(in);
    job->drawState=draw_state(in);job->uniforms=in.selected->uniforms;
    if(const auto p=in.selected->draw.pipeline){job->vs=p->vs;job->fs=p->fs;job->vsEntry=p->vsEntry;job->fsEntry=p->fsEntry;}
    size_t bytes=0;for(const auto& u:job->uniforms)bytes+=u->bytes.size();
    if(writer().add([job]{initialize(job);},bytes))in.autoUniformExports.insert(in.drawIdentity);
    else log_line("[INSPECTOR-UNIFORM-EXPORT-SKIPPED] writer busy");
}
void asset_texture_failed(const std::shared_ptr<AssetExport>& job,size_t index,const std::string& why) {
    {std::lock_guard lock(job->lock);auto& t=job->textures.at(index);t.status="unavailable";t.reason=why;log_line("[INSPECTOR-EXPORT-SKIPPED] file="+t.file+" reason="+why);}
    if(!writer().add([job]{update_manifest(job);}))failed(job,"manifest writer busy");
}
bool save_asset_pixels(const std::shared_ptr<AssetExport>& job,size_t index,std::vector<uint8_t>&& rgba) {
    {std::lock_guard lock(job->lock);job->textures.at(index).status="writing";}
    const size_t bytes=rgba.size();auto pixels=std::make_shared<std::vector<uint8_t>>(std::move(rgba));
    return writer().add([job,index,pixels] {
        try {
            TextureAsset t;std::string dir;{std::lock_guard lock(job->lock);t=job->textures.at(index);dir=job->directory;}
            const uint64_t size=uint64_t(t.width)*t.height*4;
            if(!size || size!=pixels->size() || size>maxTextureExportBytes)throw std::runtime_error("invalid bitmap dimensions");
            // BITMAPV4HEADER: explicit BGRA masks and negative height preserve
            // alpha and top-left row order in standard bitmap readers.
            std::vector<uint8_t> bmp(122+size);auto put=[&](size_t off,uint32_t v){for(unsigned n=0;n<4;++n)bmp[off+n]=uint8_t(v>>(n*8));};
            bmp[0]='B';bmp[1]='M';put(2,bmp.size());put(10,122);put(14,108);put(18,t.width);put(22,uint32_t(-int32_t(t.height)));bmp[26]=1;bmp[28]=32;put(30,3);put(34,size);
            put(54,0x00ff0000);put(58,0x0000ff00);put(62,0x000000ff);put(66,0xff000000);put(70,0x73524742);
            for(size_t i=0;i<size;i+=4){bmp[122+i]=(*pixels)[i+2];bmp[123+i]=(*pixels)[i+1];bmp[124+i]=(*pixels)[i];bmp[125+i]=(*pixels)[i+3];}
            if(!write_file(std::filesystem::path(dir)/t.file,bmp.data(),bmp.size()))throw std::runtime_error("cannot write bitmap");
            {std::lock_guard lock(job->lock);job->textures[index].status="saved";}
            log_line("[INSPECTOR-EXPORT-SAVED] path="+dir+"/"+t.file+" width="+std::to_string(t.width)+" height="+std::to_string(t.height));update_manifest(job);
        }catch(const std::exception& e){asset_texture_failed(job,index,e.what());}
    },bytes);
}
void flush_asset_exports(){auto& w=writer();std::unique_lock lock(w.mutex);w.cv.wait(lock,[&]{return w.tasks.empty() && !w.busy;});}
}
