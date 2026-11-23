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
    vec4 tileLightPlace; // the coloured shadow map's (glass_shadow.vert)
    vec4 tileOptions;    // x 1 when the tile sways (render/gpu/wind.h, tileSways)
};

out float gl_ClipDistance[1];

void main() {
    // A frame's draw, or a placed plant's part (drawn_objects.glsl).
    DrawnObject drawn = drawnObject(uint(gl_InstanceIndex), uint(gl_BaseInstanceARB));
    InstanceData instance = drawn.instance;

    // A liana's tube or ivy: its template placed on its curve (liana_data.glsl), swayed in the tiles that sway, as the
    // camera draws it (the ivy's cards grown for the camera's distance, so its shadow matches what is seen).
    if (instance.hanging.w > 0u) {
        LianaDrawnVertex bent = lianaDrawnVertex(instance.hanging.w, position, uv, cameraOrigin.xyz);
        vec3 lianaWorld = bent.shape.from.xyz + bent.offset;

        if (tileOptions.x != 0.0) {
            lianaWorld += lianaSwayOffset(bent.shape, instance.sway, uintBitsToFloat(instance.hanging.y), bent.along, bent.arm, bent.shape.from.xyz + bent.swaysAt,
                                          cameraOrigin.xyz, false);
        }

        gl_Position = tileMatrix * vec4(lianaWorld, 1.0);
        gl_ClipDistance[0] = planarClipDistance(tileClipPlane, lianaWorld);
        textureUv = bent.uv;
        materialIndex = instance.material.x;
        return;
    }

    vec4 world = instance.model * vec4(position, 1.0);
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
    textureUv = uv;
    materialIndex = instance.material.x;
}
