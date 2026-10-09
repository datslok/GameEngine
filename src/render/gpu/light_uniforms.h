#pragma once

#include "scene/lighting.h"

#include <cstdint>

// The most lights the shader can use. Must match the array sizes in triangle.frag.
inline constexpr int maxDirectionalLights = 4;
inline constexpr int maxPointLights = 16;
inline constexpr int maxSpotLights = 4;

struct DirectionalLightUniform {
    float toLight[4];  // xyz: unit direction towards the light
    float radiance[4]; // rgb: colour times intensity
};

struct PointLightUniform {
    float positionRange[4]; // xyz: world position, w: range
    float radiance[4];      // rgb: colour times intensity, w: radius of the glowing source, for the falloff
};

struct SpotLightUniform {
    float positionRange[4];     // xyz: world position, w: range
    float directionCosOuter[4]; // xyz: unit beam direction, w: cosine of the outer cone angle
    float radianceCosInner[4];  // rgb: colour times intensity, w: cosine of the inner cone angle
    float sourceRadius[4];      // x: radius of the glowing source, for the falloff
};

/*
* Mirrors the LightData block in triangle.frag byte for byte.
* std140 pads a vec3 to 16 bytes, so every member is a 4-component vector and nothing is padded behind our back.
*/
struct LightUniformData {
    float ambient[4];
    float cameraPosition[4]; // xyz: where the viewer is, for specular highlights
    std::int32_t counts[4]; // x: directional lights used, y: point lights used, z: spotlights used
    DirectionalLightUniform directional[maxDirectionalLights];
    PointLightUniform points[maxPointLights];
    SpotLightUniform spots[maxSpotLights];
};

static_assert(sizeof(LightUniformData) == 944, "LightUniformData must match the shader's LightData block");

// The lights the shader really gets: those that can be drawn, up to its limits, in order. Their positions in these lists
// are their slots in the shader, which shadow planning relies on too.
FrameLighting selectDrawableLights(const FrameLighting& lighting);

// Convert a frame's lights and the camera position into the shader's layout, dropping lights past the limits or that cannot be drawn.
LightUniformData packLighting(const FrameLighting& lighting, const Vec3& cameraPosition);
