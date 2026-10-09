#pragma once
// Soft, fading sun shadows in WoW's own pixel shaders (fog.ini soft_shadows / shadow_fade, docs/fog.md): every shadow-receiving
// fragment shader DXVK hands the driver is rewritten (src/spirv_soft_shadows.hpp) before it is compiled - inline code in a
// pipeline's stage, a shader module, and the identifier DXVK asks for (so a pipeline looked up by identifier is the patched
// one, or is compiled again from the patched code). Discovery keeps recording the game's original code and hashes.
// The settings are taken once, at the first shader the game builds: a change takes effect at the next game start.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace depth_probe::discovery {

struct SoftShadowSettings { float size = 0, fade = 0, indoor = 1; };   // indoor: the game's indoor shadows kept (1 = all)

// The settings in force for this game run: wanted() (fog.ini now) is asked once, at the first call, and kept.
SoftShadowSettings soft_shadows_latch(const std::function<SoftShadowSettings()>& wanted);
// A fragment shader with soft shadows: its code and where it reads the live settings (live: DXVK's shared pixel data there).
struct SoftShadowShader {
    std::vector<uint32_t> code;
    bool live = false;
    uint32_t set = 0, binding = 0;
};
// The patched shader for one that receives the game's sun shadows (cached per code), or null: off, not a receiver, or the
// driver refused a patched pipeline once (then never again this run).
std::shared_ptr<const SoftShadowShader> soft_shadow_code(const uint32_t* code, size_t bytes, SoftShadowSettings);
void soft_shadows_refused(VkResult);
struct SoftShadowStats { uint32_t shaders = 0, taps = 0, liveShaders = 0; bool refused = false; SoftShadowSettings active; bool latched = false; };
SoftShadowStats soft_shadow_stats();

// Live settings (fog.ini soft_shadows / shadow_fade / shadow_fade_distance / indoor_shadows, read every frame): patched shaders take them from
// DXVK's shared pixel data (spirv_soft_shadows.hpp liveOffset), written at record time for every draw of such a pipeline.
// Only a run that started with soft_shadows > 0 has patched shaders; size 0 then gives the game's own shadows back.
void soft_shadows_present(float size, float fade, float fadeDistance, float indoor, uint64_t frame);
struct Device;
struct Command;
void soft_shadows_observe_draw(Device&, const Command&);

// Copies of vkCreateGraphicsPipelines' create infos with the patched fragment code (inline SPIR-V, the first node of a stage's
// chain); infos() is null when nothing was patched.
class SoftShadowPipelines {
public:
    SoftShadowPipelines(uint32_t count, const VkGraphicsPipelineCreateInfo* infos, SoftShadowSettings);
    const VkGraphicsPipelineCreateInfo* infos() const { return patched ? copies.data() : nullptr; }
private:
    std::vector<VkGraphicsPipelineCreateInfo> copies;
    std::deque<std::vector<VkPipelineShaderStageCreateInfo>> stages;
    std::deque<VkShaderModuleCreateInfo> modules;
    std::vector<std::shared_ptr<const SoftShadowShader>> code;
    bool patched = false;
};

}  // namespace depth_probe::discovery
