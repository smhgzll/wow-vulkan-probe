// Included by discovery_test.cpp; exercises public descriptor/push hooks.
static void sampler_tracking_cases(VkCommandBuffer cmd) {
    using namespace depth_probe::discovery;
    auto code=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/inspector_sampler.frag.spv");
    auto shader=std::make_shared<Shader>(inspect_spirv(code.data(),code.size()*4));
    CHECK(shader->valid && shader->replaySafe && shader->samplerAccessesComplete && shader->samplers.size()==2);
    for(const auto& a:shader->samplers)CHECK(a.index.known && a.index.fromPush && a.index.value==4 && a.index.mask==65535);
    code=read_spirv_file(std::string(DEPTH_PROBE_TEST_ROOT_DIR)+"/tests/inspector_sampler_helper.frag.spv");
    auto helper=inspect_spirv(code.data(),code.size()*4);CHECK(helper.replaySafe && helper.samplers.size()==1 && !helper.samplers[0].index.known);
    auto& s=devices.at(device);
    VkDescriptorBindingFlags flags=VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT|VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT|VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{};bf.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;bf.bindingCount=1;bf.pBindingFlags=&flags;
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_SAMPLER,4096,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;si.pNext=&bf;si.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;si.bindingCount=1;si.pBindings=&binding;
    VkDescriptorSetLayout sl;CHECK(H(vkCreateDescriptorSetLayout)(device,&si,nullptr,&sl)==VK_SUCCESS);
    CHECK(s.setLayouts.at(id(sl))->supported && s.setLayouts.at(id(sl))->largeSamplerArray);
    // The exception is bounded and sampler-only.
    binding.descriptorCount=4097;VkDescriptorSetLayout rejected;CHECK(H(vkCreateDescriptorSetLayout)(device,&si,nullptr,&rejected)==VK_SUCCESS);CHECK(!s.setLayouts.at(id(rejected))->supported);H(vkDestroyDescriptorSetLayout)(device,rejected,nullptr);
    binding.descriptorCount=4096;binding.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;CHECK(H(vkCreateDescriptorSetLayout)(device,&si,nullptr,&rejected)==VK_SUCCESS);CHECK(!s.setLayouts.at(id(rejected))->supported);H(vkDestroyDescriptorSetLayout)(device,rejected,nullptr);
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,8};
    VkPipelineLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.setLayoutCount=1;li.pSetLayouts=&sl;li.pushConstantRangeCount=1;li.pPushConstantRanges=&range;
    VkPipelineLayout layout,compatible,incompatible;CHECK(H(vkCreatePipelineLayout)(device,&li,nullptr,&layout)==VK_SUCCESS);CHECK(H(vkCreatePipelineLayout)(device,&li,nullptr,&compatible)==VK_SUCCESS);
    range.size=12;CHECK(H(vkCreatePipelineLayout)(device,&li,nullptr,&incompatible)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=handle<VkDescriptorPool>(555);ai.descriptorSetCount=1;ai.pSetLayouts=&sl;
    VkDescriptorSet set;CHECK(H(vkAllocateDescriptorSets)(device,&ai,&set)==VK_SUCCESS);auto tracked=s.sets.at(id(set));CHECK(!tracked->incomplete && tracked->descriptors.empty());
    VkSamplerCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;VkSampler a,b;
    CHECK(H(vkCreateSampler)(device,&ci,nullptr,&a)==VK_SUCCESS);CHECK(H(vkCreateSampler)(device,&ci,nullptr,&b)==VK_SUCCESS);
    auto write=[&](uint32_t element,VkSampler sampler) {
        VkDescriptorImageInfo image{sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};VkWriteDescriptorSet w{};w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.dstArrayElement=element;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;w.pImageInfo=&image;H(vkUpdateDescriptorSets)(device,1,&w,0,nullptr);
    };write(900,a);write(901,a);CHECK(tracked->descriptors.size()==2 && !tracked->incomplete);
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;CHECK(H(vkBeginCommandBuffer)(cmd,&begin)==VK_SUCCESS);
    uint32_t packed=(901u<<16)|900;
    H(vkCmdPushConstants)(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,4,4,&packed);
    auto& command=s.commands.at(id(cmd));CHECK(command.push[1].known==2 && !command.push[0].known);
    auto pipeline=std::make_shared<Pipeline>();pipeline->layout=s.layouts.at(id(layout));pipeline->fs=shader;
    auto resolve=[&] {SubmittedDraw out;out.draw.pipeline=pipeline;out.draw.push=command.push;out.draw.sets[0]={id(set),tracked->generation,pipeline->layout,{}};resolve_sampler_resources(s,out,0,*tracked);return out;};
    auto exact=resolve();CHECK(!exact.descriptorsIncomplete && exact.resources.size()==2 && coverage_resources_supported(exact));
    CHECK(exact.resources[0].element==900 && exact.resources[1].element==901 && exact.resources[0].samplerIndexed && exact.resources[0].updateAfterBind);
    const auto material=material_identity(exact);CHECK(material);
    write(999,b);revalidate_sampler_resources(s,exact);CHECK(!exact.descriptorsIncomplete);CHECK(material_identity(resolve())==material);
    write(901,b);revalidate_sampler_resources(s,exact);CHECK(exact.descriptorsIncomplete && !coverage_resources_supported(exact));CHECK(material_identity(resolve())!=material);write(901,a);
    packed=(900u<<16)|900;H(vkCmdPushConstants)(cmd,compatible,VK_SHADER_STAGE_FRAGMENT_BIT,4,4,&packed);CHECK(resolve().resources.size()==1);
    packed=(4096u<<16)|900;H(vkCmdPushConstants)(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,4,4,&packed);CHECK(!coverage_resources_supported(resolve()));
    packed=(899u<<16)|900;H(vkCmdPushConstants)(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,4,4,&packed);CHECK(!coverage_resources_supported(resolve()));
    packed=(901u<<16)|900;H(vkCmdPushConstants)(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,4,4,&packed);
    H(vkCmdPushConstants)(cmd,incompatible,VK_SHADER_STAGE_FRAGMENT_BIT,0,4,&packed);auto unknown=resolve();CHECK(unknown.descriptorIssues==SamplerIndexUnknown && !material_identity(unknown) && coverage_resources_supported(unknown));
    H(vkCmdPushConstants)(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,4,4,&packed);
    // Optional entry points preserve forwarding, stage separation and unknown pNext.
    VkPushConstantsInfoKHR pi{};pi.sType=VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO_KHR;pi.layout=layout;pi.stageFlags=VK_SHADER_STAGE_VERTEX_BIT;pi.offset=4;pi.size=4;pi.pValues=&packed;
    H(vkCmdPushConstants2KHR)(cmd,&pi);CHECK(command.push[0].known==2 && command.push[1].known==2);
    VkBaseInStructure other{};other.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;pi.pNext=&other;H(vkCmdPushConstants2)(cmd,&pi);CHECK(!command.push[0].known && command.push[1].known==2);
    H(vkDestroySampler)(device,a,nullptr);CHECK(!coverage_resources_supported(resolve()));
    auto uabImage=unknown;uabImage.resources.push_back({});uabImage.resources.back().updateAfterBind=true;uabImage.resources.back().descriptor.type=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;CHECK(!coverage_resources_supported(uabImage));
    CHECK(H(vkResetCommandBuffer)(cmd,0)==VK_SUCCESS);CHECK(!s.commands.at(id(cmd)).push[1].known);
    H(vkFreeDescriptorSets)(device,ai.descriptorPool,1,&set);H(vkDestroySampler)(device,b,nullptr);
    for(auto l:{layout,compatible,incompatible})H(vkDestroyPipelineLayout)(device,l,nullptr);
    H(vkDestroyDescriptorSetLayout)(device,sl,nullptr);
    std::puts("PASS: sparse 4096 sampler heap, packed push indices, stage/layout/reset tracking, unknown/OOB/dead slots, UAB image guard and submit revalidation");
}
