#include "water_reflections.hpp"
#include "surface_inspector.hpp"
#include "inspector_uniform_gpu.hpp"

namespace depth_probe::discovery {
namespace {
template<class Map,class Value> void remember(Device& s,Map& map,uint64_t key,Value&& value) {
    if(map.size()<maxObjects || map.count(key))map[key]=std::forward<Value>(value);else s.limits=true;
}
VKAPI_ATTR VkResult VKAPI_CALL buffer(VkDevice d,const VkBufferCreateInfo* info,const VkAllocationCallbacks* a,VkBuffer* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto create=next<PFN_vkCreateBuffer>(*data,"vkCreateBuffer");
    auto patched=*info;VkBufferUsageFlags2CreateInfo usage2{};
    uint64_t usage=info->usage;bool supported=!info->flags;
    // Copy only the completely understood single-node chain. Never mutate the
    // caller's flags2 node or silently drop an extension from the create call.
    if(info->pNext) {
        auto n=static_cast<const VkBaseInStructure*>(info->pNext);
        supported &= n->sType==VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO && !n->pNext;
        if(supported){usage2=*reinterpret_cast<const VkBufferUsageFlags2CreateInfo*>(n);usage=usage2.usage;}
    }
    const bool attempt=inspector_enabled() && supported && (usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) && !(usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if(attempt){patched.usage|=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;if(info->pNext){usage2.usage|=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;patched.pNext=&usage2;}}
    auto r=create(d,attempt?&patched:info,a,out);bool added=attempt && r==VK_SUCCESS;
    if(attempt && r!=VK_SUCCESS){log_line("[INSPECTOR-UNIFORM-CREATE-FALLBACK] patchedResult="+std::to_string(r));r=create(d,info,a,out);}
    if(r==VK_SUCCESS){std::lock_guard lock(mutex);auto& s=device_state(d);auto b=std::make_shared<Buffer>();b->handle=id(*out);b->generation=++s.serial;b->size=info->size;b->usage=info->usage;
        b->captureCompatible=!info->flags;
        for(auto* n=static_cast<const VkBaseInStructure*>(info->pNext);n;n=n->pNext) {
            if(n->sType==VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO) {
                const auto usage=reinterpret_cast<const VkBufferUsageFlags2CreateInfo*>(n)->usage;
                b->usage=VkBufferUsageFlags(usage);b->captureCompatible &= usage==b->usage;
            } else b->captureCompatible=false;
        }
        b->transferPatched=added;if(added)b->usage|=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        remember(s,s.buffers,id(*out),std::move(b));}return r;
}
VKAPI_ATTR void VKAPI_CALL destroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    retire_uniform_source(d,id(b));
    {std::lock_guard lock(mutex);device_state(d).buffers.erase(id(b));}
    next<PFN_vkDestroyBuffer>(*data,"vkDestroyBuffer")(d,b,a);
}
VKAPI_ATTR VkResult VKAPI_CALL image(VkDevice d,const VkImageCreateInfo* info,const VkAllocationCallbacks* a,VkImage* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreateImage>(*data,"vkCreateImage")(d,info,a,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);auto value=std::make_shared<Image>();
        value->handle=id(*out);value->generation=++s.serial;value->format=info->format;value->extent=info->extent;
        value->sharing=info->sharingMode;value->type=info->imageType;value->flags=info->flags;value->usage=info->usage;value->layers=info->arrayLayers;value->mips=info->mipLevels;value->samples=info->samples;
        value->previewCompatible=info->tiling==VK_IMAGE_TILING_OPTIMAL;
        for(auto* n=static_cast<const VkBaseInStructure*>(info->pNext);n;n=n->pNext)
            if(n->sType!=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)value->previewCompatible=false;
        remember(s,s.images,id(*out),std::move(value));
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice d,VkImage h,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    release_inspector_image(d,h);
    {std::lock_guard lock(mutex);auto& s=device_state(d);destroy_water_source(s,id(h));s.images.erase(id(h));}
    next<PFN_vkDestroyImage>(*data,"vkDestroyImage")(d,h,a);
}
VKAPI_ATTR VkResult VKAPI_CALL view(VkDevice d,const VkImageViewCreateInfo* info,const VkAllocationCallbacks* a,VkImageView* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreateImageView>(*data,"vkCreateImageView")(d,info,a,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);auto value=std::make_shared<View>();
        value->handle=id(*out);value->generation=++s.serial;value->image=find_ptr(s.images,id(info->image));
        value->type=info->viewType;value->components=info->components;value->range=info->subresourceRange;value->format=info->format;
        value->previewCompatible=!info->flags;
        for(auto* n=static_cast<const VkBaseInStructure*>(info->pNext);n;n=n->pNext) {
            if(n->sType!=VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO)value->previewCompatible=false;
            else value->previewCompatible &= bool(reinterpret_cast<const VkImageViewUsageCreateInfo*>(n)->usage&VK_IMAGE_USAGE_SAMPLED_BIT);
        }
        remember(s,s.views,id(*out),std::move(value));
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroyView(VkDevice d,VkImageView h,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);device_state(d).views.erase(id(h));}
    next<PFN_vkDestroyImageView>(*data,"vkDestroyImageView")(d,h,a);
}
VKAPI_ATTR VkResult VKAPI_CALL swapchainImages(VkDevice d,VkSwapchainKHR sc,uint32_t* count,VkImage* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkGetSwapchainImagesKHR>(*data,"vkGetSwapchainImagesKHR")(d,sc,count,out);
    if((r==VK_SUCCESS || r==VK_INCOMPLETE) && out) {
        SwapchainInfo info;
        {std::lock_guard lock(g_state_mutex);auto it=g_swapchains.find({d,id(sc)});if(it!=g_swapchains.end())info=it->second;}
        std::lock_guard lock(mutex);auto& s=device_state(d);
        for(uint32_t i=0;i<*count;++i)if(!s.images.count(id(out[i]))) {
            auto value=std::make_shared<Image>();value->handle=id(out[i]);value->generation=++s.serial;value->format=info.format;
            value->extent={info.width,info.height,1};value->usage=info.usage;value->layers=value->mips=value->samples=1;value->swapchain=true;
            remember(s,s.images,id(out[i]),std::move(value));
        }
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroySwapchain(VkDevice d,VkSwapchainKHR sc,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    std::vector<VkImage> images;
    {std::lock_guard lock(g_state_mutex);auto it=g_swapchains.find({d,id(sc)});if(it!=g_swapchains.end())images=it->second.images;}
    {std::lock_guard lock(mutex);auto& s=device_state(d);for(auto i:images)s.images.erase(id(i));s.isolation=0;s.isolationTargets.clear();}
    next<PFN_vkDestroySwapchainKHR>(*data,"vkDestroySwapchainKHR")(d,sc,a);
}
VKAPI_ATTR VkResult VKAPI_CALL sampler(VkDevice d,const VkSamplerCreateInfo* info,const VkAllocationCallbacks* a,VkSampler* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreateSampler>(*data,"vkCreateSampler")(d,info,a,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);auto v=std::make_shared<Sampler>();
        v->handle=id(*out);v->generation=++s.serial;v->compare=info->compareEnable;v->op=info->compareOp;
        v->address[0]=info->addressModeU;v->address[1]=info->addressModeV;v->address[2]=info->addressModeW;
        v->min=info->minFilter;v->mag=info->magFilter;v->mipmap=info->mipmapMode;
        v->lodBias=info->mipLodBias;v->minLod=info->minLod;v->maxLod=info->maxLod;
        v->signature=14695981039346656037ull;
        for(uint32_t x:{v->compare,v->op,v->min,v->mag,v->mipmap,uint32_t(info->addressModeU),uint32_t(info->addressModeV),uint32_t(info->addressModeW),info->unnormalizedCoordinates})hash_value(v->signature,x);
        for(float x:{info->mipLodBias,info->minLod,info->maxLod,info->maxAnisotropy})v->signature=hash_bytes(&x,sizeof(x),v->signature);
        hash_value(v->signature,info->anisotropyEnable);hash_value(v->signature,info->borderColor);
        remember(s,s.samplers,id(*out),std::move(v));
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroySampler(VkDevice d,VkSampler h,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);device_state(d).samplers.erase(id(h));}
    next<PFN_vkDestroySampler>(*data,"vkDestroySampler")(d,h,a);
}
VKAPI_ATTR VkResult VKAPI_CALL setLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo* info,const VkAllocationCallbacks* a,VkDescriptorSetLayout* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreateDescriptorSetLayout>(*data,"vkCreateDescriptorSetLayout")(d,info,a,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);auto v=std::make_shared<SetLayout>();v->signature=14695981039346656037ull;v->flags=info->flags;v->diagnostic="flags="+hex(info->flags)+" pNext="+pnext_types(info->pNext);
        const auto* flags=chain<VkDescriptorSetLayoutBindingFlagsCreateInfo>(info->pNext,VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
        if(info->flags&VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT)v->supported=false;
        v->largeSamplerArray=info->bindingCount==1 && info->pBindings[0].descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER &&
            !info->pBindings[0].pImmutableSamplers && info->pBindings[0].descriptorCount<=maxSamplerDescriptors &&
            !(info->flags&~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) &&
            (!flags || (flags->bindingCount==1 && !(flags->pBindingFlags[0]&~VkDescriptorBindingFlags(VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT|VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT|VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT))));
        const size_t descriptorLimit=v->largeSamplerArray?maxSamplerDescriptors:maxDescriptors;
        size_t total=0;
        for(uint32_t i=0;i<info->bindingCount;++i) {
            const auto& b=info->pBindings[i];total+=b.descriptorCount;
            if(total>descriptorLimit){v->supported=false;break;}
            Binding value;value.binding=b.binding;value.count=b.descriptorCount;value.type=b.descriptorType;value.stages=b.stageFlags;
            if(flags && i<flags->bindingCount)value.flags=flags->pBindingFlags[i];
            if(b.pImmutableSamplers)for(uint32_t j=0;j<b.descriptorCount;++j)value.immutable.push_back(find_ptr(s.samplers,id(b.pImmutableSamplers[j])));
            v->bindings[b.binding]=std::move(value);
        }
        for(auto& [number,b]:v->bindings) {
            for(uint32_t x:{number,b.count,uint32_t(b.type),b.stages,b.flags})hash_value(v->signature,x);
            for(auto& immutable:b.immutable)hash_value(v->signature,immutable?immutable->signature:0);
        }
        for(auto* n=static_cast<const VkBaseInStructure*>(info->pNext);n;n=n->pNext)if(n->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)v->supported=false;
        if(v->largeSamplerArray)inspector_diagnostic(s,"sampler-array-layout","count="+std::to_string(info->pBindings[0].descriptorCount)+" flags="+std::to_string(flags?flags->pBindingFlags[0]:0));
        if(!v->supported)inspector_diagnostic(s,"set-layout:"+v->diagnostic,"unsupported=1 bindingCount="+std::to_string(info->bindingCount));
        remember(s,s.setLayouts,id(*out),std::move(v));
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroySetLayout(VkDevice d,VkDescriptorSetLayout h,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);device_state(d).setLayouts.erase(id(h));}
    next<PFN_vkDestroyDescriptorSetLayout>(*data,"vkDestroyDescriptorSetLayout")(d,h,a);
}
VKAPI_ATTR VkResult VKAPI_CALL layout(VkDevice d,const VkPipelineLayoutCreateInfo* info,const VkAllocationCallbacks* a,VkPipelineLayout* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreatePipelineLayout>(*data,"vkCreatePipelineLayout")(d,info,a,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);auto v=std::make_shared<Layout>();v->signature=14695981039346656037ull;v->flags=info->flags;v->diagnostic="flags="+hex(info->flags)+" pNext="+pnext_types(info->pNext);
        if(info->setLayoutCount>maxSets || info->pNext)v->supported=false;
        hash_value(v->signature,info->flags);
        for(uint32_t i=0;i<std::min(info->setLayoutCount,uint32_t(maxSets));++i){auto l=find_ptr(s.setLayouts,id(info->pSetLayouts[i]));v->sets.push_back(l);hash_value(v->signature,l?l->signature:0);if(!l || !l->supported)v->supported=false;}
        v->pushSignature=14695981039346656037ull;hash_value(v->pushSignature,info->pushConstantRangeCount);
        for(uint32_t i=0;i<info->pushConstantRangeCount;++i){const auto& p=info->pPushConstantRanges[i];hash_value(v->signature,p.stageFlags);hash_value(v->signature,p.offset);hash_value(v->signature,p.size);hash_value(v->pushSignature,p.stageFlags);hash_value(v->pushSignature,p.offset);hash_value(v->pushSignature,p.size);}
        if(info->pushConstantRangeCount)v->pushRanges.assign(info->pPushConstantRanges,info->pPushConstantRanges+info->pushConstantRangeCount);
        create_water_layout(*data,*v,*info);
        if(v->water)water_state(s).layouts.push_back(v->water);
        if(!v->supported)inspector_diagnostic(s,"pipeline-layout:"+v->diagnostic,"unsupported=1 setCount="+std::to_string(info->setLayoutCount));
        remember(s,s.layouts,id(*out),std::move(v));
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroyLayout(VkDevice d,VkPipelineLayout h,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);device_state(d).layouts.erase(id(h));}
    next<PFN_vkDestroyPipelineLayout>(*data,"vkDestroyPipelineLayout")(d,h,a);
}
VKAPI_ATTR VkResult VKAPI_CALL allocateSets(VkDevice d,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkAllocateDescriptorSets>(*data,"vkAllocateDescriptorSets")(d,info,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);
        for(uint32_t i=0;i<info->descriptorSetCount;++i) {
            auto v=std::make_shared<Set>();v->handle=id(out[i]);v->generation=++s.serial;v->pool=id(info->descriptorPool);v->layout=find_ptr(s.setLayouts,id(info->pSetLayouts[i]));
            v->incomplete=!v->layout || !v->layout->supported || info->pNext;
            if(v->incomplete)inspector_diagnostic(s,"descriptor-allocation:"+pnext_types(info->pNext),"layout="+(v->layout?v->layout->diagnostic:"missing")+" supported="+std::to_string(v->layout && v->layout->supported));
            if(v->layout)for(auto& [b,binding]:v->layout->bindings)for(uint32_t j=0;j<binding.immutable.size();++j) {
                Descriptor value;value.type=binding.type;value.sampler=binding.immutable[j];value.known=binding.type==VK_DESCRIPTOR_TYPE_SAMPLER && bool(value.sampler);
                v->descriptors[{b,j}]=std::move(value);
            }
            remember(s,s.sets,id(out[i]),std::move(v));
        }
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL freeSets(VkDevice d,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* values) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkFreeDescriptorSets>(*data,"vkFreeDescriptorSets")(d,pool,count,values);
    if(r==VK_SUCCESS){std::lock_guard lock(mutex);auto& s=device_state(d);for(uint32_t i=0;i<count;++i)s.sets.erase(id(values[i]));}return r;
}
void forget_pool(Device& s,VkDescriptorPool pool){std::erase_if(s.sets,[&](const auto& item){return item.second->pool==id(pool);});}
VKAPI_ATTR VkResult VKAPI_CALL resetDescriptorPool(VkDevice d,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkResetDescriptorPool>(*data,"vkResetDescriptorPool")(d,pool,flags);
    if(r==VK_SUCCESS){std::lock_guard lock(mutex);forget_pool(device_state(d),pool);}return r;
}
VKAPI_ATTR void VKAPI_CALL destroyDescriptorPool(VkDevice d,VkDescriptorPool pool,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);forget_pool(device_state(d),pool);}
    next<PFN_vkDestroyDescriptorPool>(*data,"vkDestroyDescriptorPool")(d,pool,a);
}
std::vector<std::pair<uint32_t,uint32_t>> descriptor_keys(const Set& set,uint32_t binding,uint32_t element,uint32_t count,VkDescriptorType type) {
    std::vector<std::pair<uint32_t,uint32_t>> keys;
    if(!set.layout || count>(set.layout->largeSamplerArray?maxSamplerDescriptors:maxDescriptors))return keys;
    auto it=set.layout->bindings.find(binding);
    for(uint32_t n=0;n<count && it!=set.layout->bindings.end();) {
        if(it->second.type!=type)return {};
        if(element>=it->second.count) {auto next=std::next(it);if(next==set.layout->bindings.end() || next->first!=it->first+1)return {};it=next;element=0;continue;}
        keys.emplace_back(it->first,element++);++n;
    }
    if(keys.size()!=count)keys.clear();
    return keys;
}
}
void write_descriptors(Device& s,Set& set,uint32_t count,const VkWriteDescriptorSet* writes) {
    for(uint32_t i=0;i<count;++i) {
        const auto& w=writes[i];auto keys=descriptor_keys(set,w.dstBinding,w.dstArrayElement,w.descriptorCount,w.descriptorType);
        if(keys.size()!=w.descriptorCount || w.pNext) {
            set.incomplete=true;inspector_diagnostic(s,"descriptor-write:type="+std::to_string(w.descriptorType)+" pNext="+pnext_types(w.pNext),
                "binding="+std::to_string(w.dstBinding)+" count="+std::to_string(w.descriptorCount)+" resolved="+std::to_string(keys.size())+" layout="+(set.layout?set.layout->diagnostic:"missing"));continue;
        }
        for(uint32_t j=0;j<w.descriptorCount;++j) {
            Descriptor v;v.type=w.descriptorType;
            if(w.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || w.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
               w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || w.descriptorType==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT || w.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER) {
                const auto& image=w.pImageInfo[j];v.layout=image.imageLayout;
                if(w.descriptorType!=VK_DESCRIPTOR_TYPE_SAMPLER)v.view=find_ptr(s.views,id(image.imageView));
                if(w.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER || w.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    auto& b=set.layout->bindings.at(keys[j].first);
                    v.sampler=keys[j].second<b.immutable.size()?b.immutable[keys[j].second]:find_ptr(s.samplers,id(image.sampler));
                }
                v.known=(w.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER || bool(v.view)) &&
                    ((w.descriptorType!=VK_DESCRIPTOR_TYPE_SAMPLER && w.descriptorType!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) || bool(v.sampler));
            } else if(w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                      w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                const auto& buffer=w.pBufferInfo[j];v.buffer=id(buffer.buffer);v.offset=buffer.offset;v.range=buffer.range;v.known=true;
                if(auto tracked=find_ptr(s.buffers,v.buffer))v.bufferGeneration=tracked->generation;
            } else set.incomplete=true;
            set.descriptors[keys[j]]=std::move(v);
        }
    }
}
void apply_template(Device& s,Set& set,const DescriptorTemplate& templ,const void* data) {
    if(!templ.supported || !data){set.incomplete=true;inspector_diagnostic(s,"descriptor-template","supported="+std::to_string(templ.supported)+" dataPresent="+std::to_string(data!=nullptr));return;}
    for(const auto& entry:templ.entries) {
        if(entry.descriptorCount>maxDescriptors || entry.offset>64*1024*1024 || entry.stride>64*1024*1024){set.incomplete=true;continue;}
        std::vector<VkDescriptorImageInfo> images;std::vector<VkDescriptorBufferInfo> buffers;
        bool image=entry.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER || entry.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            entry.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || entry.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || entry.descriptorType==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        bool buffer=entry.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || entry.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            entry.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || entry.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        if(!image && !buffer){set.incomplete=true;continue;}
        for(uint32_t j=0;j<entry.descriptorCount;++j) {
            const auto* source=static_cast<const char*>(data)+entry.offset+j*entry.stride;
            if(image){VkDescriptorImageInfo value;std::memcpy(&value,source,sizeof(value));images.push_back(value);}
            else{VkDescriptorBufferInfo value;std::memcpy(&value,source,sizeof(value));buffers.push_back(value);}
        }
        VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstBinding=entry.dstBinding;
        write.dstArrayElement=entry.dstArrayElement;write.descriptorType=entry.descriptorType;write.descriptorCount=entry.descriptorCount;
        write.pImageInfo=images.data();write.pBufferInfo=buffers.data();write_descriptors(s,set,1,&write);
    }
}
namespace {
VKAPI_ATTR void VKAPI_CALL updateSets(VkDevice d,uint32_t wc,const VkWriteDescriptorSet* writes,uint32_t cc,const VkCopyDescriptorSet* copies) {
    auto data=get_device_data(d);if(!data)return;
    next<PFN_vkUpdateDescriptorSets>(*data,"vkUpdateDescriptorSets")(d,wc,writes,cc,copies);
    std::lock_guard lock(mutex);auto& s=device_state(d);
    for(uint32_t i=0;i<wc;++i)if(auto set=find_ptr(s.sets,id(writes[i].dstSet)))write_descriptors(s,*set,1,writes+i);
    for(uint32_t i=0;i<cc;++i) {
        const auto& c=copies[i];auto dst=find_ptr(s.sets,id(c.dstSet)),src=find_ptr(s.sets,id(c.srcSet));if(!dst)continue;
        if(!src || !src->layout || !src->layout->bindings.count(c.srcBinding)){dst->incomplete=true;continue;}
        auto type=src->layout->bindings.at(c.srcBinding).type;
        auto sk=descriptor_keys(*src,c.srcBinding,c.srcArrayElement,c.descriptorCount,type),dk=descriptor_keys(*dst,c.dstBinding,c.dstArrayElement,c.descriptorCount,type);
        if(sk.size()!=c.descriptorCount || dk.size()!=c.descriptorCount){dst->incomplete=true;continue;}
        std::vector<Descriptor> values;for(auto key:sk){auto found=src->descriptors.find(key);values.push_back(found==src->descriptors.end()?Descriptor{}:found->second);}
        for(uint32_t j=0;j<c.descriptorCount;++j) {
            const auto& b=dst->layout->bindings.at(dk[j].first);
            if(dk[j].second<b.immutable.size())values[j].sampler=b.immutable[dk[j].second];
            dst->descriptors[dk[j]]=values[j];
        }
        dst->incomplete|=src->incomplete;
    }
}
VkResult createTemplateImpl(VkDevice d,const VkDescriptorUpdateTemplateCreateInfo* info,const VkAllocationCallbacks* a,VkDescriptorUpdateTemplate* out,const char* name) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreateDescriptorUpdateTemplate>(*data,name)(d,info,a,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);DescriptorTemplate v;
        v.type=info->templateType;
        if(v.type==VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR){v.layout=id(info->pipelineLayout);v.set=info->set;v.point=info->pipelineBindPoint;}
        v.supported=info->descriptorUpdateEntryCount<=maxDescriptors && !info->pNext;
        if(v.supported)v.entries.assign(info->pDescriptorUpdateEntries,info->pDescriptorUpdateEntries+info->descriptorUpdateEntryCount);
        remember(s,s.templates,id(*out),std::move(v));
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL createTemplate(VkDevice d,const VkDescriptorUpdateTemplateCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorUpdateTemplate* o){return createTemplateImpl(d,i,a,o,"vkCreateDescriptorUpdateTemplate");}
VKAPI_ATTR VkResult VKAPI_CALL createTemplateKHR(VkDevice d,const VkDescriptorUpdateTemplateCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorUpdateTemplate* o){return createTemplateImpl(d,i,a,o,"vkCreateDescriptorUpdateTemplateKHR");}
void destroyTemplateImpl(VkDevice d,VkDescriptorUpdateTemplate t,const VkAllocationCallbacks* a,const char* name) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);device_state(d).templates.erase(id(t));}
    next<PFN_vkDestroyDescriptorUpdateTemplate>(*data,name)(d,t,a);
}
VKAPI_ATTR void VKAPI_CALL destroyTemplate(VkDevice d,VkDescriptorUpdateTemplate t,const VkAllocationCallbacks* a){destroyTemplateImpl(d,t,a,"vkDestroyDescriptorUpdateTemplate");}
VKAPI_ATTR void VKAPI_CALL destroyTemplateKHR(VkDevice d,VkDescriptorUpdateTemplate t,const VkAllocationCallbacks* a){destroyTemplateImpl(d,t,a,"vkDestroyDescriptorUpdateTemplateKHR");}
void updateTemplateImpl(VkDevice d,VkDescriptorSet set,VkDescriptorUpdateTemplate t,const void* values,const char* name) {
    auto data=get_device_data(d);if(!data)return;
    next<PFN_vkUpdateDescriptorSetWithTemplate>(*data,name)(d,set,t,values);
    std::lock_guard lock(mutex);auto& s=device_state(d);auto target=find_ptr(s.sets,id(set));if(!target)return;
    auto found=s.templates.find(id(t));if(found==s.templates.end()){target->incomplete=true;return;}apply_template(s,*target,found->second,values);
}
VKAPI_ATTR void VKAPI_CALL updateTemplate(VkDevice d,VkDescriptorSet s,VkDescriptorUpdateTemplate t,const void* v){updateTemplateImpl(d,s,t,v,"vkUpdateDescriptorSetWithTemplate");}
VKAPI_ATTR void VKAPI_CALL updateTemplateKHR(VkDevice d,VkDescriptorSet s,VkDescriptorUpdateTemplate t,const void* v){updateTemplateImpl(d,s,t,v,"vkUpdateDescriptorSetWithTemplateKHR");}
VKAPI_ATTR VkResult VKAPI_CALL renderPass(VkDevice d,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks* a,VkRenderPass* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreateRenderPass>(*data,"vkCreateRenderPass")(d,info,a,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);auto p=std::make_shared<Pass>();
        p->supported=info->attachmentCount<=32 && info->subpassCount<=32 && !info->pNext;
        if(info->attachmentCount<=32 && info->subpassCount<=32) {
            if(info->attachmentCount)p->attachments.assign(info->pAttachments,info->pAttachments+info->attachmentCount);
            for(uint32_t i=0;i<info->subpassCount;++i) {
                const auto& v=info->pSubpasses[i];Subpass sub;
                if(v.colorAttachmentCount<=8)for(uint32_t j=0;j<v.colorAttachmentCount;++j)sub.colors.push_back(v.pColorAttachments[j].attachment);else p->supported=false;
                if(v.pDepthStencilAttachment)sub.depth=v.pDepthStencilAttachment->attachment;
                p->subpasses.push_back(std::move(sub));
            }
        }
        remember(s,s.passes,id(*out),std::move(p));
    }
    return r;
}
VkResult renderPass2Impl(VkDevice d,const VkRenderPassCreateInfo2* info,const VkAllocationCallbacks* a,VkRenderPass* out,const char* name) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreateRenderPass2>(*data,name)(d,info,a,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);auto p=std::make_shared<Pass>();
        p->supported=info->attachmentCount<=32 && info->subpassCount<=32 && !info->pNext;
        if(info->attachmentCount<=32 && info->subpassCount<=32) {
            for(uint32_t i=0;i<info->attachmentCount;++i) {
                auto& v=info->pAttachments[i];p->attachments.push_back({v.flags,v.format,v.samples,v.loadOp,v.storeOp,v.stencilLoadOp,v.stencilStoreOp,v.initialLayout,v.finalLayout});
            }
            for(uint32_t i=0;i<info->subpassCount;++i) {
                const auto& v=info->pSubpasses[i];Subpass sub;
                if(v.colorAttachmentCount<=8)for(uint32_t j=0;j<v.colorAttachmentCount;++j)sub.colors.push_back(v.pColorAttachments[j].attachment);else p->supported=false;
                if(v.pDepthStencilAttachment)sub.depth=v.pDepthStencilAttachment->attachment;
                if(v.viewMask || v.pNext)p->supported=false;
                p->subpasses.push_back(std::move(sub));
            }
        }
        remember(s,s.passes,id(*out),std::move(p));
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL renderPass2(VkDevice d,const VkRenderPassCreateInfo2* i,const VkAllocationCallbacks* a,VkRenderPass* o){return renderPass2Impl(d,i,a,o,"vkCreateRenderPass2");}
VKAPI_ATTR VkResult VKAPI_CALL renderPass2KHR(VkDevice d,const VkRenderPassCreateInfo2* i,const VkAllocationCallbacks* a,VkRenderPass* o){return renderPass2Impl(d,i,a,o,"vkCreateRenderPass2KHR");}
VKAPI_ATTR void VKAPI_CALL destroyPass(VkDevice d,VkRenderPass p,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);device_state(d).passes.erase(id(p));}next<PFN_vkDestroyRenderPass>(*data,"vkDestroyRenderPass")(d,p,a);
}
VKAPI_ATTR VkResult VKAPI_CALL framebuffer(VkDevice d,const VkFramebufferCreateInfo* info,const VkAllocationCallbacks* a,VkFramebuffer* out) {
    auto data=get_device_data(d);if(!data)return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next<PFN_vkCreateFramebuffer>(*data,"vkCreateFramebuffer")(d,info,a,out);
    if(r==VK_SUCCESS) {
        std::lock_guard lock(mutex);auto& s=device_state(d);Framebuffer v;
        v.width=info->width;v.height=info->height;v.layers=info->layers;v.imageless=info->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
        if(!v.imageless && info->attachmentCount<=32)for(uint32_t i=0;i<info->attachmentCount;++i)v.views.push_back(find_ptr(s.views,id(info->pAttachments[i])));
        remember(s,s.framebuffers,id(*out),std::move(v));
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroyFramebuffer(VkDevice d,VkFramebuffer h,const VkAllocationCallbacks* a) {
    auto data=get_device_data(d);if(!data)return;
    {std::lock_guard lock(mutex);device_state(d).framebuffers.erase(id(h));}next<PFN_vkDestroyFramebuffer>(*data,"vkDestroyFramebuffer")(d,h,a);
}
}
void set_target(Command& c,uint32_t index) {
    c.target.colors.clear();c.target.depth.reset();c.target.subpass=index;
    c.target.known=c.pass && c.pass->supported && index<c.pass->subpasses.size();
    if(!c.target.known)return;
    auto view=[&](uint32_t i){return i<c.attachments.size()?c.attachments[i]:std::shared_ptr<View>{};};
    const auto& sub=c.pass->subpasses[index];
    for(auto i:sub.colors)if(i!=VK_ATTACHMENT_UNUSED){c.target.colors.push_back(view(i));if(!view(i))c.target.known=false;}
    if(sub.depth!=VK_ATTACHMENT_UNUSED){c.target.depth=view(sub.depth);if(!c.target.depth)c.target.known=false;}
}
PFN_vkVoidFunction resource_hook(const char* name) {
#define HOOK(api,fn) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(fn)
    HOOK(vkCreateBuffer,buffer);HOOK(vkDestroyBuffer,destroyBuffer);
    HOOK(vkCreateImage,image);HOOK(vkDestroyImage,destroyImage);HOOK(vkCreateImageView,view);HOOK(vkDestroyImageView,destroyView);
    HOOK(vkGetSwapchainImagesKHR,swapchainImages);HOOK(vkDestroySwapchainKHR,destroySwapchain);
    HOOK(vkCreateSampler,sampler);HOOK(vkDestroySampler,destroySampler);
    HOOK(vkCreateDescriptorSetLayout,setLayout);HOOK(vkDestroyDescriptorSetLayout,destroySetLayout);
    HOOK(vkCreatePipelineLayout,layout);HOOK(vkDestroyPipelineLayout,destroyLayout);
    HOOK(vkAllocateDescriptorSets,allocateSets);HOOK(vkFreeDescriptorSets,freeSets);
    HOOK(vkResetDescriptorPool,resetDescriptorPool);HOOK(vkDestroyDescriptorPool,destroyDescriptorPool);HOOK(vkUpdateDescriptorSets,updateSets);
    HOOK(vkCreateDescriptorUpdateTemplate,createTemplate);HOOK(vkCreateDescriptorUpdateTemplateKHR,createTemplateKHR);
    HOOK(vkDestroyDescriptorUpdateTemplate,destroyTemplate);HOOK(vkDestroyDescriptorUpdateTemplateKHR,destroyTemplateKHR);
    HOOK(vkUpdateDescriptorSetWithTemplate,updateTemplate);HOOK(vkUpdateDescriptorSetWithTemplateKHR,updateTemplateKHR);
    HOOK(vkCreateRenderPass,renderPass);HOOK(vkCreateRenderPass2,renderPass2);HOOK(vkCreateRenderPass2KHR,renderPass2KHR);HOOK(vkDestroyRenderPass,destroyPass);
    HOOK(vkCreateFramebuffer,framebuffer);HOOK(vkDestroyFramebuffer,destroyFramebuffer);
#undef HOOK
    return nullptr;
}
}
