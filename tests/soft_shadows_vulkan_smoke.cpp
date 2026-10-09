// Soft shadows on the real driver: WoW's terrain / model pixel shaders (tests/fixtures), as DXVK hands them over (inline
// SPIR-V in the stage's chain), go through the layer's SoftShadowPipelines (src/game_soft_shadows.*) and are compiled into
// graphics pipelines - the game's own code and the soft-shadow code - with the driver's shader disk cache off, so both are
// really compiled. The pipeline layout is reflected from the shaders (DXVK's sampler heap, the 2D / shadow image pairs, the
// constant buffers, the push block). Prints the compile times. DEPTH_PROBE_SMOKE_VALIDATION=1: under the Khronos validation layer
// (the terrain pair only: the model shader has no vertex shader of its own here).
#include "game_soft_shadows.hpp"

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
static std::atomic<unsigned> errors{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* p, void*) {
    ++errors;
    std::fprintf(stderr, "VALIDATION: %s\n", p->pMessage);
    return VK_FALSE;
}

static std::vector<uint32_t> load(const std::string& name) {
    std::ifstream in(std::string(DEPTH_PROBE_TEST_ROOT_DIR) + "/tests/fixtures/" + name, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), w.size() * 4);
    return w;
}

// What a pipeline needs from a module: its descriptors, vertex inputs (vertex shader) and colour outputs (fragment shader).
struct Interface {
    std::map<std::pair<uint32_t, uint32_t>, std::pair<VkDescriptorType, uint32_t>> descriptors;   // (set, binding) -> type, count
    std::map<uint32_t, VkFormat> inputs, outputs;                                                   // location -> format
    bool push = false;
};
static Interface reflect(const std::vector<uint32_t>& w) {
    std::map<uint32_t, std::vector<uint32_t>> types;   // id -> opcode, operands after the id
    std::map<uint32_t, uint32_t> constant, set, binding, location;
    std::set<uint32_t> builtin, bufferBlock;
    std::vector<std::array<uint32_t, 3>> vars;          // type, id, storage class
    for (size_t i = 5; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        const uint32_t* a = &w[i + 1];
        if (op == 71 && n >= 3) {
            if (a[1] == 34) set[a[0]] = a[2];
            if (a[1] == 33) binding[a[0]] = a[2];
            if (a[1] == 30) location[a[0]] = a[2];
            if (a[1] == 11) builtin.insert(a[0]);
            if (a[1] == 3) bufferBlock.insert(a[0]);
        }
        if (op >= 19 && op <= 32 && n >= 2) { std::vector<uint32_t> t{op}; t.insert(t.end(), a + 1, a + n - 1); types[a[0]] = t; }
        if (op == 43 && n >= 4) constant[a[1]] = a[2];
        if (op == 59 && n >= 4) vars.push_back({a[0], a[1], a[2]});
        i += n;
    }
    auto format = [&](uint32_t t) {
        uint32_t count = 1;
        if (types[t][0] == 23) { count = types[t][2]; t = types[t][1]; }
        const bool isFloat = types[t][0] == 22, isSigned = types[t][0] == 21 && types[t][2];
        static const VkFormat f[3][4]{{VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT},
                                      {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT},
                                      {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT}};
        return f[isFloat ? 0 : isSigned ? 1 : 2][count - 1];
    };
    Interface out;
    for (auto [pointer, id, storage] : vars) {
        uint32_t t = types[pointer][2];   // OpTypePointer: storage class, pointee
        if (storage == 9) { out.push = true; continue; }
        if (storage == 1 && location.count(id) && !builtin.count(id)) out.inputs[location[id]] = format(t);
        if (storage == 3 && location.count(id) && !builtin.count(id))
            out.outputs[location[id]] = format(t) == VK_FORMAT_R32G32B32A32_SFLOAT ? VK_FORMAT_R8G8B8A8_UNORM : format(t);
        if (!binding.count(id)) continue;
        uint32_t count = 1;
        if (types[t][0] == 28) { count = constant[types[t][2]]; t = types[t][1]; }
        else if (types[t][0] == 29) { count = 64; t = types[t][1]; }
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        switch (types[t][0]) {
        case 26: type = VK_DESCRIPTOR_TYPE_SAMPLER; break;
        case 27: type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
        case 25: {   // image: sampled type, dim, depth, arrayed, ms, sampled
            const bool buffer = types[t][2] == 5, storageImage = types[t][6] == 2;
            type = buffer ? (storageImage ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
                          : (storageImage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            break;
        }
        default: type = storage == 12 || bufferBlock.count(t) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
        out.descriptors[{set.count(id) ? set[id] : 0, binding[id]}] = {type, count};   // (a 2D and a shadow view share a binding)
    }
    return out;
}

int main() {
    setenv("__GL_SHADER_DISK_CACHE", "0", 1);   // NVIDIA: compile for real every run
    setenv("DEPTH_PROBE_LAYER_LOG", "/tmp/depth-probe-soft-shadows-smoke.log", 1);
    auto loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    CHECK(loader);
    if (!loader) return 1;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    VkInstance instance{};
#define I(api) reinterpret_cast<PFN_##api>(gipa(instance, #api))
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    const bool validate = std::getenv("DEPTH_PROBE_SMOKE_VALIDATION");
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* debugExtension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    debug.pfnUserCallback = validation;
    if (validate) { ii.pNext = &debug; ii.enabledLayerCount = 1; ii.ppEnabledLayerNames = &layer; ii.enabledExtensionCount = 1; ii.ppEnabledExtensionNames = &debugExtension; }
    if (I(vkCreateInstance)(&ii, nullptr, &instance) != VK_SUCCESS) { std::puts("SKIP: no Vulkan instance"); return 0; }
    uint32_t count = 0;   // the discrete GPU (the game's) when there is one
    I(vkEnumeratePhysicalDevices)(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    I(vkEnumeratePhysicalDevices)(instance, &count, gpus.data());
    CHECK(count);
    if (!count) return 1;
    VkPhysicalDevice gpu = gpus[0];
    VkPhysicalDeviceProperties properties{};
    for (auto g : gpus) {
        I(vkGetPhysicalDeviceProperties)(g, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu = g; break; }
    }
    I(vkGetPhysicalDeviceProperties)(gpu, &properties);
    // Every feature the device has (the game's shaders need demote-to-helper and descriptor indexing, as DXVK enables them),
    // and inline shader code in a stage's chain (maintenance5 or graphics pipeline libraries, as DXVK uses it).
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    uint32_t extensionCount = 0;
    I(vkEnumerateDeviceExtensionProperties)(gpu, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    I(vkEnumerateDeviceExtensionProperties)(gpu, nullptr, &extensionCount, available.data());
    auto has = [&](const char* name) { for (auto& e : available) if (!std::strcmp(e.extensionName, name)) return true; return false; };
    std::vector<const char*> extensions;
    features.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13;
    void** tail = &f13.pNext;
    if (has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) { extensions.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME); *tail = &m5; tail = &m5.pNext; }
    else if (has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        extensions.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        *tail = &gpl; tail = &gpl.pNext;
    }
    CHECK(!extensions.empty());
    I(vkGetPhysicalDeviceFeatures2)(gpu, &features);
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &features;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = uint32_t(extensions.size());
    di.ppEnabledExtensionNames = extensions.data();
    VkDevice device{};
    CHECK(I(vkCreateDevice)(gpu, &di, nullptr, &device) == VK_SUCCESS);
    CHECK(f13.shaderDemoteToHelperInvocation && f12.runtimeDescriptorArray);
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
#define D(api) reinterpret_cast<PFN_##api>(gdpa(device, #api))

    const auto vs = load("wow_terrain_vs_44c15e1a.spv");
    const depth_probe::discovery::SoftShadowSettings soft{1.f, .5f};
    struct Case { const char* name; const char* file; } cases[]{{"terrain", "wow_terrain_fs_9e7279ff.spv"}, {"model", "wow_model_fs_ba3c7f84.spv"},
                                                                {"indoor", "wow_indoor_fs_17cef8ff.spv"}};
    std::string report;
    for (const auto& c : cases) {
        if (validate && std::strcmp(c.name, "terrain")) continue;
        const auto fs = load(c.file);
        // The layout: both stages' descriptors, per set, every stage.
        auto vi = reflect(vs), fi = reflect(fs);
        std::map<uint32_t, std::vector<VkDescriptorSetLayoutBinding>> sets;
        auto all = vi.descriptors;
        all.insert(fi.descriptors.begin(), fi.descriptors.end());
        for (auto [key, value] : all) sets[key.first].push_back({key.second, value.first, value.second, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
        std::vector<VkDescriptorSetLayout> layouts(sets.empty() ? 0 : sets.rbegin()->first + 1);
        for (uint32_t s = 0; s < layouts.size(); ++s) {
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = uint32_t(sets[s].size());
            li.pBindings = sets[s].data();
            CHECK(D(vkCreateDescriptorSetLayout)(device, &li, nullptr, &layouts[s]) == VK_SUCCESS);
        }
        VkPushConstantRange push{VK_SHADER_STAGE_ALL_GRAPHICS, 0, std::min(properties.limits.maxPushConstantsSize, 256u)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = uint32_t(layouts.size());
        pli.pSetLayouts = layouts.data();
        pli.pushConstantRangeCount = vi.push || fi.push ? 1 : 0;
        pli.pPushConstantRanges = &push;
        VkPipelineLayout layout{};
        CHECK(D(vkCreatePipelineLayout)(device, &pli, nullptr, &layout) == VK_SUCCESS);
        // Fixed state: the vertex shader's inputs from one buffer, the fragment shader's outputs as colour attachments.
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (auto [loc, format] : vi.inputs) attributes.push_back({loc, 0, format, 0});
        VkVertexInputBindingDescription vertexBinding{0, 64, VK_VERTEX_INPUT_RATE_VERTEX};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &vertexBinding;
        vertexInput.vertexAttributeDescriptionCount = uint32_t(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        std::vector<VkFormat> colorFormats;
        for (uint32_t loc = 0; !fi.outputs.empty() && loc <= fi.outputs.rbegin()->first; ++loc)
            colorFormats.push_back(fi.outputs.count(loc) ? fi.outputs[loc] : VK_FORMAT_UNDEFINED);
        std::vector<VkPipelineColorBlendAttachmentState> blends(colorFormats.size());
        for (auto& b : blends) b.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = uint32_t(blends.size());
        blend.pAttachments = blends.data();
        const VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamics;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = uint32_t(colorFormats.size());
        rendering.pColorAttachmentFormats = colorFormats.data();
        // DXVK's way: no modules, the code inline in each stage's chain.
        VkShaderModuleCreateInfo vsCode{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}, fsCode{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        vsCode.codeSize = vs.size() * 4; vsCode.pCode = vs.data();
        fsCode.codeSize = fs.size() * 4; fsCode.pCode = fs.data();
        VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].pNext = &vsCode; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].pName = "main";
        stages[1].pNext = &fsCode; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &rendering;
        gi.stageCount = 2; gi.pStages = stages;
        gi.pVertexInputState = &vertexInput; gi.pInputAssemblyState = &assembly; gi.pViewportState = &viewport;
        gi.pRasterizationState = &raster; gi.pMultisampleState = &multisample; gi.pColorBlendState = &blend; gi.pDynamicState = &dynamic;
        gi.layout = layout;
        depth_probe::discovery::SoftShadowPipelines patched(1, &gi, soft);
        CHECK(patched.infos() && patched.infos()->pStages != gi.pStages);   // the game's own arrays are not touched
        CHECK(stages[1].pNext == &fsCode && fsCode.pCode == fs.data());
        if (!patched.infos()) continue;
        const auto* patchedCode = static_cast<const VkShaderModuleCreateInfo*>(patched.infos()->pStages[1].pNext);
        CHECK(patchedCode->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && patchedCode->codeSize > fsCode.codeSize);
        CHECK(patched.infos()->pStages[0].pNext == &vsCode);   // the vertex stage as it was
        double ms[2]{};
        for (int which = 0; which < 2; ++which) {
            const auto start = std::chrono::steady_clock::now();
            VkPipeline pipeline{};
            const VkResult r = D(vkCreateGraphicsPipelines)(device, VK_NULL_HANDLE, 1, which ? patched.infos() : &gi, nullptr, &pipeline);
            ms[which] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (r != VK_SUCCESS) std::fprintf(stderr, "%s %s: vkCreateGraphicsPipelines = %d\n", c.name, which ? "soft" : "original", int(r));
            CHECK(r == VK_SUCCESS && pipeline);
            if (pipeline) D(vkDestroyPipeline)(device, pipeline, nullptr);
        }
        char line[200];
        std::snprintf(line, sizeof line, "%s%s %.0f ms -> %.0f ms (%zu -> %zu words)", report.empty() ? "" : ", ", c.name, ms[0], ms[1], fs.size(), patchedCode->codeSize / 4);
        report += line;
        D(vkDestroyPipelineLayout)(device, layout, nullptr);
        for (auto l : layouts) D(vkDestroyDescriptorSetLayout)(device, l, nullptr);
    }
    const auto stats = depth_probe::discovery::soft_shadow_stats();
    CHECK(stats.shaders == (validate ? 1u : 3u) && stats.taps == (validate ? 24u : 63u));
    D(vkDestroyDevice)(device, nullptr);
    I(vkDestroyInstance)(instance, nullptr);
    if (validate && errors) std::fprintf(stderr, "%u validation errors\n", errors.load());
    if (failures || (validate && errors)) return 1;
    std::printf("PASS: the game's shaders with soft shadows compile on %s (%s) - compile %s%s\n", properties.deviceName,
                extensions[0], report.c_str(), validate ? "; no validation errors" : "");
    return 0;
}
