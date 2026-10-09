#pragma once
#include "process_memory.hpp"
#include <cstdint>
#include <string>

// WoW 3.3.5a (12340) outdoor light read from the game's memory: the block WoW copies into its shader constants (water VS
// UBO c49..c52, c55). Found 2026-09-26 with tools/find-sun-direction.py and verified against three water captures while
// the sun moved (docs/fog.md, "Sun direction"): [0x00CE04A8] (also [0x00AEEDDC]) -> block; block + 0x7C = direction the
// light travels (world, z-up, unit), +0x88 / +0x94 / +0xA0 = the colours WoW sends as c50 / c51 / c52 (by their values:
// ambient, direct light, specular - the names are an interpretation). Verified in one session only.
namespace depth_probe {

struct WowSunLayout {
    uint64_t staticPointers[2]{0x00CE04A8, 0x00AEEDDC}; // tried in order; the first readable, sane block wins
    uint32_t direction = 0x7C, ambient = 0x88, diffuse = 0x94, specular = 0xA0;
    // The sun/moon DISC WoW draws in the sky is not where the lighting direction points: same 45 degree azimuth, but higher
    // (Teldrassil 2026-09-26: disc 56.3 degrees, light 36.8). Found by a memory scan and checked against screenshots with
    // the memory camera (projects onto the disc, the light direction 15 degrees below it). Static, light-travel direction.
    uint64_t skyDisc = 0x00D43180;
};
// DEPTH_PROBE_MEM_SUN_STATIC=<hex>[,<hex>] replaces the static pointers, DEPTH_PROBE_MEM_SUN_OFFSET=<hex> the direction offset (the
// colours follow at the same relative offsets), DEPTH_PROBE_MEM_SKY_SUN=<hex> the sky disc address (0 = none).
WowSunLayout wow_sun_layout_from_env();

struct GameSun {
    bool ok = false;
    std::string failure;            // why not ok (every tried chain, "; "-separated)
    uint64_t staticPointer = 0, block = 0;
    float direction[3]{};           // light travel direction, world
    float ambient[3]{}, diffuse[3]{}, specular[3]{};
    float toward[3]{};              // -direction: toward the light (what the fog's phase function uses)
    float elevationDeg = 0, azimuthDeg = 0; // of `toward`: above the horizon, and atan2(y, x)
    bool skyOk = false;                     // the sky disc direction was read (independent of the block)
    float skyToward[3]{};                   // toward the disc
    float skyElevationDeg = 0, skyAzimuthDeg = 0;
};
GameSun read_wow_sun(const MemoryReader&, const WowSunLayout&);

// Weather, from Wow.exe itself (2026-09-26): the SMSG_WEATHER (0x2F4) case of the handler at 0x526530 (jump table -> 0x526A69,
// log "Weather changed to %d, intensity %f") calls [0x00CD7544]->0x7846A0(kind, intensity, ...), which stores the target
// intensity (clamped 0..1) at +0x00 and the old one at +0x04; every frame 0x784850 blends +0x04 -> +0x00 over time into +0x08,
// the intensity in effect. The effect objects at +0x13C / +0x140 / +0x144 are non-null while rain / snow / sandstorm is on
// (0x783B60 returns 1 / 2 / 3 from them). Below 0.25 the game draws no precipitation: it scales (intensity - .25) * 4/3.
// Checked live in Booty Bay rain: +0x08 = 0.466, rain object set, its texture name (RainDrop01.blp) at +0x38.
struct WowWeatherLayout {
    uint64_t staticPointer = 0x00CD7544;    // DEPTH_PROBE_MEM_WEATHER=<hex> replaces it (0 = do not read)
    uint32_t target = 0x00, current = 0x08, effects = 0x13C;
};
WowWeatherLayout wow_weather_layout_from_env();

struct GameWeather {
    bool ok = false;
    std::string failure;
    int kind = 0;               // 0 clear, 1 rain, 2 snow, 3 sandstorm
    float intensity = 0;        // in effect now (0..1)
    float target = 0;           // where it is going
};
GameWeather read_wow_weather(const MemoryReader&, const WowWeatherLayout&);
const char* weather_name(int kind);
// How far the weather hides the sun (0 = not at all, 1 = fully overcast): none below the game's 0.25 precipitation
// threshold, all from 0.45 (medium rain), smooth between; clear weather (no effect object) never does.
float weather_overcast(const GameWeather&);
}
