#version 450
#extension GL_GOOGLE_include_directive : require

#include "instances.glsl"
#include "planar_reflection.glsl"

/*
* The coloured shadow map's glass (render/gpu/transparency.h): each pane as the shadow pass places it, with what its
* fragments need to find their share of the light: its normal, the way to the light (a local light's position less the
* point, which interpolates exactly as the point does, or the direction towards the sun), its texture coordinates and
* material, and how much its object scales lengths (thick glass's thickness).
*/
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

layout(location = 0) out vec3 worldNormal;
layout(location = 1) out vec2 textureUv;
layout(location = 2) out vec3 towardsLight;
layout(location = 4) flat out uint materialIndex;
layout(location = 5) flat out float objectScale;

// Must match GpuShadowView in render/gpu/gpu_scene.h.
layout(push_constant) uniform ShadowView {
    mat4 tileMatrix;
    vec4 tileClipPlane;
    vec4 tileLightPlace; // a local light's position (w 1), or the direction towards the sun (w 0)
};

out float gl_ClipDistance[1];

void main() {
    InstanceData instance = instances[drawList[gl_InstanceIndex]];
    vec4 world = instance.model * vec4(position, 1.0);
    gl_Position = tileMatrix * world;
    gl_ClipDistance[0] = planarClipDistance(tileClipPlane, world.xyz);
    worldNormal = mat3(instance.normalMatrix) * normal;
    towardsLight = tileLightPlace.w > 0.5 ? tileLightPlace.xyz - world.xyz : tileLightPlace.xyz;
    textureUv = uv;
    materialIndex = instance.material.x;
    objectScale = instance.handedness.y;
}
