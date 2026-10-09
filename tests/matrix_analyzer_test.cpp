#include "matrix_analyzer.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace depth_probe;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)

namespace {
std::vector<uint8_t> bytes_of(const std::vector<float>& floats) {
    std::vector<uint8_t> out(floats.size() * 4);
    std::memcpy(out.data(), floats.data(), out.size());
    return out;
}
const MatrixCandidate* best(const std::vector<MatrixCandidate>& c) { return c.empty() ? nullptr : &c.front(); }
}

int main() {
    // Buffer too small to hold any 4x4 window.
    CHECK(find_matrix_candidates(std::vector<uint8_t>(63, 0)).empty());

    // Plain identity.
    {
        std::vector<float> m{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        auto cands = find_matrix_candidates(bytes_of(m));
        auto* c = best(cands);
        CHECK(c && c->classification == "Identity / unused");
        CHECK(c->confidence > .5f);
    }

    // A projection matrix built to match the documented real-WoW water
    // camera structure (docs/water.md / wow-water-camera-semantics memory):
    // FOV scale factors far from 1, a z-remap row with two nonzero terms
    // (near/far coefficients ~1.00025 / ~-0.20005), and a clean (0,0,1,0)
    // divide row - not simultaneously orthonormal because sx/sy != 1.
    {
        std::vector<float> m{
            1.732f, 0.f,    0.f,      0.f,
            0.f,    2.309f, 0.f,      0.f,
            0.f,    0.f,    1.00025f, -0.20005f,
            0.f,    0.f,    1.f,       0.f,
        };
        auto cands = find_matrix_candidates(bytes_of(m));
        auto* c = best(cands);
        CHECK(c && c->classification == "Projection (candidate)");
        CHECK(c->divideRow == 3);
        CHECK(!c->rowOrthonormal && !c->colOrthonormal);
        CHECK(c->confidence > .3f && c->confidence < .7f);
    }

    // A non-axis-aligned rotation (30 degrees around Y) + translation: the
    // shape of an ordinary View/World matrix. Must NOT be mistaken for a
    // projection (no row/column has a clean Z-slot divide pattern).
    {
        const float cs = 0.8660254f, sn = 0.5f;
        std::vector<float> m{
            cs,  0.f, sn,  120.5f,
            0.f, 1.f, 0.f, 45.25f,
            -sn, 0.f, cs,  -300.75f,
            0.f, 0.f, 0.f, 1.f,
        };
        auto cands = find_matrix_candidates(bytes_of(m));
        auto* c = best(cands);
        CHECK(c && c->classification == "View or World (rotation/translation candidate)");
        CHECK(c->rowOrthonormal);
        CHECK(c->divideRow == -1 && c->divideCol == -1);
        CHECK(std::fabs(std::fabs(c->determinant) - 1.f) < .01f);
    }

    // An axis-aligned reflection (matches the documented real-WoW water
    // reflection-plane transform shape: orthonormal columns, det ~ -1).
    // This is a genuine structural ambiguity (a sparse orthonormal row is
    // indistinguishable from a divide row from one snapshot) - must be
    // reported as ambiguous, not confidently classified either way.
    {
        std::vector<float> m{
            1.f, 0.f,  0.f, 10.f,
            0.f, 1.f,  0.f, 20.f,
            0.f, 0.f, -1.f, 30.f,
            0.f, 0.f,  0.f, 1.f,
        };
        auto cands = find_matrix_candidates(bytes_of(m));
        auto* c = best(cands);
        CHECK(c && c->classification.substr(0, 9) == "Ambiguous");
        CHECK(c->confidence < .3f);
        CHECK(std::fabs(c->determinant + 1.f) < .01f);
    }

    // Singular / degenerate.
    {
        std::vector<float> m{0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
        auto cands = find_matrix_candidates(bytes_of(m));
        auto* c = best(cands);
        CHECK(c && c->classification == "Degenerate (near-singular)");
        CHECK(c->confidence < .1f);
    }

    // No recognizable structure at all.
    {
        std::vector<float> m{3,7,1,9, 2,8,4,6, 5,1,9,2, 4,6,3,8};
        auto cands = find_matrix_candidates(bytes_of(m));
        auto* c = best(cands);
        CHECK(c && c->classification == "Unknown (well-formed 4x4, no recognized structure)");
    }

    // Scanning: a real candidate embedded at a non-zero, non-first offset
    // inside a larger buffer (zero padding before and after) must be found
    // with the correct byte offset, and must rank above the degenerate
    // all-zero padding windows.
    {
        std::vector<float> buffer(64, 0.f); // 256 bytes of padding
        const float cs = 0.8660254f, sn = 0.5f;
        const std::vector<float> rot{cs,0,sn,0, 0,1,0,0, -sn,0,cs,0, 0,0,0,1};
        const size_t floatOffset = 20; // byte 80, 16-byte aligned
        std::copy(rot.begin(), rot.end(), buffer.begin() + floatOffset);
        auto cands = find_matrix_candidates(bytes_of(buffer));
        CHECK(!cands.empty());
        CHECK(cands.front().offset == floatOffset * 4);
        CHECK(cands.front().classification == "View or World (rotation/translation candidate)");
    }

    // transpose4x4 and invert4x4.
    {
        float m[16]{1,2,3,4, 0,1,4,5, 5,6,0,1, 6,7,2,1};
        float t[16]; transpose4x4(m, t);
        CHECK(t[1] == m[4] && t[4] == m[1] && t[15] == m[15]);
        float inv[16];
        CHECK(invert4x4(m, inv));
        // m * inv ~= identity.
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
            float sum = 0; for (int k = 0; k < 4; ++k) sum += m[r*4+k] * inv[k*4+c];
            CHECK(std::fabs(sum - (r == c ? 1.f : 0.f)) < 1e-3f);
        }
        float singular[16]{0,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        float out[16];
        CHECK(!invert4x4(singular, out));
    }

    // maxCandidates is honoured and results are sorted by confidence.
    {
        std::vector<float> buffer(400, 0.f);
        for (size_t w = 0; w + 16 <= buffer.size(); w += 4) buffer[w] = 1.f; // scattered noise, some windows near-identity-ish
        std::vector<float> id{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::copy(id.begin(), id.end(), buffer.begin() + 40);
        auto cands = find_matrix_candidates(bytes_of(buffer), 3);
        CHECK(cands.size() <= 3);
        for (size_t i = 1; i < cands.size(); ++i) CHECK(cands[i-1].confidence >= cands[i].confidence);
    }

    std::puts("PASS: identity/projection/rotation/ambiguous/degenerate/unknown classification, offset scanning, transpose, inverse, candidate cap");
}
