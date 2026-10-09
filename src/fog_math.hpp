#pragma once
#include <cstdint>

// Froxel volumetric fog: CPU reference of shaders/fog_common.glsl (keep identical). Frustum-aligned grid of
// X x Y x Z froxels over view depth [0, maxDistance]; view depth is the same axis depth reconstruction
// produces (distance along the camera forward), so a scene pixel's slice is exact, not approximated.
// References (architecture only): Wronski 2014 (AC IV froxels), Hillaire 2015 (Frostbite energy-conserving
// integration), Unreal volumetric fog grid.
namespace depth_probe::fog {

struct Grid {
    uint32_t x = 160, y = 90, z = 64;
    float maxDistance = 400.f;   // fog volume end (view depth, yards), independent of the camera far plane
    float distribution = 8.f;    // C: ~linear slices within C yards, exponential beyond
};
enum class Preset { Low, Medium, High };
enum class HeightReference { World, Camera, Ground };   // what the fog's base height is measured from (volumetric_fog.hpp)
inline const char* height_reference_name(HeightReference r) { return r == HeightReference::Ground ? "ground" : r == HeightReference::Camera ? "camera" : "world"; }
// XY from the render size: one froxel per 16 / 12 / 8 pixels; Z 48 / 64 / 96.
Grid grid_for(Preset, uint32_t width, uint32_t height, float maxDistance, float distribution);

// Slice coordinate t in [0,1] <-> view depth. d(t) = C * (exp(t * ln(1 + D / C)) - 1).
float depth_of_slice(const Grid&, float t);
float slice_of_depth(const Grid&, float viewDepth);

struct Medium {
    float baseDensity = .02f;    // extinction (1/yard) at baseHeight
    float heightFalloff = .08f;  // 1/yard: density halves every ln2/falloff yards up
    float baseHeight = 0.f;      // world z
    float albedo[3]{.9f, .9f, .92f};
    float emissive[3]{0, 0, 0};  // radiance per yard, added to in-scattering
    float maxExtinction = 1.f;   // clamp (1/yard); exp() below baseHeight grows without bound
};
// sigma_t at world height z: baseDensity * exp(-(z - baseHeight) * heightFalloff), clamped to [0, maxExtinction].
float extinction_at(const Medium&, float worldZ);

// Henyey-Greenstein phase function, normalised over the sphere (1/4pi for g = 0). cosTheta =
// dot(view ray direction, direction toward the light): the scattering angle between the light's
// propagation and the propagation toward the eye. g > 0 peaks when looking toward the light.
float henyey_greenstein(float cosTheta, float g);
// The sun's phase (fog_common.glsl fogSunPhase, keep identical): (1 - aureole) x HG(g) + aureole x HG(aureoleG).
float sun_phase(float cosTheta, float g, float aureole, float aureoleG);

// One slab of homogeneous medium of thickness ds with extinction sigmaT and in-scattering source S
// (radiance per yard = sigma_s * L_in + emissive). Frostbite's analytic integral:
//   transmittance T = exp(-sigmaT ds),  scattered = S * (1 - T) / sigmaT   (-> S * ds as sigmaT -> 0).
struct Slab { float scattered[3]; float transmittance; };
Slab integrate_slab(const float source[3], float sigmaT, float ds);

// Front-to-back accumulation: result.scatter += T_acc * slab.scatter; T_acc *= slab.T.
struct Accumulated { float scatter[3]{0, 0, 0}; float transmittance = 1; };
void accumulate(Accumulated&, const Slab&);

// Composite: color * T + inscatter.
void composite(const float scene[3], const Accumulated&, float out[3]);

// Smoke lattice (fog_noise_bake.comp): cells per side of the baked 3D texture; the smoke field repeats every noise_period
// yards (the warp samples it at half the frequency), and the noise origin is reduced modulo that period.
constexpr int noiseLatticeCells = 64;
inline double noise_period(float scale) { return 2.0 * double(scale) * noiseLatticeCells; }

// Smoke coverage (fog_inject.comp fogNoiseFactor): the normalised noise z ~ N(0,1) is shaped by
// smoothstep(t - .7, t + .7, z) with threshold t = smoke_threshold(amount); dividing by smoke_mean(t) = E[shape] keeps
// the average density.
float smoke_threshold(float amount);
float smoke_mean(float threshold);

// A camera's frustum as the grid sees it: basis vectors (world) and the projection's P00, P11.
struct FrustumCamera { float right[3], up[3], forward[3]; float p00, p11; };
// Grid coordinate (uvw in [0,1]^3: u,v = ndc*.5+.5 with v up, w = slice coordinate) of a point given relative to the
// camera; false when it lies outside the grid. Temporal reprojection (fog_light.comp fogReproject) does exactly this
// with the previous camera and offset + (currentCamera - previousCamera).
bool froxel_uvw(const Grid&, const FrustumCamera&, const float offset[3], float uvw[3]);
// The inverse: offset from the camera of the grid point uvw.
void froxel_offset(const Grid&, const FrustumCamera&, const float uvw[3], float offset[3]);

// The game's sun shadow cascades (roadmap M10; found 2026-09-27 with tools/shadow-constants.py): WoW's world vertex shaders
// hold four light matrices in constants c53..c64, three rows each (x, y, depth), applied to the VIEW-space position
// (x = camera right, y = up, z = forward) and giving the light's clip space: x, y in [-1, 1] (shadow-map texture u = .5 + .5x,
// v = .5 - .5y, D3D's), depth in [0, 1] (Shadowglen: 40/80/320/1280 yd wide around the camera, one shared depth row along
// minus the light direction). rows[3k + i] = cascade k, row i (xyz + offset in w).
// nullptr when the rows look like that, else why not.
const char* cascade_rows_problem(const float rows[12][4]);
// The cascades from the finest to the coarsest (by the u row's scale): order[j] = the cascade to try j-th.
void cascade_order(const float rows[12][4], int order[4]);
// The direction toward the light in view space (minus the shared depth row, unit); false when it is zero.
bool cascade_light_view(const float rows[12][4], float toward[3]);
// fog_light.comp fogGameShadow's choice (keep identical): the first cascade, in the rows' order, whose light clip x, y of the
// view-space point lie within [-1 + margin, 1 - margin]; its x, y, depth. -1 when none covers it.
int cascade_lookup(const float rows[12][4], const float view[3], float margin, float xyd[3]);

// Sun glare (fog_common.glsl fogGlarePsf / fogGlareAmount, keep identical): veiling glare around the drawn disc by the angle
// from it (degrees): a 2.5-degree core and an 8-degree skirt, 1 at the centre, faded to 0 between 12 and 20 (the composite's
// search radius: no ring at the cut); the pull toward the sun's colour it gives with
// the visible share of the disc (cube root - a sliver of the sun is still blinding) and the strength, clamped to [0, 1].
float glare_psf(float degrees);
float glare_amount(float strength, float visible, float degrees);

// light_limit (fog_common.glsl fogSoftLimit, keep identical): the brightest channel of the light the fog adds is kept below
// `ceiling` - unchanged up to half of it, then bent smoothly (same slope at the knee) toward the ceiling. Looking into the
// sun through a sunlit canopy gap the fog added ~2.7x white (sun 12 x the forward phase peak) and the 8-bit frame, then
// WoW's glow, went white (Shadowglen). ceiling <= 0: no limit.
float soft_limit(float peak, float ceiling);

// Auto exposure (fog_exposure.comp counts the fogged picture's luminance in 64 bins of log2 from -8, 1/8 wide). The key is
// the mean log-luminance of the brightest part of the picture (the texels between the 40th and 98th percentile - what makes
// a frame feel bright, not its dark corners or a few white pixels); false with nothing counted.
bool exposure_key(const uint32_t* bins, uint32_t count, float lowestLog2, float binLog2, float lowShare, float highShare, float& key);
// The exposure that brings the key down to `target`, never below `darkest` and never above 1 (only darkens: WoW's night and
// caves stay as dark as the game draws them). Calibrated on captures: the game's own daytime key .44-.57, fogged Booty Bay
// .53-.70, the blown-out screenshots .67-.89 -> target .55.
float exposure_for(float key, float target, float darkest);

// Temporal history weight for this frame (fog_light.comp mixes the reprojected previous volume by it). A froxel's light
// depends on what the camera saw (screen-space shadows), so after a turn or a step the history holds the shafts of the old
// view; at .95 they took ~20 frames to catch up (Teldrassil: "the fog and the shafts change as I walk around"). The weight
// falls linearly from `still` at rest to `moving` (never above `still`) at a turn of 1 degree or a step of .5 yd per frame.
float motion_history_weight(float still, float moving, float turnDegrees, float stepYards);

// Ground level of the area (height_reference = ground): fog_ground.comp counts the floor memory's cells (lowest surface seen
// per half-yard cell) within 128 yd by height relative to the camera, in bins of binSize from `lowest`. The level is the
// given percentile of those cells, interpolated within its bin - a low one: the land and the water surface, not the canopy
// tops or roofs seen from above. False with fewer than minCells cells (nothing seen yet).
bool ground_level(const uint32_t* bins, uint32_t count, float lowest, float binSize, float percentile, uint32_t minCells, float& level);

// Sun disc finder (fog_sun_find.comp counts disc-coloured sky texels of the quarter-resolution picture; the host decides).
// A cluster is the disc only when it is big enough, compact, a small part of the sky, and away from the picture's edges -
// a disc cut by an edge has a shifted centre. Booty Bay 9:00-9:16 rejected with this: 15x23-texel smears of bright haze,
// a 24x5 streak along the horizon, 5-texel specks.
struct DiscCluster {
    uint32_t count = 0, sky = 0;          // texels in the cluster, sky texels searched
    double meanX = 0, meanY = 0;          // centroid (quarter-resolution texels)
    double spreadX = 0, spreadY = 0;      // standard deviation (texels)
    uint32_t width = 0, height = 0;       // picture size (texels)
};
const char* disc_cluster_problem(const DiscCluster&);   // nullptr: a disc; else why not ("spread", "edge", ...)
// WoW draws its sun disc in the vertical half-plane of its light's azimuth (x == y in every capture: azimuth 45): only the
// elevation comes from the picture. arcToward: the game light's horizontal direction (unit xy). d is kept unit.
void disc_onto_arc(const float arcToward[2], double d[3]);
// Whether a cluster of `count` picture texels is (nearly) the whole drawn disc: at least wholeShare of the area a disc of
// radiusDegrees covers there (picture width pictureWidth, projection P00 p00; cosFromForward: its direction's cosine to the
// view axis - off-centre the disc projects larger, area / cos^3). 1080p, P00 1.104, 1.4 degrees: ~130 texels, whole from ~53.
constexpr double wholeDiscShare = .4;
bool disc_whole(uint32_t count, double radiusDegrees, double p00, uint32_t pictureWidth, double cosFromForward);
// Temporal filter for the fog sun: a sighting within smoothDegrees of the current direction refines it; a different one
// becomes a candidate that must be seen confirmHits times in a row (each within confirmDegrees of the candidate) before the
// fog sun moves there - one bright cloud or glint can no longer throw it across the sky.
// Once there is a fog sun, only a whole disc refines or moves it. A part of one (whole = false) then only confirms the current
// direction when near it: its centroid is off the disc's centre, and a small warm spot can be something else - Booty Bay
// 2026-09-27: a torch flame against the orange horizon, 13 texels at elevation 3.5 on the light's arc, seen three times, took
// the fog sun (and sun_glare) 42 degrees down from the real disc, which only showed 13-25 texels past a ship's hull. Before
// there is one, parts can start it (three in a row, as any candidate): with none the fog's sun is the sky address - in
// Mulgore 2026-09-28 the shadows' direction at 54 degrees against a disc at 29, the shafts gathering 25 degrees off the sun.
struct DiscTrack {
    static constexpr int confirmHits = 3;
    static constexpr double smoothDegrees = 2, confirmDegrees = 1, keep = .8;
    enum Step { Smoothed, Waiting, Moved, Confirmed, Partial };   // Confirmed: a part near the fog sun; Partial: a part elsewhere
    bool valid = false;
    double dir[3]{};                      // the accepted direction (unit)
    double candidate[3]{};
    int hits = 0;                         // consecutive sightings of the candidate
    Step add(const double d[3], bool whole = true);
    void reset() { valid = false; hits = 0; }
};
}
