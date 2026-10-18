#pragma once

#include "render/gpu/display_output.h"
#include "render/gpu/tone_mapping.h"
#include "render/gpu/uniform_limits.h"

/*
* Mirrors the ToneMapData block in tone_map.frag (std140, two vec4s).
*/
struct ToneMapUniformData {
    float parameters[4]; // x: exposure, y: peak brightness, z: curve (a TONE_MAP_ number), w: unused
    float output[4];     // x: what SDR white is written as, y: 1 to dither for an 8-bit screen, 0 for a float one
};

static_assert(sizeof(ToneMapUniformData) == 32, "ToneMapUniformData must match the shader's ToneMapData block");
static_assert(sizeof(ToneMapUniformData) <= maxUniformBlockBytes, "the shader can only read the first 4 KB of a uniform block");

// The settings and this frame's display output in the shader's layout. Throws std::invalid_argument like toneMap.
ToneMapUniformData packToneMapping(const ToneMappingSettings& settings, const DisplayOutput& display = {});
