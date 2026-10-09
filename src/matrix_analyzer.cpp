#include "matrix_analyzer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace depth_probe {
namespace {
float at(const float m[16], int row, int col) { return m[row * 4 + col]; }

float determinant4x4(const float m[16]) {
    // Standard cofactor expansion along the first row.
    auto det3 = [](float a1, float a2, float a3, float b1, float b2, float b3, float c1, float c2, float c3) {
        return a1 * (b2 * c3 - b3 * c2) - a2 * (b1 * c3 - b3 * c1) + a3 * (b1 * c2 - b2 * c1);
    };
    const float a = at(m, 0, 0), b = at(m, 0, 1), c = at(m, 0, 2), d = at(m, 0, 3);
    const float m10 = at(m, 1, 0), m11 = at(m, 1, 1), m12 = at(m, 1, 2), m13 = at(m, 1, 3);
    const float m20 = at(m, 2, 0), m21 = at(m, 2, 1), m22 = at(m, 2, 2), m23 = at(m, 2, 3);
    const float m30 = at(m, 3, 0), m31 = at(m, 3, 1), m32 = at(m, 3, 2), m33 = at(m, 3, 3);
    return a * det3(m11, m12, m13, m21, m22, m23, m31, m32, m33)
         - b * det3(m10, m12, m13, m20, m22, m23, m30, m32, m33)
         + c * det3(m10, m11, m13, m20, m21, m23, m30, m31, m33)
         - d * det3(m10, m11, m12, m20, m21, m22, m30, m31, m32);
}

// Whether a length-4 vector looks like (0,0,+-1,0): the shape of the
// row/column that turns an input's Z (depth) into the clip-space W a
// perspective divide uses. The dominant slot must specifically be index 2
// (Z), not index 3 (W) - a plain (0,0,0,1) is just the ordinary affine
// identity row/column that every rigid transform has (see
// lastRowAffine/lastColAffine) and is not, by itself, evidence of a
// projection. This follows from what "perspective" means (depth drives the
// divide), not from any register offset specific to a shader or engine.
struct DivideCheck { bool matches = false; float cleanliness = 1.f; };
DivideCheck divide_pattern(const float v[4]) {
    const float cleanliness = std::fabs(std::fabs(v[2]) - 1.f) + std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[3]);
    return {cleanliness < .08f, cleanliness};
}

struct Vec3 { float x, y, z; };
float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Max deviation from "these three vectors are mutually orthonormal" across
// unit length (all three) and pairwise orthogonality (all three pairs).
float orthonormal_error(Vec3 a, Vec3 b, Vec3 c) {
    float e = 0.f;
    for (float len : {length(a), length(b), length(c)}) e = std::max(e, std::fabs(len - 1.f));
    e = std::max({e, std::fabs(dot(a, b)), std::fabs(dot(a, c)), std::fabs(dot(b, c))});
    return e;
}

bool near_affine_row(const float row[4]) {
    return std::fabs(row[0]) < .02f && std::fabs(row[1]) < .02f && std::fabs(row[2]) < .02f && std::fabs(row[3] - 1.f) < .02f;
}

void classify(MatrixCandidate& cand) {
    const float* m = cand.raw;
    cand.determinant = determinant4x4(m);

    cand.looksLikeIdentity = true;
    for (int r = 0; r < 4 && cand.looksLikeIdentity; ++r)
        for (int c = 0; c < 4; ++c) {
            const float expected = r == c ? 1.f : 0.f;
            if (std::fabs(at(m, r, c) - expected) > 1e-3f) { cand.looksLikeIdentity = false; break; }
        }

    const Vec3 row0{at(m,0,0),at(m,0,1),at(m,0,2)}, row1{at(m,1,0),at(m,1,1),at(m,1,2)}, row2{at(m,2,0),at(m,2,1),at(m,2,2)};
    const Vec3 col0{at(m,0,0),at(m,1,0),at(m,2,0)}, col1{at(m,0,1),at(m,1,1),at(m,2,1)}, col2{at(m,0,2),at(m,1,2),at(m,2,2)};
    const float rowErr = orthonormal_error(row0, row1, row2);
    const float colErr = orthonormal_error(col0, col1, col2);
    cand.rowOrthonormal = rowErr < .05f;
    cand.colOrthonormal = colErr < .05f;
    cand.orthonormalError = std::min(rowErr, colErr);

    const float row3[4]{at(m,3,0),at(m,3,1),at(m,3,2),at(m,3,3)};
    const float col3[4]{at(m,0,3),at(m,1,3),at(m,2,3),at(m,3,3)};
    cand.lastRowAffine = near_affine_row(row3);
    cand.lastColAffine = near_affine_row(col3);

    for (int r = 0; r < 4; ++r) {
        const float row[4]{at(m,r,0),at(m,r,1),at(m,r,2),at(m,r,3)};
        if (divide_pattern(row).matches) { cand.divideRow = r; break; }
    }
    for (int c = 0; c < 4; ++c) {
        const float col[4]{at(m,0,c),at(m,1,c),at(m,2,c),at(m,3,c)};
        if (divide_pattern(col).matches) { cand.divideCol = c; break; }
    }

    char buf[96];
    std::snprintf(buf, sizeof buf, "determinant = %.6f", double(cand.determinant));
    cand.reasons.emplace_back(buf);

    const bool hasDivide = cand.divideRow >= 0 || cand.divideCol >= 0;
    const bool hasOrthonormal = (cand.rowOrthonormal || cand.colOrthonormal) && std::fabs(std::fabs(cand.determinant) - 1.f) < .1f;

    if (cand.looksLikeIdentity) {
        cand.classification = "Identity / unused";
        cand.confidence = .8f;
        cand.reasons.emplace_back("all diagonal entries ~1, all off-diagonal entries ~0");
    } else if (std::fabs(cand.determinant) < 1e-6f) {
        cand.classification = "Degenerate (near-singular)";
        cand.confidence = .05f;
        cand.reasons.emplace_back("determinant is ~0: not an invertible transform, unlikely to be a usable matrix");
    } else if (hasDivide && hasOrthonormal) {
        // A sparse single-+-1-per-row block (e.g. an axis-aligned rotation or
        // reflection, or a square-FOV/1:1-aspect projection) satisfies both
        // tests at once. Say so rather than silently picking one.
        cand.classification = "Ambiguous (axis-aligned rotation/reflection, or square-FOV projection)";
        cand.confidence = .15f;
        cand.reasons.emplace_back("matches both the perspective-divide pattern and an orthonormal 3x3 block - these tests cannot disambiguate a sparse/axis-aligned matrix");
        cand.reasons.emplace_back("needs temporal or cross-draw evidence: a real projection stays fixed while the camera moves, a rotation changes with camera orientation");
    } else if (hasDivide) {
        cand.classification = "Projection (candidate)";
        cand.confidence = .5f;
        if (cand.divideRow >= 0) {
            std::snprintf(buf, sizeof buf, "row %d matches a perspective-divide pattern (one component near +-1, rest ~0)", cand.divideRow);
            cand.reasons.emplace_back(buf);
        }
        if (cand.divideCol >= 0) {
            std::snprintf(buf, sizeof buf, "column %d matches a perspective-divide pattern (one component near +-1, rest ~0)", cand.divideCol);
            cand.reasons.emplace_back(buf);
        }
        cand.reasons.emplace_back("row/column-major convention not determined - the divide component may read as a row here or a column under the other convention");
    } else if (hasOrthonormal) {
        cand.classification = "View or World (rotation/translation candidate)";
        cand.confidence = .25f + .15f * (1.f - std::min(cand.orthonormalError, 1.f));
        std::snprintf(buf, sizeof buf, "upper-left 3x3 %s orthonormal (max error %.4f), |determinant| ~ 1",
                       cand.rowOrthonormal && cand.colOrthonormal ? "rows and columns" : cand.rowOrthonormal ? "rows" : "columns",
                       double(cand.orthonormalError));
        cand.reasons.emplace_back(buf);
        cand.reasons.emplace_back("cannot distinguish View from World/WorldView from a single snapshot - needs cross-frame or cross-draw comparison");
    } else if (cand.lastRowAffine || cand.lastColAffine) {
        cand.classification = "Affine transform (candidate: scale/shear present)";
        cand.confidence = .2f;
        cand.reasons.emplace_back(cand.lastRowAffine ? "row 3 ~= (0,0,0,1)" : "column 3 ~= (0,0,0,1)");
    } else {
        cand.classification = "Unknown (well-formed 4x4, no recognized structure)";
        cand.confidence = .1f;
    }
    cand.reasons.emplace_back("confidence reflects single-snapshot mathematical structure only; no temporal or cross-draw evidence collected yet");
}
}

std::vector<MatrixCandidate> find_matrix_candidates(const std::vector<uint8_t>& bytes, size_t maxCandidates) {
    std::vector<MatrixCandidate> out;
    if (bytes.size() < 64) return out;
    for (size_t offset = 0; offset + 64 <= bytes.size(); offset += 16) {
        MatrixCandidate cand;
        cand.offset = offset;
        std::copy_n(reinterpret_cast<const float*>(bytes.data() + offset), 16, cand.raw);
        bool finite = true;
        for (float v : cand.raw) if (!std::isfinite(v)) { finite = false; break; }
        if (!finite) continue;
        classify(cand);
        out.push_back(std::move(cand));
    }
    std::stable_sort(out.begin(), out.end(), [](const MatrixCandidate& a, const MatrixCandidate& b) { return a.confidence > b.confidence; });
    if (out.size() > maxCandidates) out.resize(maxCandidates);
    return out;
}

void transpose4x4(const float in_[16], float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[c * 4 + r] = in_[r * 4 + c];
}

bool invert4x4(const float in_[16], float out[16]) {
    // Cofactor / adjugate method; readable over fast for a debug-only tool.
    float cof[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sub[9]; int k = 0;
            for (int rr = 0; rr < 4; ++rr) if (rr != r) for (int cc = 0; cc < 4; ++cc) if (cc != c) sub[k++] = in_[rr * 4 + cc];
            const float minor = sub[0] * (sub[4] * sub[8] - sub[5] * sub[7])
                              - sub[1] * (sub[3] * sub[8] - sub[5] * sub[6])
                              + sub[2] * (sub[3] * sub[7] - sub[4] * sub[6]);
            cof[r * 4 + c] = ((r + c) % 2 ? -1.f : 1.f) * minor;
        }
    }
    float det = 0.f;
    for (int c = 0; c < 4; ++c) det += in_[c] * cof[c];
    if (std::fabs(det) < 1e-6f) return false;
    const float inv = 1.f / det;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[c * 4 + r] = cof[r * 4 + c] * inv; // adjugate = cofactor transpose
    return true;
}
}
