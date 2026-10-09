#include "depth_tracking.hpp"

namespace depth_probe {
std::recursive_mutex g_depth_lifecycle_mutex;
DeviceHandleMap<DepthCandidate> g_depth_candidates;

namespace {
struct SelectionState {
    VkExtent2D extent{};
    std::optional<SceneDepthChoice> choice;
};
DeviceHandleMap<SelectionState> selections;

bool supported_depth_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
        default: return false;
    }
}

void log_changed(const DeviceHandleKey& key, const std::optional<SceneDepthChoice>& oldChoice,
                 const std::optional<SceneDepthChoice>& newChoice, const char* reason) {
    std::ostringstream log;
    log << "[SCENE-DEPTH-CHANGED] device=" << key.device
        << " swapchain=0x" << std::hex << key.handle
        << " old=0x" << (oldChoice ? image_handle_u64(oldChoice->candidate.image) : 0)
        << " new=0x" << (newChoice ? image_handle_u64(newChoice->candidate.image) : 0)
        << std::dec << " oldGeneration=" << (oldChoice ? oldChoice->candidate.generation : 0)
        << " newGeneration=" << (newChoice ? newChoice->candidate.generation : 0)
        << " reason=\"" << reason << '"';
    log_line(log.str());
}
} // namespace

VkImageAspectFlags depth_barrier_aspects(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_DEPTH_BIT;
    }
}

DepthPreparation prepare_depth_candidate(const DeviceData& data, const VkImageCreateInfo& info) {
    // Deliberately narrow: no transient, aliasing, protected, sparse, MSAA, arrays,
    // mip chains, external memory, DRM modifiers or unknown image extensions.
    constexpr VkImageCreateFlags allowedFlags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                                VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    constexpr VkImageUsageFlags allowedUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (!(info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) || !supported_depth_format(info.format))
        return {false, false, "not a supported depth attachment format"};
    if (info.imageType != VK_IMAGE_TYPE_2D || info.extent.depth != 1 ||
        info.samples != VK_SAMPLE_COUNT_1_BIT || info.mipLevels != 1 || info.arrayLayers != 1)
        return {false, false, "requires 2D, one sample, one mip and one layer"};
    if (info.extent.width < 64 || info.extent.height < 64 ||
        info.extent.width > 16384 || info.extent.height > 16384)
        return {false, false, "extent outside 64..16384 preparation range"};
    if (info.tiling != VK_IMAGE_TILING_OPTIMAL || (info.usage & ~allowedUsage) || (info.flags & ~allowedFlags))
        return {false, false, "unsupported tiling, usage or create flags"};

    // These structures are valid in both VkImageCreateInfo and the capability
    // query. Never discard an unknown pNext and pretend its support was checked.
    for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const auto& list = *reinterpret_cast<const VkImageFormatListCreateInfo*>(next);
            if (list.viewFormatCount && (!list.pViewFormats ||
                std::find(list.pViewFormats, list.pViewFormats + list.viewFormatCount, info.format) ==
                    list.pViewFormats + list.viewFormatCount))
                return {false, false, "view format list excludes native depth format"};
        } else if (next->sType == VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO) {
            const auto& stencil = *reinterpret_cast<const VkImageStencilUsageCreateInfo*>(next);
            if (stencil.stencilUsage & ~allowedUsage)
                return {false, false, "unsupported stencil usage"};
        } else {
            return {false, false, "unsupported image pNext"};
        }
    }

    if (!data.physical_device || !data.get_format_properties)
        return {true, false, "format query unavailable"};
    VkFormatProperties features{};
    data.get_format_properties(data.physical_device, info.format, &features);
    constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((features.optimalTilingFeatures & needed) != needed)
        return {true, false, "format lacks sampled/depth attachment support"};

    const VkImageUsageFlags usage = info.usage | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageFormatProperties limits{};
    VkResult result = VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (data.get_image_format_properties2) {
        VkPhysicalDeviceImageFormatInfo2 query{};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        query.pNext = info.pNext;
        query.format = info.format;
        query.type = info.imageType;
        query.tiling = info.tiling;
        query.usage = usage;
        query.flags = info.flags;
        VkImageFormatProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        result = data.get_image_format_properties2(data.physical_device, &query, &properties);
        limits = properties.imageFormatProperties;
    } else if (!info.pNext && data.get_image_format_properties) {
        result = data.get_image_format_properties(data.physical_device, info.format,
            info.imageType, info.tiling, usage, info.flags, &limits);
    }
    if (result != VK_SUCCESS || info.extent.width > limits.maxExtent.width ||
        info.extent.height > limits.maxExtent.height || info.extent.depth > limits.maxExtent.depth ||
        info.mipLevels > limits.maxMipLevels || info.arrayLayers > limits.maxArrayLayers ||
        !(limits.sampleCounts & info.samples))
        return {true, false, "sampled usage combination/extent unsupported"};
    return {true, true, "eligible 2D single-sample depth attachment"};
}

int score_scene_depth_candidate(const DepthCandidate& c, VkDevice device, VkExtent2D extent) {
    // Exact extent is mandatory, never just a bonus allowing a shadow fallback.
    if (c.device != device || c.extent.width != extent.width || c.extent.height != extent.height ||
        c.extent.depth != 1 || c.samples != VK_SAMPLE_COUNT_1_BIT ||
        c.mipLevels != 1 || c.arrayLayers != 1 ||
        !(c.originalUsage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        !supported_depth_format(c.format) || !c.sampledSupported || !c.sampledReady ||
        c.image == VkImage{} || c.depthView == VkImageView{})
        return -1;
    // All readiness conditions are gates. No speculative format/activity bonus:
    // equally ready exact matches tie and keep the oldest surviving generation.
    return 100;
}

std::optional<SceneDepthChoice> select_scene_depth_candidate(
    const DeviceHandleMap<DepthCandidate>& candidates, VkDevice device, VkExtent2D extent) {
    std::optional<SceneDepthChoice> best;
    uint32_t count = 0;
    for (const auto& [key, candidate] : candidates) {
        if (key.device != device) continue;
        const int score = score_scene_depth_candidate(candidate, device, extent);
        if (score < 0) continue;
        ++count;
        if (!best || score > best->score || (score == best->score &&
            (candidate.generation < best->candidate.generation ||
             (candidate.generation == best->candidate.generation &&
              image_handle_u64(candidate.image) < image_handle_u64(best->candidate.image)))))
            best = SceneDepthChoice{candidate, score, 0};
    }
    if (best) best->matchingCandidates = count;
    return best;
}

std::optional<SceneDepthChoice> resolve_scene_depth(VkDevice device, VkSwapchainKHR swapchain) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const DeviceHandleKey key{device, swapchain_handle_u64(swapchain)};
    const auto sc = g_swapchains.find(key);
    if (sc == g_swapchains.end() || sc->second.retired) return std::nullopt;
    const VkExtent2D extent{sc->second.width, sc->second.height};
    auto choice = select_scene_depth_candidate(g_depth_candidates, device, extent);
    const auto previous = selections.find(key);
    const bool first = previous == selections.end();
    const bool resized = !first && (previous->second.extent.width != extent.width ||
                                   previous->second.extent.height != extent.height);
    const auto oldChoice = first ? std::optional<SceneDepthChoice>{} : previous->second.choice;
    const bool changed = oldChoice.has_value() != choice.has_value() ||
        (oldChoice && choice && (oldChoice->candidate.generation != choice->candidate.generation ||
                                oldChoice->candidate.depthView != choice->candidate.depthView));
    const uint32_t oldCount = oldChoice ? oldChoice->matchingCandidates : 0;
    const uint32_t newCount = choice ? choice->matchingCandidates : 0;
    if (changed && !first) log_changed(key, oldChoice, choice, "candidate selection");
    if (newCount > 1 && (first || changed || resized || oldCount != newCount)) {
        std::ostringstream log;
        log << "[SCENE-DEPTH-CANDIDATES] device=" << device << " swapchain=0x"
            << std::hex << key.handle << std::dec << " count=" << newCount
            << " policy=oldest-generation-then-handle";
        log_line(log.str());
    }
    if (first || changed || resized) {
        std::ostringstream log;
        log << (choice ? "[SCENE-DEPTH-SELECTED]" : "[SCENE-DEPTH-NOT-FOUND]")
            << " device=" << device << " swapchain=0x" << std::hex << key.handle << std::dec
            << " extent=" << extent.width << 'x' << extent.height;
        if (choice) {
            const auto& c = choice->candidate;
            log << " image=0x" << std::hex << image_handle_u64(c.image)
                << " view=0x" << image_view_handle_u64(c.depthView) << std::dec
                << " generation=" << c.generation << " format=" << format_name(c.format)
                << " score=" << choice->score
                << " reason=\"exact extent, single sample, sampled-ready depth view\"";
        }
        log_line(log.str());
    }
    selections[key] = {extent, choice};
    return choice;
}

void invalidate_scene_depth_swapchain(VkDevice device, VkSwapchainKHR swapchain, const char* reason) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const DeviceHandleKey key{device, swapchain_handle_u64(swapchain)};
    const auto it = selections.find(key);
    if (it != selections.end()) {
        if (it->second.choice) log_changed(key, it->second.choice, std::nullopt, reason);
        selections.erase(it);
    }
}

void invalidate_scene_depth_image(VkDevice device, VkImage image) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    for (auto it = selections.begin(); it != selections.end();) {
        const auto& [key, state] = *it;
        if (key.device == device && state.choice && state.choice->candidate.image == image) {
            log_changed(key, state.choice, std::nullopt, "depth image destroyed");
            it = selections.erase(it);
        } else ++it;
    }
}

void invalidate_scene_depth_device(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lifetime(g_depth_lifecycle_mutex);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    std::erase_if(selections, [&](const auto& item) { return item.first.device == device; });
}

bool should_log_depth_barrier(VkDevice device, VkImage image) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const auto it = g_depth_candidates.find({device, image_handle_u64(image)});
    if (it == g_depth_candidates.end() || it->second.barrierLogs >= 4) return false;
    ++it->second.barrierLogs;
    return true;
}
} // namespace depth_probe
