#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame_data.glsl"
#include "instances.glsl"
#include "planar_reflection.glsl"
#include "terrain_data.glsl"
#include "wind_data.glsl"

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
    InstanceData instance = instances[drawnInstance(drawList[gl_InstanceIndex])];

    // A terrain patch is lifted onto the land as the camera's views lift it (morphed from the camera), so the shadow
    // falls from the surface that is seen.
    uint terrainPatchNumber = instance.material.z;
    vec4 world = terrainPatchNumber > 0u
                     ? vec4(terrainVertex(terrainPatches[terrainPatchNumber - 1u], position.xz, cameraOrigin.xyz).position, 1.0)
                     : instance.model * vec4(position, 1.0);
    world.y += groundLift(instance.material.w, world.xyz);

    // In the sun's nearest cascades, the wind's sway as the camera's views draw it; the rest keep still shadows.
    if (tileOptions.x != 0.0) {
        world.xyz += windSwayOfVertex(instance.model, instance.sway, instance.handedness.y, instance.hanging, gl_VertexIndex, position, world.xyz, cameraOrigin.xyz, false);
    }

    gl_Position = tileMatrix * world;
    gl_ClipDistance[0] = planarClipDistance(tileClipPlane, world.xyz);
}
