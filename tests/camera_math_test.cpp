#include "camera_math.hpp"
#include "matrix_analyzer.hpp"
#include <cmath>
#include <cstdio>
using namespace depth_probe;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
#define CHECK_NEAR(a,b,eps) CHECK(std::fabs((a)-(b))<(eps))

int main() {
    // Hand-derived known-good pair, built to satisfy the verified WoW
    // structure exactly (see derive_known_wow_camera's doc comment):
    // near=1, far=101 (solved from Pzz=far/(far-1)=1.01, Pwz=-Pzz=-1.01),
    // FOV from sy=1.5 -> 2*atan(1/1.5) ~= 67.3805 deg, aspect=sy/sx=0.75,
    // identity rotation + translation (100,50,30) -> camera position
    // (-100,-50,-30) since inverse(translation-only M) negates the
    // translation and the formula reads it off column 3.
    const float projRaw[16]{2,0,0,0, 0,1.5f,0,0, 0,0,1.01f,1, 0,0,-1.01f,0};
    const float viewRaw[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 100,50,30,1};

    auto cam = derive_known_wow_camera(projRaw, viewRaw);
    CHECK(cam.valid);
    CHECK(cam.source == "known-wow-water-anchor");
    CHECK_NEAR(cam.position.x, -100.f, 1e-3f);
    CHECK_NEAR(cam.position.y, -50.f, 1e-3f);
    CHECK_NEAR(cam.position.z, -30.f, 1e-3f);
    CHECK_NEAR(cam.nearPlane, 1.f, 1e-3f);
    CHECK_NEAR(cam.farPlane, 101.f, 1e-2f);
    CHECK_NEAR(cam.fovY, 67.3805f, .05f);
    CHECK_NEAR(cam.aspect, .75f, 1e-3f);
    CHECK_NEAR(cam.forward.x, 0.f, 1e-4f); CHECK_NEAR(cam.forward.y, 0.f, 1e-4f); CHECK_NEAR(cam.forward.z, 1.f, 1e-4f);
    CHECK_NEAR(cam.right.x, 1.f, 1e-4f); CHECK_NEAR(cam.up.y, 1.f, 1e-4f);

    // Garbage input must not silently produce a plausible-looking camera.
    {
        const float garbage[16]{3,7,1,9, 2,8,4,6, 5,1,9,2, 4,6,3,8};
        auto bad = derive_known_wow_camera(garbage, garbage);
        CHECK(!bad.valid);
        CHECK(!bad.invalidReason.empty());
    }

    // A structurally-valid-looking projection paired with a NON-orthonormal
    // "view" (garbage rotation part) must be rejected by the structural
    // check, not silently accepted.
    {
        const float badView[16]{1,2,3,0, 4,5,6,0, 7,8,9,0, 100,50,30,1};
        auto bad = derive_known_wow_camera(projRaw, badView);
        CHECK(!bad.valid);
    }

    // Generic derivation, correct convention (transposed=false) on the same
    // known-good data must match the verified path exactly.
    {
        auto generic = derive_generic_camera(projRaw, viewRaw, false, "generic-columns");
        CHECK(generic.valid);
        CHECK(generic.source == "generic-columns");
        CHECK_NEAR(generic.position.x, -100.f, 1e-3f);
    }

    // Generic derivation with the WRONG convention flag on data that is
    // pre-transposed must still recover the correct camera once un-
    // transposed - this is the "try both conventions" path for shaders
    // whose layout isn't independently verified.
    {
        float projT[16], viewT[16];
        transpose4x4(projRaw, projT);
        transpose4x4(viewRaw, viewT);
        auto recovered = derive_generic_camera(projT, viewT, true, "generic-transposed");
        CHECK(recovered.valid);
        CHECK_NEAR(recovered.position.x, -100.f, 1e-3f);
        CHECK_NEAR(recovered.fovY, 67.3805f, .05f);
    }

    // compare_camera: identical camera -> zero error, dot=1.
    {
        auto same = compare_camera(cam, cam);
        CHECK_NEAR(same.positionError, 0.f, 1e-3f);
        CHECK_NEAR(same.orientationDot, 1.f, 1e-4f);
        CHECK_NEAR(same.fovDeltaDegrees, 0.f, 1e-3f);
    }
    // compare_camera: a camera moved by a known offset -> exact error.
    {
        const float viewRaw2[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 103,50,30,1}; // world +3 in X vs. position -3 in X
        auto moved = derive_known_wow_camera(projRaw, viewRaw2);
        CHECK(moved.valid);
        auto diff = compare_camera(cam, moved);
        CHECK_NEAR(diff.positionError, 3.f, 1e-3f);
    }
    // compare_camera: one side invalid -> sentinel values, not garbage math.
    {
        DerivedCamera invalidCam;
        auto none = compare_camera(cam, invalidCam);
        CHECK(none.positionError < 0 && none.orientationDot < -1);
    }

    // Basis health on the known-good view matrix (identity rotation +
    // translation): a clean orthonormal block, det=+1 (no reflection).
    {
        const auto view = read_columns(viewRaw);
        auto health = analyze_view_basis(view);
        CHECK(health.orthonormal);
        CHECK_NEAR(health.determinant, 1.f, 1e-3f);
        CHECK_NEAR(health.rightLen, 1.f, 1e-3f); CHECK_NEAR(health.upLen, 1.f, 1e-3f); CHECK_NEAR(health.forwardLen, 1.f, 1e-3f);
        CHECK_NEAR(health.rightDotUp, 0.f, 1e-3f); CHECK_NEAR(health.rightDotForward, 0.f, 1e-3f); CHECK_NEAR(health.upDotForward, 0.f, 1e-3f);
    }
    // Basis health on a reflection (det ~ -1, matching the real water
    // camera's documented handedness flip): still orthonormal, but the
    // negative determinant must be visible, not hidden.
    {
        const float reflectRaw[16]{1,0,0,0, 0,1,0,0, 0,0,-1,0, 0,0,0,1};
        auto health = analyze_view_basis(read_columns(reflectRaw));
        CHECK(health.orthonormal);
        CHECK_NEAR(health.determinant, -1.f, 1e-3f);
    }
    // Basis health on a non-orthonormal (scaled) matrix must say so.
    {
        const float scaledRaw[16]{2,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        auto health = analyze_view_basis(read_columns(scaledRaw));
        CHECK(!health.orthonormal);
        CHECK_NEAR(health.rightLen, 2.f, 1e-3f);
    }

    // mat4_mul: (A*B)*v must equal A*(B*v) - the composition order the
    // camera gizmo relies on to build viewProjection = mat4_mul(projection, view).
    {
        const auto proj = read_columns(projRaw), view = read_columns(viewRaw);
        const auto vp = mat4_mul(proj, view);
        const Vec4 v{1, 2, 3, 1};
        const auto direct = mat4_mul_vec4(proj, mat4_mul_vec4(view, v));
        const auto composed = mat4_mul_vec4(vp, v);
        CHECK_NEAR(direct.x, composed.x, 1e-3f); CHECK_NEAR(direct.y, composed.y, 1e-3f);
        CHECK_NEAR(direct.z, composed.z, 1e-3f); CHECK_NEAR(direct.w, composed.w, 1e-3f);
    }

    std::puts("PASS: known-WoW camera derivation (position/near/far/FOV/aspect/basis), structural rejection of garbage/non-orthonormal input, generic both-convention derivation, camera comparison, raw basis health (orthonormality/determinant), mat4_mul composition order");
}
