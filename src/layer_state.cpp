#include "layer_state.hpp"
#include "depth_tracking.hpp"

namespace depth_probe {

std::mutex g_state_mutex;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, DeviceData> g_devices;
std::unordered_map<VkPhysicalDevice, VkInstance> g_physical_to_instance;

DeviceHandleMap<SwapchainInfo> g_swapchains;
std::atomic<uint64_t> g_present_counter{0};

std::mutex g_log_mutex;

bool initialize_layer_command_buffer(const DeviceData& data, VkCommandBuffer cmd) {
    // Allocating through the next layer bypasses the loader trampoline.
    // See Vulkan-Loader/docs/LoaderLayerInterface.md, Creating New Dispatchable Objects.
    if (data.set_device_loader_data) {
        const VkResult result = data.set_device_loader_data(data.device, cmd);
        if (result != VK_SUCCESS) {
            log_line("[OVERLAY-ERROR] vkSetDeviceLoaderData result=" + std::to_string(result));
            return false;
        }
    } else {
        // Documented fallback for loaders predating the callback.
        *reinterpret_cast<void**>(cmd) = dispatch_key(data.device);
    }
    return true;
}

std::string log_path() {
    static std::string path = [] {
        if (const char* custom = std::getenv("DEPTH_PROBE_LAYER_LOG"); custom && *custom) {
            return std::string(custom);
        }
        return std::string("/tmp/depth-probe-") + std::to_string(getpid()) + ".log";
    }();
    return path;
}

void log_line(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream f(log_path(), std::ios::app);
    if (f) {
        f << text << '\n';
        f.flush();
    }
}

uint64_t image_handle_u64(VkImage image) {
#if VK_USE_64_BIT_PTR_DEFINES
    return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(image));
#else
    return static_cast<uint64_t>(image);
#endif
}

uint64_t image_view_handle_u64(VkImageView view) {
#if VK_USE_64_BIT_PTR_DEFINES
    return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(view));
#else
    return static_cast<uint64_t>(view);
#endif
}


uint64_t queue_handle_u64(VkQueue queue) {
    return static_cast<uint64_t>(
        reinterpret_cast<std::uintptr_t>(queue));
}

uint64_t swapchain_handle_u64(VkSwapchainKHR swapchain) {
#if VK_USE_64_BIT_PTR_DEFINES
    return static_cast<uint64_t>(
        reinterpret_cast<std::uintptr_t>(swapchain));
#else
    return static_cast<uint64_t>(swapchain);
#endif
}


const char* layout_name(VkImageLayout l) {
    switch (l) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return "SHADER_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return "TRANSFER_SRC_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return "TRANSFER_DST_OPTIMAL";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            return "PRESENT_SRC_KHR";
#ifdef VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            return "DEPTH_ATTACHMENT_OPTIMAL";
#endif
#ifdef VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
            return "DEPTH_READ_ONLY_OPTIMAL";
#endif
        default: return "OTHER";
    }
}

bool is_target_depth(VkDevice device, VkImage image) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_depth_candidates.count(
        {device, image_handle_u64(image)}) != 0;
}

std::string aspect_string(VkImageAspectFlags aspect) {
    std::ostringstream os;
    bool first = true;

    auto add = [&](VkImageAspectFlagBits bit, const char* name) {
        if (aspect & bit) {
            if (!first) os << '|';
            os << name;
            first = false;
        }
    };

    add(VK_IMAGE_ASPECT_COLOR_BIT, "COLOR");
    add(VK_IMAGE_ASPECT_DEPTH_BIT, "DEPTH");
    add(VK_IMAGE_ASPECT_STENCIL_BIT, "STENCIL");

    if (first) os << "0";

    os << " (0x" << std::hex
       << static_cast<uint32_t>(aspect)
       << std::dec << ')';

    return os.str();
}

const char* format_name(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM: return "VK_FORMAT_D16_UNORM";
        case VK_FORMAT_X8_D24_UNORM_PACK32: return "VK_FORMAT_X8_D24_UNORM_PACK32";
        case VK_FORMAT_D32_SFLOAT: return "VK_FORMAT_D32_SFLOAT";
        case VK_FORMAT_S8_UINT: return "VK_FORMAT_S8_UINT";
        case VK_FORMAT_D16_UNORM_S8_UINT: return "VK_FORMAT_D16_UNORM_S8_UINT";
        case VK_FORMAT_D24_UNORM_S8_UINT: return "VK_FORMAT_D24_UNORM_S8_UINT";
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return "VK_FORMAT_D32_SFLOAT_S8_UINT";
        default: return nullptr;
    }
}

bool is_depth_or_stencil_format(VkFormat format) {
    return format_name(format) != nullptr;
}

std::string usage_string(VkImageUsageFlags usage) {
    struct FlagName { VkImageUsageFlagBits bit; const char* name; };
    static constexpr FlagName flags[] = {
        {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "TRANSFER_SRC"},
        {VK_IMAGE_USAGE_TRANSFER_DST_BIT, "TRANSFER_DST"},
        {VK_IMAGE_USAGE_SAMPLED_BIT, "SAMPLED"},
        {VK_IMAGE_USAGE_STORAGE_BIT, "STORAGE"},
        {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACHMENT"},
        {VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, "DEPTH_STENCIL_ATTACHMENT"},
        {VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, "TRANSIENT_ATTACHMENT"},
        {VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, "INPUT_ATTACHMENT"},
    };

    std::ostringstream os;
    bool first = true;
    for (const auto& flag : flags) {
        if (usage & flag.bit) {
            if (!first) os << '|';
            os << flag.name;
            first = false;
        }
    }
    if (first) os << "0";

    os << " (0x" << std::hex << static_cast<uint32_t>(usage) << std::dec << ')';
    return os.str();
}

std::string sample_count_string(VkSampleCountFlagBits samples) {
    switch (samples) {
        case VK_SAMPLE_COUNT_1_BIT: return "1";
        case VK_SAMPLE_COUNT_2_BIT: return "2";
        case VK_SAMPLE_COUNT_4_BIT: return "4";
        case VK_SAMPLE_COUNT_8_BIT: return "8";
        case VK_SAMPLE_COUNT_16_BIT: return "16";
        case VK_SAMPLE_COUNT_32_BIT: return "32";
        case VK_SAMPLE_COUNT_64_BIT: return "64";
        default: return "?";
    }
}

std::string api_version_string(uint32_t version) {
    std::ostringstream os;
    os << VK_API_VERSION_MAJOR(version) << '.'
       << VK_API_VERSION_MINOR(version) << '.'
       << VK_API_VERSION_PATCH(version);
    return os.str();
}

std::optional<InstanceData> get_instance_data(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_instances.find(dispatch_key(instance));
    if (it == g_instances.end()) return std::nullopt;
    return it->second;
}

std::optional<DeviceData> get_device_data(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_devices.find(dispatch_key(device));
    if (it == g_devices.end()) return std::nullopt;
    return it->second;
}

std::optional<VkInstance> instance_for_physical_device(VkPhysicalDevice physicalDevice) {
    std::lock_guard<std::mutex> lock(g_state_mutex);

    auto pit = g_physical_to_instance.find(physicalDevice);
    if (pit != g_physical_to_instance.end()) {
        return pit->second;
    }

    // On many loaders physical devices share the instance dispatch key.
    auto iit = g_instances.find(dispatch_key(physicalDevice));
    if (iit != g_instances.end()) {
        return iit->second.instance;
    }

    return std::nullopt;
}

void remember_physical_device(VkInstance instance, VkPhysicalDevice physicalDevice, PFN_vkGetInstanceProcAddr next_gipa) {
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_physical_to_instance[physicalDevice] = instance;
    }

    auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        next_gipa(instance, "vkGetPhysicalDeviceProperties"));

    if (get_props) {
        VkPhysicalDeviceProperties props{};
        get_props(physicalDevice, &props);

        std::ostringstream os;
        os << "[physical-device] name=\"" << props.deviceName
           << "\" vendor=0x" << std::hex << props.vendorID
           << " device=0x" << props.deviceID << std::dec
           << " api=" << api_version_string(props.apiVersion);
        log_line(os.str());
    }
}

bool log_all_images() {
    static bool enabled = [] {
        const char* v = std::getenv("DEPTH_PROBE_LAYER_LOG_ALL");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return enabled;
}

} // namespace depth_probe
