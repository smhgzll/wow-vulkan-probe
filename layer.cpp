#include "src/draw_probe.hpp"
#include "src/pipeline_discovery.hpp"
#include "src/sync_capabilities.hpp"
#include "src/layer_state.hpp"
#include "src/overlay_renderer.hpp"
#include "src/depth_tracking.hpp"

using namespace depth_probe;

extern "C" {

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char* pName);

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device, const char* pName);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance);

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice);

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView);


LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue);

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device,
    const VkDeviceQueueInfo2* pQueueInfo,
    VkQueue* pQueue);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain);

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo);

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* pVersionStruct) {

    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    pVersionStruct->loaderLayerInterfaceVersion =
        std::min(pVersionStruct->loaderLayerInterfaceVersion,
                 static_cast<uint32_t>(CURRENT_LOADER_LAYER_INTERFACE_VERSION));

    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;

    // We don't wrap VkInstance and don't implement unknown physical-device extension commands.
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }

    return VK_SUCCESS;
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {

    auto* chainInfo = find_loader_create_info<VkLayerInstanceCreateInfo>(
        pCreateInfo ? pCreateInfo->pNext : nullptr,
        VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
        VK_LAYER_LINK_INFO);

    if (!chainInfo || !chainInfo->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr nextGIPA =
        chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    PFN_vkCreateInstance nextCreateInstance =
        reinterpret_cast<PFN_vkCreateInstance>(
            nextGIPA(VK_NULL_HANDLE, "vkCreateInstance"));

    if (!nextCreateInstance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the loader's layer link before calling the next layer/driver.
    chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;

    SyncInstanceCreate syncCreate(*pCreateInfo, nextGIPA);
    VkResult result = nextCreateInstance(&syncCreate.info, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    InstanceData data{};
    data.instance = *pInstance;
    data.surfaceMaintenance = syncCreate.surfaceMaintenance;
    data.apiVersion = pCreateInfo->pApplicationInfo ? pCreateInfo->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    data.next_gipa = nextGIPA;
    data.next_destroy_instance =
        reinterpret_cast<PFN_vkDestroyInstance>(
            nextGIPA(*pInstance, "vkDestroyInstance"));
    data.next_enumerate_physical_devices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            nextGIPA(*pInstance, "vkEnumeratePhysicalDevices"));
    data.next_enumerate_physical_device_groups =
        reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(
            nextGIPA(*pInstance, "vkEnumeratePhysicalDeviceGroups"));

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_instances[dispatch_key(*pInstance)] = data;
    }

    std::ostringstream os;
    os << "[layer-loaded] " << kLayerName
       << " pid=" << getpid()
       << " log=" << log_path();

    if (pCreateInfo && pCreateInfo->pApplicationInfo) {
        const auto* app = pCreateInfo->pApplicationInfo;
        os << " app=\"" << (app->pApplicationName ? app->pApplicationName : "?") << "\""
           << " engine=\"" << (app->pEngineName ? app->pEngineName : "?") << "\""
           << " api=" << api_version_string(app->apiVersion);
    }

    log_line(os.str());
    return VK_SUCCESS;
}

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {

    auto data = get_instance_data(instance);
    if (!data) return;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);

        for (auto it = g_physical_to_instance.begin(); it != g_physical_to_instance.end();) {
            if (it->second == instance) {
                it = g_physical_to_instance.erase(it);
            } else {
                ++it;
            }
        }

        g_instances.erase(dispatch_key(instance));
    }

    if (data->next_destroy_instance) {
        data->next_destroy_instance(instance, pAllocator);
    }
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices) {

    auto data = get_instance_data(instance);
    if (!data || !data->next_enumerate_physical_devices) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = data->next_enumerate_physical_devices(
        instance, pPhysicalDeviceCount, pPhysicalDevices);

    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
        pPhysicalDeviceCount && pPhysicalDevices) {
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; ++i) {
            remember_physical_device(instance, pPhysicalDevices[i], data->next_gipa);
        }
    }

    return result;
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {

    auto data = get_instance_data(instance);
    if (!data || !data->next_enumerate_physical_device_groups) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = data->next_enumerate_physical_device_groups(
        instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);

    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
        pPhysicalDeviceGroupCount && pPhysicalDeviceGroupProperties) {

        for (uint32_t g = 0; g < *pPhysicalDeviceGroupCount; ++g) {
            const auto& group = pPhysicalDeviceGroupProperties[g];
            for (uint32_t i = 0; i < group.physicalDeviceCount; ++i) {
                remember_physical_device(instance, group.physicalDevices[i], data->next_gipa);
            }
        }
    }

    return result;
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {

    auto* chainInfo = find_loader_create_info<VkLayerDeviceCreateInfo>(
        pCreateInfo ? pCreateInfo->pNext : nullptr,
        VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
        VK_LAYER_LINK_INFO);

    if (!chainInfo || !chainInfo->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr nextGIPA =
        chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr nextGDPA =
        chainInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    VkInstance instance = instance_for_physical_device(physicalDevice).value_or(VkInstance{});

    PFN_vkCreateDevice nextCreateDevice =
        reinterpret_cast<PFN_vkCreateDevice>(
            nextGIPA(instance, "vkCreateDevice"));

    // Some loader paths tolerate NULL here; use it only as a fallback.
    if (!nextCreateDevice && instance != VK_NULL_HANDLE) {
        nextCreateDevice =
            reinterpret_cast<PFN_vkCreateDevice>(
                nextGIPA(VK_NULL_HANDLE, "vkCreateDevice"));
    }

    if (!nextCreateDevice) {
        log_line("[error] could not resolve next vkCreateDevice");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const auto* loaderData = find_loader_create_info<VkLayerDeviceCreateInfo>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
        VK_LOADER_DATA_CALLBACK);
    const PFN_vkSetDeviceLoaderData setLoaderData = loaderData
        ? loaderData->u.pfnSetDeviceLoaderData : nullptr;

    // Advance the device layer link before calling down.
    chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;

    const auto instanceData = get_instance_data(instance).value_or(InstanceData{});
    SyncDeviceCreate syncCreate(*pCreateInfo, physicalDevice, instanceData);
    VkResult result = nextCreateDevice(
        physicalDevice, &syncCreate.info, pAllocator, pDevice);

    if (result != VK_SUCCESS) return result;

    DeviceData data{};
    data.device = *pDevice;
    data.sync = std::make_shared<SyncDeviceState>();
    data.sync->maintenance = syncCreate.maintenance;
    data.sync->singleGraphicsQueue = single_graphics_queue(*pCreateInfo, physicalDevice, instanceData);
    log_line("[OVERLAY-SYNC-SUPPORT] maintenance=" + std::to_string(data.sync->maintenance) +
        " singleGraphicsQueueFamily0=" + std::to_string(data.sync->singleGraphicsQueue) +
        " depthContract=attachment-writes-on-graphics-queue; acquire-and-render-signals-checked-at-present");
    data.physical_device = physicalDevice;
    data.get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(nextGIPA(instance,"vkGetPhysicalDeviceProperties"));
    data.get_queue_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(nextGIPA(instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
    data.get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        nextGIPA(instance, "vkGetPhysicalDeviceMemoryProperties"));
    data.get_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        nextGIPA(instance, "vkGetPhysicalDeviceFormatProperties"));
    data.get_image_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
        nextGIPA(instance, "vkGetPhysicalDeviceImageFormatProperties"));
    data.get_image_format_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        nextGIPA(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
    if (!data.get_image_format_properties2)
        data.get_image_format_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            nextGIPA(instance, "vkGetPhysicalDeviceImageFormatProperties2KHR"));
    data.next_gdpa = nextGDPA;
    data.set_device_loader_data = setLoaderData;
    data.next_destroy_device =
        reinterpret_cast<PFN_vkDestroyDevice>(
            nextGDPA(*pDevice, "vkDestroyDevice"));
    data.next_create_image =
        reinterpret_cast<PFN_vkCreateImage>(
            nextGDPA(*pDevice, "vkCreateImage"));

    data.next_destroy_image = reinterpret_cast<PFN_vkDestroyImage>(
        nextGDPA(*pDevice, "vkDestroyImage"));
    data.next_destroy_image_view = reinterpret_cast<PFN_vkDestroyImageView>(
        nextGDPA(*pDevice, "vkDestroyImageView"));

    data.next_create_image_view =
        reinterpret_cast<PFN_vkCreateImageView>(
            nextGDPA(*pDevice, "vkCreateImageView"));

    data.next_get_device_queue =
        reinterpret_cast<PFN_vkGetDeviceQueue>(
            nextGDPA(*pDevice, "vkGetDeviceQueue"));

    data.next_get_device_queue2 =
        reinterpret_cast<PFN_vkGetDeviceQueue2>(
            nextGDPA(*pDevice, "vkGetDeviceQueue2"));

    data.next_create_swapchain =
        reinterpret_cast<PFN_vkCreateSwapchainKHR>(
            nextGDPA(*pDevice, "vkCreateSwapchainKHR"));

    data.next_destroy_swapchain =
        reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            nextGDPA(*pDevice, "vkDestroySwapchainKHR"));

    data.next_get_swapchain_images =
        reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
            nextGDPA(*pDevice, "vkGetSwapchainImagesKHR"));

    data.next_queue_present =
        reinterpret_cast<PFN_vkQueuePresentKHR>(
            nextGDPA(*pDevice, "vkQueuePresentKHR"));

    data.next_create_command_pool =
        reinterpret_cast<PFN_vkCreateCommandPool>(
            nextGDPA(*pDevice, "vkCreateCommandPool"));

    data.next_destroy_command_pool =
        reinterpret_cast<PFN_vkDestroyCommandPool>(
            nextGDPA(*pDevice, "vkDestroyCommandPool"));

    data.next_allocate_command_buffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(
            nextGDPA(*pDevice, "vkAllocateCommandBuffers"));

    data.next_begin_command_buffer =
        reinterpret_cast<PFN_vkBeginCommandBuffer>(
            nextGDPA(*pDevice, "vkBeginCommandBuffer"));

    data.next_end_command_buffer =
        reinterpret_cast<PFN_vkEndCommandBuffer>(
            nextGDPA(*pDevice, "vkEndCommandBuffer"));

    data.next_queue_submit =
        reinterpret_cast<PFN_vkQueueSubmit>(
            nextGDPA(*pDevice, "vkQueueSubmit"));

    data.next_queue_wait_idle =
        reinterpret_cast<PFN_vkQueueWaitIdle>(
            nextGDPA(*pDevice, "vkQueueWaitIdle"));

    data.next_cmd_pipeline_barrier =
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(
            nextGDPA(*pDevice, "vkCmdPipelineBarrier"));

    data.next_cmd_pipeline_barrier2 =
        reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
            nextGDPA(*pDevice, "vkCmdPipelineBarrier2"));

    data.next_cmd_clear_color_image =
        reinterpret_cast<PFN_vkCmdClearColorImage>(
            nextGDPA(*pDevice, "vkCmdClearColorImage"));

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_devices[dispatch_key(*pDevice)] = data;
    }
    depth_probe::invalidate_hook_cache();

    log_line("[device-created] vkCreateImage interception active");
    return VK_SUCCESS;
}

LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);

    auto data = get_device_data(device);
    if (!data) return;

    destroy_device_probe(device);
    destroy_device_discovery(device);
    destroy_device_depth_overlays(*data);
    destroy_device_depth_views(*data);
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        std::erase_if(g_swapchains, [&](const auto& item) {
            return item.first.device == device;
        });
        g_devices.erase(dispatch_key(device));
    }
    depth_probe::invalidate_hook_cache();

    if (data->next_destroy_device) {
        data->next_destroy_device(device, pAllocator);
    }
}


LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName) {

    if (!pName) return nullptr;
    if (auto hook = discovery_hook(pName)) return hook;
    if (auto hook = sync_hook(pName)) return hook;
    if (auto hook = probe_hook(pName)) return hook;

    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance);
    if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDevices);
    if (std::strcmp(pName, "vkEnumeratePhysicalDeviceGroups") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDeviceGroups);
    if (std::strcmp(pName, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
    if (std::strcmp(pName, "vkCreateImage") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage);
    if (std::strcmp(pName, "vkDestroyImage") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImage);
    if (std::strcmp(pName, "vkCreateImageView") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR);
    if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetSwapchainImagesKHR);
    if (std::strcmp(pName, "vkCmdPipelineBarrier") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier);
    if (std::strcmp(pName, "vkCmdPipelineBarrier2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR);

    if (instance == VK_NULL_HANDLE) return nullptr;

    auto data = get_instance_data(instance);
    if (!data || !data->next_gipa) return nullptr;

    return data->next_gipa(instance, pName);
}

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName) {

    if (!pName) return nullptr;
    if (auto hook = discovery_hook(pName)) {
        // Do not advertise a new optional/core-version entry point merely
        // because the observer knows its signature. The driver still owns
        // availability for this particular device and enabled API/extensions.
        auto data = get_device_data(device);
        return data && data->next_gdpa && data->next_gdpa(device, pName) ? hook : nullptr;
    }
    if (auto hook = sync_hook(pName)) return hook;
    if (auto hook = probe_hook(pName)) return hook;

    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
    if (std::strcmp(pName, "vkCreateImage") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage);
    if (std::strcmp(pName, "vkDestroyImage") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImage);
    if (std::strcmp(pName, "vkCreateImageView") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR);
    if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetSwapchainImagesKHR);
    if (std::strcmp(pName, "vkCmdPipelineBarrier") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier);
    if (std::strcmp(pName, "vkCmdPipelineBarrier2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR);

    if (device == VK_NULL_HANDLE) return nullptr;

    auto data = get_device_data(device);
    if (!data || !data->next_gdpa) return nullptr;

    return data->next_gdpa(device, pName);
}

} // extern "C"
