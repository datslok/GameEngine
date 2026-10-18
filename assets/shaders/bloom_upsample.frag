#version 450
#extension GL_GOOGLE_include_directive : require

// The tent's numbers, shared with render/gpu/bloom.cpp, which mirrors this shader and tests the maths.
#include "render/gpu/bloom_shared.h"

// The level below (half this one's size), already grown. Read with a linear sampler.
layout(set = 2, binding = 0) uniform sampler2D smaller;

// Must match BloomPassUniformData in render/gpu/bloom.h.
layout(std140, set = 3, binding = 0) uniform BloomPassData {
    vec4 texelSizes; // xy: one texel of the smaller level in 0..1 units, zw: one texel of this level
    vec4 options;    // x: how many of the smaller level's texels apart the tent's reads are
};

layout(location = 0) out vec4 outputColour;

/*
* One growing step: the smaller level spread by a 3x3 tent (1-2-1 by 2-4-2 by 1-2-1, out of 16) around this texel's
* place. The pipeline blends additively, so the result is added to what this level already holds (its own halving).
*/
void main() {
    vec2 centre = gl_FragCoord.xy * texelSizes.zw;
    vec2 step = texelSizes.xy * options.x;

    vec3 corners = texture(smaller, centre + vec2(-step.x, -step.y)).rgb
                 + texture(smaller, centre + vec2(step.x, -step.y)).rgb
                 + texture(smaller, centre + vec2(-step.x, step.y)).rgb
                 + texture(smaller, centre + vec2(step.x, step.y)).rgb;
    vec3 edges = texture(smaller, centre + vec2(0.0, -step.y)).rgb
               + texture(smaller, centre + vec2(-step.x, 0.0)).rgb
               + texture(smaller, centre + vec2(step.x, 0.0)).rgb
               + texture(smaller, centre + vec2(0.0, step.y)).rgb;

    vec3 spread = corners * BLOOM_TENT_CORNER + edges * BLOOM_TENT_EDGE + texture(smaller, centre).rgb * BLOOM_TENT_CENTRE;
    outputColour = vec4(spread, 1.0);
}
