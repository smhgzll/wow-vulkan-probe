#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure-math candidate-matrix detection for arbitrary captured constant/uniform
// buffer bytes. Has no Vulkan or WoW-specific knowledge and makes no claim
// about row-major vs column-major convention, handedness or which shader
// register layout a game uses - it only scores mathematical structure that is
// visible from the raw bytes of a single snapshot (determinant, orthonormal
// blocks, perspective-divide patterns). Classification is always a labelled
// hypothesis with a bounded confidence and explicit reasons, never a claim of
// certainty; see the confidence cap note baked into every candidate.
//
// Multi-frame / multi-draw evidence (stability across frames, shared values
// across different draws, correlation with camera movement) is a separate,
// heavier concern and deliberately out of scope for this module - it is the
// next layer to build once this one is in place.
namespace depth_probe {
struct MatrixCandidate {
    size_t offset = 0;   // byte offset of this 4x4 block within the buffer
    float raw[16]{};     // 16 floats read in ascending memory order (raw[0..3]
                          // is the first 16 bytes at `offset`, etc.) - not yet
                          // assigned a row/col-major interpretation
    float determinant = 0.f;
    bool rowOrthonormal = false;   // rows 0..2, truncated to xyz, mutually orthonormal
    bool colOrthonormal = false;   // columns 0..2, truncated to xyz, mutually orthonormal
    float orthonormalError = 1.f;  // best (lowest) of the row/col checks; 0 = exact
    bool lastRowAffine = false;    // row 3 ~= (0,0,0,1)
    bool lastColAffine = false;    // column 3 ~= (0,0,0,1)
    bool looksLikeIdentity = false;
    int divideRow = -1;   // row index whose pattern matches a perspective-divide
                           // vector (exactly one component near +-1, rest near 0),
                           // or -1 if none found
    int divideCol = -1;   // same check across columns
    std::string classification = "Unknown";
    float confidence = 0.f;        // 0..1, see per-classification cap in the .cpp
    std::vector<std::string> reasons;
};

// Scans `bytes` at every 16-byte-aligned offset for a 64-byte (16 float)
// window and scores it as a candidate 4x4 matrix. Returns at most
// `maxCandidates` results, highest confidence first. 16-byte alignment is a
// structural assumption (D3D/HLSL constant registers and std140/std430
// layouts both pack a float4 - and therefore a mat4 row/column - on a 16-byte
// boundary), not a hardcoded per-shader offset.
std::vector<MatrixCandidate> find_matrix_candidates(const std::vector<uint8_t>& bytes, size_t maxCandidates = 8);

// raw[] interpreted as a plain 4x4 array M[row][col] = raw[row*4+col],
// transposed in place (M[row][col] <-> M[col][row]). Provided because a
// candidate's row-major vs column-major meaning is exactly what is NOT known
// from a single snapshot - callers should look at both.
void transpose4x4(const float in_[16], float out[16]);

// Cofactor-expansion inverse. Returns false (out left untouched) if the
// matrix is singular (|determinant| below a small epsilon), matching
// `MatrixCandidate::determinant` for the same input.
bool invert4x4(const float in_[16], float out[16]);
}
