#pragma once
#include "camera_math.hpp"
#include "process_memory.hpp"
#include <cstdint>
#include <string>
#include <vector>

// WoW 3.3.5a (12340) camera read from the game's own memory, as a second
// camera source independent of the GPU (water UBO) one. Pure logic over a
// MemoryReader, unit-tested with a fake memory image
// (tests/memory_camera_test.cpp). Every layout constant below is a
// HYPOTHESIS (community knowledge, not verified in this repository); the
// sanity checks, the comparison against the GPU-derived camera and the
// signature scan exist to confirm or replace them with live evidence.
namespace depth_probe {

// out[i] = sign(m[i]) * in[|m[i]| - 1]. Used both for world-axis conversions
// (in/out = x,y,z) and for facing-row assignments (in = facing rows 0..2,
// out = forward, up, right).
struct SignedPermutation {
    int8_t m[3]{1, 2, 3};
    bool operator==(const SignedPermutation& o) const { return m[0] == o.m[0] && m[1] == o.m[1] && m[2] == o.m[2]; }
    bool operator!=(const SignedPermutation& o) const { return !(*this == o); }
};
Vec3 apply(const SignedPermutation&, const Vec3&);
const std::vector<SignedPermutation>& signed_permutations(); // all 48, identity first
std::string describe_world(const SignedPermutation&);        // "+X+Y+Z"
std::string describe_rows(const SignedPermutation&);         // "F+R0 U+R2 R-R1"
bool parse_signed_permutation(const std::string&, SignedPermutation&); // "+1,+3,-2"

struct WowCameraLayout {
    uint64_t staticPointer = 0x00B7436C; // holds the world frame pointer
    uint32_t cameraOffset = 0x7E20;      // world frame + this holds the CGCamera pointer
    uint64_t directAddress = 0;          // nonzero: read the struct here, skip the chain
    uint32_t position = 0x08, facing = 0x14, nearClip = 0x38, farClip = 0x3C, fov = 0x40, aspect = 0x44;
    uint32_t span() const;               // bytes from the struct base covering every field
};
// DEPTH_PROBE_MEM_CAMERA_STATIC / _OFFSET / _ADDR / _FIELDS ("pos,facing,near,far,fov,aspect", hex ok).
WowCameraLayout wow_camera_layout_from_env();

struct RawMemoryCamera {
    bool readOk = false;
    std::string failure;       // which read failed, when !readOk
    uint64_t chainValue = 0;   // value at layout.staticPointer (0 when a direct address was used)
    uint64_t address = 0;      // struct base
    Vec3 position;
    Vec3 rows[3];              // the 3x3 facing matrix, row by row as stored
    float nearClip = 0, farClip = 0, fov = 0, aspect = 0;
};
RawMemoryCamera decode_wow_camera(const uint8_t* structBytes, const WowCameraLayout&, uint64_t address);
RawMemoryCamera read_wow_camera(const MemoryReader&, const WowCameraLayout&);
// Empty when plausible; otherwise the first failed check.
std::string memory_camera_sanity(const RawMemoryCamera&);

enum class FovAxis { Vertical, Horizontal };
// D3D-style +Z-forward 0..1 projection with the same structure as the WoW water UBO's.
Mat4Cols synthesize_projection(float fovRadians, FovAxis, float aspect, float nearClip, float farClip);
// View built in the water camera's convention (view x = right, y = up, z = forward).
// `rows` picks (forward, up, right) from the facing rows. invalid if the result is not a sane camera.
DerivedCamera memory_camera_to_derived(const RawMemoryCamera&, const SignedPermutation& rows, const Mat4Cols& projection);

struct CameraPairResiduals { float position = -1, forwardDeg = -1, upDeg = -1, rightDeg = -1; };
// gpu ~ world(memory) with (F,U,R) = rows(facing rows) in memory space.
CameraPairResiduals pair_residuals(const RawMemoryCamera&, const DerivedCamera& gpu, const SignedPermutation& world, const SignedPermutation& rows);

struct CameraComparatorResult {
    bool have = false;
    SignedPermutation world, rows;
    CameraPairResiduals latest;          // most recent pair under the best (world, rows)
    float meanScore = 0, runnerUpScore = 0;
    uint32_t samples = 0;
    bool stable = false;                 // enough samples and the best clearly beats every other conversion
};
// Detects the fixed conversion between the memory camera and a GPU-derived
// camera from time-paired samples: all 48 world-axis signed permutations x
// all 48 facing-row assignments, scored by summed basis angle error plus
// clamped position error, accumulated with decay so a mistaken early lock
// can recover.
class CameraSourceComparator {
public:
    void add(const RawMemoryCamera& memory, const DerivedCamera& gpu);
    const CameraComparatorResult& result() const { return current; }
    void reset();
private:
    std::vector<float> scores;
    float weight = 0;
    CameraComparatorResult current;
};

struct CameraScanHit { uint64_t address = 0; RawMemoryCamera raw; };
// Every 4-byte-aligned struct base in [baseAddress, baseAddress+size) whose
// fields (at `layout` offsets) pass memory_camera_sanity.
void scan_camera_structs(const uint8_t* bytes, size_t size, uint64_t baseAddress, const WowCameraLayout&, std::vector<CameraScanHit>& out, size_t maxHits);
// Aligned addresses in the block holding a 4-byte value in [target - maxBelow, target];
// `offsets` receives target - value for each hit.
void find_pointers_to(const uint8_t* bytes, size_t size, uint64_t baseAddress, uint32_t target, uint32_t maxBelow,
                      std::vector<uint64_t>& addresses, std::vector<uint32_t>& offsets, size_t maxHits);
}
