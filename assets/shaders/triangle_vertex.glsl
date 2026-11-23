// The scene's vertex shader (triangle.vert; the main pass's, with its motion vectors, triangle_motion.vert): one body,
// so the depth prepass and the main pass place every vertex alike. Included after the extensions it needs. The mesh
// shaders drawing mesh clusters (triangle.mesh, triangle_motion.mesh: MESH_CLUSTER_SHADER) place their vertices with the
// same body (shadeVertex), each output an array they write at the vertex's slot in the cluster.
#ifdef MESH_CLUSTER_SHADER
#define VERTEX_OUT(name) name[vertexSlot]
#define VERTEX_OUTPUTS []
#define VERTEX_POSITION gl_MeshVerticesEXT[vertexSlot].gl_Position
#define VERTEX_CLIP gl_MeshVerticesEXT[vertexSlot].gl_ClipDistance[0]
#else
#define VERTEX_OUT(name) name
#define VERTEX_OUTPUTS
#define VERTEX_POSITION gl_Position
#define VERTEX_CLIP gl_ClipDistance[0]
#endif

#include "frame_data.glsl"
#include "ground_layers.glsl"
#include "drawn_objects.glsl"
#include "foliage_shading.glsl"
#include "planar_reflection.glsl"
#include "render/gpu/reflection_probe_shared.h"
#include "terrain_data.glsl"
#include "impostor.glsl"
#include "wind_data.glsl"
#include "bone_vertex_data.glsl"
#include "liana_data.glsl"
#include "card_thinning_data.glsl"
#include "level_fade.glsl"

// Whether this pipeline draws a capture (a reflection probe's cube face, or a flat mirror's picture): its
// view-projection comes as push constants (render/gpu/reflection_probe.h, cubeFaceViewProjection;
// render/gpu/planar_reflection.h, planarViewProjection), the frame's being the camera's.
#ifdef MESH_CLUSTER_SHADER
// The mesh shaders draw the camera's passes only (ROADMAP phase 21, stage 1): never a capture.
const bool captureView = false;
const mat4 captureViewProjection = mat4(1.0);
const vec4 captureClipPlane = vec4(0.0);
const vec4 captureEye = vec4(0.0);
#else
layout(constant_id = PROBE_CAPTURE_VERTEX_CONSTANT) const bool captureView = false;

// Must match GpuCaptureView.
layout(push_constant) uniform CaptureView {
    mat4 captureViewProjection;
    vec4 captureClipPlane; // what is behind it is cut away: a mirror's plane (planarClipPlane), or one nothing is behind
    vec4 captureEye;       // xyz: the eye it is seen from (the camera, or the camera reflected across the mirror); w: its error allowed a metre away, for card thinning
};

// Every pipeline clamps depth rather than clip at the near plane, so a mirror's picture cuts what is behind the mirror
// here: a negative distance clips (render/gpu/planar_reflection.h). The mesh shaders declare theirs with gl_Position.
out float gl_ClipDistance[1];

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
// xyz: the surface's direction of increasing u; w: +1 or -1, which side of it "up the picture" is (MeshVertex).
layout(location = 3) in vec4 tangent;

#ifdef MAIN_MOTION
// The main pass's motion vectors (render/gpu/taa.h): last frame's shape (a deforming mesh's previous vertices, vertex
// binding 1; the same vertices for every other mesh).
layout(location = 4) in vec3 previousPosition;
#endif
#endif

#ifdef MAIN_MOTION
// Each instance's model matrix last frame (its current one when it did not move).
layout(std430, set = 1, binding = 21) readonly buffer PreviousModels {
    mat4 previousModels[];
};
#endif

layout(location = 0) out vec3 worldNormal VERTEX_OUTPUTS;
layout(location = 1) out vec2 textureUv VERTEX_OUTPUTS;
layout(location = 2) out vec3 relativePosition VERTEX_OUTPUTS;
layout(location = 3) out vec4 worldTangent VERTEX_OUTPUTS;
// Which entry of the material buffer shades this object: the same for every fragment of a triangle (flat, not interpolated).
layout(location = 4) flat out uint materialIndex VERTEX_OUTPUTS;
// How much the object scales lengths (uniformScale), for lengths its material gives in model units (thick glass).
layout(location = 5) flat out float objectScale VERTEX_OUTPUTS;
// Where a capture is seen from, measured from the camera (the origin outside captures), for the fragments' view direction.
layout(location = 6) flat out vec3 viewerPosition VERTEX_OUTPUTS;
// For projected layers (render/gpu/ground_layers.h): the vertex in the object's own axes, in metres, and those axes in the
// world (the model's rotation), to turn normals into the object's frame and back.
layout(location = 7) out vec3 framePosition VERTEX_OUTPUTS;
layout(location = 8) flat out mat3 frameAxes VERTEX_OUTPUTS;
// The level fade of its draw list entry (level_fade.glsl, 0 none), for the depth prepass to keep its share of the pixels.
layout(location = 11) flat out uint levelFade VERTEX_OUTPUTS;

#ifdef MAIN_MOTION
// Where the vertex was last frame through last frame's camera (motionPrevious; the fragment knows where it is now from
// its pixel and the jitter, pixelMotion), and 1 where it moved, swayed or changed shape. A triangle with no moving
// vertex keeps no motion (the camera's, from depth); one with some takes every vertex's own last place.
layout(location = 12) out vec4 previousClip VERTEX_OUTPUTS;
layout(location = 13) out float movement VERTEX_OUTPUTS;

// The vertex standing still: last frame it was where it is, seen by last frame's camera.
void stillMotion(uint vertexSlot, vec3 relative) {
    VERTEX_OUT(previousClip) = motionPrevious * vec4(relative, 1.0);
    VERTEX_OUT(movement) = 0.0;
}
#endif

#ifndef MESH_CLUSTER_SHADER
// The depth prepass runs this same shader, and the main pass then shades only fragments whose depth EQUALS the one the
// prepass stored. Invariant makes the compiler compute the position the same way in both pipelines, bit for bit (it
// may not, for instance, fuse a multiply and an add in one and not the other). The mesh shaders declare theirs invariant.
invariant gl_Position;
#endif

/*
* One vertex placed and its outputs written (at vertexSlot in a mesh shader's arrays): its mesh's vertex (position,
* normal, uv, tangent, and last frame's position), the draw list entry it is drawn as (instanceIndex, and the draw's
* first: baseInstance, placed plants' from PLACED_INSTANCE_BASE), its number in its own mesh (meshVertex: the per-vertex
* records of hanging things and thinned cards count from the mesh's first vertex) and in the vertex pool (poolVertex:
* its binding to its tree's bones).
*/
void shadeVertex(uint vertexSlot, vec3 position, vec3 normal, vec2 uv, vec4 tangent, vec3 previousPosition, uint instanceIndex, uint baseInstance,
                 int meshVertex, uint poolVertex) {
    // A frame's draw, or a placed plant's part (drawn_objects.glsl).
    // The vertex measured from the camera, kept here and written once at the end (a mesh shader may not read its outputs).
    vec3 relative = vec3(0.0);
    DrawnObject drawn = drawnObject(instanceIndex, baseInstance);
    uint entry = drawn.entry;
    InstanceData instance = drawn.instance;

    // A twig (render/gpu/twig_records.h): its prototype's vertex placed in its part's frame by the twig's matrix first,
    // so from here it is the part's own vertex (swaying with its tree); the normal turned with it, then bent by the twig's
    // lean as the cards' were (the leaf normal blend), and the leaf's number in its texture coordinate swapped for its
    // tone seed (foliage_shading.glsl; the fragment shader tones it when its material asks).
    if (drawn.twig) {
        mat3 twigTurn = mat3(drawn.twigMatrix);
        position = (drawn.twigMatrix * vec4(position, 1.0)).xyz;
        previousPosition = (drawn.twigMatrix * vec4(previousPosition, 1.0)).xyz;
        normal = twigTurn * normal;
        normal = blendedFoliageNormal(normalize(normal), drawn.twigLean, leafOptions.x);
        uv.x = tonedLeafU(uv.x, drawn.twigPlant, drawn.twigNumber);
        tangent = vec4(twigTurn * tangent.xyz, tangent.w);
    }
    VERTEX_OUT(levelFade) = drawListFade(entry);
    mat4 normalMatrix = instance.normalMatrix;
    vec4 handedness = instance.handedness;
    VERTEX_OUT(materialIndex) = instance.material.x;
    VERTEX_OUT(objectScale) = handedness.y;

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
        relative = land.position - cameraOrigin.xyz;
        VERTEX_POSITION = (captureView ? captureViewProjection : relativeViewProjection) * vec4(relative, 1.0);
        VERTEX_CLIP = captureView ? planarClipDistance(captureClipPlane, relative) : 1.0;
        VERTEX_OUT(viewerPosition) = captureView ? captureEye.xyz : vec3(0.0);
        VERTEX_OUT(textureUv) = land.uv;
        VERTEX_OUT(frameAxes) = mat3(1.0);
        VERTEX_OUT(framePosition) = land.position;
        VERTEX_OUT(worldNormal) = land.normal;
        VERTEX_OUT(worldTangent) = land.tangent;
#ifdef MAIN_MOTION
        stillMotion(vertexSlot, relative);
#endif
        VERTEX_OUT(relativePosition) = relative;
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
        relative = middle + treeAxes * (cardPoint * normal.x * treeScale);
        VERTEX_POSITION = (captureView ? captureViewProjection : relativeViewProjection) * vec4(relative, 1.0);
        VERTEX_CLIP = captureView ? planarClipDistance(captureClipPlane, relative) : 1.0;
        VERTEX_OUT(viewerPosition) = captureView ? captureEye.xyz : vec3(0.0);
        VERTEX_OUT(textureUv) = uv * 0.5 + 0.5;
        VERTEX_OUT(frameAxes) = treeAxes;
        VERTEX_OUT(framePosition) = cardPoint;
        VERTEX_OUT(worldNormal) = toViewer;
        // w: the card's metres a unit of card point (the radius times the plant's scale, taken as even: plants are placed
        // scaled alike along every axis), to move shadow lookups to the leaf seen behind the card.
        VERTEX_OUT(worldTangent) = vec4(viewDirection, normal.x * (treeScale.x + treeScale.y + treeScale.z) / 3.0);
#ifdef MAIN_MOTION
        stillMotion(vertexSlot, relative);
#endif
        VERTEX_OUT(relativePosition) = relative;
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
        relative = hold + bent.offset + lianaSway;
        VERTEX_POSITION = (captureView ? captureViewProjection : relativeViewProjection) * vec4(relative, 1.0);
        VERTEX_CLIP = captureView ? planarClipDistance(captureClipPlane, relative) : 1.0;
        VERTEX_OUT(viewerPosition) = captureView ? captureEye.xyz : vec3(0.0);
        VERTEX_OUT(textureUv) = bent.uv;
        VERTEX_OUT(frameAxes) = mat3(1.0);
        VERTEX_OUT(framePosition) = world;
        VERTEX_OUT(worldNormal) = bent.normal;
        VERTEX_OUT(worldTangent) = vec4(bent.tangent, 1.0);
#ifdef MAIN_MOTION
        stillMotion(vertexSlot, relative);

        if (motionOptions.x != 0.0 && lianaSway != vec3(0.0)) {
            vec3 previousRelative = hold + bent.offset + lianaSwayOffset(shape, instance.sway, swing, bent.along, bent.arm, swaysAt, cameraOrigin.xyz, true);
            VERTEX_OUT(previousClip) = motionPrevious * vec4(previousRelative, 1.0);
            VERTEX_OUT(movement) = 1.0;
        }
#endif
        VERTEX_OUT(relativePosition) = relative;
        return;
    }

    // Thinned cards (scene/card_thinning.h): each shrunk or grown about its stem as the view's distance allows, in every view
    // that marks it the same way, so the prepass and the main pass agree to the bit.
    vec3 shaped = thinnedVertex(instance, entry, meshVertex, position, captureView ? captureEye.xyz : vec3(0.0), cameraOrigin.xyz,
                                captureView ? captureEye.w : levelOptions.x);
    relative = (model * vec4(shaped, 1.0)).xyz;
    relative.y += groundLift(instance.material.w, relative + cameraOrigin.xyz, instance.model[3].y);

    /*
    * The wind (render/gpu/wind.h): the same sway in every view, so the prepass and the main pass agree to the bit. A
    * vertex bound to its tree's bones (bone_vertex_data.glsl) moves by them instead, its normal and tangent turned with
    * them; one whose tree is not posed this frame, and every vertex in a capture, stands still. A twig's (its prototype's
    * vertex) is not bound: it keeps the sway until stage 3c-iii-c-ii.
    */
    BoneDrawn bones = BoneDrawn(BONE_NO_POSE, SWAY_UNBOUND, vec4(0.0));

    if (!drawn.twig) {
        bones = boneDrawn(drawn.plant, poolVertex);
    }

    if (captureView) {
        bones.firstPose = BONE_NO_POSE;
    }

    vec3 swayOffset;

    if (bonePosed(bones)) {
        BoneVertexPoses poses = boneVertexPoses(bones, false);
        swayOffset = boneSwayOfVertex(bones, poses, instance.model, instance.sway, instance.hanging, meshVertex, shaped, relative + cameraOrigin.xyz,
                                      cameraOrigin.xyz, false);
        normal = windBlendedDirection(poses.own, poses.parent, poses.weight, normal);
        tangent.xyz = windBlendedDirection(poses.own, poses.parent, poses.weight, tangent.xyz);
    } else if (bones.binding != SWAY_UNBOUND) {
        swayOffset = vec3(0.0);
    } else {
        swayOffset = windSwayOfVertex(instance.model, instance.sway, handedness.y, instance.hanging, meshVertex, shaped, relative + cameraOrigin.xyz, cameraOrigin.xyz, false);
    }

    relative += swayOffset;
    VERTEX_POSITION = (captureView ? captureViewProjection : relativeViewProjection) * vec4(relative, 1.0);
    VERTEX_CLIP = captureView ? planarClipDistance(captureClipPlane, relative) : 1.0;
    VERTEX_OUT(viewerPosition) = captureView ? captureEye.xyz : vec3(0.0);

#ifdef MAIN_MOTION
    /*
    * Last frame's place, only for a vertex whose instance moved (its matrix), that the wind moves now (last frame's
    * springs and time; the same fade, which changes too slowly to show: no offset now, past the fade or at a trunk's foot,
    * is none last frame either) or whose shape changed (its previous vertex): last frame's matrix applied to last frame's
    * shape, thinned, lifted and swayed as now. Everything else stands still. A placed plant's part (drawn_objects.glsl)
    * never moves: its matrix last frame is its matrix now, only its sway differs.
    */
    stillMotion(vertexSlot, relative);
    bool placed = baseInstance >= PLACED_INSTANCE_BASE;
    mat4 before = placed ? instance.model : previousModels[drawnInstance(entry)];
    bool moves = before != instance.model || swayOffset != vec3(0.0) || previousPosition != position;

    if (motionOptions.x != 0.0 && moves) {
        vec3 previousShaped = thinnedVertex(instance, entry, meshVertex, previousPosition, vec3(0.0), cameraOrigin.xyz, levelOptions.x);
        mat4 beforeRelative = before;
        beforeRelative[3].xyz -= cameraOrigin.xyz;
        vec3 previousRelative = (beforeRelative * vec4(previousShaped, 1.0)).xyz;
        previousRelative.y += groundLift(instance.material.w, previousRelative + cameraOrigin.xyz, before[3].y);
        previousRelative += bonePosed(bones)
                                ? boneSwayOfVertex(bones, boneVertexPoses(bones, true), before, instance.sway, instance.hanging, meshVertex, previousShaped,
                                                   previousRelative + cameraOrigin.xyz, cameraOrigin.xyz, true)
                                : bones.binding != SWAY_UNBOUND
                                      ? vec3(0.0)
                                      : windSwayOfVertex(before, instance.sway, handedness.y, instance.hanging, meshVertex, previousShaped,
                                                         previousRelative + cameraOrigin.xyz, cameraOrigin.xyz, true);
        VERTEX_OUT(previousClip) = motionPrevious * vec4(previousRelative, 1.0);
        VERTEX_OUT(movement) = 1.0;
    }
#endif

    // The GPU interpolates UVs across the triangle.
    VERTEX_OUT(textureUv) = uv;

    // The model is a rotation times a scale: the vertex stretched by the scale is where it sits along the object's axes.
    vec3 frameScale = projectionScale(mat3(instance.model));
    VERTEX_OUT(frameAxes) = projectionAxes(mat3(instance.model), frameScale);
    VERTEX_OUT(framePosition) = shaped * frameScale;

    // The normal matrix keeps normals perpendicular to the surface under non-uniform scaling.
    vec3 transformedNormal = mat3(normalMatrix) * normal;
    float normalLength = length(transformedNormal);

    VERTEX_OUT(worldNormal) = normalLength > 0.0 ? transformedNormal / normalLength : vec3(0.0);

    // Tangents lie in the surface like edges, so the model matrix itself carries them. On a mirrored object the bitangent
    // rebuilt from normal and tangent would come out reversed, so the sign takes the matrix's handedness. Mirrored and
    // tested as transformTangent in render/gpu/normal_mapping.cpp.
    vec3 transformedTangent = mat3(model) * tangent.xyz;
    float tangentLength = length(transformedTangent);

    VERTEX_OUT(worldTangent) = vec4(tangentLength > 0.0 ? transformedTangent / tangentLength : vec3(0.0), tangent.w * handedness.x);
    VERTEX_OUT(relativePosition) = relative;
}

#ifndef MESH_CLUSTER_SHADER
// The vertex shaders: the vertex from the vertex buffers, the instance and mesh numbers from the draw. Every mesh but a
// deforming one has last frame's shape at vertex binding 1 as its own vertices.
void main() {
#ifdef MAIN_MOTION
    vec3 previous = previousPosition;
#else
    vec3 previous = position;
#endif
    shadeVertex(0u, position, normal, uv, tangent, previous, uint(gl_InstanceIndex), uint(gl_BaseInstanceARB), gl_VertexIndex - gl_BaseVertexARB,
                uint(gl_VertexIndex));
}
#endif
