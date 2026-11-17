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
#include "terrain_data.glsl"
#include "wind_data.glsl"
#include "card_thinning_data.glsl"

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
    uint entry = drawList[gl_InstanceIndex];
    uint instance = drawnInstance(entry);

    // Thinned cards (card_thinning_data.glsl) shaped exactly as triangle.vert shapes them, now and last frame alike.
    vec3 shaped = thinnedVertex(instances[instance], entry, gl_VertexIndex, position, vec3(0.0), cameraOrigin.xyz, levelOptions.x);
    vec3 previousShaped = thinnedVertex(instances[instance], entry, gl_VertexIndex, previousPosition, vec3(0.0), cameraOrigin.xyz, levelOptions.x);

    // As triangle.vert does: the camera subtracted from the translation first (camera-relative rendering).
    mat4 model = instances[instance].model;
    model[3].xyz -= cameraOrigin.xyz;
    uint kind = instances[instance].material.w;
    vec3 relativePosition = (model * vec4(shaped, 1.0)).xyz;
    relativePosition.y += groundLift(kind, relativePosition + cameraOrigin.xyz);
    float scale = instances[instance].handedness.y;
    vec4 sway = instances[instance].sway;
    relativePosition += windSwayOfVertex(instances[instance].model, sway, scale, instances[instance].hanging, gl_VertexIndex, shaped, relativePosition + cameraOrigin.xyz, cameraOrigin.xyz, false);
    gl_Position = relativeViewProjection * vec4(relativePosition, 1.0);

    // Last frame's sway from last frame's springs and time (the same fade: it changes too slowly to show).
    mat4 before = previousModels[instance];
    mat4 beforeInWorld = before;
    before[3].xyz -= cameraOrigin.xyz;
    currentClip = view.current * vec4(relativePosition, 1.0);
    vec3 previousRelative = (before * vec4(previousShaped, 1.0)).xyz;
    previousRelative.y += groundLift(kind, previousRelative + cameraOrigin.xyz);
    previousRelative += windSwayOfVertex(beforeInWorld, sway, scale, instances[instance].hanging, gl_VertexIndex, previousShaped, previousRelative + cameraOrigin.xyz, cameraOrigin.xyz, true);
    previousClip = view.previous * vec4(previousRelative, 1.0);
    textureUv = uv;
    materialIndex = instances[instance].material.x;
}
