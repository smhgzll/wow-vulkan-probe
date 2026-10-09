#include "overlay_internal.hpp"
#include "shader_resources.hpp"
#include <sys/stat.h>
#include <array>

namespace depth_probe {
struct PipelineGuard {
    const DeviceData& data;
    const DepthOverlayFns& f;
    VkPipeline effect{}, menu{}, gizmo{};
    bool committed = false;
    ~PipelineGuard() {
        if (!committed) {
            if (effect) f.destroyPipeline(data.device, effect, nullptr);
            if (menu) f.destroyPipeline(data.device, menu, nullptr);
            if (gizmo) f.destroyPipeline(data.device, gizmo, nullptr);
        }
    }
};
bool create_overlay_pipelines(const DeviceData& data, const DepthOverlayFns& f,
    const DepthOverlayState& st, VkPipeline& effect, VkPipeline& menu, VkPipeline& gizmo) {
    PipelineGuard replacement{data, f};
    VkResult r;
    const std::string dir = depth_shader_dir();

    const auto vertCode =
        read_spirv_file(dir + "/depth.vert.spv");

    const auto fragCode =
        read_spirv_file(dir + "/depth.frag.spv");

    if (vertCode.empty() ||
        fragCode.empty()) {
        log_line(
            "[DEPTH-OVERLAY-ERROR] SPIR-V dosyalari okunamadi dir=" +
            dir);
        return false;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType =
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

    VkShaderModule vert = VkShaderModule{};
    VkShaderModule frag = VkShaderModule{};

    smInfo.codeSize =
        vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();

    r =
        f.createShaderModule(
            data.device,
            &smInfo,
            nullptr,
            &vert);

    if (r != VK_SUCCESS) {
        log_line("[DEPTH-OVERLAY-ERROR] vertex shader module");
        return false;
    }

    smInfo.codeSize =
        fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();

    r =
        f.createShaderModule(
            data.device,
            &smInfo,
            nullptr,
            &frag);

    if (r != VK_SUCCESS) {
        f.destroyShaderModule(
            data.device,
            vert,
            nullptr);

        log_line("[DEPTH-OVERLAY-ERROR] fragment shader module");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};

    stages[0].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";

    stages[1].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(st.width);
    viewport.height = static_cast<float>(st.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {st.width, st.height};

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};

    cba.blendEnable = VK_TRUE;

    cba.srcColorBlendFactor =
        VK_BLEND_FACTOR_SRC_ALPHA;

    cba.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    cba.colorBlendOp =
        VK_BLEND_OP_ADD;

    cba.srcAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE;

    cba.dstAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    cba.alphaBlendOp =
        VK_BLEND_OP_ADD;

    cba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType =
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.layout = st.pipelineLayout;
    gp.renderPass = st.renderPass;
    gp.subpass = 0;

    r =
        f.createGraphicsPipelines(
            data.device,
            VkPipelineCache{},
            1,
            &gp,
            nullptr,
            &replacement.effect);

    f.destroyShaderModule(
        data.device,
        vert,
        nullptr);

    f.destroyShaderModule(
        data.device,
        frag,
        nullptr);

    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "[DEPTH-OVERLAY-ERROR] vkCreateGraphicsPipelines result="
           << static_cast<int>(r);
        log_line(os.str());
        return false;
    }

    // Retained UI: same pass/layout/blend state, fixed command recording.
    // Visibility and controls come from the fence-protected mapped buffer.
    const auto menuVertCode = read_spirv_file(dir + "/menu.vert.spv");
    const auto menuFragCode = read_spirv_file(dir + "/menu.frag.spv");
    if (menuVertCode.empty() || menuFragCode.empty()) {
        log_line("[UI-ERROR] menu SPIR-V unavailable dir=" + dir);
        return false;
    }
    vert = {}; frag = {};
    smInfo.codeSize = menuVertCode.size() * sizeof(uint32_t);
    smInfo.pCode = menuVertCode.data();
    if (f.createShaderModule(data.device, &smInfo, nullptr, &vert) != VK_SUCCESS) return false;
    smInfo.codeSize = menuFragCode.size() * sizeof(uint32_t);
    smInfo.pCode = menuFragCode.data();
    r = f.createShaderModule(data.device, &smInfo, nullptr, &frag);
    if (r == VK_SUCCESS) {
        stages[0].module = vert;
        stages[1].module = frag;
        r = f.createGraphicsPipelines(data.device, {}, 1, &gp, nullptr, &replacement.menu);
    }
    if (vert) f.destroyShaderModule(data.device, vert, nullptr);
    if (frag) f.destroyShaderModule(data.device, frag, nullptr);
    if (r != VK_SUCCESS) return false;

    // 3D camera gizmo (F8): same render target/layout/descriptor set as the
    // menu, but LINE_LIST topology and its own shaders - a genuinely
    // different primitive shape, not something the quad-based menu pipeline
    // can produce. No depth test in this first version (see docs/inspector.md):
    // always drawn on top, simplest option, matches the explicit "acceptable
    // for v1" instruction.
    const auto gizmoVertCode = read_spirv_file(dir + "/gizmo.vert.spv");
    const auto gizmoFragCode = read_spirv_file(dir + "/gizmo.frag.spv");
    if (gizmoVertCode.empty() || gizmoFragCode.empty()) {
        log_line("[UI-ERROR] gizmo SPIR-V unavailable dir=" + dir);
        return false;
    }
    vert = {}; frag = {};
    smInfo.codeSize = gizmoVertCode.size() * sizeof(uint32_t);
    smInfo.pCode = gizmoVertCode.data();
    if (f.createShaderModule(data.device, &smInfo, nullptr, &vert) != VK_SUCCESS) return false;
    smInfo.codeSize = gizmoFragCode.size() * sizeof(uint32_t);
    smInfo.pCode = gizmoFragCode.data();
    r = f.createShaderModule(data.device, &smInfo, nullptr, &frag);
    if (r == VK_SUCCESS) {
        stages[0].module = vert;
        stages[1].module = frag;
        auto gizmoIa = ia; gizmoIa.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        auto gizmoRs = rs; gizmoRs.lineWidth = 1.0f; // portable without VK_DYNAMIC_STATE_LINE_WIDTH/wideLines
        auto gizmoCba = cba; gizmoCba.blendEnable = VK_FALSE; // solid lines, simplest for v1
        VkPipelineColorBlendStateCreateInfo gizmoCb = cb; gizmoCb.pAttachments = &gizmoCba;
        auto gizmoGp = gp; gizmoGp.pInputAssemblyState = &gizmoIa; gizmoGp.pRasterizationState = &gizmoRs; gizmoGp.pColorBlendState = &gizmoCb;
        r = f.createGraphicsPipelines(data.device, {}, 1, &gizmoGp, nullptr, &replacement.gizmo);
    }
    if (vert) f.destroyShaderModule(data.device, vert, nullptr);
    if (frag) f.destroyShaderModule(data.device, frag, nullptr);
    if (r != VK_SUCCESS) return false;
    log_line("[UI-READY] renderer=minimal-vulkan retained=1");

    effect = replacement.effect; menu = replacement.menu; gizmo = replacement.gizmo;
    replacement.committed = true;
    return true;
}

static std::array<ShaderStamp, 6> shader_stamps() {
    std::array<ShaderStamp, 6> stamps{};
    const auto dir = depth_shader_dir();
    const char* names[] = {"depth.vert.spv", "depth.frag.spv", "menu.vert.spv", "menu.frag.spv", "gizmo.vert.spv", "gizmo.frag.spv"};
    for (size_t i = 0; i < stamps.size(); ++i) {
        struct stat info{};
        if (!stat((dir + "/" + names[i]).c_str(), &info))
            stamps[i] = {int64_t(info.st_mtim.tv_sec), int64_t(info.st_mtim.tv_nsec),
                         uint64_t(info.st_size), uint64_t(info.st_ino)};
    }
    return stamps;
}
void initialize_shader_reload(DepthOverlayState& st) {
    const char* flag = std::getenv("DEPTH_PROBE_SHADER_HOT_RELOAD");
    st.shaderReloadEnabled = flag && std::strcmp(flag, "1") == 0;
    if (st.shaderReloadEnabled) st.shaderStamps = shader_stamps();
    st.recordedGeneration.resize(st.colorImages.size());
}
// Called only after this image's existing submit/present fence waits. Other
// images retain old executable recordings and may still be in flight.
void prepare_shader_frame(const DeviceData& data, const DepthOverlayFns& f,
    DepthOverlayState& st, uint32_t index) {
    const auto now = std::chrono::steady_clock::now();
    if (st.shaderReloadEnabled && now >= st.shaderPollAfter) {
        st.shaderPollAfter = now + std::chrono::milliseconds(500);
        const auto stamps = shader_stamps();
        if (stamps != st.shaderStamps) {
            st.shaderStamps = stamps; // One attempt per change, including invalid files.
            log_line("[SHADER-RELOAD-DETECTED]");
            VkPipeline effect{}, menu{}, gizmo{};
            // Bound memory even when an application stops acquiring an old image.
            if (st.retiredPipelines.size() < 8 && create_overlay_pipelines(data, f, st, effect, menu, gizmo)) {
                st.retiredPipelines.push_back({st.pipeline, st.menuPipeline, st.gizmoPipeline, st.pipelineGeneration});
                st.pipeline = effect; st.menuPipeline = menu; st.gizmoPipeline = gizmo; ++st.pipelineGeneration;
                st.shaderReloadStatus = 1;
                log_line("[SHADER-RELOAD-SUCCESS] generation=" + std::to_string(st.pipelineGeneration));
            } else {
                st.shaderReloadStatus = 2;
                log_line("[SHADER-RELOAD-FAILED] previous pipeline retained; check SPIR-V/ABI or retained generation limit");
            }
        }
    }
    if (st.recordedGeneration[index] != st.pipelineGeneration) st.commandRecorded[index] = false;
    for (auto it = st.retiredPipelines.begin(); it != st.retiredPipelines.end();) {
        if (std::find(st.recordedGeneration.begin(), st.recordedGeneration.end(), it->generation)
                == st.recordedGeneration.end()) {
            f.destroyPipeline(data.device, it->effect, nullptr);
            f.destroyPipeline(data.device, it->menu, nullptr);
            f.destroyPipeline(data.device, it->gizmo, nullptr);
            it = st.retiredPipelines.erase(it);
        } else ++it;
    }
}
} // namespace depth_probe
