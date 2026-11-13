#version 450
#extension GL_GOOGLE_include_directive : require

#include "instances.glsl"
#include "planar_reflection.glsl"

// The shadow pass only needs where each vertex is, as seen from one face of the light's cube.
layout(location = 0) in vec3 position;

// The tile's view-projection, in world space like the instances (the shadow cache recognises tiles by world matrices),
// and the plane (world space) what is behind is clipped at: an image light's mirror, or one nothing is behind.
// Must match GpuShadowView in render/gpu/gpu_scene.h.
layout(push_constant) uniform ShadowView {
    mat4 tileMatrix;
    vec4 tileClipPlane;
};

out float gl_ClipDistance[1];

void main() {
    vec4 world = instances[drawList[gl_InstanceIndex]].model * vec4(position, 1.0);
    gl_Position = tileMatrix * world;
    gl_ClipDistance[0] = planarClipDistance(tileClipPlane, world.xyz);
}
