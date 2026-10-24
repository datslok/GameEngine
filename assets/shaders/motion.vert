#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The velocity pass (render/gpu/taa.h): the objects that moved since last frame, drawn again over the main pass's depth,
* each vertex placed as the main pass places it (with the jitter, so it lands on the same pixels) and also projected
* without the jitter as it is now and as it was last frame, measured from this frame's camera. The fragment shader
* turns the two into the pixel's motion. Where it was last frame is last frame's matrix applied to last frame's shape:
* a deforming mesh's previous vertices (vertex binding 1), the same vertices for every other mesh.
*/
#include "frame_data.glsl"
#include "instances.glsl"

// Each instance's model matrix last frame (its current one when it did not move), as instances numbers them.
layout(std430, set = 1, binding = 21) readonly buffer PreviousModels {
    mat4 previousModels[];
};

// Must match GpuMotionView.
layout(push_constant) uniform MotionView {
    mat4 current;
    mat4 previous;
} view;

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 uv;
layout(location = 4) in vec3 previousPosition;

layout(location = 0) out vec4 currentClip;
layout(location = 1) out vec4 previousClip;
// For a cut-out's holes (motion.frag): its texture coordinates and material.
layout(location = 2) out vec2 textureUv;
layout(location = 3) flat out uint materialIndex;

invariant gl_Position;

void main() {
    uint instance = drawList[gl_InstanceIndex];

    // As triangle.vert does: the camera subtracted from the translation first (camera-relative rendering).
    mat4 model = instances[instance].model;
    model[3].xyz -= cameraOrigin.xyz;
    vec3 relativePosition = (model * vec4(position, 1.0)).xyz;
    gl_Position = relativeViewProjection * vec4(relativePosition, 1.0);

    mat4 before = previousModels[instance];
    before[3].xyz -= cameraOrigin.xyz;
    currentClip = view.current * vec4(relativePosition, 1.0);
    previousClip = view.previous * (before * vec4(previousPosition, 1.0));
    textureUv = uv;
    materialIndex = instances[instance].material.x;
}
