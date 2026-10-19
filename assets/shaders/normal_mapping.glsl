// Normal mapping, included with #include "normal_mapping.glsl": the arithmetic mirrors render/gpu/normal_mapping.cpp,
// which tests it, and tests/gpu checks these functions against it on the GPU.

#ifndef NORMAL_MAPPING_GLSL
#define NORMAL_MAPPING_GLSL

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

#endif
