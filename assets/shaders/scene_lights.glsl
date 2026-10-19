// The frame's lights in set 1's storage buffers, written once per frame (render/gpu/light_uniforms.h). Resource
// declarations only (no functions), included by triangle.frag and cluster_lights.comp.
#ifndef SCENE_LIGHTS_GLSL
#define SCENE_LIGHTS_GLSL

// Must match GpuDirectionalLight, GpuPointLight and GpuSpotLight in render/gpu/light_uniforms.h. Tile numbers are -1
// for a light without a shadow; a point light's six cube faces are six tiles in a row from its first one.
struct DirectionalLightData {
    vec4 toLight;  // xyz: unit direction towards the light
    vec4 radiance; // rgb: colour times intensity
    int shadowTile;
};

struct PointLightData {
    vec4 positionRange; // xyz: position relative to the camera, w: range
    vec4 radiance;      // rgb: colour times intensity, w: radius of the glowing source
    int firstShadowTile;
    float shadowStrength; // 0..1 while shadows fade between lights
    float emitterRadius;  // the size of its glowing part, for soft shadows
};

struct SpotLightData {
    vec4 positionRange;     // xyz: position relative to the camera, w: range
    vec4 directionCosOuter; // xyz: unit beam direction, w: cosine of the outer cone angle
    vec4 radianceCosInner;  // rgb: colour times intensity, w: cosine of the inner cone angle
    float sourceRadius;     // radius of the glowing source, for the falloff
    int shadowTile;
    float shadowStrength;
    float emitterRadius;
};

layout(std430, set = 1, binding = 3) readonly buffer DirectionalLights {
    DirectionalLightData directional[];
};

layout(std430, set = 1, binding = 4) readonly buffer PointLights {
    PointLightData points[];
};

layout(std430, set = 1, binding = 5) readonly buffer SpotLights {
    SpotLightData spots[];
};

#endif
