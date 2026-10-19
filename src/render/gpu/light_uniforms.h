#pragma once

#include "scene/lighting.h"

#include <cstdint>
#include <vector>

/*
* How many lights of each kind the shader's light buffers hold. They are storage buffers (runtime arrays), so these
* are the renderer's choice rather than a limit of the GPU: 1024 point lights take 48 KB. Directional lights stay few,
* since each one always gets a shadow tile (light_priority.cpp checks that every seat fits the atlas).
*/
inline constexpr int maxDirectionalLights = 4;
inline constexpr int maxPointLights = 1024;
inline constexpr int maxSpotLights = 256;

/*
* How many lights get a seat in the shader by default (LightPrioritySettings), well below the buffers' room: until
* clustered shading, every pixel loops over every seated light, so the cost grows with lights times pixels. A light out
* of a pixel's range costs only a distance check, so dozens are cheap; hundreds wait for clustering.
*/
inline constexpr int defaultPointSeats = 64;
inline constexpr int defaultSpotSeats = 8;

/*
* The light records triangle.frag reads, one storage buffer per kind (std430, the layout of storage buffers: a scalar
* takes 4 bytes and a vec4 16, with no padding of array elements to 16 bytes as uniform blocks need). Each light carries
* its own shadow: the shadow plan's tile numbers, its strength while it fades between lights, and the glowing size
* that softens it. Tile numbers are -1 for a light without a shadow. Positions are measured from the camera
* (camera-relative rendering, render/gpu/camera_relative.h), so the viewer is at the shader's origin.
*/
struct GpuDirectionalLight {
    float toLight[4];  // xyz: unit direction towards the light
    float radiance[4]; // rgb: colour times intensity
    std::int32_t shadowTile = -1;
    float unused[3]{};
};

struct GpuPointLight {
    float positionRange[4]; // xyz: position relative to the camera, w: range
    float radiance[4];      // rgb: colour times intensity, w: radius of the glowing source, for the falloff
    std::int32_t firstShadowTile = -1; // the first of its six cube faces' tiles
    float shadowStrength = 0.0f;
    float emitterRadius = 0.0f;
    float unused = 0.0f;
};

struct GpuSpotLight {
    float positionRange[4];     // xyz: position relative to the camera, w: range
    float directionCosOuter[4]; // xyz: unit beam direction, w: cosine of the outer cone angle
    float radianceCosInner[4];  // rgb: colour times intensity, w: cosine of the inner cone angle
    float sourceRadius = 1.0f;  // radius of the glowing source, for the falloff
    std::int32_t shadowTile = -1;
    float shadowStrength = 0.0f;
    float emitterRadius = 0.0f;
};

static_assert(sizeof(GpuDirectionalLight) == 48, "GpuDirectionalLight must match the shader's DirectionalLightData");
static_assert(sizeof(GpuPointLight) == 48, "GpuPointLight must match the shader's PointLightData");
static_assert(sizeof(GpuSpotLight) == 64, "GpuSpotLight must match the shader's SpotLightData");

// A frame's lights as the shader gets them: the ambient light and the counts (which go into the frame's uniform block,
// gpu_scene.h), and one record per light, in slot order.
struct GpuLights {
    float ambient[4]{};
    std::int32_t counts[4]{}; // x: directional lights used, y: point lights used, z: spotlights used
    std::vector<GpuDirectionalLight> directional;
    std::vector<GpuPointLight> points;
    std::vector<GpuSpotLight> spots;
};

// Whether the shader can use a light at all: a zero direction cannot be normalised, and the falloff divides by the range.
bool canBeDrawn(const DirectionalLight& light);
bool canBeDrawn(const PlacedPointLight& placed);
bool canBeDrawn(const PlacedSpotLight& placed);

// The lights the shader really gets: those that can be drawn, up to its limits, in order. Their positions in these lists
// are their slots in the shader, which shadow planning relies on too.
FrameLighting selectDrawableLights(const FrameLighting& lighting);

// Convert a frame's lights into the shader's layout, positions measured from the camera, dropping lights past the limits
// or that cannot be drawn. Every light starts without a shadow; attachShadows (shadow_map.h) adds the plan's.
GpuLights packLighting(const FrameLighting& lighting, const Vec3& cameraPosition);
