#pragma once
#include <cstdint>
#include <string>

// Pure camera-derivation math: turns a 4x4 projection + a 4x4 view matrix
// into a usable camera (position/basis/FOV/near/far). No Vulkan dependency,
// unit-testable standalone (tests/camera_math_test.cpp).
//
// Convention is explicit and NOT guessed: `Mat4Cols` mirrors the GLSL
// `mat4(c0,c1,c2,c3)` constructor used by shaders/water_camera.frag exactly -
// each Vec4 argument is a COLUMN, and `raw[16]` (as produced by
// MatrixAnalyzer/CameraAnalyzer, 16 floats in ascending memory order) reads
// as 4 consecutive columns in that same order. `derive_known_wow_camera()`
// applies this convention because it is verified from the actual shader
// source, not assumed - see its doc comment. For a matrix of UNKNOWN
// convention, callers should try both `read_columns(raw)` and
// `read_columns` of the transpose and compare results against a trusted
// reference (e.g. `compare_camera` against a verified anchor) rather than
// picking one blindly.
namespace depth_probe {
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct Mat4Cols { Vec4 c0, c1, c2, c3; };

Mat4Cols read_columns(const float raw[16]);
Vec4 mat4_mul_vec4(const Mat4Cols& m, const Vec4& v);
// result = a * b (apply b first, then a) - e.g. viewProjection = mat4_mul(projection, view).
Mat4Cols mat4_mul(const Mat4Cols& a, const Mat4Cols& b);
// Returns false (out untouched) if singular. Reuses matrix_analyzer's
// cofactor solver internally rather than reimplementing it.
bool mat4_inverse(const Mat4Cols& m, Mat4Cols& out);

struct DerivedCamera {
    bool valid = false;
    std::string invalidReason;  // set when valid is false
    std::string source;         // e.g. "known-wow-water-anchor", "generic-columns", "generic-transposed"

    Vec3 position, right, up, forward;
    float fovY = 0, aspect = 0, nearPlane = 0, farPlane = 0;
    Mat4Cols view, projection, inverseView, inverseProjection;
};

// Applies the EXACT convention verified from shaders/water_camera.frag:
// P = mat4(projRaw as 4 columns), M = mat4(viewRaw as 4 columns) (world ->
// view), position = inverseView.c3.xyz / .w, near/far from P.c3.z/P.c2.z,
// FOV from P.c1.y, aspect from P.c1.y/P.c0.x. Structural validity (finite,
// P's sparsity pattern, M's orthonormal upper-left 3x3) is checked the same
// way the shader checks it before trusting the result.
DerivedCamera derive_known_wow_camera(const float projRaw[16], const float viewRaw[16]);

// For a matrix pair of UNKNOWN convention: same derivation, but the caller
// picks which of the two plausible readings (`raw[16]` as columns directly,
// or as columns of the transpose) to try. Structural validity is checked;
// a mathematically nonsensical near/far/FOV (non-finite, negative, or a
// degenerate near>=far) marks the result invalid rather than silently
// returning nonsense.
DerivedCamera derive_generic_camera(const float projRaw[16], const float viewRaw[16], bool transposed, const std::string& source);

struct CameraComparison {
    float positionError = -1;     // world units, -1 if not computable
    float orientationDot = -2;    // forward.forward dot product, 1 = identical, -2 if not computable
    float fovDeltaDegrees = -1;
};
CameraComparison compare_camera(const DerivedCamera& a, const DerivedCamera& b);

float mat4_determinant(const Mat4Cols& m);

// Raw structural health of a view-like matrix's rotation part (columns 0..2,
// BEFORE any normalization) - exposes exactly what derive_known_wow_camera's
// internal validity gate checks, as visible numbers rather than a pass/fail,
// per the user's explicit "do not automatically claim this is the canonical
// WoW camera merely because the math succeeds" instruction. An orthonormal
// rotation has all three lengths ~1 and all three dot products ~0; det
// near -1 (rather than +1) means the transform includes a reflection/
// handedness flip, which is real, load-bearing information - not an error.
struct BasisHealth {
    float determinant = 0;
    float rightLen = 0, upLen = 0, forwardLen = 0;       // |column 0|, |column 1|, |column 2|
    float rightDotUp = 0, rightDotForward = 0, upDotForward = 0;
    bool orthonormal = false; // all lengths within .01 of 1 and all dots within .01 of 0
};
BasisHealth analyze_view_basis(const Mat4Cols& view);
}
