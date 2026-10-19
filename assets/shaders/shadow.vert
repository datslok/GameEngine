#version 450

// The shadow pass only needs where each vertex is, as seen from one face of the light's cube.
layout(location = 0) in vec3 position;

// One caster in one tile (render/gpu/gpu_scene.h, GpuShadowInstance): the tile's view-projection times the model matrix.
struct ShadowInstanceData {
    mat4 transform;
};

// Every redrawn tile's casters, one tile after another; an instanced draw's gl_InstanceIndex counts from its batch's first one.
layout(std430, set = 1, binding = 10) readonly buffer ShadowInstances {
    ShadowInstanceData shadowInstances[];
};

void main() {
    gl_Position = shadowInstances[gl_InstanceIndex].transform * vec4(position, 1.0);
}