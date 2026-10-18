#pragma once

#include "math/vec3.h"
#include "render/gpu/tone_mapping_shared.h"
#include "render/gpu/uniform_limits.h"

/*
* Tone mapping: the scene is lit in real, unbounded brightness (a floodlight's hotspot can be 40 times a white wall), but
* a screen shows only up to its peak. A tone mapping curve squeezes 0..infinity into 0..peak: nearly unchanged in the
* dark and the mid-tones, rolling off smoothly towards the peak, the way film or the eye responds to bright light,
* instead of cutting every channel off at the peak (which flattens bright things to white and shifts their colour).
*
* These functions mirror tone_map.frag, the final pass, so tests can check the maths without a GPU.
*/

enum class ToneMapCurve {
    Clamp = TONE_MAP_CLAMP,       // no curve: everything above the peak is cut off (how the engine looked before HDR)
    Reinhard = TONE_MAP_REINHARD, // x / (1 + x): gentle and nearly linear in the dark, but bright colours look flat
    Aces = TONE_MAP_ACES,         // the film-like curve most games use: more contrast, darker shadows
    AgX = TONE_MAP_AGX            // newer: bright saturated lights fade towards white instead of to a pure primary
};

struct ToneMappingSettings {
    ToneMapCurve curve = ToneMapCurve::AgX;

    // Multiplies the scene's brightness before the curve, like a camera's exposure. 1 leaves it unchanged.
    float exposure = 1.0f;
};

// The brightest an ordinary (SDR) screen shows: its white. An HDR screen goes several times past it.
inline constexpr float sdrPeakBrightness = 1.0f;

/*
* One pixel's linear colour (after lighting) to the linear colour the screen should show, 0..peakBrightness.
* Throws std::invalid_argument for a negative exposure or a peak that is not positive.
*/
Vec3 toneMap(const Vec3& linear, const ToneMappingSettings& settings, float peakBrightness = sdrPeakBrightness);

/*
* Mirrors the ToneMapData block in tone_map.frag (std140, one vec4).
*/
struct ToneMapUniformData {
    float parameters[4]; // x: exposure, y: peak brightness, z: curve (a TONE_MAP_ number), w: unused
};

static_assert(sizeof(ToneMapUniformData) == 16, "ToneMapUniformData must match the shader's ToneMapData block");
static_assert(sizeof(ToneMapUniformData) <= maxUniformBlockBytes, "the shader can only read the first 4 KB of a uniform block");

// The settings in the shader's layout. Throws std::invalid_argument like toneMap.
ToneMapUniformData packToneMapping(const ToneMappingSettings& settings, float peakBrightness = sdrPeakBrightness);
