#pragma once

#include "render/gpu/bloom.h"
#include "render/gpu/camera_finish.h"
#include "render/gpu/display_output.h"
#include "render/gpu/tone_mapping.h"
#include "render/gpu/uniform_limits.h"

/*
* Mirrors the ToneMapData block in tone_map.frag (std140, nine vec4s).
*/
struct ToneMapUniformData {
    float parameters[4]; // x: exposure, y: peak brightness, z: curve (a TONE_MAP_ number), w: bloom strength (0 = none)
    float output[4];     // x: what SDR white is written as, y: 1 to dither for an 8-bit screen, 0 for a float one,
                         // z: what the bloom texture is multiplied by (1 / its level count), w: 1 to multiply the exposure by the
                         // adapted one (render/gpu/eye_adaptation.h)
    float nightVision[4]; // x: the rods' threshold, y: the cones' (stops), z: the blue shift, w: 1 for night vision on

    // The camera's finish (render/gpu/camera_finish.h).
    float whiteBalance[12]; // the white balance matrix's three rows, each with an unused w
    float grade[4];         // x: contrast, y: saturation, z: lift, w: gamma
    float lens[4];          // x: gain, y: vignetting, z: chromatic aberration (pixels at the corner), w: 1 to grade
    float view[4];          // xy: the tangents of the half field of view across and up, z: film grain, w: the frame
                            // (modulo cameraFrameCycle)
};

static_assert(sizeof(ToneMapUniformData) == 144, "ToneMapUniformData must match the shader's ToneMapData block");
static_assert(sizeof(ToneMapUniformData) <= maxUniformBlockBytes, "the shader can only read the first 4 KB of a uniform block");

// The settings, this frame's display output, its bloom mix and the camera's finish in the shader's layout. Throws
// std::invalid_argument like toneMap, or for camera settings out of range.
ToneMapUniformData packToneMapping(const ToneMappingSettings& settings, const DisplayOutput& display = {}, const BloomComposite& bloom = {},
                                   const CameraSettings& camera = {}, const CameraView& view = {});
