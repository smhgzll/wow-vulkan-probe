#include "memory_sun.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
using namespace depth_probe;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
#define CHECK_NEAR(a,b,eps) CHECK(std::fabs((a)-(b))<(eps))

namespace {
struct FakeMemory final : MemoryReader {
    std::map<uint64_t, uint8_t> bytes;
    void put(uint64_t address, const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i) bytes[address + i] = static_cast<const uint8_t*>(data)[i];
    }
    void put_u32(uint64_t address, uint32_t v) { put(address, &v, 4); }
    bool read(uint64_t address, void* out, size_t size) const override {
        for (size_t i = 0; i < size; ++i) {
            auto it = bytes.find(address + i);
            if (it == bytes.end()) return false;
            static_cast<uint8_t*>(out)[i] = it->second;
        }
        return true;
    }
};
// The live block from the game (pid 32325, 2026-09-26 01:18, Stranglethorn coast): block + 0x7C .. + 0xB8.
const float liveBlock[15]{-.5968f, -.5968f, -.5364f, .2118f, .251f, .3608f, .7216f, .4784f, .3451f, .9804f, .9333f, .8706f, 0, .7f, .03f};
}

int main() {
    const WowSunLayout layout;
    {   // the verified chain
        FakeMemory m;
        m.put_u32(0x00CE04A8, 0x17530020);
        m.put(0x17530020 + 0x7C, liveBlock, sizeof liveBlock);
        const auto s = read_wow_sun(m, layout);
        CHECK(s.ok);
        CHECK(s.staticPointer == 0x00CE04A8 && s.block == 0x17530020);
        CHECK_NEAR(s.toward[0], .5969f, 1e-3f);
        CHECK_NEAR(s.toward[2], .5365f, 1e-3f);
        CHECK_NEAR(s.elevationDeg, 32.44f, .1f);
        CHECK_NEAR(s.azimuthDeg, 45.f, .01f);
        CHECK_NEAR(s.ambient[2], .3608f, 1e-6f);   // = water UBO c50
        CHECK_NEAR(s.diffuse[0], .7216f, 1e-6f);   // = c51
        CHECK_NEAR(s.specular[1], .9333f, 1e-6f);  // = c52
    }
    {   // the sky disc (static, light-travel direction as stored; values from the live game, Teldrassil 03:15)
        FakeMemory m;
        m.put_u32(0x00CE04A8, 0x17530020);
        m.put(0x17530020 + 0x7C, liveBlock, sizeof liveBlock);
        const float disc[3]{-.39269f, -.39269f, -.83162f};
        m.put(0x00D43180, disc, sizeof disc);
        const auto s = read_wow_sun(m, layout);
        CHECK(s.ok && s.skyOk);
        CHECK_NEAR(s.skyToward[2], .83162f, 1e-4f);
        CHECK_NEAR(s.skyElevationDeg, 56.27f, .05f);
        CHECK_NEAR(s.skyAzimuthDeg, 45.f, .01f);
        CHECK(s.elevationDeg < s.skyElevationDeg - 20);   // the lighting direction is well below the disc
        const float bad[3]{0, 0, 0};
        m.put(0x00D43180, bad, sizeof bad);
        const auto noDisc = read_wow_sun(m, layout);
        CHECK(noDisc.ok && !noDisc.skyOk);                  // a garbage disc does not break the block
    }
    {   // first pointer null -> the second chain
        FakeMemory m;
        m.put_u32(0x00CE04A8, 0);
        m.put_u32(0x00AEEDDC, 0x20000000);
        m.put(0x20000000 + 0x7C, liveBlock, sizeof liveBlock);
        const auto s = read_wow_sun(m, layout);
        CHECK(s.ok && s.staticPointer == 0x00AEEDDC);
    }
    {   // garbage direction (not unit) and unreadable chains are rejected with a reason for each
        FakeMemory m;
        float bad[15];
        for (int i = 0; i < 15; ++i) bad[i] = liveBlock[i];
        bad[2] = .3f;
        m.put_u32(0x00CE04A8, 0x17530020);
        m.put(0x17530020 + 0x7C, bad, sizeof bad);
        const auto s = read_wow_sun(m, layout);
        CHECK(!s.ok);
        CHECK(s.failure.find("not unit") != std::string::npos && s.failure.find("[0xaeeddc] unreadable") != std::string::npos);
    }
    {   // colours out of range
        FakeMemory m;
        float bad[15];
        for (int i = 0; i < 15; ++i) bad[i] = liveBlock[i];
        bad[4] = NAN;
        m.put_u32(0x00CE04A8, 0x17530020);
        m.put(0x17530020 + 0x7C, bad, sizeof bad);
        CHECK(!read_wow_sun(m, layout).ok);
    }
    {   // the real process: reading our own memory at WoW's addresses must fail cleanly, never fault
        const auto s = read_wow_sun(self_memory_reader(), layout);
        CHECK(!s.ok && !s.failure.empty());
    }
    {   // weather: the live object (Booty Bay rain, 2026-09-26): +0x00 target .466, +0x04 old .550, +0x08 in effect .466, rain object set
        const WowWeatherLayout wl;
        FakeMemory m;
        m.put_u32(0x00CD7544, 0x0EC90070);
        const float head[3]{.46561f, .55031f, .46561f};
        m.put(0x0EC90070, head, sizeof head);
        const uint32_t effects[3]{0x1D3F28B0, 0, 0};
        m.put(0x0EC90070 + 0x13C, effects, sizeof effects);
        auto w = read_wow_weather(m, wl);
        CHECK(w.ok && w.kind == 1 && std::string(weather_name(w.kind)) == "RAIN");
        CHECK_NEAR(w.intensity, .46561f, 1e-6f);
        CHECK_NEAR(weather_overcast(w), 1.f, 1e-6f);             // medium rain: the sun is gone
        const float lightRain = .35f;
        m.put(0x0EC90070 + 8, &lightRain, 4);
        CHECK_NEAR(weather_overcast(read_wow_weather(m, wl)), .5f, 1e-5f);
        const float drizzle = .2f;                                 // below the game's precipitation threshold
        m.put(0x0EC90070 + 8, &drizzle, 4);
        CHECK(weather_overcast(read_wow_weather(m, wl)) == 0.f);
        const uint32_t snow[3]{0, 0x1000, 0};
        m.put(0x0EC90070 + 0x13C, snow, sizeof snow);
        CHECK(read_wow_weather(m, wl).kind == 2);
        const uint32_t none[3]{};
        m.put(0x0EC90070 + 0x13C, none, sizeof none);
        m.put(0x0EC90070 + 8, head, 4);
        w = read_wow_weather(m, wl);
        CHECK(w.ok && w.kind == 0 && weather_overcast(w) == 0.f);  // no precipitation object: clear, whatever the number says
        const float bad = 3.f;
        m.put(0x0EC90070 + 8, &bad, 4);
        CHECK(!read_wow_weather(m, wl).ok);
        m.put_u32(0x00CD7544, 0);
        CHECK(!read_wow_weather(m, wl).ok);
        CHECK(!read_wow_weather(self_memory_reader(), wl).ok);   // WoW's address in our own process: fails cleanly
    }
    std::puts("PASS: WoW sun block chain, sky disc, fallback chain, sanity rejections, safe self-read, weather object");
}
