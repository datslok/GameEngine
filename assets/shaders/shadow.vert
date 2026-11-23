#version 450
#extension GL_GOOGLE_include_directive : require
// gl_BaseVertexARB: the drawn mesh's first vertex in the shared mesh buffer (render/gpu/mesh_pool.h); the per-vertex
// records (hanging things, thinned cards) count from the mesh's own first vertex.
#extension GL_ARB_shader_draw_parameters : require

#include "frame_data.glsl"
#include "drawn_objects.glsl"
#include "planar_reflection.glsl"
#include "terrain_data.glsl"
#include "wind_data.glsl"
#include "bone_vertex_data.glsl"
#include "liana_data.glsl"

// The shadow pass only needs where each vertex is, as seen from one face of the light's cube.
layout(location = 0) in vec3 position;

// The tile's view-projection, in world space like the instances (the shadow cache recognises tiles by world matrices),
// and the plane (world space) what is behind is clipped at: an image light's mirror, or one nothing is behind.
// Must match GpuShadowView in render/gpu/gpu_scene.h.
layout(push_constant) uniform ShadowView {
    mat4 tileMatrix;
    vec4 tileClipPlane;
    vec4 tileLightPlace; // the coloured shadow map's (glass_shadow.vert)
    vec4 tileOptions;    // x 1 when the tile sways (render/gpu/wind.h, tileSways)
};

out float gl_ClipDistance[1];

void main() {
    // A frame's draw, or a placed plant's part (drawn_objects.glsl).
    DrawnObject drawn = drawnObject(uint(gl_InstanceIndex), uint(gl_BaseInstanceARB));
    InstanceData instance = drawn.instance;

    // A liana's tube: its template bent onto its curve (liana_data.glsl), swayed in the tiles that sway, as the camera
    // draws it (its ivy is a cut-out: shadow_cut_out.vert).
    if (instance.hanging.w > 0u) {
        LianaDrawnVertex bent = lianaDrawnVertex(instance.hanging.w, position, vec2(0.0), cameraOrigin.xyz);
        vec3 lianaWorld = bent.shape.from.xyz + bent.offset;

        if (tileOptions.x != 0.0) {
            lianaWorld += lianaSwayOffset(bent.shape, instance.sway, uintBitsToFloat(instance.hanging.y), bent.along, bent.arm, bent.shape.from.xyz + bent.swaysAt,
                                          cameraOrigin.xyz, false);
        }

        gl_Position = tileMatrix * vec4(lianaWorld, 1.0);
        gl_ClipDistance[0] = planarClipDistance(tileClipPlane, lianaWorld);
        return;
    }

    // A terrain patch is lifted onto the land as the camera's views lift it (morphed from the camera), so the shadow
    // falls from the surface that is seen.
    uint terrainPatchNumber = instance.material.z;
    vec4 world = terrainPatchNumber > 0u
                     ? vec4(terrainVertex(terrainPatches[terrainPatchNumber - 1u], position.xz, cameraOrigin.xyz).position, 1.0)
                     : instance.model * vec4(position, 1.0);
    world.y += groundLift(instance.material.w, world.xyz, instance.model[3].y);

    // In the sun's nearest cascades, the wind's sway as the camera's views draw it, a vertex bound to its tree's bones
    // moved by them (still when its tree is not posed); the rest keep still shadows.
    if (tileOptions.x != 0.0) {
        int meshVertex = gl_VertexIndex - gl_BaseVertexARB;
        BoneDrawn bones = boneDrawn(drawn.plant, uint(gl_VertexIndex));

        if (bonePosed(bones)) {
            world.xyz += boneSwayOfVertex(bones, boneVertexPoses(bones, false), instance.model, instance.sway, instance.hanging, meshVertex, position, world.xyz,
                                          cameraOrigin.xyz, false);
        } else if (bones.binding == SWAY_UNBOUND) {
            world.xyz += windSwayOfVertex(instance.model, instance.sway, instance.handedness.y, instance.hanging, meshVertex, position, world.xyz, cameraOrigin.xyz,
                                          false);
        }
    }

    gl_Position = tileMatrix * world;
    gl_ClipDistance[0] = planarClipDistance(tileClipPlane, world.xyz);
}
