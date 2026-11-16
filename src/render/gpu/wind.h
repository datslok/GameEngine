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
* The field (RenderSettings::wind, F5 Wind). The springs' frequencies from measurements (docs/WIND.md): trees sway about
* 0.3-0.5 Hz, plants and grass faster, hanging lianas slower still (5.22 sqrt(L / g): 0.13 Hz for 20 m).
*/
struct WindSettings {
    bool enabled = true;

    // The near and far cascades' cells (metres): WIND_CELLS of them across each, 128 m and 1 km.
    float nearCellMetres = 2.0f;
    float farCellMetres = 16.0f;

    // Each category's spring (trees, plants, grass, hanging things).
    std::array<WindSpring, WIND_CATEGORIES> springs{{
        {.frequency = 0.3f, .damping = 0.3f, .response = 0.01f},
        {.frequency = 1.0f, .damping = 0.4f, .response = 0.02f},
        {.frequency = 2.0f, .damping = 0.3f, .response = 0.03f},
        {.frequency = 0.15f, .damping = 0.15f, .response = 0.02f},
    }};

    // Arrows of the wind round the camera, as the debug lines draw them.
    bool showField = false;
};

void visit(WindSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a cell not above 0, a frequency not above 0 or a damping or response below 0.
void validateWind(const WindSettings& settings);

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
* last frame's; each cascade's cell (metres, and 1 / its window's width); flags whether last frame's windows hold.
*/
struct GpuWindField {
    float wind[4];
    float gusts[4];
    float step[4];
    float springs[WIND_CATEGORIES][4];
    std::int32_t windows[WIND_CASCADES][4];
    float cells[WIND_CASCADES][4];
    std::int32_t flags[4];
};

// The record for a frame: off without wind or with the setting off. previousKnown: last frame's windows are the ones
// its spring images hold.
GpuWindField packWindField(const Wind* wind, const WindSettings& settings, const std::array<WindWindow, WIND_CASCADES>& windows,
                           const std::array<WindWindow, WIND_CASCADES>& previous, bool previousKnown, float seconds, float stepSeconds);
