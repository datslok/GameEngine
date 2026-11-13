#version 450
#extension GL_GOOGLE_include_directive : require

#include "instances.glsl"
#include "planar_reflection.glsl"

// The shadow pass for cut-outs: where each vertex is, as shadow.vert places it, and its texture coordinates and
// material for cut_out.frag to find the holes, at the locations triangle.vert gives them.
layout(location = 0) in vec3 position;
layout(location = 2) in vec2 uv;

layout(location = 1) out vec2 textureUv;
layout(location = 4) flat out uint materialIndex;

// Must match GpuShadowView in render/gpu/gpu_scene.h.
layout(push_constant) uniform ShadowView {
    mat4 tileMatrix;
    vec4 tileClipPlane;
};

out float gl_ClipDistance[1];

void main() {
    InstanceData instance = instances[drawList[gl_InstanceIndex]];
    vec4 world = instance.model * vec4(position, 1.0);
    gl_Position = tileMatrix * world;
    gl_ClipDistance[0] = planarClipDistance(tileClipPlane, world.xyz);
    textureUv = uv;
    materialIndex = instance.material.x;
}
