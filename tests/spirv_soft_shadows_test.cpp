// Soft shadows inside WoW's pixel shaders (src/spirv_soft_shadows.*) on the game's own shaders (tests/fixtures: the terrain
// FS 9e7279ff - cascades 9 + 5 + 5 + 5 taps - the model FS ba3c7f84 - 10 + 5 + 5 + 5 - and a vertex shader).
// With --write DIR the patched modules are written there (tools/spirv-eval.py can run them).
#include "spirv_soft_shadows.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#ifdef DEPTH_PROBE_HAVE_SPIRV_TOOLS   // SPIRV-Tools' C API (libSPIRV-Tools-shared; no headers needed)
extern "C" {
struct spv_const_binary_t { const uint32_t* code; size_t wordCount; };
struct spv_diagnostic_t { size_t line, column, index; char* error; bool isTextSource; };
void* spvContextCreate(int env);
void spvContextDestroy(void*);
int spvValidate(void*, const spv_const_binary_t*, spv_diagnostic_t**);
void spvDiagnosticDestroy(spv_diagnostic_t*);
}
static bool valid(const std::vector<uint32_t>& code, const char* what) {
    void* context = spvContextCreate(18 /* SPV_ENV_VULKAN_1_1 */);
    spv_diagnostic_t* d = nullptr;
    const spv_const_binary_t b{code.data(), code.size()};
    const int r = spvValidate(context, &b, &d);
    if (d) { std::fprintf(stderr, "%s: word %zu: %s\n", what, d->index, d->error); spvDiagnosticDestroy(d); }
    spvContextDestroy(context);
    return r == 0;
}
#else
static bool valid(const std::vector<uint32_t>&, const char*) { return true; }
#endif

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static std::vector<uint32_t> load(const std::string& name) {
    std::ifstream in(std::string(DEPTH_PROBE_TEST_ROOT_DIR) + "/tests/fixtures/" + name, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), w.size() * 4);
    return w;
}
struct Counts { size_t insns = 0, dref = 0, fetch = 0, queryImage = 0, capImageQuery = 0; bool wellFormed = true; uint32_t maxId = 0; bool unique = true; };
static Counts scan(const std::vector<uint32_t>& w) {
    Counts c;
    std::set<uint32_t> results;
    // opcodes whose operands are (result type, result id, ...) among those the patch emits or the game uses around it
    static const std::set<uint16_t> typed{12, 43, 44, 61, 65, 79, 81, 86, 90, 95, 100, 103, 110, 111, 129, 130, 131, 133, 136, 142, 169, 184, 186};
    for (size_t i = 5; i < w.size();) {
        const uint16_t n = uint16_t(w[i] >> 16), op = uint16_t(w[i] & 0xffff);
        if (!n || i + n > w.size()) { c.wellFormed = false; break; }
        ++c.insns;
        if (op == 90) ++c.dref;
        if (op == 95) ++c.fetch;
        if (op == 103) ++c.queryImage;
        if (op == 17 && w[i + 1] == 50) ++c.capImageQuery;
        if (typed.count(op) && n >= 3) { c.unique &= results.insert(w[i + 2]).second; c.maxId = std::max(c.maxId, w[i + 2]); }
        i += n;
    }
    return c;
}

int main(int argc, char** argv) {
    const char* out = argc > 2 && !std::strcmp(argv[1], "--write") ? argv[2] : nullptr;
    depth_probe::softshadow::Params p;
    p.size = 1.5f; p.fade = .6f;
    for (auto [file, taps] : {std::pair{"wow_terrain_fs_9e7279ff.spv", 24u}, std::pair{"wow_model_fs_ba3c7f84.spv", 25u}}) {
        const auto code = load(file);
        CHECK(code.size() > 1000);
        const auto before = scan(code);
        const auto r = depth_probe::softshadow::patch(code.data(), code.size(), p);
        CHECK(r.taps == taps && !r.code.empty() && r.live && r.liveSet == 2 && r.liveBinding == 4);   // DXVK's D3D9SharedPS: live settings
        const auto after = scan(r.code);
        std::fprintf(stderr, "%s: %u taps rewritten, %zu -> %zu instructions, depth compares %zu -> %zu, fetches %zu, bound %u -> %u\n", file, r.taps,
                     before.insns, after.insns, before.dref, after.dref, after.fetch, code[3], r.code[3]);
        CHECK(after.wellFormed && after.unique);
        CHECK(after.dref == before.dref + 3 * taps && after.fetch == before.fetch + 2 * taps && after.queryImage >= taps);
        CHECK(after.capImageQuery == 1 && after.maxId < r.code[3]);
        CHECK(valid(code, file) && valid(r.code, file));
        p.fade = 0;   // without the fade too
        CHECK(valid(depth_probe::softshadow::patch(code.data(), code.size(), p).code, file));
        p.fade = .6f;
        const auto again = depth_probe::softshadow::patch(r.code.data(), r.code.size(), p);   // a patched shader has 4x the samples per cascade: still found
        CHECK(again.taps > 0);   // (never done by the layer: it patches the game's code once, before the driver sees it)
        if (out) { std::ofstream o(std::string(out) + "/" + file, std::ios::binary); o.write(reinterpret_cast<const char*>(r.code.data()), std::streamsize(r.code.size() * 4)); }
    }
    // WoW indoors (the Den, frame-scan-49703-1): one shadow map read 14 times - each scaled by the live indoor strength.
    {
        const auto code = load("wow_indoor_fs_17cef8ff.spv");
        const auto before = scan(code);
        const auto r = depth_probe::softshadow::patch(code.data(), code.size(), p);
        const auto after = scan(r.code);
        std::fprintf(stderr, "wow_indoor_fs_17cef8ff.spv: %u taps, indoor %d, live %d at s%u/b%u; depth compares %zu -> %zu\n", r.taps, int(r.indoor), int(r.live),
                     r.liveSet, r.liveBinding, before.dref, after.dref);
        CHECK(r.taps == 14 && r.indoor && r.live && r.liveSet == 2 && r.liveBinding == 4 && after.dref == before.dref && after.fetch == 0);
        CHECK(after.wellFormed && after.unique && valid(r.code, "indoor"));
        if (out) { std::ofstream o(std::string(out) + "/wow_indoor_fs_17cef8ff.spv", std::ios::binary); o.write(reinterpret_cast<const char*>(r.code.data()), std::streamsize(r.code.size() * 4)); }
    }
    const auto vs = load("wow_terrain_vs_44c15e1a.spv");
    const auto rv = depth_probe::softshadow::patch(vs.data(), vs.size(), p);
    CHECK(rv.code.empty() && rv.why == "not a fragment shader");
    const uint32_t junk[6]{0x07230203, 0x10000, 0, 10, 0, 0};
    CHECK(depth_probe::softshadow::patch(junk, 6, p).code.empty());
    if (failures) return 1;
    std::puts("PASS: soft shadows patched into the game's terrain (24 taps) and model (25 taps) pixel shaders - well-formed, unique ids, 3 compares + 2 fetches per tap, live settings, ImageQuery once; the indoor shader's 14 samples scaled live"
#ifdef DEPTH_PROBE_HAVE_SPIRV_TOOLS
              ", valid SPIR-V (Vulkan 1.1)"
#endif
              "; vertex shaders and junk untouched");
    return 0;
}
