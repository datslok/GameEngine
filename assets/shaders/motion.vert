#version 450
#extension GL_GOOGLE_include_directive : require

/*
* The velocity pass (render/gpu/taa.h): the objects that moved since last frame, drawn again over the main pass's depth,
* each vertex placed as the main pass places it (with the jitter, so it lands on the same pixels) and also projected
* without the jitter as it is now and as it was last frame, measured from this frame's camera. The fragment shader
* turns the two into the pixel's motion.
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

layout(location = 0) out vec4 currentClip;
layout(location = 1) out vec4 previousClip;

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
    previousClip = view.previous * (before * vec4(position, 1.0));
}
