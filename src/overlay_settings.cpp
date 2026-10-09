#include "overlay_settings.hpp"
#include "layer_state.hpp"
#include <charconv>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <limits>
#include <locale>
#include <cstdio>

namespace depth_probe {
namespace {
struct Field { const char* name; float OverlaySettings::* member; float low, high; };
constexpr Field fields[]{
    {"ao_strength", &OverlaySettings::aoStrength, 0, 3},
    {"ao_radius", &OverlaySettings::aoRadius, 1, 32},
    {"ao_bias", &OverlaySettings::aoBias, 0, .02f},
    {"rainbow_strength", &OverlaySettings::rainbowStrength, 0, 2},
    {"hue_frequency", &OverlaySettings::hueFrequency, .1f, 10},
    {"opacity", &OverlaySettings::opacity, 0, 1},
    {"near_plane", &OverlaySettings::nearPlane, 0, 100000},
    {"far_plane", &OverlaySettings::farPlane, 0, 1000000},
    {"vertical_fov", &OverlaySettings::verticalFov, 0, 170},
    {"ortho_height", &OverlaySettings::orthoHeight, 0, 100000},
    {"ssao_strength", &OverlaySettings::ssaoStrength, 0, 4},
    {"ssao_radius", &OverlaySettings::ssaoRadius, .001f, 100},
    {"ssao_bias", &OverlaySettings::ssaoBias, 0, 10},
    {"sharpen_strength", &OverlaySettings::sharpenStrength, 0, 2},
    {"chromatic_amount", &OverlaySettings::chromaticAmount, 0, 8},
    {"contour_frequency", &OverlaySettings::contourFrequency, 1, 500},
    {"contour_intensity", &OverlaySettings::contourIntensity, 0, 1},
    {"fog_start", &OverlaySettings::fogStart, 0, 1000000},
    {"fog_end", &OverlaySettings::fogEnd, 0, 1000000},
    {"fog_density", &OverlaySettings::fogDensity, 0, 5},
    {"fog_red", &OverlaySettings::fogR, 0, 1},
    {"fog_green", &OverlaySettings::fogG, 0, 1},
    {"fog_blue", &OverlaySettings::fogB, 0, 1},
    {"edge_strength", &OverlaySettings::edgeStrength, 0, 3},
    {"edge_width", &OverlaySettings::edgeWidth, 1, 8},
    {"ssr_step_size", &OverlaySettings::ssrStepSize, .001f, 10},
    {"ssr_thickness", &OverlaySettings::ssrThickness, .001f, 10},
    {"ssr_max_distance", &OverlaySettings::ssrMaxDistance, .1f, 1000},
    {"ssr_fade", &OverlaySettings::ssrFade, .001f, .5f},
    {"reflection_strength", &OverlaySettings::reflectionStrength, 0, 1}
};
struct UIntField { const char* name; uint32_t OverlaySettings::* member; uint32_t maximum; };
constexpr UIntField uintFields[]{
    {"effect_enabled", &OverlaySettings::effectEnabled, 1},
    {"debug_mode", &OverlaySettings::debugMode, 14},
    {"depth_convention", &OverlaySettings::depthConvention, 2},
    {"projection_mode", &OverlaySettings::projectionMode, 2},
    {"ssao_enabled", &OverlaySettings::ssaoEnabled, 1},
    {"ssao_samples", &OverlaySettings::ssaoSamples, 16},
    {"ao_blur", &OverlaySettings::aoBlur, 1},
    {"rainbow_enabled", &OverlaySettings::rainbowEnabled, 1},
    {"ao_legacy_enabled", &OverlaySettings::legacyAOEnabled, 1},
    {"sharpen_enabled", &OverlaySettings::sharpenEnabled, 1},
    {"chromatic_enabled", &OverlaySettings::chromaticEnabled, 1},
    {"fog_enabled", &OverlaySettings::fogEnabled, 1},
    {"contours_enabled", &OverlaySettings::contoursEnabled, 1},
    {"outline_enabled", &OverlaySettings::outlineEnabled, 1},
    {"gpu_timing_enabled", &OverlaySettings::gpuTimingEnabled, 1},
    {"draw_probe_capture", &OverlaySettings::drawProbeCapture, 1},
    {"ssr_enabled", &OverlaySettings::ssrEnabled, 1},
    {"ssr_steps", &OverlaySettings::ssrSteps, 32}
};
std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? std::string{} : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
template<class T> bool parse(const std::string& s, T& result) {
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), result);
    return error == std::errc{} && end == s.data() + s.size();
}
struct SettingsStore {
    std::mutex mutex;
    const std::string path = settings_config_path();
    OverlaySettings value = load_settings_file(path);
    std::filesystem::file_time_type modified{};
    std::chrono::steady_clock::time_point nextPoll{};
    SettingsStore() {
        const char* ssr=std::getenv("DEPTH_PROBE_SSR");
        if(ssr && (!std::strcmp(ssr,"0") || !std::strcmp(ssr,"1")))value.ssrEnabled=ssr[0]=='1';
        const char* timing=std::getenv("DEPTH_PROBE_GPU_TIMING");
        if(timing && !std::strcmp(timing,"1"))value.gpuTimingEnabled=1;
        const char* ssao = std::getenv("DEPTH_PROBE_SSAO");
        if (ssao && (!std::strcmp(ssao,"0") || !std::strcmp(ssao,"1"))) value.ssaoEnabled = ssao[0]=='1';
        std::error_code error;
        modified = std::filesystem::last_write_time(path, error);
    }
};
SettingsStore& store() { static SettingsStore value; return value; }
}
uint32_t setting_label(const char* key) {
    uint32_t hash = 2166136261u;
    for (; *key; ++key) hash = (hash ^ static_cast<unsigned char>(*key)) * 16777619u;
    return 1000 + (hash & 65535u);
}
const std::vector<SettingControl>& settings_controls() {
    static const auto controls = [] {
        std::vector<SettingControl> result;
        const auto add = [&](const char* name) {
            for (const auto& f : fields) if (!std::strcmp(name,f.name)) result.push_back({f.name,f.low,f.high,f.member,nullptr});
            for (const auto& f : uintFields) if (!std::strcmp(name,f.name)) result.push_back({f.name,0,float(f.maximum),nullptr,f.member});
        };
        for (const auto key : {"debug_mode", "depth_convention", "projection_mode", "near_plane", "far_plane",
                "vertical_fov", "ortho_height", "ssao_enabled", "ssao_strength", "ssao_radius", "ssao_bias",
                "ssao_samples", "ao_blur", "rainbow_enabled", "ao_legacy_enabled",
                "sharpen_enabled", "sharpen_strength", "chromatic_enabled", "chromatic_amount",
                "fog_enabled", "fog_start", "fog_end", "fog_density", "fog_red", "fog_green", "fog_blue",
                "contours_enabled", "contour_frequency", "contour_intensity", "outline_enabled", "edge_strength", "edge_width",
                "ssr_enabled", "ssr_steps", "ssr_step_size", "ssr_thickness", "ssr_max_distance", "ssr_fade", "reflection_strength"}) add(key);
        // Future optional effects join the same metadata-driven controls.
        for (const auto& f : fields) {
            if (std::any_of(result.begin(),result.end(),[&](const auto& c){return c.key==f.name;}))continue;
            if (f.member==&OverlaySettings::aoStrength || f.member==&OverlaySettings::aoRadius ||
                f.member==&OverlaySettings::aoBias || f.member==&OverlaySettings::rainbowStrength ||
                f.member==&OverlaySettings::hueFrequency || f.member==&OverlaySettings::opacity)continue;
            add(f.name);
        }
        for (const auto& f : uintFields) {
            if (f.member==&OverlaySettings::effectEnabled || std::any_of(result.begin(),result.end(),[&](const auto& c){return c.key==f.name;}))continue;
            add(f.name);
        }
        for (auto& c : result) {
            if (!std::strcmp(c.key,"ssao_samples"))c.low=8;
            if (!std::strcmp(c.key,"ssr_steps"))c.low=16;
        }
        return result;
    }();
    return controls;
}
float setting_value(const OverlaySettings& s, const SettingControl& c) { return c.floating ? s.*c.floating : float(s.*c.integer); }
static bool logarithmic(const SettingControl& c) { return c.floating && c.high>=100; }
float setting_fraction(const OverlaySettings& s, const SettingControl& c) {
    float value=std::clamp(setting_value(s,c),c.low,c.high);
    if(logarithmic(c))return std::log1p((value-c.low)/.01f)/std::log1p((c.high-c.low)/.01f);
    return (value-c.low)/(c.high-c.low);
}
void set_setting_fraction(OverlaySettings& s, const SettingControl& c, float fraction) {
    fraction=std::clamp(fraction,0.f,1.f);
    float value=logarithmic(c) ? c.low+.01f*std::expm1(fraction*std::log1p((c.high-c.low)/.01f)) : c.low+fraction*(c.high-c.low);
    if(c.floating)s.*c.floating=value;else s.*c.integer=uint32_t(std::lround(value));
}
void step_setting(OverlaySettings& s, const SettingControl& c, int direction) {
    if(c.floating)set_setting_fraction(s,c,setting_fraction(s,c)+direction*.01f);
    else s.*c.integer=uint32_t(std::clamp(int(s.*c.integer)+direction,int(c.low),int(c.high)));
}
OverlaySettings sanitize_settings(OverlaySettings s) {
    const OverlaySettings defaults;
    for (const auto& f : fields) s.*(f.member) = std::isfinite(s.*(f.member))
        ? std::clamp(s.*(f.member), f.low, f.high) : defaults.*(f.member);
    for (const auto& f : uintFields) if (s.*(f.member) > f.maximum) s.*(f.member) = defaults.*(f.member);
    s.ssaoSamples = std::clamp(s.ssaoSamples, 8u, 16u);
    s.ssrSteps = std::clamp(s.ssrSteps, 16u, 32u);
    return s;
}
std::string settings_config_path() {
    std::filesystem::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg && std::filesystem::path(xdg).is_absolute()) base = xdg;
    else if (const char* userHome = std::getenv("HOME"); userHome && *userHome) base = std::filesystem::path(userHome) / ".config";
    else return {};
    return (base / "vulkan-depth-probe/settings.ini").string();
}
OverlaySettings load_settings_file(const std::string& path, bool* malformed) {
    OverlaySettings result;
    bool bad = false;
    std::ifstream input(path);
    if (!input) {
        if (malformed) *malformed = false;
        log_line("[SETTINGS-LOADED] path=" + path + " source=defaults (missing/unreadable)");
        return result;
    }
    std::string content(65537, '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<size_t>(input.gcount()));
    if (content.size() > 65536 || input.bad()) {
        if (malformed) *malformed = true;
        log_line("[SETTINGS-LOADED] path=" + path + " source=defaults (oversized/read-error)");
        return result;
    }
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line.substr(0, line.find_first_of("#;")));
        if (line.empty() || line[0] == '[') continue;
        const auto equal = line.find('=');
        if (equal == std::string::npos) { bad = true; continue; }
        const auto key = trim(line.substr(0, equal));
        const auto text = trim(line.substr(equal + 1));
        bool found = false;
        for (const auto& f : fields) if (key == f.name) {
            found = true;
            float value = 0;
            if (parse(text, value) && std::isfinite(value)) result.*(f.member) = value;
            else { result.*(f.member) = OverlaySettings{}.*(f.member); bad = true; }
            break;
        }
        if (!found) for (const auto& f : uintFields) if (key == f.name) {
            uint32_t value = 0;
            if (!parse(text, value) || value > f.maximum) { bad = true; value = OverlaySettings{}.*(f.member); }
            result.*(f.member) = value;
            break;
        }
    }
    if (malformed) *malformed = bad;
    log_line("[SETTINGS-LOADED] path=" + path + (bad ? " invalid-values=defaults" : " source=config"));
    return sanitize_settings(result);
}
bool save_settings_file(const std::string& path, const OverlaySettings& input) {
    if (path.empty()) return false;
    std::error_code error;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) return false;
    const auto s = sanitize_settings(input);
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (const auto& f : fields) output << f.name << '=' << s.*(f.member) << '\n';
    for (const auto& f : uintFields) output << f.name << '=' << s.*(f.member) << '\n';
    const std::string content = output.str();
    std::string temporary = path + ".tmp.XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) return false;
    FILE* file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(temporary.c_str()); return false; }
    bool ok = std::fwrite(content.data(), 1, content.size(), file) == content.size();
    ok = std::fflush(file) == 0 && ok;
    ok = fsync(fd) == 0 && ok;
    ok = std::fclose(file) == 0 && ok;
    if (ok) ok = std::rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) unlink(temporary.c_str());
    log_line(std::string(ok ? "[SETTINGS-SAVED] path=" : "[SETTINGS-SAVE-FAILED] path=") + path);
    return ok;
}
OverlaySettings overlay_settings() { auto& s = store(); std::lock_guard lock(s.mutex); return s.value; }
void set_overlay_settings(OverlaySettings value) { auto& s = store(); std::lock_guard lock(s.mutex); s.value = sanitize_settings(value); }
void reset_overlay_settings() { set_overlay_settings({}); log_line("[SETTINGS-RESET]"); }
bool save_overlay_settings() {
    auto& s = store(); std::lock_guard lock(s.mutex);
    const bool saved = save_settings_file(s.path, s.value);
    if (saved) {
        std::error_code error;
        s.modified = std::filesystem::last_write_time(s.path, error);
    }
    return saved;
}
void reload_overlay_settings() {
    auto& s = store(); std::lock_guard lock(s.mutex);
    s.value = load_settings_file(s.path);
    std::error_code error;
    s.modified = std::filesystem::last_write_time(s.path, error);
}
void poll_settings_reload() {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now < s.nextPoll) return;
    s.nextPoll = now + std::chrono::seconds(1);
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(s.path, error);
    if (!error && modified != s.modified) { s.modified = modified; s.value = load_settings_file(s.path); }
}
} // namespace depth_probe
