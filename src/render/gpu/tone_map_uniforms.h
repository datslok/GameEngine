#pragma once

#include "render/gpu/bloom.h"
#include "render/gpu/display_output.h"
#include "render/gpu/tone_mapping.h"
#include "render/gpu/uniform_limits.h"

/*
* Mirrors the ToneMapData block in tone_map.frag (std140, two vec4s).
*/
struct ToneMapUniformData {
    float parameters[4]; // x: exposure, y: peak brightness, z: curve (a TONE_MAP_ number), w: bloom strength (0 = none)
    float output[4];     // x: what SDR white is written as, y: 1 to dither for an 8-bit screen, 0 for a float one,
                         // z: what the bloom texture is multiplied by (1 / its level count), w: 1 to multiply the exposure by the
                         // adapted one (render/gpu/eye_adaptation.h)
};

static_assert(sizeof(ToneMapUniformData) == 32, "ToneMapUniformData must match the shader's ToneMapData block");
static_assert(sizeof(ToneMapUniformData) <= maxUniformBlockBytes, "the shader can only read the first 4 KB of a uniform block");

// The settings, this frame's display output and its bloom mix in the shader's layout. Throws std::invalid_argument like
// toneMap.
ToneMapUniformData packToneMapping(const ToneMappingSettings& settings, const DisplayOutput& display = {}, const BloomComposite& bloom = {});
