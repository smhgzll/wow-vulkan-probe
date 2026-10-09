#include "memory_camera.hpp"
#include "process_memory.hpp"
#include "memory_camera_scan.hpp"
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sys/mman.h>
#include <unistd.h>
using namespace depth_probe;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
#define CHECK_NEAR(a,b,eps) CHECK(std::fabs((a)-(b))<(eps))

namespace {
// Sparse fake address space: only bytes explicitly placed are readable.
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

Vec3 neg(const Vec3& v) { return {-v.x, -v.y, -v.z}; }
float dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Real WoW water UBO (exports/water-capture-101750-1-mode2-view0/water_ubo.bin,
// Booty Bay): P at +256, V at +336. fovY 54, aspect 16:9, near .2, far 791.58.
const float realP[16]{1.1039682626724243f,0,0,0, 0,1.9626103639602661f,0,0, 0,0,1.0002527236938477f,1.0f, 0,0,-0.20005054771900177f,0};
const float realV[16]{0.6243106126785278f,0.42488765716552734f,0.6555202007293701f,0, -0.7811762094497681f,0.33956727385520935f,0.5238872170448303f,0,
                      -4.750454252189229e-09f,0.8391451239585876f,-0.5439075827598572f,0, 9326.787109375f,5958.53125f,9244.5615234375f,1.0f};
const float realV2[16]{-0.1638123244047165f,0.5227610468864441f,0.8365921378135681f,0, -0.9864915013313293f,-0.08680733293294907f,-0.13892070949077606f,0,
                       3.2913356484698397e-09f,0.8480479717254639f,-0.529919445514679f,0, -1919.31982421875f,7566.2197265625f,12154.05078125f,1.0f};

// A CGCamera-shaped struct at the default layout offsets.
std::vector<uint8_t> make_struct(const WowCameraLayout& l, const Vec3& pos, const Vec3 rows[3], float n, float f, float fov, float aspect) {
    std::vector<uint8_t> s(l.span(), 0xCD);
    auto put3 = [&](uint32_t o, const Vec3& v) { float a[3]{v.x, v.y, v.z}; std::memcpy(s.data() + o, a, 12); };
    put3(l.position, pos);
    for (int r = 0; r < 3; ++r) put3(l.facing + r * 12, rows[r]);
    std::memcpy(s.data() + l.nearClip, &n, 4); std::memcpy(s.data() + l.farClip, &f, 4);
    std::memcpy(s.data() + l.fov, &fov, 4); std::memcpy(s.data() + l.aspect, &aspect, 4);
    return s;
}

// Memory camera for a GPU camera under the default hypothesis: rows = forward, left, up.
RawMemoryCamera memory_from_gpu(const DerivedCamera& g, const WowCameraLayout& l) {
    const Vec3 rows[3]{g.forward, neg(g.right), g.up};
    auto s = make_struct(l, g.position, rows, .2f, 791.58f, 54.f / 57.29578f, 16.f / 9.f);
    return decode_wow_camera(s.data(), l, 0x10000000);
}

DerivedCamera rotated_gpu(float yawDeg, float pitchDeg, const Vec3& pos) {
    // Right-handed z-up world: forward from yaw/pitch, right = forward x up (as the real capture shows).
    const float y = yawDeg / 57.29578f, p = pitchDeg / 57.29578f;
    const Vec3 f{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
    const Vec3 r{std::sin(y), -std::cos(y), 0};
    const Vec3 u{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
    const float viewRaw[16]{r.x, u.x, f.x, 0, r.y, u.y, f.y, 0, r.z, u.z, f.z, 0, -dot3(r, pos), -dot3(u, pos), -dot3(f, pos), 1};
    return derive_known_wow_camera(realP, viewRaw);
}
}

int main() {
    // Signed permutations: 48 unique, identity first, parse/describe round trip.
    {
        const auto& all = signed_permutations();
        CHECK(all.size() == 48);
        CHECK(all[0] == SignedPermutation{});
        std::set<int> seen;
        for (auto& p : all) seen.insert(p.m[0] * 100 + p.m[1] * 10 + p.m[2]);
        CHECK(seen.size() == 48);
        SignedPermutation p;
        CHECK(parse_signed_permutation("+1,+3,-2", p));
        CHECK(describe_rows(p) == "F+R0 U+R2 R-R1");
        CHECK(!parse_signed_permutation("1,1,2", p));
        CHECK(!parse_signed_permutation("1,2", p));
        SignedPermutation w; w.m[0] = 2; w.m[1] = -1; w.m[2] = 3;
        CHECK(describe_world(w) == "+Y-X+Z");
        const auto v = apply(w, Vec3{1, 2, 3});
        CHECK(v.x == 2 && v.y == -1 && v.z == 3);
    }

    const WowCameraLayout layout;
    CHECK(layout.span() == 0x48);
    const auto gpu = derive_known_wow_camera(realP, realV);
    CHECK(gpu.valid);
    CHECK_NEAR(gpu.position.x, -14414.52f, .05f);

    // Pointer chain through fake memory, then decode.
    {
        FakeMemory mem;
        const uint32_t worldFrame = 0x2000000, camera = 0x3000000;
        mem.put_u32(layout.staticPointer, worldFrame);
        mem.put_u32(worldFrame + layout.cameraOffset, camera);
        const Vec3 rows[3]{gpu.forward, neg(gpu.right), gpu.up};
        auto s = make_struct(layout, gpu.position, rows, .2f, 791.58f, .9425f, 1.7778f);
        mem.put(camera, s.data(), s.size());
        auto c = read_wow_camera(mem, layout);
        CHECK(c.readOk);
        CHECK(c.chainValue == worldFrame && c.address == camera);
        CHECK(memory_camera_sanity(c).empty());
        CHECK_NEAR(c.fov, .9425f, 1e-6f);

        FakeMemory nullFrame; nullFrame.put_u32(layout.staticPointer, 0);
        CHECK(!read_wow_camera(nullFrame, layout).readOk);
        CHECK(read_wow_camera(nullFrame, layout).failure.find("null") != std::string::npos);
        FakeMemory dangling; dangling.put_u32(layout.staticPointer, worldFrame); dangling.put_u32(worldFrame + layout.cameraOffset, camera);
        CHECK(read_wow_camera(dangling, layout).failure == "camera struct unreadable");
        CHECK(read_wow_camera(FakeMemory{}, layout).failure == "static pointer unreadable");

        WowCameraLayout direct = layout; direct.directAddress = camera;
        CHECK(read_wow_camera(mem, direct).readOk && read_wow_camera(mem, direct).chainValue == 0);
    }

    // Sanity rejects: degrees instead of radians, skewed facing, far < near.
    {
        const Vec3 rows[3]{gpu.forward, neg(gpu.right), gpu.up};
        auto s = make_struct(layout, gpu.position, rows, .2f, 791.58f, 54.f, 1.7778f);
        CHECK(!memory_camera_sanity(decode_wow_camera(s.data(), layout, 1)).empty());
        const Vec3 skew[3]{gpu.forward, gpu.forward, gpu.up};
        s = make_struct(layout, gpu.position, skew, .2f, 791.58f, .94f, 1.7778f);
        CHECK(memory_camera_sanity(decode_wow_camera(s.data(), layout, 1)) == "facing rows not orthogonal");
        s = make_struct(layout, gpu.position, rows, .2f, .1f, .94f, 1.7778f);
        CHECK(memory_camera_sanity(decode_wow_camera(s.data(), layout, 1)) == "far clip implausible");
    }

    // Under the correct convention, a memory camera rebuilt with a synthesized
    // projection reproduces the real game's clip coordinates.
    {
        const auto mem = memory_from_gpu(gpu, layout);
        const auto proj = synthesize_projection(mem.fov, FovAxis::Vertical, mem.aspect, mem.nearClip, mem.farClip);
        const auto cam = memory_camera_to_derived(mem, SignedPermutation{{1, 3, -2}}, proj);
        CHECK(cam.valid);
        CHECK(cam.source == "wow-memory-cgcamera");
        CHECK_NEAR(cam.position.x, gpu.position.x, .05f);
        CHECK_NEAR(dot3(cam.forward, gpu.forward), 1.f, 1e-5f);
        CHECK_NEAR(dot3(cam.right, gpu.right), 1.f, 1e-5f);
        CHECK_NEAR(dot3(cam.up, gpu.up), 1.f, 1e-5f);
        CHECK(analyze_view_basis(cam.view).determinant < 0);
        const auto vpMem = mat4_mul(cam.projection, cam.view), vpGpu = mat4_mul(gpu.projection, gpu.view);
        const Vec4 point{gpu.position.x + gpu.forward.x * 20 + 3, gpu.position.y + gpu.forward.y * 20, gpu.position.z + gpu.forward.z * 20 - 1, 1};
        const auto a = mat4_mul_vec4(vpMem, point), b = mat4_mul_vec4(vpGpu, point);
        CHECK_NEAR(a.x / a.w, b.x / b.w, 2e-3f);
        CHECK_NEAR(a.y / a.w, b.y / b.w, 2e-3f);
        CHECK_NEAR(a.z / a.w, b.z / b.w, 1e-4f);
        // Horizontal interpretation of the same number is a different projection.
        const auto horizontal = synthesize_projection(mem.fov, FovAxis::Horizontal, mem.aspect, mem.nearClip, mem.farClip);
        CHECK(horizontal.c1.y > proj.c1.y * 1.3f);
        // A wrong row assignment (left used as right) mirrors the image: not the game's camera.
        const auto mirrored = memory_camera_to_derived(mem, SignedPermutation{{1, 3, 2}}, proj);
        CHECK(!mirrored.valid || dot3(mirrored.right, gpu.right) < -.99f);
    }

    // Comparator: the default hypothesis is detected as identity with ~0 residuals.
    {
        CameraSourceComparator cmp;
        const auto gpu2 = derive_known_wow_camera(realP, realV2);
        CHECK(gpu2.valid);
        for (int i = 0; i < 30; ++i) {
            const auto g = i % 3 == 0 ? gpu : i % 3 == 1 ? gpu2 : rotated_gpu(i * 11.f, -20.f + i, Vec3{-14400.f + i, 420.f, 20.f});
            CHECK(g.valid);
            cmp.add(memory_from_gpu(g, layout), g);
        }
        const auto& r = cmp.result();
        CHECK(r.have && r.stable);
        CHECK(r.world == SignedPermutation{});
        CHECK(r.rows == (SignedPermutation{{1, 3, -2}}));
        CHECK(r.latest.position < 1e-2f && r.latest.forwardDeg < .1f && r.latest.upDeg < .1f && r.latest.rightDeg < .1f);
    }

    // Comparator: an unexpected storage order AND a reflected world conversion
    // (gpu = (x, -y, z) of memory) are both recovered.
    {
        CameraSourceComparator cmp;
        SignedPermutation world; world.m[0] = 1; world.m[1] = -2; world.m[2] = 3; // gpu = world(mem); an involution, so mem = world(gpu)
        for (int i = 0; i < 30; ++i) {
            const auto g = rotated_gpu(i * 13.f, -30.f + 2 * i, Vec3{-8000.f + i * 3, 1500.f - i, 40.f});
            // Stored rows: up, forward, right (mem space).
            const Vec3 rows[3]{apply(world, g.up), apply(world, g.forward), apply(world, g.right)};
            auto s = make_struct(layout, apply(world, g.position), rows, .2f, 791.58f, .9425f, 1.7778f);
            cmp.add(decode_wow_camera(s.data(), layout, 1), g);
        }
        const auto& r = cmp.result();
        CHECK(r.stable);
        CHECK(r.world == world);
        CHECK(r.rows == (SignedPermutation{{2, 1, 3}}));
        CHECK(r.latest.position < 1e-2f && r.latest.forwardDeg < .1f);
    }

    // Unrelated memory never becomes stable.
    {
        CameraSourceComparator cmp;
        const Vec3 fixedRows[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        auto s = make_struct(layout, Vec3{100, 200, 300}, fixedRows, .2f, 791.58f, .9425f, 1.7778f);
        const auto frozen = decode_wow_camera(s.data(), layout, 1);
        for (int i = 0; i < 40; ++i) cmp.add(frozen, rotated_gpu(i * 9.f, i - 20.f, Vec3{-14400.f + 5 * i, 420.f, 20.f}));
        CHECK(!cmp.result().stable);
    }

    // Signature scan: finds the live struct, skips garbage and a default identity struct.
    {
        std::vector<uint8_t> block(0x4000);
        for (size_t i = 0; i < block.size(); ++i) block[i] = uint8_t((i * 2654435761u) >> 13);
        const Vec3 rows[3]{gpu.forward, neg(gpu.right), gpu.up};
        auto live = make_struct(layout, gpu.position, rows, .2f, 791.58f, .9425f, 1.7778f);
        std::memcpy(block.data() + 0x1234 - 0x1234 % 4, live.data(), live.size());
        const Vec3 identity[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        auto dead = make_struct(layout, Vec3{}, identity, .2f, 791.58f, .9425f, 1.7778f);
        std::memcpy(block.data() + 0x2000, dead.data(), dead.size());
        std::vector<CameraScanHit> hits;
        scan_camera_structs(block.data(), block.size(), 0x7000000, layout, hits, 16);
        CHECK(hits.size() == 1);
        CHECK(hits[0].address == 0x7000000 + 0x1234 - 0x1234 % 4);

        std::vector<uint64_t> addresses; std::vector<uint32_t> offsets;
        std::vector<uint8_t> pointers(64, 0);
        const uint32_t target = 0x3001000;
        const uint32_t exact = target, inside = target - 0x7E20, below = target - 0x20000;
        std::memcpy(pointers.data() + 8, &exact, 4); std::memcpy(pointers.data() + 16, &inside, 4); std::memcpy(pointers.data() + 24, &below, 4);
        find_pointers_to(pointers.data(), pointers.size(), 0x400000, target, 0x10000, addresses, offsets, 8);
        CHECK(addresses.size() == 2 && addresses[0] == 0x400008 && offsets[0] == 0 && addresses[1] == 0x400010 && offsets[1] == 0x7E20);
    }

    // /proc/<pid>/maps parsing.
    {
        const auto regions = parse_proc_maps(
            "00400000-00401000 r--p 00000000 00:2a 123 /games/wow/Wow.exe\n"
            "7f000000-7f100000 rw-p 00000000 00:00 0 \n"
            "garbage line\n"
            "f7f00000-f7f01000 r-xs 00000000 00:00 0 [vdso]\n");
        CHECK(regions.size() == 3);
        CHECK(regions[0].begin == 0x400000 && regions[0].end == 0x401000 && regions[0].readable && !regions[0].writable && regions[0].path == "/games/wow/Wow.exe");
        CHECK(regions[1].writable && regions[1].privateMapping && regions[1].path.empty());
        CHECK(!regions[2].privateMapping && regions[2].path == "[vdso]");
    }

    // The real reader: reads our own memory, fails (without faulting) on unmapped memory.
    {
        const auto& reader = self_memory_reader();
        const uint32_t local = 0xC0FFEE11;
        uint32_t out = 0;
        CHECK(reader.read_u32(uint64_t(uintptr_t(&local)), out) && out == local);
        CHECK(!reader.read_u32(0, out));
        const long page = sysconf(_SC_PAGESIZE);
        auto* two = static_cast<uint8_t*>(mmap(nullptr, size_t(page) * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        CHECK(two != MAP_FAILED);
        CHECK(munmap(two + page, size_t(page)) == 0);
        uint8_t buf[16];
        CHECK(reader.read(uint64_t(uintptr_t(two)), buf, 16));
        CHECK(!reader.read(uint64_t(uintptr_t(two + page)), buf, 16));
        CHECK(!reader.read(uint64_t(uintptr_t(two + page - 8)), buf, 16));
        munmap(two, size_t(page));
        CHECK(!read_self_maps().empty());
    }

    // Background scanner over this test process: finds a heap camera struct and
    // the static -> +0x7E20 -> camera chain placed below 0x02000000.
    {
        const Vec3 rows[3]{gpu.forward, neg(gpu.right), gpu.up};
        auto* live = new std::vector<uint8_t>(make_struct(layout, gpu.position, rows, .2f, 791.58f, .9425f, 1.7778f));
        const uint32_t camera = uint32_t(uintptr_t(live->data()));
        auto* worldFrame = new std::vector<uint32_t>(0x8000 / 4, 0);
        (*worldFrame)[0x7E20 / 4] = camera;
        void* fixed = mmap(reinterpret_cast<void*>(0x01000000), 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        const bool haveStatic = fixed == reinterpret_cast<void*>(0x01000000);
        if (haveStatic) { const uint32_t frame = uint32_t(uintptr_t(worldFrame->data())); std::memcpy(static_cast<uint8_t*>(fixed) + 0x36C, &frame, 4); }
        MemoryCameraScanner scanner;
        CHECK(scanner.start(layout));
        CHECK(!scanner.start(layout));
        for (int i = 0; i < 600 && !scanner.status().finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto st = scanner.status();
        CHECK(st.finished && !st.running && st.generation == 1);
        CHECK(st.bytesScanned > 0 && st.bytesScanned <= st.bytesPlanned);
        bool found = false;
        for (auto& h : st.hits) found |= h.address == camera && memory_camera_sanity(h.raw).empty();
        CHECK(found);
        if (haveStatic) {
            bool chain = false;
            for (auto& c : st.chains) chain |= c.staticAddress == 0x0100036C && c.offset == 0x7E20 && c.camera == camera;
            CHECK(chain);
            munmap(fixed, 4096);
        } else std::puts("note: 0x01000000 unavailable, chain search not exercised");
        delete worldFrame;
        delete live;
    }

    std::puts("PASS: memory camera layout/chain/sanity, real-capture reprojection, conversion detection, scan, background scanner + chain, maps, safe self reads");
    return 0;
}
