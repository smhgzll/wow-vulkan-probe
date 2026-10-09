#include "sync_capabilities.hpp"

namespace depth_probe {
namespace {
bool enabled(const std::vector<const char*>& names, const char* name) {
    return std::any_of(names.begin(), names.end(), [=](auto n) { return !std::strcmp(n, name); });
}
bool advertised(const std::vector<VkExtensionProperties>& names, const char* name) {
    return std::any_of(names.begin(), names.end(), [=](const auto& n) { return !std::strcmp(n.extensionName, name); });
}
bool active() {
    const char* e = std::getenv("DEPTH_PROBE_DEPTH_PRESENT");
    return e && *e && std::strcmp(e, "0");
}
}
SyncInstanceCreate::SyncInstanceCreate(const VkInstanceCreateInfo& original, PFN_vkGetInstanceProcAddr gipa)
    : info(original) {
    for (uint32_t i = 0; i < original.enabledExtensionCount; ++i) extensions.push_back(original.ppEnabledExtensionNames[i]);
    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa({}, "vkEnumerateInstanceExtensionProperties"));
    const auto api = original.pApplicationInfo ? original.pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    if (active() && api >= VK_API_VERSION_1_1 && enabled(extensions, VK_KHR_SURFACE_EXTENSION_NAME) && enumerate) {
        uint32_t count = 0;
        if (enumerate(nullptr, &count, nullptr) == VK_SUCCESS) {
            std::vector<VkExtensionProperties> properties(count);
            if (enumerate(nullptr, &count, properties.data()) == VK_SUCCESS &&
                advertised(properties, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
                advertised(properties, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
                for (const char* n : {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME})
                    if (!enabled(extensions, n)) extensions.push_back(n);
            }
        }
    }
    surfaceMaintenance = enabled(extensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
}
SyncDeviceCreate::SyncDeviceCreate(const VkDeviceCreateInfo& original, VkPhysicalDevice physical, const InstanceData& instance)
    : info(original) {
    for (uint32_t i = 0; i < original.enabledExtensionCount; ++i) extensions.push_back(original.ppEnabledExtensionNames[i]);
    for (auto* n = static_cast<const VkBaseInStructure*>(original.pNext); n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT ||
            n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR) {
            maintenance = reinterpret_cast<const VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(n)->swapchainMaintenance1 &&
                (enabled(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) ||
                 enabled(extensions, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME));
            return; // Do not override an application's explicit feature choice.
        }
    }
    if (!active() || !instance.surfaceMaintenance || instance.apiVersion < VK_API_VERSION_1_1 ||
        !enabled(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !instance.next_gipa) return;
    auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        instance.next_gipa(instance.instance, "vkEnumerateDeviceExtensionProperties"));
    auto getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        instance.next_gipa(instance.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!enumerate || !getFeatures) return;
    uint32_t count = 0;
    if (enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS) return;
    std::vector<VkExtensionProperties> properties(count);
    if (enumerate(physical, nullptr, &count, properties.data()) != VK_SUCCESS ||
        !advertised(properties, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) return;
    feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &feature;
    getFeatures(physical, &features);
    if (!feature.swapchainMaintenance1) return;
    if (!enabled(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
        extensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    feature.pNext = const_cast<void*>(original.pNext);
    info.pNext = &feature;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    maintenance = true;
}
bool single_graphics_queue(const VkDeviceCreateInfo& info, VkPhysicalDevice physical, const InstanceData& instance) {
    if (!instance.next_gipa) return false;
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        instance.next_gipa(instance.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!query) return false;
    for (auto* n = static_cast<const VkBaseInStructure*>(info.pNext); n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO &&
            reinterpret_cast<const VkDeviceGroupDeviceCreateInfo*>(n)->physicalDeviceCount != 1) return false;
    }
    uint32_t count = 0;
    query(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    query(physical, &count, families.data());
    uint32_t graphics = 0;
    for (uint32_t i = 0; i < info.queueCreateInfoCount; ++i) {
        const auto& q = info.pQueueCreateInfos[i];
        if (q.queueFamilyIndex >= count) return false;
        if (families[q.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            if (q.queueFamilyIndex != 0 || q.flags != 0) return false;
            graphics += q.queueCount;
        }
    }
    return graphics == 1;
}
} // namespace depth_probe
