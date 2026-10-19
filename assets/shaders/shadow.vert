#version 450
#extension GL_GOOGLE_include_directive : require

#include "instances.glsl"

// The shadow pass only needs where each vertex is, as seen from one face of the light's cube.
layout(location = 0) in vec3 position;

// The tile's view-projection, in world space like the instances (the shadow cache recognises tiles by world matrices).
// Must match GpuShadowView in render/gpu/gpu_scene.h.
layout(push_constant) uniform ShadowView {
    mat4 tileMatrix;
};

void main() {
    gl_Position = tileMatrix * (instances[drawList[gl_InstanceIndex]].model * vec4(position, 1.0));
}
