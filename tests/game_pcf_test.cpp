// WoW's shadow filter taps (src/game_pcf.*) on the pixel constants of a real frame scan (Mulgore, frame-scan-110504-1,
// terrain FS 9e7279ff: c18 the fog colour, c19..c27 the 9 tap offsets).
#include "game_pcf.hpp"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

using namespace depth_probe::gamepcf;

int main() {
    float regs[30][4]{};
    const float taps[9][2]{{.000391f, -.000488f}, {-.000098f, -.000391f}, {.000098f, -.000293f}, {.000488f, -.000195f}, {-.000293f, -.000098f},
                           {.000293f, .000098f}, {-.000488f, -.000195f}, {-.000195f, -.000293f}, {-.000488f, -.000195f}};
    regs[16][3] = .00025f;
    regs[18][0] = .243137f; regs[18][1] = .352941f; regs[18][2] = .368627f; regs[18][3] = 1;   // the fog colour: not a tap
    for (int i = 0; i < 9; ++i) { regs[19 + i][0] = taps[i][0]; regs[19 + i][1] = taps[i][1]; }
    size_t first = 0, count = 0;
    CHECK(find_taps(regs, 30, first, count) && first == 19 && count == 9);
    CHECK(spread(regs, first, count, 2.f) && std::fabs(regs[22][0] - 2 * .000488f) < 1e-9f);
    CHECK(!spread(regs, first, count, 2.f));                 // the same memory seen again: spread once only
    CHECK(!find_taps(regs, 30, first, count));               // spread offsets no longer look like the game's
    float few[10][4]{};
    for (int i = 0; i < 5; ++i) few[2 + i][0] = .0002f;      // too short a run (a colour's small components, a bias)
    CHECK(!find_taps(few, 10, first, count));
    float big[12][4]{};
    for (int i = 0; i < 9; ++i) big[1 + i][0] = .01f;        // offsets of another texture (much wider): not the shadow taps
    CHECK(!find_taps(big, 12, first, count));
    float mixed[12][4]{};
    for (int i = 0; i < 9; ++i) { mixed[1 + i][0] = taps[i][0]; mixed[1 + i][1] = taps[i][1]; }
    mixed[5][3] = 1.f;                                        // a w of 1 breaks the run
    CHECK(!find_taps(mixed, 12, first, count));
    CHECK(!spread(regs, 19, 9, 1.f));
    if (failures) return 1;
    std::puts("PASS: WoW shadow filter taps found (c19..c27 in the terrain FS, fog colour and short / wide / broken runs rejected), spread once only");
    return 0;
}
