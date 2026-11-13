#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/camera_finish_shared.h"

#include <cstdint>

/*
* The camera's finish: what a real camera and its lens do to the light after the scene is lit, applied in the tone
* mapping pass (tone_map.frag). Every effect starts neutral or off (the user's choice), so the picture is unchanged
* until one is switched on.
*
* - White balance and tint: which light shows as white. A camera set for tungsten (3000 K) shows a lamp-lit room as a
*   daylit one, and daylight blue. Done as the eye's cones adapt: each cone's response scaled (von Kries, with the
*   Bradford cone matrix) so the chosen light lands where 6500 K light does.
* - The grade (contrast, saturation, lift, gamma, gain), in linear light after the exposure and before the curve, as a
*   film colourist's: contrast in stops about mid grey, saturation about each pixel's luminance, then the ASC CDL's
*   slope (gain), offset (lift) and power (1 / gamma).
* - Natural vignetting: a lens lights the sensor's edges less, as cos^4 of the angle off its axis (the light comes in
*   slanted, spread over more area, from an aperture that looks narrower).
* - Chromatic aberration: a cheap lens bends red and blue differently, so they are scaled a hair apart about the
*   picture's centre.
* - Film grain: noise strongest in the mid-tones, different every frame.
*
* These functions mirror camera_finish.glsl, tested here without a GPU and against it by the GPU tests.
*/
struct CameraSettings {
    // The colour temperature of the light that shows as 6500 K light does (kelvin, on the blackbody locus, 1667 to
    // 25000), and how far that light lies off the locus (Duv in CIE 1960 uv, positive greener). 6500 and 0: unchanged.
    float whiteBalance = 6500.0f;
    float tint = 0.0f;

    // The grade, all neutral by default: contrast (stops about mid grey are multiplied by it), saturation (0 grey),
    // lift (added to the light, raising the blacks), gamma (the light raised to 1 / gamma) and gain (multiplied).
    float contrast = 1.0f;
    float saturation = 1.0f;
    float lift = 0.0f;
    float gamma = 1.0f;
    float gain = 1.0f;

    // The lens: how much of a real lens's cos^4 falloff to show (0 none, 1 all), how many pixels apart red and blue
    // land at the picture's corners, and the film grain's strength. All off by default.
    float vignetting = 0.0f;
    float chromaticAberration = 0.0f;
    float filmGrain = 0.0f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(CameraSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a setting outside its range (written so not a number fails too).
void validateCamera(const CameraSettings& settings);

// Whether the white balance or the grade is off its neutral values, so the pass applies them.
bool gradesColour(const CameraSettings& settings);

// A blackbody's colour at this temperature (1667 to 25000 K) as CIE 1931 xy: Kim et al. 2002's cubic fit of the
// Planckian locus.
Vec2 planckianChromaticity(float kelvin);

// The locus at this temperature moved tint (Duv) across it in CIE 1960 uv, towards green when positive.
Vec2 whiteBalanceChromaticity(float kelvin, float tint);

// Linear sRGB of a colour given as CIE 1931 xy and luminance Y.
Vec3 chromaticityToLinearSrgb(Vec2 xy, float luminance);

// Linear sRGB to linear sRGB (in the top-left 3 x 3): light of the white balance's colour to 6500 K light's, by a von
// Kries scaling of the Bradford cone responses. The identity at 6500 K and no tint.
Mat4 whiteBalanceMatrix(float kelvin, float tint);

// The grade on linear light after the exposure: contrast, saturation, then gain, lift and gamma; never negative.
Vec3 gradeColour(const Vec3& exposed, const CameraSettings& settings);

// What reaches the sensor at a point of the picture (NDC, -1..1 each way) given the tangents of the half field of view
// across and up: 1 - strength + strength cos^4 of the angle off the axis.
float naturalVignetting(Vec2 ndc, Vec2 tanHalfFov, float strength);

// Where a channel is read for the pixel at this place (pixels) in a picture of this size: moved along the line from the
// centre, shiftPixels at the corner and in proportion nearer in.
Vec2 aberrationPlace(Vec2 pixel, Vec2 size, float shiftPixels);

// The grain's noise for a pixel and a frame: pcg3d's first word's top 24 bits, in 0..1.
float grainNoise(std::uint32_t x, std::uint32_t y, std::uint32_t frame);

// Film grain on the tone mapped light (0..peak): (noise - 0.5) x strength x 4 l (1 - l) x peak added to every channel,
// l the luminance over the peak, so nothing at black or the peak, most at half; kept within 0..peak.
Vec3 filmGrain(const Vec3& mapped, float noise, float strength, float peak);

// The frame number the grain turns with is taken modulo this, so it stays exact as a float in the shader's block.
inline constexpr std::uint32_t cameraFrameCycle = CAMERA_FRAME_CYCLE;

// What the pass needs of the frame's camera: its vertical field of view (radians), its aspect ratio and the frame number.
struct CameraView {
    float verticalFov = 1.0f;
    float aspectRatio = 1.0f;
    std::uint32_t frame = 0;
};
