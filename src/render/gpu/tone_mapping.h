#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"
#include "render/gpu/tone_mapping_shared.h"

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

    // Multiplies the scene's brightness before the curve, like a camera's exposure. 1 leaves it unchanged (chosen by the
    // user by eye with eye adaptation on; 1.5 kept scenes lit for the old, clipped look about as bright in their
    // shadows and mid-tones, since the curves darken the faintest values: AgX shows 0.006 as 0.005 and 0.5 as 0.52).
    float exposure = 1.0f;

    /*
    * Eye adaptation (render/gpu/eye_adaptation.h): the exposure above becomes a bias, multiplied by one that follows
    * the scene's brightness. The reference is the scene brightness (its middle pixels' mean luminance) shown as it is;
    * the strength how much of a difference from it is made up (1 all, 0 none: a night five stops darker shows 2.5
    * stops darker at 0.5); the exposure stays within its lowest and highest stops (at most one brighter, chosen by eye
    * so dark scenes stay dark); it eases towards its target with a
    * time constant, slower into the dark than into the light, as an eye does.
    */
    bool eyeAdaptation = true;
    float adaptationReference = 0.3f;
    float adaptationStrength = 0.5f;
    float darkAdaptationSeconds = 1.0f;
    float darkAdaptationDelaySeconds = 1.0f; // into the dark, the eye first holds this long (holdThenAdaptExposure)
    float brightAdaptationSeconds = 0.7f;
    float lowestExposureEv = -4.0f;
    float highestExposureEv = 1.0f;

    /*
    * Night vision (nightVision below): in the dark the eye sees with its rods, which see no colour, so dim pixels fade
    * towards a grey (a little blue, by the blue shift, 0..1) while the eye is set for a dim scene. All rods below the
    * rods' stops, all cones above the cones', a smooth blend between; measured, like the eye adaptation's mean, in the
    * stops of the light before exposure. The light lab's moonlit field is near -8 EV and its sunlit room by day near
    * -6, so the thresholds sit between those (by measurement, since the engine's light has no physical units yet: the
    * real thresholds are about 0.005 and 3 cd/m2).
    */
    bool nightVision = true;
    float rodsFullEv = -9.5f;
    float conesFullEv = -7.0f;
    float nightBlueShift = 0.5f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(ToneMappingSettings& settings, SettingsVisitor& visitor);

// The brightest an ordinary (SDR) screen shows: its white. An HDR screen goes several times past it.
inline constexpr float sdrPeakBrightness = 1.0f;

// Throws std::invalid_argument for a negative exposure or a peak that is not positive.
void validateToneMapping(const ToneMappingSettings& settings, float peakBrightness);

/*
* The rods' light from a linear colour: Ward Larson's scotopic luminance, Y (1.33 (1 + (Y + Z) / X) - 1.68) from CIE XYZ,
* divided by white's, so white keeps its brightness when the rods take over and only colours shift: blue looks brighter
* and red darker than by day (the Purkinje shift). Black (X = 0, which no other colour has) is 0.
*/
float rodLuminance(const Vec3& linear);

/*
* How much of a pixel the rods see: 1 - smoothstep(rods' stops, cones' stops, the brighter of the pixel's stops and the
* scene's the eye is set for). A pixel goes grey only when both are dim: a lamp in a dark street keeps its colour, and
* so does a dark corner of a sunlit room, the eye set for the window.
*/
float rodShare(float pixelEv, float eyeEv, const ToneMappingSettings& settings);

/*
* Night vision: the colour blended by the rods' share towards the rods' grey, rodLuminance times the night tint (white
* at no blue shift, NIGHT_TINT at full, luminance 1 either way). Applied to the light before exposure; off, it is not
* called at all.
*/
Vec3 nightVision(const Vec3& linear, float eyeEv, const ToneMappingSettings& settings);

/*
* One pixel's linear colour (after lighting) to the linear colour the screen should show, 0..peakBrightness.
* Throws std::invalid_argument for a negative exposure or a peak that is not positive.
*/
Vec3 toneMap(const Vec3& linear, const ToneMappingSettings& settings, float peakBrightness = sdrPeakBrightness);
