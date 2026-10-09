#pragma once
#include "discovery_internal.hpp"
#include "depth_reconstruction.hpp"
#include "fog_math.hpp"
#include "memory_sun.hpp"
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <array>
#include <optional>

// Froxel volumetric fog (docs/fog.md). Two halves:
//  - Volume passes (inject -> light -> integrate) are compute dispatches in the layer's OWN command buffer,
//    submitted on the application's queue right before the application batch that needs them. The
//    application's compute bind point is never touched (DXVK assumes its bindings persist).
//  - The composite is one fullscreen pass recorded INTO the application's command buffer after the world
//    has been drawn and before anything else uses the world's color target (WoW's glow/UI), then the
//    application's graphics state is restored exactly as the water replay does.
// "After the world": a world pass (depth attachment = the scene depth, draws with the world depth range)
// leaves the fog PENDING; further passes on the same color+depth targets are the same world (DXVK splits
// passes at flushes). The composite is recorded right before the first command that does something else
// with those targets (a pass on other targets, a copy/blit/clear). A pass on the same targets that draws no
// world geometry means the UI shares the world's targets: the fog is then dropped, never drawn over UI.
namespace depth_probe::discovery {

struct FogParams {                   // std140 mirror of shaders/fog_common.glsl
    float grid[4], dist[4], proj[4], viewport[4], depthRange[4], right[4], up[4], forward[4];
    float medium[4], albedo[4], emissive[4], ambient[4], sunDir[4], sunColor[4];
    uint32_t mode[4];
    // Temporal reprojection (fog_light.comp): this frame's sub-froxel jitter, the history weight, and the previous
    // frame's camera to reproject each froxel into the previous lighting volume.
    float jitter[4];      // xyz in [0,1) within the froxel, w = history weight (0 = no history this frame)
    float prevRight[4];   // xyz, w = previous P00
    float prevUp[4];      // xyz, w = previous P11
    float prevForward[4];
    float prevOffset[4];  // current camera - previous camera (world, computed in double)
    // World-anchored density noise (fog_inject.comp): periodic over 1536 yards so the origin can be reduced exactly.
    float noise[4];       // amount, lattice size (yards), unused, unused
    float noiseOrigin[4]; // (camera + wind drift) modulo 1536, xyz
    float noiseDetail[4]; // extra drift of the finer smoke octaves (rising + sideways), modulo 1536, xyz
    // Sun shadows (fog_light.comp fogSunVisibility): the shadow image holds the PREVIOUS frame's scene depth (written by its
    // composite), so its camera is that frame's camera.
    float shadowRight[4];    // that camera's right, w = its P00
    float shadowUp[4];       // up, w = its P11
    float shadowForward[4];  // forward, w = march length in yards (0 = no shadows this frame)
    float shadowOffset[4];   // current camera - that camera (world, computed in double)
    float shadowViewport[4]; // that frame's world viewport (full-resolution pixels)
    float shadowImage[4];    // shadow image width, height, downsample factor, unused
    // Height memory (fog_memory_*.comp): top-down grid of the highest surface seen per world cell, toroidal around the camera.
    float memoryA[4];        // camera position in cells relative to its own cell (fraction x, y), camera world z, cell size
    int32_t memoryB[4];      // camera cell x, y, grid size, flags (1 read, 2 clear everything, 4 write this frame)
    float memoryC[4];        // decay this frame (yards), sky light share of the ambient, this submission's compute slot, open-sky sun
    // Sun map: the same toroidal grid in the plane across the sun; per cell the seen surface closest to the sun.
    float sunMapA[4];        // camera position in cells relative to its cell (u, v fraction), camera distance along sunS, cell size
    int32_t sunMapB[4];      // camera cell u, v, grid size, flags (1 read, 2 clear everything, 4 write this frame)
    float sunU[4], sunV[4], sunS[4];   // the basis: across the sun (u, v), toward it (s)
    // Sun disc finder: xy = the game light's horizontal direction (unit; WoW's disc is on that azimuth), z = sine of the
    // band's half width around that vertical half-plane, w = sine of the lowest elevation searched. z = 0: whole sky.
    float sunArc[4];
    // The game's sun cascades (roadmap M10; fog_light.comp fogGameShadow): cascade k = rows 3k..3k+2 (light clip x, y, depth)
    // of the view-space position in the shadow camera's frame; cascadeInfo x = 1 when the reduced maps hold them, y = depth
    // bias, z = map size, w = v sign (1: v = .5 - .5y, D3D's).
    float cascade[12][4];
    float cascadeInfo[4];
    float phase[4];      // sun_aureole: x = weight of the narrow forward lobe (0 = none), y = its g (fog::sun_phase); z = the drawn
                         // disc's angular radius (radians, sun_glare's visibility samples)
    float glare[4];      // sun_glare: xyz = toward the disc WoW draws (the seen disc, else the fog's sun), w = strength (0 = none)
    float rays[4];       // sun_rays (toward glare.xyz): x = strength x the weather's sun share (0 = none), y = share of the way to the
                         // disc marched, z = weight kept per step, w = the open sky's weight falloff from the disc (degrees)
    // Scene shading (fog_shade.frag, applied by the composite to what is behind the fog): x contact shadows, y ambient
    // occlusion, z terrain shadows (strengths, 0 = off), w 1 = frameCascade holds this frame's cascade rows (and the reduced
    // maps this frame's): the shadows added only where the game's own are not.
    float shade[4];
    float shadeLight[4];         // xyz toward the game's sun in view space (the cascades' light, else the fog's sun), w terrain shadows from (yd)
    float frameCascade[12][4];   // this frame's cascade rows (view space, finest first)
    float pointShadow[4];        // point-light shadows: x strength (0 = off), y range (yards), z lights in pointLights, w yards around a light that block nothing
    float pointLights[8][4];     // the nearest point lights: view-space position, w weight (day / night)
    float pointShadowLength[4];  // x yards behind the caster where its point-light shadow is gone; yzw unused
};
static_assert(sizeof(FogParams) == 1248);

// Defaults = the full look, every effect on (2026-10-08, the user: "all custom settings on by default"): the values of the user's tuned
// fog.ini (LOOK: SHAFTS + the SHADOWS page). RESET TO DEFAULTS used to restore the conservative off values below each comment (it turned
// the soft shadows off in game). tests/fog_vulkan_smoke.cpp pins the old off values where it measures one feature at a time.
struct FogSettings {
    FogSettings() { medium.baseDensity = .012f; medium.heightFalloff = .04f; medium.baseHeight = -7.f; }
    fog::Preset preset = fog::Preset::Medium;
    // Height reference: medium.baseHeight is an offset from
    //  GROUND - the area's ground level (the floor memory within 128 yd, fog_ground.comp): the fog lies on the land and stays
    //           there - valleys hold more of it, a hilltop less, wherever the camera is (works at any altitude: Booty Bay
    //           z~0, Teldrassil z~1300);
    //  CAMERA - the camera's slowly followed height: the layer moved up and down with the character (Teldrassil, "the fog
    //           changes as I walk around");
    //  WORLD  - nothing: an absolute world z.
    fog::HeightReference heightReference = fog::HeightReference::Ground;
    float maxDistance = 400.f, distribution = 8.f;
    fog::Medium medium;
    float anisotropy = .35f;
    float ambient[3]{.25f, .28f, .33f};  // MANUAL ambient, and the fallback while the game's block is not readable
    // GAME: the fog's ambient in-scattering = WoW's own ambient light colour (light block +0x88, water UBO c50; changes with
    // zone and time of day) x ambientScale. MANUAL: `ambient`.
    bool ambientFromGame = true;
    float ambientScale = 1.f;
    bool sunEnabled = true;
    // GAME: direction read from WoW's light block each frame (memory_sun.hpp); sunDirection is the fallback when the read
    // fails. MANUAL: sunDirection always.
    bool sunFromGame = true;
    // With sunFromGame: true = the direction of the sun/moon DISC drawn in the sky (the rays come from what you see); false =
    // WoW's lighting direction (lights the terrain, ~15-20 degrees lower than the disc). The disc falls back to the lighting.
    bool sunAtSkyDisc = true;
    // GAME: the sun's colour in the fog = WoW's glow colour of the sun (light block +0xA0: lilac in Teldrassil, warm white in
    // Booty Bay; the rays looked grey on the purple world with a fixed white) x sunColor[0] as the intensity. MANUAL: sunColor.
    bool sunColorFromGame = true;
    float sunDirection[3]{.4f, .3f, .866f}; // toward the light, world (normalised on load)
    float sunColor[3]{19.2f, 18.048f, 15.552f};   // with sunColorFromGame only [0] counts: the intensity
    float temporalWeight = .9f;   // 0 = temporal off (no jitter, no history)
    float temporalMoving = .8f;   // history weight while the camera turns / steps (fog::motion_history_weight; <= temporalWeight)
    float noiseAmount = .3f;      // 0 = homogeneous height fog
    float noiseScale = 24.f;      // lattice size in yards: 12, 24 or 48 (must divide 1536)
    float noiseWind = .2f;        // yards per second - slow drift, not moving smoke
    bool shadows = true;          // sun + sky shadows in the fog (previous frame's depth + the height memory)
    float shadowLength = 120.f;   // yards marched toward the sun per froxel (a canopy 50 yd up needs ~90 at 34 degrees)
    float skyLight = .85f;        // share of the ambient that comes from above and is shadowed (0 = all ambient isotropic)
    bool lightGizmo = false;      // draw where the light sources are (shaders/gizmo.vert markers)
    bool lightSources = false;    // the game's point lights, read from the lit models and drawn as gizmos (docs/lights.md)
    // WoW's own distance fog (a flat colour, no light - it covered the far shafts): its fog register in the world's vertex
    // constants rewritten so both ends lie this many times farther (game_fog_capture.cpp). 1 = as the game, 0 = none.
    float gameFogDistance = 0;   // ours does the distance (it is on by default)
    // WoW's sun shadow filter spread this many times (its 9 tap offsets, pixel constants; game_pcf.hpp): softer edges.
    float shadowSoftness = 3;
    // Soft sun shadows inside WoW's own pixel shaders (src/game_soft_shadows.*): each shadow tap rewritten to a blocker search +
    // a penumbra growing with the distance from the caster (softShadows = its size, 1 ~ .05 yd per yard; 0 = the game's own
    // edges) and the shadow lifted with that distance (shadowFade: how much is gone shadowFadeDistance yards from the caster;
    // 0 = none). The shaders are patched when the game starts with softShadows > 0; from then on all three change live.
    float softShadows = 2, shadowFade = .5f, shadowFadeDistance = 5;
    // The game's shadows indoors (WoW's interiors: one 2048 map read by the models, not the four cascades): how much of them
    // stays, live (1 = as the game, 0 = none - a sun's shadow in a cave). Its shaders are patched with the soft ones.
    float indoorShadows = .5f;
    // Scene shading before the fog (fog_shade.frag, docs/fog.md): shadows / occlusion the game does not draw. Strengths, 0 = off.
    // contactShadows: a short screen march toward the sun from every surface - grass, stones, feet, sills the 2048 cascades
    // miss - only where the game's own cascades see the sun. ambientOcclusion: corners, wall feet, under trees (a horizon test
    // around each pixel). terrainShadows: the same march over up to 100 yd - hills the game draws into no cascade.
    float contactShadows = .5f, ambientOcclusion = .8f, terrainShadows = .4f;
    // Terrain shadows only from this many yards from the camera on (faded in over the next max(10, half of it) yards): near
    // the character the game's own shadows are there, far hills are what they are for. 0 = everywhere.
    float terrainShadowsFrom = 80;
    // Shadows from the game's point lights (torches, fires: game_lights, fog_shade.frag): toward each of the 8 nearest a march
    // over the depth; blocked = darker by pointShadows, fading out to pointShadowRange; where the sun reaches, full at night
    // and 30% at noon (indoors / in the game's sun shadow always full). Nothing within pointShadowIgnore yards of a light
    // blocks it (its brazier or lantern). Needs no LIGHT SOURCES gizmo. 0 = off.
    float pointShadows = .3f, pointShadowRange = 12, pointShadowIgnore = 2.5f;
    // How far behind what casts it a point-light shadow reaches (fading out from a third of it): a fire is low and wide - the
    // shadow of a head would run on endlessly over the ground and up the next slope (Valley of Trials, 2026-09-28).
    float pointShadowLength = 5;
    // The sun's strength in the fog where the sky is open, relative to under cover (height memory): 1 = the same. The same
    // strength that makes shafts under a canopy made open zones a white glare; .25 kept Teldrassil's shafts and cleared
    // Booty Bay (tools/fog-whatif.py on captures of both).
    float openSun = .25f;
    float weatherSun = 1;         // how far WoW's rain / snow / sandstorm hides the sun (memory_sun weather_overcast; 0 = ignore the weather)
    bool transparentFog = true;   // blended world draws without depth write (particles, glows) drawn after the fog: not fogged with the surface behind
    float skyFog = 1;             // share of the fog applied over sky pixels (1 = all; 0 = the game's sky untouched - its own haze and sun glow stay)
    // Cap on the fog's opacity (1 - transmittance; Unreal's Fog Max Opacity): the fog never hides more than this share of what
    // lies behind it, its light scaled with it. Shafts need the density (.02) that, over an open harbour, erased everything
    // beyond ~150 yd into a bright veil (Booty Bay); near fog and shafts stay under the cap. 1 = no cap.
    float maxOpacity = .6f;
    // Ceiling of the light the fog adds (fog::soft_limit: unchanged up to half of it, then bent toward it). Looking into the sun
    // through a sunlit canopy gap the fog added ~2.7x white and the frame went white (Shadowglen). 0 = no limit.
    float lightLimit = .8f;
    // Auto exposure (eye adaptation): the fogged world is darkened when its bright part is brighter than exposureTarget
    // (fog_exposure.comp measures, fog::exposure_for decides; ~0.4 s to darken, ~1.5 s back), never below exposureMin and never
    // brightened. Applied by the composite, so before WoW's glow (which then blows out less) and never to the UI; only while
    // the fog is on.
    bool autoExposure = true;
    float exposureTarget = .55f, exposureMin = .6f;
    // The game's own sun shadow cascades for the fog's sun (roadmap M10): world-fixed - the shadows the ground gets - instead of
    // the camera-dependent screen march wherever a cascade covers the point, and the cascades' light direction as the fog's
    // sun. gameShadowFlip: the maps' v axis. -1 = v = .5 + .5y: WoW's own pixel shaders run on a frame scan's constants
    // (tools/spirv-eval.py, 2026-09-28, frame-scan-110504-1) sample cascade k at (.5 + .5x, .5 + .5y) with reference z; on
    // fog-capture-110504-1 the pixels this puts in shadow are 31% darker in the game's own picture (the other axis choices:
    // 0.94-1.10). 1 = D3D's v = .5 - .5y (the first guess - wrong). WoW draws only its casters into the maps (models,
    // buildings - no terrain), so the fog's sun map (world-anchored, from the surfaces seen) still adds the land's shadows.
    bool gameShadows = false;
    float gameShadowFlip = -1;
    // The sun's aureole: a narrow forward-scattering lobe mixed into the phase (fog::sun_phase). Air right beside an occluder
    // that is still in the sun glows toward a camera looking past it at the sun - light bleeding around a trunk or a rock edge
    // with the sun half behind it (the user, Shadowglen). Measured on capture 62384-2: .02-.04 wraps the edges, .1+ turns the
    // whole opening white. 0 = the single lobe as before.
    float sunAureole = .04f, sunAureoleG = .9f;
    // Sun glare (the eye's / lens' veiling glare, not the air's): the result is pulled toward the sun's colour around the drawn
    // disc - also over whatever stands in front of it (a mast the sun is half behind: the user, Booty Bay) - by the share of the
    // disc that is visible this frame (cube root: a sliver of the sun is still blinding) times sun_glare (fog::glare_amount).
    // Added after the fog, before WoW's glow and the UI; the fog volume and its shafts are untouched. 0 = none.
    float sunGlare = .7f, sunGlareRadius = 1.4f;   // strength; the drawn disc's radius in degrees (~25-30 px at 1080p)
    // Sun rays (the user, Booty Bay: "beams coming out of the gaps"): light streaming from the drawn disc through the gaps of
    // what stands in front of it - a quarter-resolution march toward the disc on the screen over the open sky near it (GPU
    // Gems 3 ch. 13); the composite adds only its beams (minus its mean turned +-4/8 degrees about the disc, not the smooth
    // glow) in the sun's colour after the fog, bent toward .8; the disc itself is left alone. A look, not the air's physics:
    // the froxels cannot resolve such gaps.
    float sunRays = 1.5f, sunRaysLength = .85f;     // strength (0 = none; ~1.5); share of the way to the disc marched
};

// What one composite needs its volume computed with.
struct FogCapture;   // volumetric_fog.cpp: one frame's read-back (fog capture)
struct FogFrame {
    FogParams params{};
    fog::Grid grid;
    uint32_t parity = 0;          // which of the two lighting volumes this frame writes (the other is the history)
    double camera[3]{};
    const void* shadowWrittenBy = nullptr;  // the FogResources whose shadow image this frame's composite filled (nullptr: none)
    int compositeSlot = -1;                 // timestamp slot of this frame's composite (read only once its batch was submitted)
    const void* pictureWrittenBy = nullptr; // the FogResources whose quarter-resolution picture this frame's composite filled
    std::shared_ptr<FogCapture> capture;    // this frame is being captured: its final parameters are written at submit
    const void* cascadesWrittenBy = nullptr; // the FogResources whose reduced cascade maps this frame's composite filled
    float cascadeRows[12][4]{};             // their light matrices (this frame's view space), finest first
};

struct FogPending {
    bool active = false;
    std::shared_ptr<Image> color, depth;
    VkImageLayout colorLayout = VK_IMAGE_LAYOUT_UNDEFINED, depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool layoutsKnown = false;
    VkImageView waterMask{};                         // this cycle's water mask (water_reflections), if the water replay ran
    VkFormat colorViewFormat = VK_FORMAT_UNDEFINED; // the format the world pass rendered with (may differ from the image's, e.g. sRGB views)
    uint32_t width = 0, height = 0, worldPasses = 0;
    WorldViewport viewport;
    uint64_t frame = 0;
};

struct FogResources;
struct FogState {
    FogSettings settings;
    std::filesystem::file_time_type settingsTime{};
    std::string settingsPath, settingsNote;
    uint32_t presentsSinceSettingsCheck = 0;
    FogPending pending;
    std::shared_ptr<FogResources> resources;
    std::deque<std::pair<uint64_t, std::shared_ptr<FogResources>>> retired; // destroyed a few presents later
    uint64_t sceneDepthImage = 0;
    uint64_t waterSerialSeen = 0;
    // CAMERA height reference: camera world z followed with a ~6 s time constant (zoom/jumps do not pump the fog).
    double referenceZ = 0; bool haveReference = false;
    // GROUND height reference: the area's floor level (world z, smoothed ~3 s) from fog_ground.comp readings; the last
    // reading and how many floor cells it had. Forgotten on a teleport.
    bool groundValid = false;
    double groundZ = 0;
    float groundReading = 0;
    uint32_t groundCells = 0, groundBasisCells = 0;   // cells of the last reading / the most any reading behind the estimate had
    std::chrono::steady_clock::time_point groundTime{};
    uint64_t groundReadings = 0, groundNextLog = 0;
    // The area's water level (world z) from the water surface's pixels (fog_memory_scatter.comp): the floor under it counts as
    // the surface. Forgotten on a teleport or 128 yd from where water was last seen.
    bool waterValid = false;
    double waterZ = 0, waterSeen[2]{};
    uint32_t waterPixels = 0;   // of the last reading
    std::chrono::steady_clock::time_point referenceTime{};
    float lastBaseHeight = 0;       // effective world z of the medium's base, last composite
    // Temporal: the last frame whose volume was actually computed, and why history was last dropped.
    uint32_t nextParity = 0, lastComputedParity = 1, jitterIndex = 0;
    bool prevValid = false;
    FogFrame prevFrame;
    uint64_t settingsGeneration = 1, prevSettingsGeneration = 0, historyResets = 0;
    std::string lastReset = "start";
    bool lastHistoryUsed = false;
    float lastHistoryWeight = 0;    // the history weight the last computed frame used (lower while the camera moves)
    // Auto exposure: the adapted exposure the composite applies, the last measured key (bright-part luminance) and its target.
    float exposure = 1, exposureKey = 0, exposureGoal = 1;
    // The game's sun cascades seen this frame (fog_observe_draw on the first world draw that samples four large square depth
    // images with a comparison; its vertex constants c53..c64 hold the light matrices). In the cascades' own order after
    // sorting finest first; the images' sampled layouts as the draw's descriptors declare them.
    struct GameCascades {
        uint64_t frame = UINT64_MAX;
        uint32_t attempts = 0;
        bool valid = false;
        std::array<std::shared_ptr<Image>, 4> images{};
        std::array<VkImageLayout, 4> layouts{};
        std::array<uint32_t, 4> bindings{};
        float rows[12][4]{};
        std::string problem = "none seen yet";
    } cascades;
    uint64_t cascadesLastValid = 0;   // the last frame the game's cascades were found (0: never this session)
    std::optional<GameCascades> cascadesInjected;   // tests: used instead of the observed ones
    std::optional<std::vector<std::array<float, 3>>> pointLightsInjected;   // tests: world positions used instead of the game's lights
    std::unordered_map<uint64_t, bool> cascadePipelines;   // pipeline serial -> its fragment shader has four or more depth-compare bindings
    std::string cascadesLogged;
    uint64_t cascadeFramesUsed = 0, cascadesNextLog = 0;
    bool exposureValid = false;
    std::chrono::steady_clock::time_point exposureTime{};
    uint64_t exposureReadings = 0, exposureNextLog = 0;
    bool lastWaterSurface = false;
    // The game's sun (read at present while the fog is on and sun_source = game).
    GameSun gameSun;
    GameWeather weather;                    // read with the sun; kind 0 = clear
    bool weatherLogged = false;
    float lastSunWeatherScale = 1;          // the last composite's sun factor from the weather (1 = none)
    uint64_t sunReads = 0, sunFailures = 0, sunNextLog = 0;
    bool sunLoggedOk = false, sunLoggedAny = false;
    bool lastSunFromGame = false;   // the last composite used the game's direction
    bool lastSunFromDisc = false;   // ... and it was the sky disc's
    bool lastAmbientFromGame = false;
    bool lastSunColorFromGame = false;
    float lastSunToward[3]{};       // the direction the last composite's fog sun came from (light gizmo); lastSunOn = it lit
    bool lastSunOn = false;
    std::string lastShadowProblem = "off";   // empty: this frame's light pass marched the sun shadows
    // The sun/moon disc seen on the screen (fog_sun_find.comp): its direction, when it was last seen, where on the screen.
    bool seenDiscValid = false;
    float seenDisc[3]{};
    std::chrono::steady_clock::time_point seenDiscTime{};
    float seenDiscPixel[2]{};
    uint32_t seenDiscTexels = 0;
    uint64_t seenDiscCount = 0, seenDiscNextLog = 0;
    fog::DiscTrack discTrack;         // temporal filter of the sightings (fog_math.hpp)
    // Since the last [FOG-SUN] sighting log: searches skipped while the camera turned, clusters rejected (by reason),
    // sightings waiting for confirmation, moves of the fog sun, sun map clears.
    struct DiscStats { uint64_t turning = 0, waiting = 0, moved = 0, sunMapClears = 0; std::map<std::string, uint64_t> rejected; } discStats;
    uint32_t discEpoch = 0;                  // +1 on a teleport: searches dispatched before it are ignored when read back
    std::string lastSunSource = "none";      // where the last composite's sun direction came from
    // Height memory bookkeeping: the camera it was last written around (a jump over 200 yd clears it: teleports, and the
    // continents share coordinate ranges), and when (decay).
    bool memoryValid = false;
    double memoryCamera[3]{};
    std::chrono::steady_clock::time_point memoryTime{};
    uint64_t memoryClears = 0;
    // Sun map basis: kept while the sun direction stays within half a degree (it moves ~0.05 degree a minute); a new basis
    // clears the map (it is re-learned from what is seen).
    bool sunBasisValid = false;
    double sunBasis[3][3]{};   // u, v, s
    uint64_t sunMapClears = 0;
    // Fog capture ($DEPTH_PROBE_EXPORT_DIR/fog-capture-<pid>-<n>/): armed by the menu or by creating the file
    // $DEPTH_PROBE_EXPORT_DIR/fog-capture-request. Stage 1: the next composite (after 60 fog frames) captures; 4: done, the
    // view is switched off again if the capture had turned it on.
    int captureStage = 0;
    uint32_t captureRestoreMode = 0, presentsSinceCaptureCheck = 0, captureSequence = 0, captureWaited = 0;
    bool captureTurnedFogOn = false;   // the fog was off when armed: FINAL for the capture, off again afterwards
    uint32_t fogFramesOn = 0;           // presents since the fog was turned on: a capture waits for 60 (sun read, a depth,
                                        // the memories and the temporal history filled - the first in-game one caught frame 1)
    std::string captureDir, captureNote;
    std::vector<std::shared_ptr<FogCapture>> captures;   // copies in flight (written when their event is set)
    float lastAmbient[3]{};         // the ambient the last composite used

    // Per-present bookkeeping (reset at present) and the last complete frame's values for the HUD.
    uint32_t compositesThisFrame = 0, worldPassesThisFrame = 0, droppedThisFrame = 0;
    // Transparent-aware fog (milestone 3): per log interval - draws held back, issued after the fog / by a fallback flush,
    // layers kept in place (same geometry as the draw before, EQUAL test), and why candidates could not be held back.
    uint64_t deferred = 0, deferredAfterFog = 0, deferredAfterWater = 0, deferredFallback = 0, deferLayers = 0, deferPostponed = 0;
    std::map<const char*, uint64_t> deferRejected;
    uint32_t lastComposites = 0, lastWorldPasses = 0, lastDropped = 0;
    std::string lastTrigger = "none", lastProblem;
    // GPU milliseconds (exponential averages) and CPU milliseconds of our hooks.
    double gpuMemoryClear = 0, gpuMemoryScatter = 0, gpuSunFind = 0;   // the compute submission's stages before inject
    double gpuInject = 0, gpuLight = 0, gpuIntegrate = 0, gpuComposite = 0, cpuComposite = 0, cpuPrelude = 0;
    uint32_t lastGrid[3]{};                  // froxel grid of the last composite (for the perf log)
    uint64_t computeSubmits = 0, computeSkipped = 0;
    // This frame's passes, one line each (kept while the fog is on, cleared at present): logged when the fog is dropped,
    // so the frames that flicker show their own pass order (the first 12 drops of a session).
    std::vector<std::string> frameTrace;
    uint32_t dropsTraced = 0;
    // Pass timeline logging: remaining frames to log.
    uint32_t timelineFrames = 0;
    uint64_t timelineNextFrame = 0, timelinePass = 0;
};

FogState& fog_state(Device&);
// fog.ini's settings, reloaded when the file changed - for what uses them with the fog off (the game's own fog and shadow
// filter patches, game_fog_capture.cpp).
const FogSettings& fog_settings_now(Device&);
// Hook entry points (discovery::mutex held unless noted).
void fog_observe_draw(Device&, Command&);
void fog_pass_begin(Command&);
void fog_pass_end(const DeviceData&, Device&, Command&, VkCommandBuffer);
void fog_before_pass_begin(const DeviceData&, Device&, Command&, VkCommandBuffer, const Image* color, const Image* depth);
void fog_before_transfer(const DeviceData&, Device&, Command&, VkCommandBuffer, VkImage a, VkImage b, const char* what);
void fog_layout_event(Device&, const Image&, const VkImageSubresourceRange&, VkImageLayout, bool known);
// Transparent-aware fog. fog_defer_candidate: the cheap test at record time (before any snapshot); fog_defer_draw: holds the
// snapshotted draw back if it can be issued again exactly (true = do not forward it now). fog_flush_deferred: issues the held
// draws (after the composite, or before anything else touches their target: a pass begin, a barrier or transfer on it, the
// end of the command buffer); `endOfCommand` = nothing of the application follows, the state need not be put back.
bool fog_defer_candidate(Device&, const Command&, const DrawSemantic&, const DrawFacts&, uint32_t kind, uint32_t count, uint32_t first, int32_t vertexOffset);
bool fog_defer_draw(Device&, Command&, const std::shared_ptr<Draw>&);
void fog_flush_deferred(const DeviceData&, Device&, Command&, VkCommandBuffer, const char* why, bool endOfCommand = false);
bool fog_deferred_target(const Command&, uint64_t image);   // a held draw renders into this image
// Queue level, before forwarding the application's submit (takes discovery::mutex itself).
void fog_before_submit(const DeviceData&, VkQueue, const std::vector<VkCommandBuffer>&);
void fog_present(Device&);
void fog_destroy(Device&);
bool fog_enabled();
// F10 page "VOLUMETRIC FOG": rows that cycle through preset steps; every change is saved to fog.ini.
// GPU time (ms, smoothed) of the height memory + sun map + sun finder stages, and of the whole fog per frame.
inline double fog_gpu_memories(const FogState& s) { return s.gpuMemoryClear + s.gpuMemoryScatter + s.gpuSunFind; }
inline double fog_gpu_total(const FogState& s) { return fog_gpu_memories(s) + s.gpuInject + s.gpuLight + s.gpuIntegrate + s.gpuComposite; }
unsigned fog_menu_rows();
// The SHADOWS AND OCCLUSION page (inspector_ui.cpp page 12): shadow edges, contact shadows, ambient occlusion, terrain shadows.
unsigned shade_menu_rows();
std::string shade_menu_label(Device&, unsigned row);
std::string shade_menu_hint(unsigned row);
void shade_menu_click(Device&, unsigned row);
std::string fog_menu_label(Device&, unsigned row);
void fog_menu_click(Device&, unsigned row);
}
