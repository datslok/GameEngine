#pragma once

#include "core/settings_visitor.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/wind_shared.h"
#include "scene/wind.h"

#include <array>
#include <cstdint>

/*
* The wind force field (docs/WIND.md, after Horizon Zero Dawn's global wind force field): round the camera, in a near
* and a far cascade of WIND_CELLS x WIND_CELLS cells over the ground, each cell holds a spring for each category of
* growing thing (trees, plants, grass, hanging things), pushed each frame by the wind there and integrated in a compute
* pass (wind_field.comp), its state kept from frame to frame. The springs, not the wind, are what plants sample: they lag
* behind a gust, overshoot and settle as real stems do. Everything here is mirrored by wind.glsl and tested against it.
*/

// One category's spring: its natural frequency (Hz), its damping ratio (1 critical) and its response, the
// displacement it rests at per (m/s)^2 of wind (drag grows with the square of the speed).
struct WindSpring {
    float frequency = 1.0f;
    float damping = 0.3f;
    float response = 0.01f;
};

/*
* How a category's leaves flutter: how often (Hz), and over how many metres their phase turns once (windPhase). Leaves
* closer than about an eighth of that shiver nearly in step, so a long one keeps a frond or tuft moving together and
* rolls across a meadow as a wave.
*/
struct WindFlutter {
    float hertz = 3.0f;
    float metres = 0.5f;
};

/*
* The field (RenderSettings::wind, F5 Wind). The springs' frequencies from measurements (docs/WIND.md): trees sway about
* 0.3-0.5 Hz, plants and grass faster, hanging things as chains (5.2255 sqrt(L / g)): the short band at 0.6 Hz (a
* chain about 1.1 m long), the medium at 0.2 Hz (9 m), the long at 0.08 Hz (56 m).
*/
struct WindSettings {
    bool enabled = true;

    // The near and far cascades' cells (metres): WIND_CELLS of them across each, 128 m and 1 km.
    float nearCellMetres = 2.0f;
    float farCellMetres = 16.0f;

    // The field's springs (trees, plants, grass, then hanging things short, medium and long, their frequencies falling).
    std::array<WindSpring, WIND_SPRINGS> springs{{
        {.frequency = 0.3f, .damping = 0.3f, .response = 0.01f},
        {.frequency = 1.0f, .damping = 0.4f, .response = 0.02f},
        {.frequency = 2.0f, .damping = 0.3f, .response = 0.03f},
        {.frequency = 0.6f, .damping = 0.15f, .response = 0.02f},
        {.frequency = 0.2f, .damping = 0.15f, .response = 0.02f},
        {.frequency = 0.08f, .damping = 0.15f, .response = 0.02f},
    }};

    /*
    * Each category's flutter. The trees' leaves and the hanging ivy shiver fast and each on its own; the lower layers
    * (plants, grass and the floor's cover) slower and in step over a frond or tuft (the user, Nov 16 2026: too rippley).
    */
    std::array<WindFlutter, WIND_CATEGORIES> flutters{{
        {.hertz = 3.0f, .metres = 0.5f},
        {.hertz = 1.5f, .metres = 8.0f},
        {.hertz = 1.2f, .metres = 8.0f},
        {.hertz = 3.0f, .metres = 0.5f},
    }};

    // Arrows of the wind round the camera, as the debug lines draw them.
    bool showField = false;

    /*
    * Swaying (docs/WIND.md, stage 2): what the vertex shaders do with the springs. Off, everything stands still while the
    * field still runs. The sway fades out between fadeStart and fadeEnd metres from the camera (an impostor card does
    * not sway, so a tree turning into one, at about 1 km in the forest, does not jump; 400 m chosen by eye from far off); shadowCascades of the sun's nearest cascades sway with it,
    * redrawn every frame they hold something swaying, and the rest keep still shadows, as Horizon Zero Dawn's do.
    */
    bool sway = true;
    float swayFadeStart = 80.0f;
    float swayFadeEnd = 400.0f;
    int swayShadowCascades = 1;
};

void visit(WindSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a cell not above 0, a frequency not above 0 or a damping or response below 0, a sway
// fade starting below 0 or ending no later than it starts, negative swaying shadow cascades, or a flutter's hertz or
// metres not above 0, or hanging bands whose frequencies do not fall from the short to the long.
void validateWind(const WindSettings& settings);

// A category's flutter as the sway functions take it: hertz in x, metres in y.
Vec2 windFlutterRate(const WindSettings& settings, int category);

/*
* Value noise in 3D: hashed lattice values (pcg3d, core/hash.h) between 0 and 1, blended smoothly (smoothstep) between
* the eight corners round a place.
*/
float windNoise(const Vec3& place);

/*
* The wind at a place on the ground (x and z) and a time (seconds): towards the wind's direction at its speed, scaled by
* 1 + gustStrength (2 g - 1) for a gust noise g read where the air now passing was gustSize away a moment ago (the
* gusts travel with the wind, changing shape WIND_GUST_EVOLUTION as fast), and pushed sideways by turbulence times the
* speed times 2 n - 1 for a faster noise n in eddies turbulenceSize across. Never against itself (the speed never below 0).
*/
Vec2 windVelocity(const Wind& wind, const Vec2& place, float seconds);

// Where a spring of a response rests in a wind: velocity |velocity| response.
Vec2 windSpringTarget(const Vec2& velocity, float response);

/*
* A spring's state (x, y its displacement, z, w its velocity) after seconds (at most WIND_LONGEST_STEP) pulled towards
* target: x'' = (2 pi f)^2 (target - x) - 2 zeta (2 pi f) x', by semi-implicit Euler in substeps of at most
* WIND_SUBSTEP_TURN radians of its swing (at most WIND_MOST_SUBSTEPS).
*/
Vec4 windSpringStep(const Vec4& state, const Vec2& target, float frequency, float damping, float seconds);

// A cascade's window: its first cell (cells counted from the world's origin along x and z).
struct WindWindow {
    int originX = 0;
    int originZ = 0;
};

struct WindCell {
    int x = 0;
    int z = 0;
};

// The window of cells cellMetres across with a place's cell in its middle (WIND_CELLS / 2 cells after its first).
WindWindow windWindowAround(const Vec2& place, float cellMetres);

/*
* The cell a texel holds: the one cell of the window the same as the texel modulo WIND_CELLS, so the window wraps round
* its texels and moving it keeps every cell it still holds where it was.
*/
WindCell windCellOfTexel(int texelX, int texelZ, const WindWindow& window);

bool windCellInWindow(const WindCell& cell, const WindWindow& window);

/*
* The field's record, std430 as wind_data.glsl reads it (scene set 1, binding 37): wind the direction (x, z), speed and
* time; gusts the gusts' strength and size and the turbulence's; step the frame's step and whether the field is on (1)
* and its spring images are filled; each category's spring (frequency, damping, response); each cascade's window and
* last frame's; each cascade's cell (metres, and 1 / its window's width); flags whether last frame's windows hold; sway
* whether things sway (1, with the field on), where the sway's fade starts and ends (metres) and last frame's time;
* each category's flutter (hertz, metres).
*/
struct GpuWindField {
    float wind[4];
    float gusts[4];
    float step[4];
    float springs[WIND_SPRINGS][4];
    std::int32_t windows[WIND_CASCADES][4];
    float cells[WIND_CASCADES][4];
    std::int32_t flags[4];
    float sway[4];
    float flutters[WIND_CATEGORIES][4];
};

static_assert(sizeof(GpuWindField) == 304, "GpuWindField must match wind.glsl's WindField");

// The record for a frame: off without wind or with the setting off. previousKnown: last frame's windows are the ones
// its spring images hold.
GpuWindField packWindField(const Wind* wind, const WindSettings& settings, const std::array<WindWindow, WIND_CASCADES>& windows,
                           const std::array<WindWindow, WIND_CASCADES>& previous, bool previousKnown, float seconds, float stepSeconds);

/*
* Swaying (docs/WIND.md, stage 2). A sway as the shaders read it (GpuInstance::sway): x the spring's category
* (WIND_CATEGORY_*, -1 still), y the height its bend is measured against (the mesh's units), z its bend, w its flutter.
*/
Vec4 packSway(const WindSway& sway);

// How much of the sway is left at a distance from the camera: all of it to start, none past end, smoothly between.
float windSwayFade(float distance, float start, float end);

// A lean softly held under WIND_MOST_LEAN (radians): lean / (1 + |lean| / most), so a storm bends things far over but
// never flat, while a breeze's lean is nearly unchanged.
Vec2 windSoftLean(const Vec2& lean);

/*
* How much a place reads a cascade's window rather than the next coarser one: 1 inside, falling to 0 over the
* WIND_BLEND_CELLS cells before its last cell from its edge (the cells beside the edge blend with the far side's under
* the wrapping sampler), so a tree crossing the near window's edge as the camera walks never jumps.
*/
float windWindowShare(const Vec2& place, const WindWindow& window, float cellMetres);

// A smooth phase (radians) that changes once every metres along the world: neighbouring vertices of a leaf or branch
// share it, things metres apart do not.
float windPhase(const Vec3& place, float metres);

/*
* A leaf's shiver (an offset, metres): three sines of the time at about rate's hertz (x), their phases from the place
* turning once every rate's metres (y), each direction within amount.
*/
Vec3 windFlutterOffset(const Vec3& place, float amount, const Vec2& rate, float seconds);

/*
* Where the wind moves a vertex (an offset in the world, metres). fromFoot: the vertex from its foot, in the world (the
* model's rotation and scale applied); modelHeight: its height in the mesh's own frame; place: where it is in the
* world; sway: packSway's; scale: the model's scale; spring: its spring's displacement (a lean, radians, along the
* world's x and z); fade: windSwayFade's; flutterRate: its category's (windFlutterRate); seconds: the field's time. The
* main bend leans it by the spring times the bend, softened (windSoftLean) and faded, by h^2 / height and kept at its
* distance from the foot; its branches bob up and down by WIND_BRANCH_SHARE of the lean times their distance from the
* trunk at WIND_BRANCH_HERTZ; its leaves flutter (windFlutterOffset) by flutter times the lean. Both grow with height
* share to its full height, so a foot never moves. Nothing for a still sway.
*/
Vec3 windSwayOffset(const Vec3& fromFoot, float modelHeight, const Vec3& place, const Vec4& sway, float scale, const Vec2& spring, float fade,
                    const Vec2& flutterRate, float seconds);

// Whether a draw sways this frame: it has a sway, and part of it is nearer the camera than the fade's end.
bool drawSways(const WindSway& sway, const Vec3& boundsCentre, float boundsRadius, const Vec3& camera, const WindSettings& settings);

// Whether a shadow tile sways: one of the sun's nearest swayShadowCascades (sunCascade counted from 0, -1 other lights).
bool tileSways(int sunCascade, const WindSettings& settings);

// A swaying tile's fingerprint changes every frame (frame, a count) while something sways in it, so it is redrawn;
// otherwise it stays the casters' own.
std::uint64_t swayingShadowSignature(std::uint64_t signature, bool somethingSways, std::uint64_t frame);

/*
* Hanging things (docs/WIND.md, stage 3). A hanging vertex as the shaders read it (scene set 1, binding 40, from the
* hanging mesh's first entry on, three words each): its anchors (first in the low 16 bits), its share along and its arm
* as half floats (GLSL's packHalf2x16), and its swing's length as a half float in the low 16 bits.
*/
std::array<std::uint32_t, 3> packHangingVertex(const HangingVertex& vertex);

// A hanging chain of a length's slowest swing (Hz): sqrt(g / L) / WIND_CHAIN_PERIOD, L floored at WIND_SHORTEST_SWING.
float windChainHertz(float swingLength);

// The hanging bands' frequencies (short, medium, long) as windChainShares takes them.
Vec3 windHangingBands(const WindSettings& settings);

/*
* A strand's share of each hanging band (short, medium, long): its chain's frequency (windChainHertz) between two
* neighbouring bands' blends them by log frequency; above the short band all short, below the long all long. bands:
* their frequencies, falling.
*/
Vec3 windChainShares(float swingLength, const Vec3& bands);

// An anchor as the shaders read it (binding 41): foot (xyz, w its tree's height in its mesh's units), point (xyz, w the
// tree's scale) and the tree's bend (x).
struct GpuWindAnchor {
    float foot[4];
    float point[4];
    float bend[4];
};

static_assert(sizeof(GpuWindAnchor) == 48, "GpuWindAnchor must match wind_data.glsl's WindAnchorRecord");

GpuWindAnchor packWindAnchor(const WindAnchor& anchor);

/*
* How an anchor moves: as its tree's own vertex at its point would (windSwayOffset, the trees' category, no flutter),
* spring the tree's spring at its foot, fade windSwayFade's at its foot.
*/
Vec3 windAnchorOffset(const GpuWindAnchor& anchor, const Vec2& spring, float fade, float seconds);

/*
* A hanging vertex's own swing (an offset, metres): the hanging spring times swing, softened and faded, is a lean
* (radians) the vertex swings out by over its arm, rising to keep its distance from where it hangs (a pendulum's arc);
* and its leaves' flutter (flutter times the lean at flutterRate, as windSwayOffset's). Nothing with no arm and no flutter.
*/
Vec3 windSwingOffset(float arm, const Vec3& place, float swing, float flutter, const Vec2& spring, float fade, const Vec2& flutterRate, float seconds);

// A hanging vertex's whole motion: its anchors' motions blended by its share along, plus its swing.
Vec3 windHangingOffset(const Vec3& first, const Vec3& second, float along, const Vec3& swing);
