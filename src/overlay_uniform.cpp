#include "overlay_uniform.hpp"
#include "overlay_internal.hpp"
#include "overlay_menu.hpp"
#include "pipeline_discovery.hpp"
#include "surface_inspector.hpp"
#include "scene_debugger.hpp"

namespace depth_probe {
bool create_overlay_uniforms(const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st) {
    if (!data.get_memory_properties || !data.physical_device) return false;
    VkPhysicalDeviceMemoryProperties properties{};
    data.get_memory_properties(data.physical_device, &properties);
    st.uniforms.resize(st.colorImages.size());
    for (auto& uniform : st.uniforms) {
        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = sizeof(OverlayFrameData);
        buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult r = f.createBuffer(data.device, &buffer, nullptr, &uniform.buffer);
        if (r != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        f.getBufferMemoryRequirements(data.device, uniform.buffer, &requirements);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            const auto flags = properties.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                type = i;
                if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
            }
        }
        if (type == UINT32_MAX) return false;
        uniform.coherent = properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        r = f.allocateMemory(data.device, &allocation, nullptr, &uniform.memory);
        if (r != VK_SUCCESS) return false;
        r = f.bindBufferMemory(data.device, uniform.buffer, uniform.memory, 0);
        if (r != VK_SUCCESS) return false;
        r = f.mapMemory(data.device, uniform.memory, 0, VK_WHOLE_SIZE, 0, &uniform.mapped);
        if (r != VK_SUCCESS) return false;
    }
    return true;
}
void destroy_overlay_uniforms(const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st) {
    for (auto& uniform : st.uniforms) {
        if (uniform.mapped) f.unmapMemory(data.device, uniform.memory);
        if (uniform.buffer) f.destroyBuffer(data.device, uniform.buffer, nullptr);
        if (uniform.memory) f.freeMemory(data.device, uniform.memory, nullptr);
    }
    st.uniforms.clear();
}
VkResult write_overlay_uniform(const DeviceData& data, const DepthOverlayFns& f,
    const MappedUniform& uniform, const OverlayFrameData& value) {
    if (!uniform.mapped) return VK_ERROR_MEMORY_MAP_FAILED;
    std::memcpy(uniform.mapped, &value, sizeof(value));
    if (uniform.coherent) return VK_SUCCESS;
    // Each image owns an entire allocation. Mapping and flushing offset 0 through
    // allocation end satisfies nonCoherentAtomSize, without sharing atoms across frames.
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = uniform.memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    return f.flushMappedMemoryRanges(data.device, 1, &range);
}
VkResult update_overlay_uniform(const DeviceData& data, const DepthOverlayFns& f,
    DepthOverlayState& st, uint32_t imageIndex) {
    if (imageIndex >= st.uniforms.size()) return VK_ERROR_INITIALIZATION_FAILED;
    const auto timing=collect_gpu_timing(data,f,st,imageIndex);
    if(timing!=VK_SUCCESS)return timing;
    prepare_shader_frame(data, f, st, imageIndex);
    poll_settings_reload();
    inspector_context(data.device,st);
    poll_menu_input(st.width, st.height);
    const auto& menu = overlay_menu();
    OverlayFrameData value{};
    value.uniform.settings = overlay_settings();
    if (g_perf_effect_off.load(std::memory_order_relaxed)) value.uniform.settings.effectEnabled = 0;   // PERFORMANCE TEST phase
    value.uniform.heartbeat = current_heartbeat(st.width, st.height);
    value.uniform.viewport[0] = static_cast<float>(st.width);
    value.uniform.viewport[1] = static_cast<float>(st.height);
    value.uniform.viewport[3] = !st.sceneColor.empty();
    value.uniform.viewport[2] = menu_scale(st.width, st.height);
    value.uniform.menu[0] = menu.visible;
    value.uniform.menu[1] = menu.selected;
    value.uniform.menu[2] = menu.status;
    value.uniform.menu[3] = menu_input_available();
    value.uniform.pointer[0] = menu.pointerX;
    value.uniform.pointer[1] = menu.pointerY;
    value.uniform.pointer[2] = menu.pointerValid && menu_input_available();
    populate_menu_uniform(value.uniform);
    populate_diagnostics(data,st,value.uniform);
    populate_discovery_menu(data.device,value.uniform);
    populate_inspector_menu(data.device,value.uniform);
    const auto inspectorResult=update_inspector_gpu(data,f,st,imageIndex,value.uniform);
    if(inspectorResult!=VK_SUCCESS)return inspectorResult;
    populate_inspector_menu(data.device,value.uniform);
    // Camera Telemetry HUD (F9) / 3D gizmo (F8): independent of menu.visible
    // on purpose - both keep working after F10 closes. See scene_debugger.hpp.
    update_camera_telemetry(data.device,value.uniform,uint64_t(st.depthImage));
    value.previewDraw = {value.uniform.pick[3] ? 3u : 0u,1,0,0};
    value.pickDraw = {value.uniform.pick[2] ? 3u : 0u,1,0,0};
    value.menuDraw = {menu.visible ? 6u : 0u, 1, 0, 0};
    value.hudDraw = {value.uniform.hud[0] ? 6u : 0u, 1, 6, 0};
    // camera gizmo (24 vertices), then the light gizmo's 4 markers x 40 (shaders/gizmo.vert); either may be off
    value.gizmoDraw = {value.uniform.gizmoAnchor[3] != 0.f || value.uniform.lightGizmo[0] != 0.f || value.uniform.lightGizmo[1] != 0.f ? 24u + 4u * 40u + 16u * 40u : 0u, 1, 0, 0};
    return write_overlay_uniform(data, f, st.uniforms[imageIndex], value);
}
} // namespace depth_probe
