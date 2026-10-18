#version 450
#extension GL_GOOGLE_include_directive : require

// The filter's numbers, shared with render/gpu/bloom.cpp, which mirrors this shader and tests the maths.
#include "render/gpu/bloom_shared.h"

// The level above: the lit scene for level 0, otherwise the previous level. Read with a linear sampler, so each read
// where four texels meet is their average.
layout(set = 2, binding = 0) uniform sampler2D source;

// Must match BloomPassUniformData in render/gpu/bloom.h.
layout(std140, set = 3, binding = 0) uniform BloomPassData {
    vec4 texelSizes; // xy: one source texel in 0..1 units, zw: one target texel
    vec4 options;    // x: 1 for the Karis average (only from the scene)
};

layout(location = 0) out vec4 outputColour;

float karisWeight(vec3 colour) {
    return 1.0 / (1.0 + dot(colour, vec3(BLOOM_LUMA_RED, BLOOM_LUMA_GREEN, BLOOM_LUMA_BLUE)));
}

// A read the given number of source texels from this texel's centre. Nothing below black, so a stray negative value
// cannot spread.
vec3 read(vec2 centre, float texelsX, float texelsY) {
    return max(texture(source, centre + vec2(texelsX, texelsY) * texelSizes.xy).rgb, vec3(0.0));
}

/*
* One halving with the 13-tap filter (Jimenez 2014): thirteen reads that each average a 2x2 block, grouped into five
* overlapping 4x4 boxes, the inner one weighted a half and the four corner ones an eighth each. With the Karis average,
* each box is also weighted by 1 / (1 + its luminance) and the sum divided by the weights, so a lone very bright pixel
* is outweighed by the dark boxes beside it while a uniform area keeps its colour.
*/
void main() {
    vec2 centre = gl_FragCoord.xy * texelSizes.zw;

    vec3 a = read(centre, -2.0, -2.0);
    vec3 b = read(centre, 0.0, -2.0);
    vec3 c = read(centre, 2.0, -2.0);
    vec3 d = read(centre, -2.0, 0.0);
    vec3 e = read(centre, 0.0, 0.0);
    vec3 f = read(centre, 2.0, 0.0);
    vec3 g = read(centre, -2.0, 2.0);
    vec3 h = read(centre, 0.0, 2.0);
    vec3 i = read(centre, 2.0, 2.0);
    vec3 j = read(centre, -1.0, -1.0);
    vec3 k = read(centre, 1.0, -1.0);
    vec3 l = read(centre, -1.0, 1.0);
    vec3 m = read(centre, 1.0, 1.0);

    vec3 boxes[5] = vec3[](
        (j + k + l + m) * 0.25,
        (a + b + d + e) * 0.25,
        (b + c + e + f) * 0.25,
        (d + e + g + h) * 0.25,
        (e + f + h + i) * 0.25);
    float weights[5] = float[](BLOOM_INNER_BOX_WEIGHT, BLOOM_OUTER_BOX_WEIGHT, BLOOM_OUTER_BOX_WEIGHT,
                               BLOOM_OUTER_BOX_WEIGHT, BLOOM_OUTER_BOX_WEIGHT);
    bool karis = options.x > 0.5;

    vec3 sum = vec3(0.0);
    float weightSum = 0.0;

    for (int box = 0; box < 5; ++box) {
        float weight = weights[box] * (karis ? karisWeight(boxes[box]) : 1.0);
        sum += boxes[box] * weight;
        weightSum += weight;
    }

    outputColour = vec4(sum / weightSum, 1.0);
}
