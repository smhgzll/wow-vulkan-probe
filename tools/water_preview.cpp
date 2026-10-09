// Replays the water composite shader on a saved debug capture (exports/water-capture-*), on a real GPU, without the game or the layer.
// Used to look at foam/ripple/reflection changes on real WoW frames and to time the composite pass.
//
//   g++ -O2 -std=c++20 -Isrc tools/water_preview.cpp src/water_surface.cpp -lvulkan -o /tmp/depth-probe-water-preview
//   tools/water-preview.py <capture-dir> [options]      (builds params.bin, runs this tool, writes PNGs)
//
// water_preview <shader-dir> <capture-dir> <params.bin> <out.raw> <repeat> [gpu-name-substring]
// params.bin is the raw WaterParams block (416 bytes: water_reflections.hpp). out.raw is the composited 1920x1080 B8G8R8A8 image. Prints the GPU time of one composite.
// DEPTH_PROBE_PREVIEW_WAVES=<height scale> replaces the capture's wave block with the layer's DEFAULT Gerstner waves (water_surface.hpp), for
// captures taken before the waves existed; the phases are those of time 0.
#include "preview_common.hpp"
#include "water_surface.hpp"
int main(int argc,char** argv) {
    if(argc<6){std::fprintf(stderr,"usage: water_preview <shader-dir> <capture-dir> <params.bin> <out.raw> <repeat> [gpu-name]\n");return 1;}
    const std::string shaders=argv[1],capture=argv[2],paramsPath=argv[3],outPath=argv[4];const int repeat=std::max(1,std::atoi(argv[5]));const std::string wanted=argc>6?argv[6]:"";
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;
    VkInstance instance;OK(vkCreateInstance(&ii,nullptr,&instance));
    uint32_t count=0;vkEnumeratePhysicalDevices(instance,&count,nullptr);std::vector<VkPhysicalDevice> devices(count);vkEnumeratePhysicalDevices(instance,&count,devices.data());
    Context c;VkPhysicalDeviceProperties props{};bool found=false;
    for(int pass=0;pass<2 && !found;++pass)for(auto d:devices){
        VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(d,&p);
        const bool match=!wanted.empty()?std::string(p.deviceName).find(wanted)!=std::string::npos:p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if(pass==0 && !match)continue;
        c.physical=d;props=p;found=true;break;
    }
    if(!found){std::fprintf(stderr,"no matching GPU\n");return 1;}
    vkGetPhysicalDeviceMemoryProperties(c.physical,&c.memory);
    uint32_t qc=0;vkGetPhysicalDeviceQueueFamilyProperties(c.physical,&qc,nullptr);std::vector<VkQueueFamilyProperties> families(qc);vkGetPhysicalDeviceQueueFamilyProperties(c.physical,&qc,families.data());
    for(uint32_t i=0;i<qc;++i)if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){c.family=i;break;}
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=c.family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;OK(vkCreateDevice(c.physical,&di,nullptr,&c.device));
    vkGetDeviceQueue(c.device,c.family,0,&c.queue);VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=c.family;OK(vkCreateCommandPool(c.device,&pi,nullptr,&c.pool));
    std::printf("gpu: %s\n",props.deviceName);
    const uint32_t W=1920,H=1080;
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // ---- inputs from the capture ----
    auto cameraBytes=read_file(capture+"/camera.bin");
    // Captures from before texel 18 (reference plane valid, plane height, camera world z): derive it from texel 13 and the view matrix.
    if(cameraBytes.size()>=20*16){float* t=reinterpret_cast<float*>(cameraBytes.data());
        if(t[18*4]==0 && t[18*4+1]==0 && t[18*4+2]==0 && t[13*4]>.5f){const float* c7=t+16*4;const float* c8=t+17*4;
            t[18*4]=1;t[18*4+1]=t[13*4+1];t[18*4+2]=-(c7[0]*c8[0]+c7[1]*c8[1]+c7[2]*c8[2]);}}
    auto camera=make_image(c,VK_FORMAT_R32G32B32A32_SFLOAT,20,1,1,1,usage);upload(c,camera,cameraBytes,{full(20,1)},1,1);
    auto mask=make_image(c,VK_FORMAT_R32G32B32A32_SFLOAT,W,H,1,1,usage);upload(c,mask,read_file(capture+"/mask.bin"),{full(W,H)},1,1);
    // D24 (24-bit value in the low bits of each 32-bit word) -> float, so it can be sampled like the game's depth.
    auto depth=make_image(c,VK_FORMAT_R32_SFLOAT,W,H,1,1,usage);
    {
        auto raw=read_file(capture+"/scene_depth.bin");std::vector<uint8_t> out(raw.size());
        for(size_t i=0;i<raw.size()/4;++i){uint32_t v;std::memcpy(&v,raw.data()+i*4,4);const float f=float(v&0xffffff)/16777215.f;std::memcpy(out.data()+i*4,&f,4);}
        upload(c,depth,out,{full(W,H)},1,1);
    }
    const auto sceneBytes=read_file(capture+"/scene_before.bin");
    auto scene=make_image(c,VK_FORMAT_B8G8R8A8_UNORM,W,H,1,1,usage);upload(c,scene,sceneBytes,{full(W,H)},1,1);
    auto planarColor=make_image(c,VK_FORMAT_R16G16B16A16_SFLOAT,1,1,1,1,usage);upload(c,planarColor,std::vector<uint8_t>(8,0),{full(1,1)},1,1);
    auto planarDepth=make_image(c,VK_FORMAT_R32_SFLOAT,1,1,1,1,usage);{float one=1;std::vector<uint8_t> b(4);std::memcpy(b.data(),&one,4);upload(c,planarDepth,b,{full(1,1)},1,1);}
    auto sky=make_image(c,VK_FORMAT_R16G16B16A16_SFLOAT,128,64,1,1,usage);
    {std::ifstream probe(capture+"/sky_memory.bin");upload(c,sky,probe?read_file(capture+"/sky_memory.bin"):std::vector<uint8_t>(128*64*8,0),{full(128,64)},1,1);}
    // The water shader's own output (optional: a synthetic capture has it), with the layer's mip chain (water_color_mips: 2:1 linear blits).
    uint32_t ownLevels=1;while((std::max(W,H)>>ownLevels)>0)++ownLevels;   // the full chain, as the layer (waterColorLevels)
    auto ownColor=make_image(c,VK_FORMAT_R16G16B16A16_SFLOAT,W,H,1,ownLevels,usage|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    {std::ifstream probe(capture+"/water_color.bin");upload(c,ownColor,probe?read_file(capture+"/water_color.bin"):std::vector<uint8_t>(size_t(W)*H*8,0),{full(W,H)},1,1,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);}
    {
        auto cmd=begin(c);
        auto level_barrier=[&](uint32_t level,uint32_t count,VkImageLayout from,VkImageLayout to){
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=b.srcAccessMask;
            b.oldLayout=from;b.newLayout=to;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=ownColor.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,level,count,0,1};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);};
        level_barrier(1,ownLevels-1,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        int32_t w=int32_t(W),h=int32_t(H);
        for(uint32_t level=1;level<ownLevels;++level){const int32_t nw=std::max(w/2,1),nh=std::max(h/2,1);
            VkImageBlit r{};r.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level-1,0,1};r.srcOffsets[1]={w,h,1};r.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,1};r.dstOffsets[1]={nw,nh,1};
            vkCmdBlitImage(cmd,ownColor.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,ownColor.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&r,VK_FILTER_LINEAR);
            level_barrier(level,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);w=nw;h=nh;}
        level_barrier(0,ownLevels,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        finish(c,cmd);
    }
    // The texture arrays: water_detail.bin (binding 8, CLASSIC) and water_modern.bin (binding 10, MODERN; a flat 1x1 set when it is missing).
    auto textures=[&](const char* name,uint32_t& levelsOut){
        std::vector<uint8_t> file;{std::ifstream probe(shaders+"/"+name);if(probe)file=read_file(shaders+"/"+name);}
        uint32_t header[4]{1,1,3,1};std::vector<uint8_t> texels{128,128,255,128,255,255,255,255,0,0,0,0};
        if(file.size()>20){std::memcpy(header,file.data()+4,16);texels.assign(file.begin()+20,file.end());}
        else std::printf("%s missing: flat textures\n",name);
        std::vector<VkBufferImageCopy> regions;VkDeviceSize offset=0;
        for(uint32_t layer=0;layer<header[2];++layer)for(uint32_t level=0;level<header[3];++level){const uint32_t e=header[0]>>level;regions.push_back(full(e,e,level,layer,offset));offset+=VkDeviceSize(e)*e*4;}
        auto image=make_image(c,VK_FORMAT_R8G8B8A8_UNORM,header[0],header[0],header[2],header[3],usage);
        upload(c,image,texels,regions,header[3],header[2]);levelsOut=header[3];return image;
    };
    uint32_t detailLevels=1,modernLevels=1;Image detail=textures("water_detail.bin",detailLevels),modern=textures("water_modern.bin",modernLevels);
    auto target=make_image(c,VK_FORMAT_B8G8R8A8_UNORM,W,H,1,1,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    upload(c,target,sceneBytes,{full(W,H)},1,1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    auto paramsBytes=read_file(paramsPath);
    if(const char* e=std::getenv("DEPTH_PROBE_PREVIEW_WAVES");e && *e && paramsBytes.size()>=352) {
        const auto waves=depth_probe::water::pack_waves(depth_probe::water::WaveSettings{},float(std::atof(e)),1.f,0.0);
        std::memcpy(paramsBytes.data()+176,waves.a,sizeof waves.a);std::memcpy(paramsBytes.data()+256,waves.b,sizeof waves.b);std::memcpy(paramsBytes.data()+336,waves.surface,sizeof waves.surface);
        std::printf("default waves x%g: %g active, sum A %.3f yd\n",std::atof(e),waves.surface[0],waves.surface[1]);
    }auto ubo=make_buffer(c,std::max<size_t>(paramsBytes.size(),16),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,true);std::memcpy(ubo.map,paramsBytes.data(),paramsBytes.size());
    // ---- pipeline ----
    auto sampler=[&](VkFilter filter,VkSamplerMipmapMode mip,VkSamplerAddressMode u,VkSamplerAddressMode v,float lod){
        VkSamplerCreateInfo i{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};i.magFilter=i.minFilter=filter;i.mipmapMode=mip;i.addressModeU=u;i.addressModeV=v;i.addressModeW=v;i.maxLod=lod;
        VkSampler sm;OK(vkCreateSampler(c.device,&i,nullptr,&sm));return sm;};
    auto nearest=sampler(VK_FILTER_NEAREST,VK_SAMPLER_MIPMAP_MODE_NEAREST,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,0);
    auto skySampler=sampler(VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_NEAREST,VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,0);
    auto detailSampler=sampler(VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_LINEAR,VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_REPEAT,float(detailLevels));
    auto modernSampler=sampler(VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_LINEAR,VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_REPEAT,float(modernLevels));
    auto tintSampler=sampler(VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_LINEAR,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_LOD_CLAMP_NONE);
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(uint32_t i=0;i<11;++i)bindings.push_back({i,i==6?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sli.bindingCount=uint32_t(bindings.size());sli.pBindings=bindings.data();
    VkDescriptorSetLayout setLayout;OK(vkCreateDescriptorSetLayout(c.device,&sli,nullptr,&setLayout));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&setLayout;VkPipelineLayout layout;OK(vkCreatePipelineLayout(c.device,&pli,nullptr,&layout));
    VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_B8G8R8A8_UNORM;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rpi.attachmentCount=1;rpi.pAttachments=&attachment;rpi.subpassCount=1;rpi.pSubpasses=&sub;VkRenderPass pass;OK(vkCreateRenderPass(c.device,&rpi,nullptr,&pass));
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fbi.renderPass=pass;fbi.attachmentCount=1;fbi.pAttachments=&target.view;fbi.width=W;fbi.height=H;fbi.layers=1;VkFramebuffer framebuffer;OK(vkCreateFramebuffer(c.device,&fbi,nullptr,&framebuffer));
    auto vs=shader(c,shaders+"/depth.vert.spv"),fs=shader(c,shaders+"/water_composite.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;stages[1].pName="main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ca{};ca.colorWriteMask=15;ca.blendEnable=VK_TRUE;ca.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;ca.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ca.colorBlendOp=VK_BLEND_OP_ADD;ca.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;ca.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;ca.alphaBlendOp=VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&ca;
    VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dyn.dynamicStateCount=2;dyn.pDynamicStates=dynamic;
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gpi.stageCount=2;gpi.pStages=stages;gpi.pVertexInputState=&vin;gpi.pInputAssemblyState=&ia;gpi.pViewportState=&vp;gpi.pRasterizationState=&rs;
    gpi.pMultisampleState=&ms;gpi.pDepthStencilState=&ds;gpi.pColorBlendState=&cb;gpi.pDynamicState=&dyn;gpi.layout=layout;gpi.renderPass=pass;VkPipeline pipeline;OK(vkCreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&gpi,nullptr,&pipeline));
    // ---- descriptors ----
    VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,10},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}};VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.maxSets=1;dpi.poolSizeCount=2;dpi.pPoolSizes=sizes;
    VkDescriptorPool dpool;OK(vkCreateDescriptorPool(c.device,&dpi,nullptr,&dpool));VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dai.descriptorPool=dpool;dai.descriptorSetCount=1;dai.pSetLayouts=&setLayout;
    VkDescriptorSet set;OK(vkAllocateDescriptorSets(c.device,&dai,&set));
    const VkImageLayout read=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo images[11]{{nearest,camera.view,read},{nearest,mask.view,read},{nearest,depth.view,read},{nearest,scene.view,read},{nearest,planarColor.view,read},{nearest,planarDepth.view,read},{},{skySampler,sky.view,read},{detailSampler,detail.view,read},{tintSampler,ownColor.view,read},{modernSampler,modern.view,read}};
    VkDescriptorBufferInfo uboInfo{ubo.buffer,0,paramsBytes.size()};std::vector<VkWriteDescriptorSet> writes;
    for(uint32_t i=0;i<11;++i){VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=i;w.descriptorCount=1;
        w.descriptorType=i==6?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;if(i==6)w.pBufferInfo=&uboInfo;else w.pImageInfo=&images[i];writes.push_back(w);}
    vkUpdateDescriptorSets(c.device,uint32_t(writes.size()),writes.data(),0,nullptr);
    // ---- render once (output), then `repeat` times between timestamps ----
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qpi.queryType=VK_QUERY_TYPE_TIMESTAMP;qpi.queryCount=2;VkQueryPool queries;OK(vkCreateQueryPool(c.device,&qpi,nullptr,&queries));
    auto record=[&](VkCommandBuffer cmd,int times,bool stamp) {
        if(stamp){vkCmdResetQueryPool(cmd,queries,0,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,queries,0);}
        for(int n=0;n<times;++n) {
            VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=pass;rb.framebuffer=framebuffer;rb.renderArea.extent={W,H};vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
            VkViewport viewport{0,0,float(W),float(H),0,1};VkRect2D scissor{{0,0},{W,H}};vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,nullptr,0,nullptr);
        }
        if(stamp)vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,queries,1);
    };
    auto readback=make_buffer(c,size_t(W)*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    {
        auto cmd=begin(c);record(cmd,1,false);barrier(cmd,target.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&(const VkBufferImageCopy&)full(W,H));finish(c,cmd);
        std::ofstream out(outPath,std::ios::binary);out.write(static_cast<const char*>(readback.map),std::streamsize(size_t(W)*H*4));
    }
    {
        auto cmd=begin(c);barrier(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);record(cmd,1,false);record(cmd,repeat,true);finish(c,cmd);   // one warm-up, then the timed run
        uint64_t t[2]{};OK(vkGetQueryPoolResults(c.device,queries,0,2,sizeof(t),t,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
        const double ms=double(t[1]-t[0])*double(props.limits.timestampPeriod)/1e6/repeat;
        std::printf("composite gpu time: %.3f ms per pass (avg of %d)\n",ms,repeat);
    }
    return 0;
}
