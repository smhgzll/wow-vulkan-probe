// Opt-in real-loader smoke test. No window or game process is required.
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <atomic>
#include <cstring>

static void require(VkResult result, const char* name) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "%s failed: %d\n", name, result);
        std::exit(1);
    }
}
template <typename T> static T function(PFN_vkVoidFunction fn, const char* name) {
    if (!fn) { std::fprintf(stderr, "missing %s\n", name); std::exit(1); }
    return reinterpret_cast<T>(fn);
}
static std::atomic<unsigned> validationErrors{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    std::fprintf(stderr, "validation: %s\n", data->pMessage);
    return VK_FALSE;
}
int main() {
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    if (!gipa) return 1;
#define INSTANCE(name) function<PFN_##name>(gipa(instance, #name), #name)
    VkInstance instance{};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "depth-probe-loader-smoke";
    app.apiVersion = VK_API_VERSION_1_3;
    const bool validation = std::getenv("DEPTH_PROBE_SMOKE_VALIDATION") != nullptr;
    const char* layers[]{"VK_LAYER_depth_probe", "VK_LAYER_KHRONOS_validation"};
    std::vector<const char*> extensions{VK_KHR_SURFACE_EXTENSION_NAME};
    if (validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    // Request advertised surface dependencies explicitly too, so a downstream
    // validation layer's global enumeration does not mask them during negotiation.
    auto enumerateExtensions = function<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa({}, "vkEnumerateInstanceExtensionProperties"), "enumerate instance extensions");
    uint32_t extensionCount = 0;
    require(enumerateExtensions(nullptr, &extensionCount, nullptr), "count instance extensions");
    std::vector<VkExtensionProperties> advertised(extensionCount);
    require(enumerateExtensions(nullptr, &extensionCount, advertised.data()), "instance extensions");
    for (const char* name : {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME})
        for (const auto& property : advertised)
            if (!std::strcmp(property.extensionName, name)) { extensions.push_back(name); break; }
    VkValidationFeatureEnableEXT syncValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &syncValidation;
    VkDebugUtilsMessengerCreateInfoEXT debug{};
    debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    debug.pfnUserCallback = validationMessage;
    validationFeatures.pNext = &debug;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = validation ? 2 : 1; ci.ppEnabledLayerNames = layers;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size()); ci.ppEnabledExtensionNames = extensions.data();
    ci.pNext = validation ? &validationFeatures : nullptr;
    require(INSTANCE(vkCreateInstance)(&ci, nullptr, &instance), "vkCreateInstance");
    VkDebugUtilsMessengerEXT messenger{};
    if (validation) require(INSTANCE(vkCreateDebugUtilsMessengerEXT)(instance, &debug, nullptr, &messenger), "create debug messenger");
    uint32_t count = 0;
    auto enumerate = INSTANCE(vkEnumeratePhysicalDevices);
    require(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (!count) return 1;
    std::vector<VkPhysicalDevice> physicals(count);
    require(enumerate(instance, &count, physicals.data()), "vkEnumeratePhysicalDevices");
    auto physical = physicals.front();
    VkPhysicalDeviceProperties properties{};
    INSTANCE(vkGetPhysicalDeviceProperties)(physical, &properties);
    INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties)(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties)(physical, &count, queues.data());
    uint32_t family = 0;
    while (family < count && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == count) return 1;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = &swapchainExtension;
    VkDevice device{};
    require(INSTANCE(vkCreateDevice)(physical, &di, nullptr, &device), "vkCreateDevice");
    auto gdpa = INSTANCE(vkGetDeviceProcAddr);
#define DEVICE(name) function<PFN_##name>(gdpa(device, #name), #name)
    VkQueue queue{};
    DEVICE(vkGetDeviceQueue)(device, family, 0, &queue);
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_D24_UNORM_S8_UINT;
    ii.extent = {1920, 1080, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkPhysicalDeviceMemoryProperties memory{};
    INSTANCE(vkGetPhysicalDeviceMemoryProperties)(physical, &memory);
    // Exercise preparation before any swapchain exists at multiple resolutions,
    // including a potential shadow target. No WSI or draw activity is inferred.
    const VkExtent3D extents[] = {{1920, 1080, 1}, {2560, 1440, 1}, {1280, 720, 1}, {2048, 2048, 1}};
    VkImageStencilUsageCreateInfo stencil{};
    stencil.sType = VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO;
    stencil.stencilUsage = ii.usage;
    VkImageFormatListCreateInfo formatList{};
    formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    formatList.pNext = &stencil;
    formatList.viewFormatCount = 1; formatList.pViewFormats = &ii.format;
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
        ii.extent = extents[iteration];
        ii.pNext = iteration % 2 ? &formatList : nullptr;
        ii.flags = iteration % 2 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0;
        VkImage image{};
        require(DEVICE(vkCreateImage)(device, &ii, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements requirements{};
        DEVICE(vkGetImageMemoryRequirements)(device, image, &requirements);
        uint32_t type = 0;
        while (type < memory.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type))) ++type;
        if (type == memory.memoryTypeCount) return 1;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = requirements.size; ai.memoryTypeIndex = type;
        VkDeviceMemory allocation{};
        require(DEVICE(vkAllocateMemory)(device, &ai, nullptr, &allocation), "vkAllocateMemory");
        require(DEVICE(vkBindImageMemory)(device, image, allocation, 0), "vkBindImageMemory");
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        VkImageView view{};
        require(DEVICE(vkCreateImageView)(device, &vi, nullptr, &view), "vkCreateImageView");
        DEVICE(vkDestroyImageView)(device, view, nullptr);
        DEVICE(vkDestroyImage)(device, image, nullptr);
        DEVICE(vkFreeMemory)(device, allocation, nullptr);
    }
    // Real GPU semaphore/fence chain, deliberately without a presentation engine.
    // The last submit consumes B; its fence proves this test's semaphore reuse.
    // It does NOT stand in for vkQueuePresentKHR or a WSI completion fence.
    VkCommandPool pool{};
    VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = family; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    require(DEVICE(vkCreateCommandPool)(device, &pi, nullptr, &pool), "create sync command pool");
    VkCommandBuffer commands[3]{};
    VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 3;
    require(DEVICE(vkAllocateCommandBuffers)(device, &cai, commands), "allocate sync commands");
    VkSemaphore original[3][3]{}, finished[3]{};
    VkFence submitted[3]{}, consumed[3]{};
    VkSemaphoreCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (unsigned slot = 0; slot < 3; ++slot) {
        for (auto& sem : original[slot]) require(DEVICE(vkCreateSemaphore)(device, &sci, nullptr, &sem), "create original semaphore");
        require(DEVICE(vkCreateSemaphore)(device, &sci, nullptr, &finished[slot]), "create completion semaphore");
        require(DEVICE(vkCreateFence)(device, &fci, nullptr, &submitted[slot]), "create submit fence");
        require(DEVICE(vkCreateFence)(device, &fci, nullptr, &consumed[slot]), "create consume fence");
    }
    for (unsigned frame = 0; frame < 30; ++frame) {
        const auto slot = frame % 3;
        if (frame >= 3) {
            const VkFence fences[]{submitted[slot], consumed[slot]};
            require(DEVICE(vkWaitForFences)(device, 2, fences, VK_TRUE, UINT64_MAX), "wait reused slot");
            require(DEVICE(vkResetFences)(device, 2, fences), "reset reused slot");
        }
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require(DEVICE(vkBeginCommandBuffer)(commands[slot], &bi), "begin sync command");
        require(DEVICE(vkEndCommandBuffer)(commands[slot]), "end sync command");
        VkSubmitInfo render{}; render.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        render.signalSemaphoreCount = frame % 2 ? 3 : 1; render.pSignalSemaphores = original[slot];
        require(DEVICE(vkQueueSubmit)(queue, 1, &render, {}), "submit producer signals");
        VkPipelineStageFlags stages[3]{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        VkSubmitInfo overlay{}; overlay.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        overlay.waitSemaphoreCount = render.signalSemaphoreCount; overlay.pWaitSemaphores = original[slot]; overlay.pWaitDstStageMask = stages;
        overlay.commandBufferCount = 1; overlay.pCommandBuffers = &commands[slot];
        overlay.signalSemaphoreCount = 1; overlay.pSignalSemaphores = &finished[slot];
        require(DEVICE(vkQueueSubmit)(queue, 1, &overlay, submitted[slot]), "submit wait-A signal-B");
        VkSubmitInfo consumer{}; consumer.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        consumer.waitSemaphoreCount = 1; consumer.pWaitSemaphores = &finished[slot]; consumer.pWaitDstStageMask = stages;
        require(DEVICE(vkQueueSubmit)(queue, 1, &consumer, consumed[slot]), "submit consume-B");
    }
    for (unsigned slot = 0; slot < 3; ++slot) {
        const VkFence fences[]{submitted[slot], consumed[slot]};
        require(DEVICE(vkWaitForFences)(device, 2, fences, VK_TRUE, UINT64_MAX), "wait final sync slot");
        DEVICE(vkDestroyFence)(device, submitted[slot], nullptr);
        DEVICE(vkDestroyFence)(device, consumed[slot], nullptr);
        for (auto sem : original[slot]) DEVICE(vkDestroySemaphore)(device, sem, nullptr);
        DEVICE(vkDestroySemaphore)(device, finished[slot], nullptr);
    }
    DEVICE(vkDestroyCommandPool)(device, pool, nullptr);
    DEVICE(vkDestroyDevice)(device, nullptr);
    if (messenger) INSTANCE(vkDestroyDebugUtilsMessengerEXT)(instance, messenger, nullptr);
    INSTANCE(vkDestroyInstance)(instance, nullptr);
    if (validationErrors) return 1;
    std::printf("PASS: %zu-bit real Vulkan loader, %s, 4 depth extents/lifecycles including format-list + stencil-usage pNext\n", sizeof(void*) * 8, properties.deviceName);
    std::printf("PASS: 30 real GPU semaphore/fence cycles across 3 slots; validation=%s; no WSI/present engine\n", validation ? "enabled" : "disabled");
    dlclose(loader);
}
