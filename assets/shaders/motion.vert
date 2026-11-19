#version 450
#extension GL_GOOGLE_include_directive : require
// gl_BaseVertexARB: the drawn mesh's first vertex in the shared mesh buffer (render/gpu/mesh_pool.h); the per-vertex
// records (hanging things, thinned cards) count from the mesh's own first vertex.
#extension GL_ARB_shader_draw_parameters : require

/*
* The velocity pass (render/gpu/taa.h): the objects that moved since last frame, drawn again over the main pass's depth,
* each vertex placed as the main pass places it (with the jitter, so it lands on the same pixels) and also projected
* without the jitter as it is now and as it was last frame, measured from this frame's camera. The fragment shader
* turns the two into the pixel's motion. Where it was last frame is last frame's matrix applied to last frame's shape:
* a deforming mesh's previous vertices (vertex binding 1), the same vertices for every other mesh. A placed plant's part
* (drawn_objects.glsl) never moves: its matrix last frame is its matrix now, only its sway differs.
*/
#include "frame_data.glsl"
#include "drawn_objects.glsl"
#include "terrain_data.glsl"
#include "wind_data.glsl"
#include "card_thinning_data.glsl"
#include "level_fade.glsl"

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
// Its level fade (level_fade.glsl): the pixels its level keeps in the depth prepass, so the motion is its own there.
layout(location = 4) flat out uint levelFade;

invariant gl_Position;

void main() {
    // A frame's draw, or a placed plant's part (drawn_objects.glsl).
    bool placed = uint(gl_BaseInstanceARB) >= PLACED_INSTANCE_BASE;
    DrawnObject drawn = drawnObject(uint(gl_InstanceIndex), uint(gl_BaseInstanceARB));
    uint entry = drawn.entry;
    InstanceData object = drawn.instance;
    levelFade = drawListFade(entry);

    // Thinned cards (card_thinning_data.glsl) shaped exactly as triangle.vert shapes them, now and last frame alike.
    vec3 shaped = thinnedVertex(object, entry, gl_VertexIndex - gl_BaseVertexARB, position, vec3(0.0), cameraOrigin.xyz, levelOptions.x);
    vec3 previousShaped = thinnedVertex(object, entry, gl_VertexIndex - gl_BaseVertexARB, previousPosition, vec3(0.0), cameraOrigin.xyz, levelOptions.x);

    // As triangle.vert does: the camera subtracted from the translation first (camera-relative rendering).
    mat4 model = object.model;
    model[3].xyz -= cameraOrigin.xyz;
    uint kind = object.material.w;
    vec3 relativePosition = (model * vec4(shaped, 1.0)).xyz;
    relativePosition.y += groundLift(kind, relativePosition + cameraOrigin.xyz);
    float scale = object.handedness.y;
    vec4 sway = object.sway;
    relativePosition += windSwayOfVertex(object.model, sway, scale, object.hanging, gl_VertexIndex - gl_BaseVertexARB, shaped, relativePosition + cameraOrigin.xyz, cameraOrigin.xyz, false);
    gl_Position = relativeViewProjection * vec4(relativePosition, 1.0);

    // Last frame's sway from last frame's springs and time (the same fade: it changes too slowly to show).
    mat4 before = placed ? object.model : previousModels[drawnInstance(entry)];
    mat4 beforeInWorld = before;
    before[3].xyz -= cameraOrigin.xyz;
    currentClip = view.current * vec4(relativePosition, 1.0);
    vec3 previousRelative = (before * vec4(previousShaped, 1.0)).xyz;
    previousRelative.y += groundLift(kind, previousRelative + cameraOrigin.xyz);
    previousRelative += windSwayOfVertex(beforeInWorld, sway, scale, object.hanging, gl_VertexIndex - gl_BaseVertexARB, previousShaped, previousRelative + cameraOrigin.xyz, cameraOrigin.xyz, true);
    previousClip = view.previous * vec4(previousRelative, 1.0);
    textureUv = uv;
    materialIndex = object.material.x;
}
