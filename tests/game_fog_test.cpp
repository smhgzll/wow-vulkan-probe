// WoW's own fog register (src/game_fog.*) on values from real frame scans (Durotar at dusk, frame-scan-114104-1).
#include <algorithm>
#include "game_fog.hpp"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) < (eps))

using namespace depth_probe::gamefog;

int main() {
    const float durotar[4]{-.00327f, 2.f, 1.f, 0.f};
    CHECK(looks_like_fog(durotar));
    NEAR(fog_start(durotar), 305.8, .5); NEAR(fog_end(durotar), 611.6, .5);
    // Not fog: a light direction, a bone row, the terrain's colour registers, an unset register.
    const float light[4]{.5905f, -.4783f, -.65f, -.6117f}, bone[4]{-.0431f, .0564f, .9035f, .0808f}, colour[4]{.9961f, .698f, .2902f, 0.f};
    const float nan[4]{NAN, 2.f, 1.f, 0.f}, shadowglen[4]{-.0029f, 1.1111f, 1.f, 0.f};
    CHECK(!looks_like_fog(light) && !looks_like_fog(bone) && !looks_like_fog(colour) && !looks_like_fog(nan) && looks_like_fog(shadowglen));
    Pusher p;
    float r[4]{-.00327f, 2.f, 1.f, 0.f};
    CHECK(!p.push(r, 2.f));                          // nothing learned yet: untouched
    for (int i = 0; i < 40; ++i) p.vote(durotar);    // the world's draws
    p.vote(shadowglen);                              // a stray copy
    p.end_frame();
    CHECK(p.known() && p.fog()[0] == durotar[0]);
    CHECK(p.push(r, 2.f));                           // pushed twice as far: both ends
    NEAR(fog_start(r), 611.6, 1); NEAR(fog_end(r), 1223.2, 1);
    CHECK(!p.push(r, 2.f) && r[0] == durotar[0] / 2);   // the same memory seen by a later draw: never pushed again
    float other[4]{-.0029f, 1.1111f, 1.f, 0.f};
    CHECK(!p.push(other, 2.f));                      // not this frame's fog
    float off[4]{-.00327f, 2.f, 1.f, 0.f};
    CHECK(p.push(off, 0.f) && off[0] == 0.f && !looks_like_fog(off));   // no fog: f = y >= 1 everywhere
    float asGame[4]{-.00327f, 2.f, 1.f, 0.f};
    CHECK(!p.push(asGame, 1.f) && asGame[0] == durotar[0]);
    p.end_frame();                                   // a frame without world draws keeps the fog
    CHECK(p.known() && p.fog()[1] == 2.f);
    // observe(): the pushed value seen again (shared memory) neither votes nor is pushed; true fog values vote and are pushed.
    Pusher q;
    for (int i = 0; i < 5; ++i) q.vote(durotar);
    q.end_frame();
    float a[4]{-.00327f, 2.f, 1.f, 0.f};
    CHECK(q.observe(a, 2.f) && a[0] == durotar[0] / 2);
    for (int i = 0; i < 50; ++i) CHECK(!q.observe(a, 2.f));   // 50 later draws on the same memory
    float b[4]{-.00327f, 2.f, 1.f, 0.f};
    CHECK(q.observe(b, 2.f));
    q.end_frame();
    CHECK(q.fog()[0] == durotar[0]);                          // still the game's value, not the pushed one
    ShaderFogRegisters s;
    CHECK(s.wants_scan());
    s.scans = 8; s.draws = 1;
    CHECK(!s.wants_scan());
    s.draws = ShaderFogRegisters::rescanEvery;
    CHECK(s.wants_scan());
    // One shader, two objects: the fog in c46 for one (a trunk), in c24 for the other (its branches) - both stay learned,
    // whichever the later scans look at (Booty Bay: each rescan used to replace the list and the other object got the fog).
    {
        ShaderFogRegisters t;
        float trunk[64][4]{}, branches[64][4]{};
        std::copy_n(durotar, 4, trunk[46]); std::copy_n(durotar, 4, branches[24]);
        branches[46][0] = .5f; branches[46][1] = .25f;   // c46 holds something else there
        t.learn(trunk, 64);
        t.learn(branches, 64);
        t.learn(trunk, 64);
        CHECK(t.registers.size() == 2 && t.scans == 3);
        CHECK(std::find(t.registers.begin(), t.registers.end(), 46) != t.registers.end() && std::find(t.registers.begin(), t.registers.end(), 24) != t.registers.end());
        Pusher u;
        u.vote(durotar); u.end_frame();
        CHECK(!u.push(branches[46], 0.f));   // the register that is not the fog in this draw is left alone
        CHECK(u.push(branches[24], 0.f) && branches[24][0] == 0.f);
    }
    if (failures) return 1;
    std::puts("PASS: WoW fog register shape (Durotar, Shadowglen; light, bone, colour, NaN rejected), start/end, frame fog by vote, push k times farther / off / as the game, never pushed twice (nor voted for), rescan schedule, fog registers learned across objects (not replaced)");
    return 0;
}
