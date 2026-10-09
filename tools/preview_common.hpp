// Shared helpers of the preview tools (Vulkan boilerplate: buffers, images, uploads, one-shot command buffers).
#pragma once
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if(!in){std::fprintf(stderr,"cannot read %s\n",path.c_str());std::exit(2);}
    std::vector<uint8_t> bytes(size_t(in.tellg()));in.seekg(0);in.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));return bytes;
}
#define OK(x) do{VkResult r_=(x);if(r_!=VK_SUCCESS){std::fprintf(stderr,"%s failed: %d (line %d)\n",#x,int(r_),__LINE__);std::exit(3);}}while(0)

struct Context {
    VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};uint32_t family=0;VkCommandPool pool{};VkPhysicalDeviceMemoryProperties memory{};
    uint32_t find_memory(uint32_t bits,VkMemoryPropertyFlags flags) const {
        for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
        std::fprintf(stderr,"no memory type\n");std::exit(4);
    }
};
struct Buffer {VkBuffer buffer{};VkDeviceMemory memory{};void* map=nullptr;};
struct Image {VkImage image{};VkImageView view{};VkDeviceMemory memory{};};
static Buffer make_buffer(const Context& c,VkDeviceSize size,VkBufferUsageFlags usage,bool host) {
    Buffer b;VkBufferCreateInfo i{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};i.size=size;i.usage=usage;OK(vkCreateBuffer(c.device,&i,nullptr,&b.buffer));
    VkMemoryRequirements r;vkGetBufferMemoryRequirements(c.device,b.buffer,&r);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;
    a.memoryTypeIndex=c.find_memory(r.memoryTypeBits,host?(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT):VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    OK(vkAllocateMemory(c.device,&a,nullptr,&b.memory));OK(vkBindBufferMemory(c.device,b.buffer,b.memory,0));
    if(host)OK(vkMapMemory(c.device,b.memory,0,VK_WHOLE_SIZE,0,&b.map));
    return b;
}
static Image make_image(const Context& c,VkFormat format,uint32_t w,uint32_t h,uint32_t layers,uint32_t levels,VkImageUsageFlags usage) {
    Image im;VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};i.imageType=VK_IMAGE_TYPE_2D;i.format=format;i.extent={w,h,1};i.mipLevels=levels;i.arrayLayers=layers;
    i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=usage;OK(vkCreateImage(c.device,&i,nullptr,&im.image));
    VkMemoryRequirements r;vkGetImageMemoryRequirements(c.device,im.image,&r);VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;
    a.memoryTypeIndex=c.find_memory(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);OK(vkAllocateMemory(c.device,&a,nullptr,&im.memory));OK(vkBindImageMemory(c.device,im.image,im.memory,0));
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=im.image;v.viewType=layers>1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;v.format=format;
    v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,layers};OK(vkCreateImageView(c.device,&v,nullptr,&im.view));return im;
}
static void barrier(VkCommandBuffer cmd,VkImage image,VkImageLayout from,VkImageLayout to,uint32_t levels=1,uint32_t layers=1) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=b.srcAccessMask;
    b.oldLayout=from;b.newLayout=to;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,layers};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
static VkCommandBuffer begin(const Context& c) {
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=c.pool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;
    VkCommandBuffer cmd;OK(vkAllocateCommandBuffers(c.device,&a,&cmd));VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(vkBeginCommandBuffer(cmd,&bi));return cmd;
}
static void finish(const Context& c,VkCommandBuffer cmd) {
    OK(vkEndCommandBuffer(cmd));VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=1;s.pCommandBuffers=&cmd;OK(vkQueueSubmit(c.queue,1,&s,VK_NULL_HANDLE));OK(vkQueueWaitIdle(c.queue));
}
// Uploads `data` into `image` (one region per entry of `regions`, offsets into `data`) and leaves it in shader-read layout.
static void upload(const Context& c,const Image& image,const std::vector<uint8_t>& data,std::vector<VkBufferImageCopy> regions,uint32_t levels,uint32_t layers,VkImageLayout finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    auto staging=make_buffer(c,data.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);std::memcpy(staging.map,data.data(),data.size());
    auto cmd=begin(c);barrier(cmd,image.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,levels,layers);
    vkCmdCopyBufferToImage(cmd,staging.buffer,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(regions.size()),regions.data());
    barrier(cmd,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,finalLayout,levels,layers);finish(c,cmd);
    vkDestroyBuffer(c.device,staging.buffer,nullptr);vkFreeMemory(c.device,staging.memory,nullptr);
}
static VkBufferImageCopy full(uint32_t w,uint32_t h,uint32_t level=0,uint32_t layer=0,VkDeviceSize offset=0) {
    VkBufferImageCopy r{};r.bufferOffset=offset;r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1};r.imageExtent={w,h,1};return r;
}
static VkShaderModule shader(const Context& c,const std::string& path) {
    auto bytes=read_file(path);VkShaderModuleCreateInfo i{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};i.codeSize=bytes.size();i.pCode=reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule m;OK(vkCreateShaderModule(c.device,&i,nullptr,&m));return m;
}

