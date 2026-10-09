#include "game_soft_shadows.hpp"

#include "discovery_internal.hpp"
#include "layer_state.hpp"
#include "spirv_soft_shadows.hpp"

#include <atomic>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

namespace depth_probe::discovery {
namespace {
struct Shared {
    std::mutex lock;
    bool latched = false, refused = false;
    SoftShadowSettings active;
    std::map<std::tuple<uint64_t, size_t, float, float>, std::shared_ptr<const SoftShadowShader>> cache;   // null: not a receiver
    uint32_t shaders = 0, taps = 0, liveShaders = 0;
    // Live settings (soft_shadows_present) and what the record-time writes did since the last log.
    std::atomic<float> size{-1}, fade{0}, fadeDistance{10}, indoor{1};
    std::atomic<uint64_t> written{0}, unmapped{0};
    uint64_t nextLog = 0;
    float loggedSize = -1, loggedFade = -1, loggedDistance = -1, loggedIndoor = -1;
    uint32_t indoorShaders = 0;
};
Shared& shared() { static Shared s; return s; }
uint64_t fnv(const uint32_t* w, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= w[i]; h *= 1099511628211ull; }
    return h;
}
}  // namespace

SoftShadowSettings soft_shadows_latch(const std::function<SoftShadowSettings()>& ask) {
    auto& s = shared();
    {
        std::lock_guard l(s.lock);
        if (s.latched) return s.active;
    }
    const auto wanted = ask();   // (outside this lock: it takes discovery's)
    std::lock_guard l(s.lock);
    if (!s.latched) {
        s.latched = true;
        s.active = wanted;
        if (wanted.size > 0 || wanted.indoor < 1)
            log_line("[SOFT-SHADOWS] on for this game run: size " + std::to_string(wanted.size) + ", fade " + std::to_string(wanted.fade) +
                 ", indoor shadows " + std::to_string(wanted.indoor));
    }
    return s.active;
}

std::shared_ptr<const SoftShadowShader> soft_shadow_code(const uint32_t* code, size_t bytes, SoftShadowSettings set) {
    if ((set.size <= 0 && set.indoor >= 1) || !code || bytes < 20 || bytes % 4) return nullptr;
    auto& s = shared();
    const size_t n = bytes / 4;
    const auto key = std::make_tuple(fnv(code, n), n, set.size, set.fade + 10.f * set.indoor);
    {
        std::lock_guard l(s.lock);
        if (s.refused) return nullptr;
        if (auto it = s.cache.find(key); it != s.cache.end()) return it->second;
    }
    softshadow::Params p;   // (patch leaves anything but a fragment shader sampling 4 cascades alone)
    p.size = set.size;
    p.fade = set.fade;
    p.indoorStrength = set.indoor;
    auto r = softshadow::patch(code, n, p);
    std::shared_ptr<const SoftShadowShader> result;
    if (!r.code.empty()) result = std::make_shared<const SoftShadowShader>(SoftShadowShader{std::move(r.code), r.live, r.liveSet, r.liveBinding});
    std::lock_guard l(s.lock);
    auto [it, fresh] = s.cache.emplace(key, result);
    if (fresh && result) {
        ++s.shaders;
        s.taps += r.taps;
        s.liveShaders += r.live;
        s.indoorShaders += r.indoor;
        char line[200];
        std::snprintf(line, sizeof line, "[SOFT-SHADOWS] shader %016llx: %u %s taps rewritten (%zu -> %zu words); %u shaders so far; ",
                      static_cast<unsigned long long>(std::get<0>(key)), r.taps, r.indoor ? "indoor shadow" : "cascade", n, result->code.size(), s.shaders);
        log_line(line + (r.live ? "live settings at set " + std::to_string(r.liveSet) + " binding " + std::to_string(r.liveBinding)
                                : "fixed settings (" + r.liveWhy + ")"));
    }
    return it->second;
}

void soft_shadows_refused(VkResult result) {
    auto& s = shared();
    std::lock_guard l(s.lock);
    if (!s.refused) log_line("[SOFT-SHADOWS] the driver refused a patched pipeline (VkResult " + std::to_string(int(result)) + "): the game's own shadows from now on");
    s.refused = true;
}

SoftShadowStats soft_shadow_stats() {
    auto& s = shared();
    std::lock_guard l(s.lock);
    return {s.shaders, s.taps, s.liveShaders, s.refused, s.active, s.latched};
}

void soft_shadows_present(float size, float fade, float fadeDistance, float indoor, uint64_t frame) {
    auto& s = shared();
    s.size = size;
    s.fade = fade;
    s.fadeDistance = fadeDistance;
    s.indoor = indoor;
    std::lock_guard l(s.lock);
    if (!s.latched || !s.shaders) return;
    const bool changed = size != s.loggedSize || fade != s.loggedFade || fadeDistance != s.loggedDistance || indoor != s.loggedIndoor;
    if (!changed && frame < s.nextLog) return;
    char line[240];
    std::snprintf(line, sizeof line, "[SOFT-SHADOWS] live: size %.2f, fade %.2f over %.0f yd, indoor shadows %.2f (%u indoor shaders); written for %llu draws (%llu without DXVK's shared pixel data) since the last line",
                  double(size), double(fade), double(fadeDistance), double(indoor), s.indoorShaders, static_cast<unsigned long long>(s.written.exchange(0)),
                  static_cast<unsigned long long>(s.unmapped.exchange(0)));
    log_line(line);
    s.loggedSize = size; s.loggedFade = fade; s.loggedDistance = fadeDistance; s.loggedIndoor = indoor;
    s.nextLog = frame + 900;
}

void soft_shadows_observe_draw(Device& d, const Command& c) {
    if (!c.pipeline || c.pipeline->softShadowSet < 0) return;
    auto& s = shared();
    const float size = s.size.load(std::memory_order_relaxed);
    if (size < 0) return;   // no settings yet: the baked ones
    uint64_t bytes = 0;
    uint8_t* p = bound_uniform_span(d, c, uint32_t(c.pipeline->softShadowSet), c.pipeline->softShadowBinding, bytes);   // its D3D9SharedPS
    if (!p || bytes < softshadow::liveOffset + 16) { s.unmapped.fetch_add(1, std::memory_order_relaxed); return; }
    static_assert(softshadow::liveIndoorOffset + 8 == softshadow::liveOffset);
    // indoor strength, (unused), magic, fade distance, size, fade - stage 7's BumpEnvMat, LScale, LOffset
    const float values[6]{s.indoor.load(std::memory_order_relaxed), 0.f, softshadow::liveMagic, s.fadeDistance.load(std::memory_order_relaxed), size,
                          s.fade.load(std::memory_order_relaxed)};
    if (std::memcmp(p + softshadow::liveIndoorOffset, values, sizeof values)) std::memcpy(p + softshadow::liveIndoorOffset, values, sizeof values);
    s.written.fetch_add(1, std::memory_order_relaxed);
}

SoftShadowPipelines::SoftShadowPipelines(uint32_t count, const VkGraphicsPipelineCreateInfo* infos, SoftShadowSettings set) {
    if (set.size <= 0 || !count || !infos) return;
    copies.assign(infos, infos + count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& info = infos[i];
        std::vector<VkPipelineShaderStageCreateInfo>* copied = nullptr;
        for (uint32_t k = 0; k < info.stageCount; ++k) {
            const auto& stage = info.pStages[k];
            if (stage.stage != VK_SHADER_STAGE_FRAGMENT_BIT || stage.module || !stage.pNext) continue;
            const auto* inlineCode = static_cast<const VkShaderModuleCreateInfo*>(stage.pNext);
            if (inlineCode->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;   // (a module identifier: patched in its own hook)
            auto patchedCode = soft_shadow_code(inlineCode->pCode, inlineCode->codeSize, set);
            if (!patchedCode) continue;
            if (!copied) {
                copied = &stages.emplace_back(info.pStages, info.pStages + info.stageCount);
                copies[i].pStages = copied->data();
            }
            auto& module = modules.emplace_back(*inlineCode);
            module.pCode = patchedCode->code.data();
            module.codeSize = patchedCode->code.size() * 4;
            (*copied)[k].pNext = &module;
            code.push_back(std::move(patchedCode));
            patched = true;
        }
    }
}

}  // namespace depth_probe::discovery
