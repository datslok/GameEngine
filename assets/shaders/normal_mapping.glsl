// Normal mapping, included with #include "normal_mapping.glsl": the arithmetic mirrors render/gpu/normal_mapping.cpp,
// which tests it, and tests/gpu checks these functions against it on the GPU.

#ifndef NORMAL_MAPPING_GLSL
#define NORMAL_MAPPING_GLSL

#include "render/gpu/normal_mapping_shared.h"

// A texel as a direction in the surface's frame: 0..1 to -1..1, the scale on the sideways parts only.
vec3 decodeNormalTexel(vec3 texel, float scale) {
    return (texel * 2.0 - 1.0) * vec3(scale, scale, 1.0);
}

/*
* The frame's axes weighted by the decoded direction: x along the tangent, y along the bitangent (up the picture),
* z along the normal. The interpolated normal and tangent are used as they arrive, not normalised first, as MikkTSpace
* asks so the result matches the baker's; only the sum is normalised. Zero when there is no usable direction.
*/
vec3 perturbNormal(vec3 normal, vec4 tangent, vec3 tangentSpaceNormal) {
    vec3 bitangent = tangent.w * cross(normal, tangent.xyz);
    vec3 combined = tangentSpaceNormal.x * tangent.xyz + tangentSpaceNormal.y * bitangent + tangentSpaceNormal.z * normal;
    float combinedLength = length(combined);
    return combinedLength > 0.0 ? combined / combinedLength : vec3(0.0);
}

/*
* Normal map filtering (Toksvig 2005): the roughness widened by the bumps a smaller mip averaged away. The texel's
* shortening beyond 8-bit rounding is the bumps' spread, (1 - L) / L, which adds twice over to alpha squared, stretched by
* the scale's square like the slopes. The texel is 0..1 as sampled. At most 1.
*/
float toksvigRoughness(float roughness, vec3 texel, float scale) {
    float texelLength = min(length(decodeNormalTexel(texel, 1.0)) + NORMAL_TOKSVIG_QUANTIZATION_SLACK, 1.0);
    float spread = (1.0 - texelLength) / texelLength * scale * scale;

    if (!(spread > 0.0)) {
        return roughness;
    }

    float alphaSquared = roughness * roughness * roughness * roughness + 2.0 * spread;
    return min(sqrt(sqrt(alphaSquared)), 1.0);
}

#endif
