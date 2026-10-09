#include "memory_sun.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace depth_probe {

WowSunLayout wow_sun_layout_from_env() {
    WowSunLayout l;
    if (const char* v = std::getenv("DEPTH_PROBE_MEM_SUN_STATIC"); v && *v) {
        char* end = nullptr;
        l.staticPointers[0] = std::strtoull(v, &end, 16);
        l.staticPointers[1] = end && *end == ',' ? std::strtoull(end + 1, nullptr, 16) : 0;
    }
    if (const char* v = std::getenv("DEPTH_PROBE_MEM_SKY_SUN"); v && *v) l.skyDisc = std::strtoull(v, nullptr, 16);
    if (const char* v = std::getenv("DEPTH_PROBE_MEM_SUN_OFFSET"); v && *v) {
        const uint32_t d = uint32_t(std::strtoul(v, nullptr, 16));
        l.ambient = d + (l.ambient - l.direction); l.diffuse = d + (l.diffuse - l.direction); l.specular = d + (l.specular - l.direction);
        l.direction = d;
    }
    return l;
}

namespace {
std::string hex(uint64_t v) { std::ostringstream o; o << "0x" << std::hex << v; return o.str(); }
bool color_ok(const float c[3]) {
    for (int i = 0; i < 3; ++i) if (!std::isfinite(c[i]) || c[i] < 0 || c[i] > 16) return false;
    return true;
}
}

namespace {
void read_sky_disc(const MemoryReader& memory, const WowSunLayout& l, GameSun& s) {
    float d[3]{};
    if (!l.skyDisc || !memory.read(l.skyDisc, d, sizeof d)) return;
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(len) || std::fabs(len - 1.f) > .02f) return;
    for (int i = 0; i < 3; ++i) s.skyToward[i] = -d[i] / len;
    s.skyElevationDeg = float(std::asin(std::fmax(-1.f, std::fmin(1.f, s.skyToward[2]))) * 180.0 / M_PI);
    s.skyAzimuthDeg = float(std::atan2(s.skyToward[1], s.skyToward[0]) * 180.0 / M_PI);
    s.skyOk = true;
}
}

GameSun read_wow_sun(const MemoryReader& memory, const WowSunLayout& l) {
    GameSun out;
    read_sky_disc(memory, l, out);
    for (uint64_t pointer : l.staticPointers) {
        if (!pointer) continue;
        auto fail = [&](const std::string& why) { out.failure += (out.failure.empty() ? "" : "; ") + ("[" + hex(pointer) + "] " + why); };
        uint32_t block = 0;
        if (!memory.read_u32(pointer, block)) { fail("unreadable"); continue; }
        if (!block) { fail("null"); continue; }
        GameSun s;
        if (!memory.read(uint64_t(block) + l.direction, s.direction, sizeof s.direction) || !memory.read(uint64_t(block) + l.ambient, s.ambient, sizeof s.ambient) ||
            !memory.read(uint64_t(block) + l.diffuse, s.diffuse, sizeof s.diffuse) || !memory.read(uint64_t(block) + l.specular, s.specular, sizeof s.specular)) {
            fail("block " + hex(block) + " unreadable");
            continue;
        }
        const float len = std::sqrt(s.direction[0] * s.direction[0] + s.direction[1] * s.direction[1] + s.direction[2] * s.direction[2]);
        if (!std::isfinite(len) || std::fabs(len - 1.f) > .02f) { fail("direction not unit (length " + std::to_string(len) + ")"); continue; }
        if (!color_ok(s.ambient) || !color_ok(s.diffuse) || !color_ok(s.specular)) { fail("colours out of range"); continue; }
        for (int i = 0; i < 3; ++i) s.toward[i] = -s.direction[i] / len;
        s.elevationDeg = float(std::asin(std::fmax(-1.f, std::fmin(1.f, s.toward[2]))) * 180.0 / M_PI);
        s.azimuthDeg = float(std::atan2(s.toward[1], s.toward[0]) * 180.0 / M_PI);
        s.ok = true;
        s.staticPointer = pointer;
        s.block = block;
        s.skyOk = out.skyOk;
        std::copy_n(out.skyToward, 3, s.skyToward);
        s.skyElevationDeg = out.skyElevationDeg; s.skyAzimuthDeg = out.skyAzimuthDeg;
        return s;
    }
    if (out.failure.empty()) out.failure = "no static pointer configured";
    return out;
}

WowWeatherLayout wow_weather_layout_from_env() {
    WowWeatherLayout l;
    if (const char* v = std::getenv("DEPTH_PROBE_MEM_WEATHER"); v && *v) l.staticPointer = std::strtoull(v, nullptr, 16);
    return l;
}

GameWeather read_wow_weather(const MemoryReader& memory, const WowWeatherLayout& l) {
    GameWeather w;
    if (!l.staticPointer) { w.failure = "not configured"; return w; }
    uint32_t object = 0, effects[3]{};
    if (!memory.read_u32(l.staticPointer, object)) { w.failure = "[" + hex(l.staticPointer) + "] unreadable"; return w; }
    if (!object) { w.failure = "[" + hex(l.staticPointer) + "] null"; return w; }
    if (!memory.read(uint64_t(object) + l.current, &w.intensity, 4) || !memory.read(uint64_t(object) + l.target, &w.target, 4) ||
        !memory.read(uint64_t(object) + l.effects, effects, sizeof effects)) {
        w.failure = "object " + hex(object) + " unreadable";
        return w;
    }
    auto sane = [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; };
    if (!sane(w.intensity) || !sane(w.target)) { w.failure = "intensity out of range"; return w; }
    for (int i = 0; i < 3 && !w.kind; ++i) if (effects[i]) w.kind = i + 1;   // the order 0x783B60 checks them in
    w.ok = true;
    return w;
}

const char* weather_name(int kind) {
    static const char* names[]{"CLEAR", "RAIN", "SNOW", "SANDSTORM"};
    return kind >= 0 && kind <= 3 ? names[kind] : "?";
}

float weather_overcast(const GameWeather& w) {
    if (!w.ok || !w.kind) return 0;
    const float t = std::clamp((w.intensity - .25f) / .2f, 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
}
