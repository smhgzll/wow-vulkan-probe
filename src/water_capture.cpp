#include "water_reflections.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

// One-shot debug capture of a water reflection pass: the private pass images, the game's depth and
// color, the water UBO / vertex / index bytes, a record of every draw in the pass (with why it was or
// was not replayed) and the SPIR-V of every shader involved. Copies are recorded into the game's own
// command buffer and read back without any wait: the host polls a VkEvent the GPU sets after them.
namespace depth_probe::discovery {
namespace {
template<class T>T raw(const DeviceData& d,const char* n){return reinterpret_cast<T>(d.next_gdpa(d.device,n));}
struct Section {std::string name,file,format;uint32_t width=0,height=0;size_t offset=0,size=0;};
size_t align(size_t v,size_t a=256){return (v+a-1)/a*a;}
// Bytes per texel of the copied aspect; 0 when the layout of the copy is not known here.
size_t texel_bytes(VkFormat f,bool depth) {
    if(depth) {
        switch(f){case VK_FORMAT_D16_UNORM:return 2;case VK_FORMAT_D24_UNORM_S8_UINT:case VK_FORMAT_X8_D24_UNORM_PACK32:case VK_FORMAT_D32_SFLOAT:case VK_FORMAT_D32_SFLOAT_S8_UINT:return 4;default:return 0;}
    }
    switch(f) {
    case VK_FORMAT_R8_UNORM:return 1;
    case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R16_SFLOAT:case VK_FORMAT_R16_UNORM:return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_SRGB:case VK_FORMAT_R8G8B8A8_SNORM:case VK_FORMAT_R8G8B8A8_UINT:case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:case VK_FORMAT_A8B8G8R8_SRGB_PACK32:case VK_FORMAT_A2B10G10R10_UNORM_PACK32:case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:case VK_FORMAT_R16G16_SFLOAT:case VK_FORMAT_R16G16_UNORM:case VK_FORMAT_R32_SFLOAT:return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:case VK_FORMAT_R16G16B16A16_UNORM:case VK_FORMAT_R16G16B16A16_SNORM:case VK_FORMAT_R32G32_SFLOAT:return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:return 16;
    default:return 0;
    }
}
std::string quote(const std::string& text) {
    std::string out="\"";
    for(unsigned char ch:text){if(ch=='"'||ch=='\\'){out+='\\';out+=char(ch);}else if(ch<32)out+=' ';else out+=char(ch);}
    return out+"\"";
}
template<class Range>std::string numbers(const Range& values) {
    std::ostringstream out;out.precision(9);out<<'[';bool first=true;for(auto v:values){if(!first)out<<',';first=false;out<<v;}out<<']';return out.str();
}
std::filesystem::path capture_root() {
    const char* env=std::getenv("DEPTH_PROBE_EXPORT_DIR");
    if(env && *env)return env;
    const char* home=std::getenv("HOME");return home&&*home?std::filesystem::path(home):std::filesystem::path("/tmp");
}
void barrier(WaterBatch& b,VkCommandBuffer cmd,VkImage image,VkImageAspectFlags aspect,VkImageLayout from,VkImageLayout to) {
    VkImageMemoryBarrier v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;v.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    v.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;v.oldLayout=from;v.newLayout=to;v.srcQueueFamilyIndex=v.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    v.image=image;v.subresourceRange={aspect,0,1,0,1};
    b.f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&v);
}
}

struct WaterCapture {
    DeviceData data;VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};VkEvent event{};
    std::vector<Section> sections;std::string dir;uint32_t polls=0;bool released=false;
    ~WaterCapture(){release();}
    void release() {
        if(released)return;
        released=true;
        if(event)raw<PFN_vkDestroyEvent>(data,"vkDestroyEvent")(data.device,event,nullptr);
        if(mapped)raw<PFN_vkUnmapMemory>(data,"vkUnmapMemory")(data.device,memory);
        if(buffer)raw<PFN_vkDestroyBuffer>(data,"vkDestroyBuffer")(data.device,buffer,nullptr);
        if(memory)raw<PFN_vkFreeMemory>(data,"vkFreeMemory")(data.device,memory,nullptr);
        event={};mapped=nullptr;buffer={};memory={};
    }
};

// $DEPTH_PROBE_EXPORT_DIR/water-capture-request (any content): the next water pass is captured, as the menu's button does - lets a tool ask the
// running game for the frame on screen (tools/frame-scan.py / fog-capture.py style). Looked for every 30 recorded passes.
void water_capture_poll_request(WaterState& w) {
    if(++w.requestPolls<30)return;
    w.requestPolls=0;
    std::error_code ec;const auto request=capture_root()/"water-capture-request";
    if(std::filesystem::exists(request,ec)){std::filesystem::remove(request,ec);w.captureArmed=true;log_line("[WATER-CAPTURE] armed by the request file");}
}
bool water_capture_wanted(const WaterState& w) {
    if(w.capture)return false;
    if(w.captureArmed)return true;
    const char* env=std::getenv("DEPTH_PROBE_WATER_CAPTURE");
    return env && env[0]=='1' && w.captureCount<4 && !w.capturedViews.count(w.mode*16+w.debug);
}

void water_capture_release_all(WaterState& w) {
    if(w.capture)w.capture->release();
    for(auto& c:w.abandoned)if(c)c->release();
    w.capture.reset();w.abandoned.clear();
}

void water_capture_record(const DeviceData& data,Device& s,VkCommandBuffer cmd,const WaterCaptureInput& in) {
    auto& w=water_state(s);auto& b=in.batch;
    const bool armed=w.captureArmed;w.captureArmed=false;
    auto fail=[&](const std::string& why){log_line("[WATER-CAPTURE] skipped: "+why);};
    static uint32_t sequence=0;const uint32_t seq=++sequence;
    auto capture=std::make_shared<WaterCapture>();capture->data=data;
    capture->dir=(capture_root()/("water-capture-"+std::to_string(getpid())+"-"+std::to_string(seq)+"-mode"+std::to_string(w.mode)+"-view"+std::to_string(w.debug))).string();
    std::error_code ec;std::filesystem::create_directories(std::filesystem::path(capture->dir)/"shaders",ec);
    if(ec){fail("cannot create "+capture->dir+": "+ec.message());return;}

    // ---- what to read back, in one host-visible buffer ----
    struct Job {Section section;VkImage image=VK_NULL_HANDLE;VkImageAspectFlags aspect=0;VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED,restore=VK_IMAGE_LAYOUT_UNDEFINED;
                VkBuffer buffer=VK_NULL_HANDLE;uint64_t bufferOffset=0;};
    std::vector<Job> jobs;size_t total=0;
    auto image=[&](const char* name,const WaterImage& im,VkFormat format,bool depth,uint32_t width,uint32_t height,const char* format_name,VkImageLayout layout) {
        if(!im.image || !im.transferSrc){log_line(std::string("[WATER-CAPTURE] ")+name+" not copyable");return;}
        const size_t bytes=texel_bytes(format,depth);if(!bytes){log_line(std::string("[WATER-CAPTURE] ")+name+" format "+std::to_string(int(format))+" not supported by the dump");return;}
        Job job;job.section={name,std::string(name)+".bin",format_name,width,height,total,size_t(width)*height*bytes};
        job.image=im.image;job.aspect=depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;job.layout=layout;job.restore=layout;
        total=align(total+job.section.size);jobs.push_back(job);
    };
    const uint32_t W=b.width,H=b.height;
    image("camera",b.camera,VK_FORMAT_R32G32B32A32_SFLOAT,false,waterCameraWidth,1,"rgba32f",VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image("mask",b.mask,VK_FORMAT_R32G32B32A32_SFLOAT,false,W,H,"rgba32f",VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image("water_color",b.waterColor,VK_FORMAT_R16G16B16A16_SFLOAT,false,W,H,"rgba16f",VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image("mask_depth",b.maskDepth,VK_FORMAT_D32_SFLOAT,true,W,H,"d32f",VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    image("planar_color",b.planar,VK_FORMAT_R16G16B16A16_SFLOAT,false,W,H,"rgba16f",VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image("planar_depth",b.planarDepth,VK_FORMAT_D32_SFLOAT,true,W,H,"d32f",VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    image("sky_memory",b.sky,VK_FORMAT_R16G16B16A16_SFLOAT,false,waterSkyWidth,waterSkyHeight,"rgba16f",VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image("scene_before",b.scene,in.colorFormat,false,W,H,("vk-format-"+std::to_string(int(in.colorFormat))).c_str(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {   // the game's own images: color after the composite, depth as the composite saw it
        WaterImage color;color.image=in.sourceColor;color.transferSrc=true;WaterImage depth;depth.image=in.sourceDepth;depth.transferSrc=true;
        image("scene_after",color,in.colorFormat,false,W,H,("vk-format-"+std::to_string(int(in.colorFormat))).c_str(),VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if(in.depthCopyable)image("scene_depth",depth,in.depthFormat,true,W,H,("vk-depth-format-"+std::to_string(int(in.depthFormat))+"-copied-depth-aspect").c_str(),VK_IMAGE_LAYOUT_GENERAL);
        else log_line("[WATER-CAPTURE] scene_depth not copyable: the game's depth image lacks TRANSFER_SRC");
    }
    auto bytes=[&](const char* name,VkBuffer buffer,uint64_t offset,uint64_t size) {
        if(!buffer || !size)return;
        Job job;job.section={name,std::string(name)+".bin","bytes",0,0,total,size_t(size)};job.buffer=buffer;job.bufferOffset=offset;total=align(total+size);jobs.push_back(job);
    };
    bytes("water_ubo",in.uniformBuffer,in.uniformOffset,in.uniformBytes);
    bytes("water_vertices",in.vertexBuffer,in.vertexOffset,in.vertexBytes);
    bytes("water_indices",in.indexBuffer,in.indexOffset,in.indexBytes);
    if(jobs.empty() || !total){fail("nothing to copy");return;}

    // ---- readback buffer, host visible and coherent ----
    auto& f=b.f;VkBufferCreateInfo bi{};bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=total;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(f.createBuffer(data.device,&bi,nullptr,&capture->buffer)!=VK_SUCCESS){fail("readback buffer of "+std::to_string(total)+" bytes");return;}
    VkMemoryRequirements requirements{};f.getBufferMemoryRequirements(data.device,capture->buffer,&requirements);
    VkPhysicalDeviceMemoryProperties memory{};data.get_memory_properties(data.physical_device,&memory);uint32_t type=UINT32_MAX;
    const VkMemoryPropertyFlags want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&want)==want){
        if(type==UINT32_MAX || (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT))type=i;}
    if(type==UINT32_MAX){fail("no host-visible coherent memory type");return;}
    VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
    if(f.allocateMemory(data.device,&ai,nullptr,&capture->memory)!=VK_SUCCESS){fail("allocation of "+std::to_string(requirements.size)+" bytes");return;}
    if(f.bindBufferMemory(data.device,capture->buffer,capture->memory,0)!=VK_SUCCESS || raw<PFN_vkMapMemory>(data,"vkMapMemory")(data.device,capture->memory,0,VK_WHOLE_SIZE,0,&capture->mapped)!=VK_SUCCESS){fail("bind/map");capture->release();return;}
    std::memset(capture->mapped,0xCD,total); // untouched bytes stay recognizable
    VkEventCreateInfo ei{};ei.sType=VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    if(raw<PFN_vkCreateEvent>(data,"vkCreateEvent")(data.device,&ei,nullptr,&capture->event)!=VK_SUCCESS){fail("event");capture->release();return;}

    // ---- record ----
    VkMemoryBarrier all{};all.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;all.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;all.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&all,0,nullptr,0,nullptr);
    auto copyBuffer=raw<PFN_vkCmdCopyBuffer>(data,"vkCmdCopyBuffer");
    for(const auto& job:jobs) {
        if(job.image) {
            const bool general=job.layout==VK_IMAGE_LAYOUT_GENERAL;
            if(!general)barrier(b,cmd,job.image,job.aspect,job.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};copy.bufferOffset=job.section.offset;copy.imageSubresource={job.aspect,0,0,1};copy.imageExtent={job.section.width,job.section.height,1};
            f.cmdCopyImageToBuffer(cmd,job.image,general?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,capture->buffer,1,&copy);
            if(!general)barrier(b,cmd,job.image,job.aspect,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,job.restore);
        } else {
            VkBufferCopy copy{job.bufferOffset,job.section.offset,job.section.size};copyBuffer(cmd,job.buffer,capture->buffer,1,&copy);
        }
    }
    VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    f.cmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    raw<PFN_vkCmdSetEvent>(data,"vkCmdSetEvent")(cmd,capture->event,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    for(const auto& job:jobs)capture->sections.push_back(job.section);

    // ---- metadata and shaders are CPU-side facts: written now ----
    const auto& d=in.water;const auto& p=*d.pipeline;const auto e=effective_state(d);
    std::ostringstream json;json.precision(9);
    json<<"{\n\"schema\":1,\"pid\":"<<getpid()<<",\"sequence\":"<<seq<<",\"trigger\":"<<quote(armed?"menu":"auto")<<",\"mode\":"<<w.mode<<",\"view\":"<<w.debug<<",\"strength\":"<<w.strength<<",\n";
    json<<"\"target\":{\"width\":"<<W<<",\"height\":"<<H<<",\"colorFormat\":"<<int(in.colorFormat)<<",\"depthFormat\":"<<int(in.depthFormat)<<",\"colorLayout\":"<<int(in.colorLayout)<<",\"depthLayout\":"<<int(in.depthLayout)<<"},\n";
    json<<"\"waterDraw\":{\"pipelineSerial\":"<<p.serial<<",\"vs\":"<<quote(hex(p.vs->hash))<<",\"fs\":"<<quote(hex(p.fs->hash))<<",\"kind\":"<<d.kind<<",\"count\":"<<d.count<<",\"first\":"<<d.first<<",\"vertexOffset\":"<<d.vertexOffset
         <<",\"instances\":"<<d.instances<<",\"indexType\":"<<int(d.indexType)<<",\n  \"positionAttribute\":{\"binding\":"<<in.attributeBinding<<",\"offset\":"<<in.attributeOffset<<",\"format\":"<<in.attributeFormat<<",\"stride\":"<<in.stride<<"},\n"
         <<"  \"uniform\":{\"offset\":"<<in.uniformOffset<<",\"bytes\":"<<in.uniformBytes<<"},\"vertexBytes\":{\"offset\":"<<in.vertexOffset<<",\"bytes\":"<<in.vertexBytes<<"},\"indexBytes\":{\"offset\":"<<in.indexOffset<<",\"bytes\":"<<in.indexBytes<<"},\n"
         <<"  \"viewport\":"<<numbers(std::array<float,6>{e.viewport.x,e.viewport.y,e.viewport.width,e.viewport.height,e.viewport.minDepth,e.viewport.maxDepth})<<",\"scissor\":"<<numbers(std::array<int,4>{e.scissor.offset.x,e.scissor.offset.y,int(e.scissor.extent.width),int(e.scissor.extent.height)})
         <<",\"state\":{\"depthTest\":"<<e.depthTest<<",\"depthWrite\":"<<e.depthWrite<<",\"compare\":"<<e.compare<<",\"blend\":"<<e.blend<<",\"cull\":"<<e.cull<<",\"frontFace\":"<<e.frontFace<<",\"topology\":"<<e.topology<<"},\n"
         <<"  \"pushWordsVertex\":"<<numbers(std::vector<uint32_t>(d.push[0].words.begin(),d.push[0].words.begin()+std::min<size_t>(24,d.push[0].words.size())))<<"},\n";
    const auto& P=in.params;
    json<<"\"params\":{\"viewport\":"<<numbers(P.viewport)<<",\"depthRange\":"<<numbers(P.depthRange)<<",\"vertex\":"<<numbers(P.vertex)<<",\"indices\":"<<numbers(P.indices)<<",\"draw\":"<<numbers(P.draw)<<",\"options\":"<<numbers(P.options)<<",\"tone\":"<<numbers(P.tone)<<",\"foam\":"<<numbers(P.foam)<<",\"look\":"<<numbers(P.look)<<",\"extra\":"<<numbers(P.extra)<<",\"fx\":"<<numbers(P.fx)
        <<",\"waveA\":"<<numbers(std::vector<float>(&P.waveA[0][0],&P.waveA[0][0]+4*water::maxWaves))<<",\"waveB\":"<<numbers(std::vector<float>(&P.waveB[0][0],&P.waveB[0][0]+4*water::maxWaves))
        <<",\"surface\":"<<numbers(P.surface)<<",\"modern\":"<<numbers(P.modern)<<",\"sun\":"<<numbers(P.sun)<<",\"sunColor\":"<<numbers(P.sunColor)
        <<",\"body\":"<<numbers(P.body)<<"},\n";
    json<<"\"sections\":[";for(size_t i=0;i<capture->sections.size();++i){const auto& x=capture->sections[i];
        json<<(i?",":"")<<"\n {\"name\":"<<quote(x.name)<<",\"file\":"<<quote(x.file)<<",\"format\":"<<quote(x.format)<<",\"width\":"<<x.width<<",\"height\":"<<x.height<<",\"bytes\":"<<x.size<<"}";}
    json<<"\n],\n\"passDraws\":[";
    std::set<uint64_t> shaders;size_t index=0;
    for(const auto& item:in.passDraws) {
        if(!item.draw||!item.draw->pipeline){continue;}
        const auto& x=*item.draw;const auto& xp=*x.pipeline;const auto xe=effective_state(x);
        json<<(index?",":"")<<"\n {\"i\":"<<index<<",\"role\":"<<quote(item.role)<<",\"why\":"<<quote(item.why)<<",\"kind\":"<<x.kind<<",\"count\":"<<x.count<<",\"instances\":"<<x.instances<<",\"first\":"<<x.first<<",\"vertexOffset\":"<<x.vertexOffset
             <<",\"pipeline\":"<<xp.serial<<",\"vs\":"<<quote(xp.vs?hex(xp.vs->hash):"none")<<",\"fs\":"<<quote(xp.fs?hex(xp.fs->hash):"none")
             <<",\"depthTest\":"<<xe.depthTest<<",\"depthWrite\":"<<xe.depthWrite<<",\"compare\":"<<xe.compare<<",\"blend\":"<<xe.blend<<",\"cull\":"<<xe.cull<<",\"frontFace\":"<<xe.frontFace<<",\"topology\":"<<xe.topology
             <<",\"viewport\":"<<numbers(std::array<float,6>{xe.viewport.x,xe.viewport.y,xe.viewport.width,xe.viewport.height,xe.viewport.minDepth,xe.viewport.maxDepth})
             <<",\"unsupported\":"<<quote(x.unsupported?x.unsupportedReason:"")<<",\"vertexBuffers\":[";
        bool firstBuffer=true;for(uint32_t v=0;v<x.vertices.size();++v)if(x.vertices[v].buffer){json<<(firstBuffer?"":",")<<"{\"binding\":"<<v<<",\"offset\":"<<x.vertices[v].offset<<",\"stride\":"<<x.vertices[v].stride<<"}";firstBuffer=false;}
        json<<"],\"pushWords\":"<<numbers(std::vector<uint32_t>(x.push[0].words.begin(),x.push[0].words.begin()+std::min<size_t>(12,x.push[0].words.size())))<<"}";
        if(xp.vs)shaders.insert(xp.vs->hash);
        if(xp.fs)shaders.insert(xp.fs->hash);
        ++index;
        if(index>=20000)break;
    }
    json<<"\n],\n\"shaders\":[";
    size_t written=0;auto dumpShader=[&](const std::shared_ptr<Shader>& shader,const char* stage){
        if(!shader || shader->words.empty() || !shaders.count(shader->hash))return;
        shaders.erase(shader->hash);
        const auto name=std::string(stage)+"-"+hex(shader->hash)+".spv";
        std::ofstream out(std::filesystem::path(capture->dir)/"shaders"/name,std::ios::binary);
        out.write(reinterpret_cast<const char*>(shader->words.data()),std::streamsize(shader->words.size()*4));
        json<<(written++?",":"")<<"\n {\"hash\":"<<quote(hex(shader->hash))<<",\"stage\":"<<quote(stage)<<",\"file\":"<<quote("shaders/"+name)<<"}";
    };
    for(const auto& item:in.passDraws)if(item.draw&&item.draw->pipeline){dumpShader(item.draw->pipeline->vs,"vs");dumpShader(item.draw->pipeline->fs,"fs");}
    json<<"\n]\n}\n";
    std::ofstream(std::filesystem::path(capture->dir)/"capture.json")<<json.str();
    std::ofstream(std::filesystem::path(capture->dir)/"README.txt")<<
        "Water debug capture. capture.json lists every section; each .bin is raw, tightly packed, row 0 first (as stored in the image).\n"
        "camera.bin    20x1 RGBA32F: texel 0..3 = reflection matrix columns (P*M*mirror*M^-1*P^-1), 4..7 = inverse projection columns,\n"
        "              8 = plane in clip space, 9 = plane in view space (xyz normal, w = d), 10 = (near, far, P00, P11), 11 = viewport,\n"
        "              12 = depth range (min,max), 13 = (camera valid, plane height, P22, P23), 14..17 = view matrix columns c5..c8 (world -> view),\n"
        "              18 = (reference plane valid, plane height, camera world z, 0).\n"
        "sky_memory.bin RGBA16F 128x64: sky by WORLD direction, u = azimuth (atan2(y,x)/2pi+.5), v = elevation 0..90 deg; the most recent sky the screen showed.\n"
        "mask.bin      RGBA32F: xyz = view-space position of the water surface, w = original alpha (0 = no water).\n"
        "water_color.bin RGBA16F: the water shader's own output (rgb, alpha) before the game blended it; seabed = (scene - water*alpha)/(1-alpha) for alpha blending.\n"
        "              Level 0 only; the layer builds a mip chain from it (2:1 linear blits) whose coarse levels give MODERN the water colour without the texture.\n"
        "scene_depth   depth aspect of the game's depth image as the composite sampled it (D24: 24-bit value in the low bits of each 32-bit word).\n"
        "water_ubo     bytes of set 2 binding 0 at the end of the pass (c0..c3 = projection columns at byte 256, c5..c8 = view columns at 336).\n"
        "water_vertices/water_indices  raw geometry bytes; positions are float3 at positionAttribute.offset with positionAttribute.stride.\n"
        "passDraws     every draw of the pass with its role (water / reflected / skipped) and the reason it was not replayed.\n";
    ++w.captureCount;w.capturedViews.insert(w.mode*16+w.debug);
    log_line("[WATER-CAPTURE] recorded dir="+capture->dir+" sections="+std::to_string(capture->sections.size())+" bytes="+std::to_string(total)+" draws="+std::to_string(index)+" trigger="+(armed?"menu":"auto"));
    w.capture=capture;
}

void water_capture_poll(const DeviceData& data,Device& s) {
    if(!s.water || !s.water->capture)return;
    auto& w=*s.water;auto capture=w.capture;
    const auto status=raw<PFN_vkGetEventStatus>(data,"vkGetEventStatus")(data.device,capture->event);
    if(status==VK_EVENT_RESET) {
        // The command buffer may be dropped without ever being submitted; keep the resources (the GPU might still use
        // them) but stop blocking new captures.
        if(++capture->polls>60000){log_line("[WATER-CAPTURE] abandoned (command buffer never executed): "+capture->dir);w.abandoned.push_back(capture);w.capture.reset();}
        return;
    }
    if(status!=VK_EVENT_SET){log_line("[WATER-CAPTURE] event status "+std::to_string(int(status))+" - abandoned");w.abandoned.push_back(capture);w.capture.reset();return;}
    size_t bytes=0;
    for(const auto& section:capture->sections) {
        std::ofstream out(std::filesystem::path(capture->dir)/section.file,std::ios::binary);
        out.write(static_cast<const char*>(capture->mapped)+section.offset,std::streamsize(section.size));bytes+=section.size;
    }
    log_line("[WATER-CAPTURE] written dir="+capture->dir+" files="+std::to_string(capture->sections.size())+" bytes="+std::to_string(bytes));
    capture->release();w.capture.reset();
}
}
