// The scene's vertex shader (triangle.vert; the main pass's, with its motion vectors, triangle_motion.vert): one body,
// so the depth prepass and the main pass place every vertex alike. Included after the extensions it needs.
#include "frame_data.glsl"
#include "ground_layers.glsl"
#include "drawn_objects.glsl"
#include "planar_reflection.glsl"
#include "render/gpu/reflection_probe_shared.h"
#include "terrain_data.glsl"
#include "impostor.glsl"
#include "wind_data.glsl"
#include "liana_data.glsl"
#include "card_thinning_data.glsl"
#include "level_fade.glsl"

// Whether this pipeline draws a capture (a reflection probe's cube face, or a flat mirror's picture): its
// view-projection comes as push constants (render/gpu/reflection_probe.h, cubeFaceViewProjection;
// render/gpu/planar_reflection.h, planarViewProjection), the frame's being the camera's.
layout(constant_id = PROBE_CAPTURE_VERTEX_CONSTANT) const bool captureView = false;

// Must match GpuCaptureView.
layout(push_constant) uniform CaptureView {
    mat4 captureViewProjection;
    vec4 captureClipPlane; // what is behind it is cut away: a mirror's plane (planarClipPlane), or one nothing is behind
    vec4 captureEye;       // xyz: the eye it is seen from (the camera, or the camera reflected across the mirror); w: its error allowed a metre away, for card thinning
};

// Every pipeline clamps depth rather than clip at the near plane, so a mirror's picture cuts what is behind the mirror
// here: a negative distance clips (render/gpu/planar_reflection.h).
out float gl_ClipDistance[1];

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
// xyz: the surface's direction of increasing u; w: +1 or -1, which side of it "up the picture" is (MeshVertex).
layout(location = 3) in vec4 tangent;

#ifdef MAIN_MOTION
// The main pass's motion vectors (render/gpu/taa.h): last frame's shape (a deforming mesh's previous vertices, vertex
// binding 1; the same vertices for every other mesh) and each instance's model matrix last frame (its current one when it
// did not move).
layout(location = 4) in vec3 previousPosition;

layout(std430, set = 1, binding = 21) readonly buffer PreviousModels {
    mat4 previousModels[];
};
#endif

layout(location = 0) out vec3 worldNormal;
layout(location = 1) out vec2 textureUv;
layout(location = 2) out vec3 relativePosition;
layout(location = 3) out vec4 worldTangent;
// Which entry of the material buffer shades this object: the same for every fragment of a triangle (flat, not interpolated).
layout(location = 4) flat out uint materialIndex;
// How much the object scales lengths (uniformScale), for lengths its material gives in model units (thick glass).
layout(location = 5) flat out float objectScale;
// Where a capture is seen from, measured from the camera (the origin outside captures), for the fragments' view direction.
layout(location = 6) flat out vec3 viewerPosition;
// For projected layers (render/gpu/ground_layers.h): the vertex in the object's own axes, in metres, and those axes in the
// world (the model's rotation), to turn normals into the object's frame and back.
layout(location = 7) out vec3 framePosition;
layout(location = 8) flat out mat3 frameAxes;
// The level fade of its draw list entry (level_fade.glsl, 0 none), for the depth prepass to keep its share of the pixels.
layout(location = 11) flat out uint levelFade;

#ifdef MAIN_MOTION
// Where the vertex was last frame through last frame's camera (motionPrevious; the fragment knows where it is now from
// its pixel and the jitter, pixelMotion), and 1 where it moved, swayed or changed shape. A triangle with no moving
// vertex keeps no motion (the camera's, from depth); one with some takes every vertex's own last place.
layout(location = 12) out vec4 previousClip;
layout(location = 13) out float movement;

// The vertex standing still: last frame it was where it is, seen by last frame's camera.
void stillMotion(vec3 relative) {
    previousClip = motionPrevious * vec4(relative, 1.0);
    movement = 0.0;
}
#endif

// The depth prepass runs this same shader, and the main pass then shades only fragments whose depth EQUALS the one the
// prepass stored. Invariant makes the compiler compute the position the same way in both pipelines, bit for bit (it
// may not, for instance, fuse a multiply and an add in one and not the other).
invariant gl_Position;

void main() {
    // A frame's draw, or a placed plant's part (drawn_objects.glsl).
    DrawnObject drawn = drawnObject(uint(gl_InstanceIndex), uint(gl_BaseInstanceARB));
    uint entry = drawn.entry;
    InstanceData instance = drawn.instance;
    levelFade = drawListFade(entry);
    mat4 normalMatrix = instance.normalMatrix;
    vec4 handedness = instance.handedness;
    materialIndex = instance.material.x;
    objectScale = handedness.y;

    // Camera-relative rendering (render/gpu/camera_relative.h): the camera is subtracted from the model matrix's
    // translation before any product, as relativeTo does, so positions are measured from the camera along the world's
    // axes and the numbers stay small and precise however far from the world's origin the camera is. Point lights
    // need to know where each fragment is, relative to the camera like the lights themselves.
    mat4 model = instance.model;
    model[3].xyz -= cameraOrigin.xyz;

    // A terrain patch (scene/terrain.h): its mesh is a flat grid, lifted onto the land here, slid into the next level's
    // grid as it nears its level's reach (seen from the camera in every view, so all views draw one surface), its normal,
    // tangent and texture coordinates from the land.
    uint terrainPatchNumber = instance.material.z;

    if (terrainPatchNumber > 0u) {
        TerrainVertex land = terrainVertex(terrainPatches[terrainPatchNumber - 1u], position.xz, cameraOrigin.xyz);
        relativePosition = land.position - cameraOrigin.xyz;
        gl_Position = (captureView ? captureViewProjection : relativeViewProjection) * vec4(relativePosition, 1.0);
        gl_ClipDistance[0] = captureView ? planarClipDistance(captureClipPlane, relativePosition) : 1.0;
        viewerPosition = captureView ? captureEye.xyz : vec3(0.0);
        textureUv = land.uv;
        frameAxes = mat3(1.0);
        framePosition = land.position;
        worldNormal = land.normal;
        worldTangent = land.tangent;
#ifdef MAIN_MOTION
        stillMotion(relativePosition);
#endif
        return;
    }

    /*
    * An impostor's card (scene/impostor.h): its corners carry the tree's middle and radius (the normal's x); turned here
    * to face the viewer (a capture's own eye), its axes the frame plane's for the way the viewer looks, so the bake's
    * frames and the card agree. The fragment shader gets the card point (offset from the middle over the radius) and the
    * view direction in the tree's frame (the tangent's xyz), and the tree's axes to turn the baked normals into the world.
    */
    if (instance.material.w == uint(INSTANCE_KIND_IMPOSTOR)) {
        vec3 treeScale = projectionScale(mat3(instance.model));
        mat3 treeAxes = projectionAxes(mat3(instance.model), treeScale);
        vec3 middle = (model * vec4(position - vec3(uv * normal.x, 0.0), 1.0)).xyz;
        vec3 viewer = captureView ? captureEye.xyz : vec3(0.0);
        vec3 toViewer = normalize(viewer - middle);
        vec3 viewDirection = transpose(treeAxes) * toViewer;
        vec3 right = impostorFrameRight(viewDirection);
        vec3 up = impostorFrameUp(viewDirection, right);
        vec3 cardPoint = right * uv.x + up * uv.y;
        relativePosition = middle + treeAxes * (cardPoint * normal.x * treeScale);
        gl_Position = (captureView ? captureViewProjection : relativeViewProjection) * vec4(relativePosition, 1.0);
        gl_ClipDistance[0] = captureView ? planarClipDistance(captureClipPlane, relativePosition) : 1.0;
        viewerPosition = captureView ? captureEye.xyz : vec3(0.0);
        textureUv = uv * 0.5 + 0.5;
        frameAxes = treeAxes;
        framePosition = cardPoint;
        worldNormal = toViewer;
        // w: the card's metres a unit of card point (the radius times the plant's scale, taken as even: plants are placed
        // scaled alike along every axis), to move shadow lookups to the leaf seen behind the card.
        worldTangent = vec4(viewDirection, normal.x * (treeScale.x + treeScale.y + treeScale.z) / 3.0);
#ifdef MAIN_MOTION
        stillMotion(relativePosition);
#endif
        return;
    }

    /*
    * A liana (scene/liana_shape.h): the template tube bent onto its record's catenary, or its ivy's cards placed along
    * it, swayed as it hangs between its trees, the same in every view; the instance's matrix only placed the template's
    * box (its bounds and level). Measured from the camera from its first hold on. Its shape never changes, so only its
    * sway moves it (an ivy card grows and shrinks with the camera's distance, too slowly to need motion vectors).
    */
    if (instance.hanging.w > 0u) {
        LianaDrawnVertex bent = lianaDrawnVertex(instance.hanging.w, position, uv, cameraOrigin.xyz);
        LianaShape shape = bent.shape;
        vec3 hold = shape.from.xyz - cameraOrigin.xyz;
        vec3 world = shape.from.xyz + bent.offset;
        vec3 swaysAt = shape.from.xyz + bent.swaysAt;
        float swing = uintBitsToFloat(instance.hanging.y);
        vec3 lianaSway = lianaSwayOffset(shape, instance.sway, swing, bent.along, bent.arm, swaysAt, cameraOrigin.xyz, false);
        relativePosition = hold + bent.offset + lianaSway;
        gl_Position = (captureView ? captureViewProjection : relativeViewProjection) * vec4(relativePosition, 1.0);
        gl_ClipDistance[0] = captureView ? planarClipDistance(captureClipPlane, relativePosition) : 1.0;
        viewerPosition = captureView ? captureEye.xyz : vec3(0.0);
        textureUv = bent.uv;
        frameAxes = mat3(1.0);
        framePosition = world;
        worldNormal = bent.normal;
        worldTangent = vec4(bent.tangent, 1.0);
#ifdef MAIN_MOTION
        stillMotion(relativePosition);

        if (motionOptions.x != 0.0 && lianaSway != vec3(0.0)) {
            vec3 previousRelative = hold + bent.offset + lianaSwayOffset(shape, instance.sway, swing, bent.along, bent.arm, swaysAt, cameraOrigin.xyz, true);
            previousClip = motionPrevious * vec4(previousRelative, 1.0);
            movement = 1.0;
        }
#endif
        return;
    }

    // Thinned cards (scene/card_thinning.h): each shrunk or grown about its stem as the view's distance allows, in every view
    // that marks it the same way, so the prepass and the main pass agree to the bit.
    vec3 shaped = thinnedVertex(instance, entry, gl_VertexIndex - gl_BaseVertexARB, position, captureView ? captureEye.xyz : vec3(0.0), cameraOrigin.xyz,
                                captureView ? captureEye.w : levelOptions.x);
    relativePosition = (model * vec4(shaped, 1.0)).xyz;
    relativePosition.y += groundLift(instance.material.w, relativePosition + cameraOrigin.xyz, instance.model[3].y);
    // The wind (render/gpu/wind.h): the same sway in every view, so the prepass and the main pass agree to the bit.
    vec3 swayOffset = windSwayOfVertex(instance.model, instance.sway, handedness.y, instance.hanging, gl_VertexIndex - gl_BaseVertexARB, shaped, relativePosition + cameraOrigin.xyz, cameraOrigin.xyz, false);
    relativePosition += swayOffset;
    gl_Position = (captureView ? captureViewProjection : relativeViewProjection) * vec4(relativePosition, 1.0);
    gl_ClipDistance[0] = captureView ? planarClipDistance(captureClipPlane, relativePosition) : 1.0;
    viewerPosition = captureView ? captureEye.xyz : vec3(0.0);

#ifdef MAIN_MOTION
    /*
    * Last frame's place, only for a vertex whose instance moved (its matrix), that the wind moves now (last frame's
    * springs and time; the same fade, which changes too slowly to show: no offset now, past the fade or at a trunk's foot,
    * is none last frame either) or whose shape changed (its previous vertex): last frame's matrix applied to last frame's
    * shape, thinned, lifted and swayed as now. Everything else stands still. A placed plant's part (drawn_objects.glsl)
    * never moves: its matrix last frame is its matrix now, only its sway differs.
    */
    stillMotion(relativePosition);
    bool placed = uint(gl_BaseInstanceARB) >= PLACED_INSTANCE_BASE;
    mat4 before = placed ? instance.model : previousModels[drawnInstance(entry)];
    bool moves = before != instance.model || swayOffset != vec3(0.0) || previousPosition != position;

    if (motionOptions.x != 0.0 && moves) {
        vec3 previousShaped = thinnedVertex(instance, entry, gl_VertexIndex - gl_BaseVertexARB, previousPosition, vec3(0.0), cameraOrigin.xyz, levelOptions.x);
        mat4 beforeRelative = before;
        beforeRelative[3].xyz -= cameraOrigin.xyz;
        vec3 previousRelative = (beforeRelative * vec4(previousShaped, 1.0)).xyz;
        previousRelative.y += groundLift(instance.material.w, previousRelative + cameraOrigin.xyz, before[3].y);
        previousRelative += windSwayOfVertex(before, instance.sway, handedness.y, instance.hanging, gl_VertexIndex - gl_BaseVertexARB, previousShaped,
                                             previousRelative + cameraOrigin.xyz, cameraOrigin.xyz, true);
        previousClip = motionPrevious * vec4(previousRelative, 1.0);
        movement = 1.0;
    }
#endif

    // The GPU interpolates UVs across the triangle.
    textureUv = uv;

    // The model is a rotation times a scale: the vertex stretched by the scale is where it sits along the object's axes.
    vec3 frameScale = projectionScale(mat3(instance.model));
    frameAxes = projectionAxes(mat3(instance.model), frameScale);
    framePosition = shaped * frameScale;

    // The normal matrix keeps normals perpendicular to the surface under non-uniform scaling.
    vec3 transformedNormal = mat3(normalMatrix) * normal;
    float normalLength = length(transformedNormal);

    worldNormal = normalLength > 0.0 ? transformedNormal / normalLength : vec3(0.0);

    // Tangents lie in the surface like edges, so the model matrix itself carries them. On a mirrored object the bitangent
    // rebuilt from normal and tangent would come out reversed, so the sign takes the matrix's handedness. Mirrored and
    // tested as transformTangent in render/gpu/normal_mapping.cpp.
    vec3 transformedTangent = mat3(model) * tangent.xyz;
    float tangentLength = length(transformedTangent);

    worldTangent = vec4(tangentLength > 0.0 ? transformedTangent / tangentLength : vec3(0.0), tangent.w * handedness.x);
}
