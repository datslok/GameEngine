#pragma once

#include "render/gpu/shader_shared.h"
#include "render/gpu/uniform_limits.h"
#include "scene/lighting.h"

#include <cstdint>

// The most lights the shader can use, from the header triangle.frag shares, so its array sizes always match.
// Each pixel loops over every light it gets, but a light out of range costs only a distance check, so dozens are cheap.
// Hundreds would need clustered shading (each pixel only looks at the lights that reach its part of the view).
inline constexpr int maxDirectionalLights = MAX_DIRECTIONAL_LIGHTS;
inline constexpr int maxPointLights = MAX_POINT_LIGHTS;
inline constexpr int maxSpotLights = MAX_SPOT_LIGHTS;

// Per-light shadow values are packed four to a vec4 (std140 pads single floats to 16 bytes), so slot i is at [i / 4][i % 4].
inline constexpr int pointLightVec4s = (maxPointLights + 3) / 4;
inline constexpr int spotLightVec4s = (maxSpotLights + 3) / 4;

struct DirectionalLightUniform {
    float toLight[4];  // xyz: unit direction towards the light
    float radiance[4]; // rgb: colour times intensity
};

struct PointLightUniform {
    float positionRange[4]; // xyz: position relative to the camera, w: range
    float radiance[4];      // rgb: colour times intensity, w: radius of the glowing source, for the falloff
};

struct SpotLightUniform {
    float positionRange[4];     // xyz: position relative to the camera, w: range
    float directionCosOuter[4]; // xyz: unit beam direction, w: cosine of the outer cone angle
    float radianceCosInner[4];  // rgb: colour times intensity, w: cosine of the inner cone angle
    float sourceRadius[4];      // x: radius of the glowing source, for the falloff
};

/*
* Mirrors the LightData block in triangle.frag byte for byte.
* std140 pads a vec3 to 16 bytes, so every member is a 4-component vector and nothing is padded behind our back.
* Positions are measured from the camera (camera-relative rendering, render/gpu/camera_relative.h), so the viewer is at
* the shader's origin and needs no field of its own.
*/
struct LightUniformData {
    float ambient[4];
    std::int32_t counts[4]; // x: directional lights used, y: point lights used, z: spotlights used
    DirectionalLightUniform directional[maxDirectionalLights];
    PointLightUniform points[maxPointLights];
    SpotLightUniform spots[maxSpotLights];
};

static_assert(sizeof(LightUniformData) == 2720, "LightUniformData must match the shader's LightData block");
static_assert(sizeof(LightUniformData) <= maxUniformBlockBytes, "the shader can only read the first 4 KB of a uniform block");

// Whether the shader can use a light at all: a zero direction cannot be normalised, and the falloff divides by the range.
bool canBeDrawn(const DirectionalLight& light);
bool canBeDrawn(const PlacedPointLight& placed);
bool canBeDrawn(const PlacedSpotLight& placed);

// The lights the shader really gets: those that can be drawn, up to its limits, in order. Their positions in these lists
// are their slots in the shader, which shadow planning relies on too.
FrameLighting selectDrawableLights(const FrameLighting& lighting);

// Convert a frame's lights into the shader's layout, positions measured from the camera, dropping lights past the limits
// or that cannot be drawn.
LightUniformData packLighting(const FrameLighting& lighting, const Vec3& cameraPosition);
