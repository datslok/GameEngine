#pragma once

#include "scene/lighting.h"

#include <cstdint>

// The most lights the shader can use. Must match the array sizes in triangle.frag.
inline constexpr int maxDirectionalLights = 4;
inline constexpr int maxPointLights = 16;

struct DirectionalLightUniform {
    float toLight[4];  // xyz: unit direction towards the light
    float radiance[4]; // rgb: colour times intensity
};

struct PointLightUniform {
    float positionRange[4]; // xyz: world position, w: range
    float radiance[4];
};

/*
* Mirrors the LightData block in triangle.frag byte for byte.
* std140 pads a vec3 to 16 bytes, so every member is a 4-component vector and nothing is padded behind our back.
*/
struct LightUniformData {
    float ambient[4];
    std::int32_t counts[4]; // x: directional lights used, y: point lights used
    DirectionalLightUniform directional[maxDirectionalLights];
    PointLightUniform points[maxPointLights];
};

static_assert(sizeof(LightUniformData) == 672, "LightUniformData must match the shader's LightData block");

// Convert a frame's lights into the shader's layout, dropping lights past the limits or that cannot be drawn.
LightUniformData packLighting(const FrameLighting& lighting);
