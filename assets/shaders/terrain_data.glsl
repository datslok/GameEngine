// The terrain's patches as the scene's vertex shaders read them (render/gpu/terrain_gpu.h), and the patch vertex built
// from the heights (terrain_heights.glsl): terrainNormal and terrainVertex mirror scene/terrain_heights.cpp. Included by
// every vertex shader reading instances.
#ifndef TERRAIN_DATA_GLSL
#define TERRAIN_DATA_GLSL

#include "terrain_heights.glsl"
#include "render/gpu/instance_shared.h"

// Must match GpuTerrainPatch.
struct TerrainPatch {
    vec4 placement; // corner x, corner z, size, quads
    vec4 morph;     // start, end, spacing
    vec4 uv;        // corner x, corner z, per metre
};

layout(std430, set = 1, binding = 36) readonly buffer TerrainPatches {
    TerrainPatch terrainPatches[];
};

// How far a vertex of an instance of a kind is lifted at a place in the world: the drawn land's height there for a mesh
// following the ground (INSTANCE_KIND_FOLLOWS_GROUND, scene/ground_follow.h, liftedOntoGround), nothing for any other.
// The drawn height, not the smooth one, which lies centimetres off the triangles and buried fallen leaves.
float groundLift(uint kind, vec3 world) {
    return kind == uint(INSTANCE_KIND_FOLLOWS_GROUND) ? terrainSurfaceHeight(world.xz) : 0.0;
}

// The land's unit normal from its slope over step metres either way.
vec3 terrainNormal(vec2 place, float step) {
    float slopeX = (terrainHeight(place + vec2(step, 0.0)) - terrainHeight(place - vec2(step, 0.0))) / (2.0 * step);
    float slopeZ = (terrainHeight(place + vec2(0.0, step)) - terrainHeight(place - vec2(0.0, step))) / (2.0 * step);
    return normalize(vec3(-slopeX, 1.0, -slopeZ));
}

// A patch's vertex: where it is, its normal, its tangent (w the bitangent's side) and its texture coordinates.
struct TerrainVertex {
    vec3 position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
};

// The vertex at a grid mesh's point (x and z, 0 to 1 across), seen from eye (terrainVertex).
TerrainVertex terrainVertex(TerrainPatch landPatch, vec2 meshPoint, vec3 eye) {
    vec2 corner = landPatch.placement.xy;
    float spacing = landPatch.morph.z;
    vec2 grid = round(meshPoint * landPatch.placement.w);
    vec2 start = corner + grid * spacing;
    float morph = terrainMorph(length(vec3(start.x, terrainHeight(start), start.y) - eye), landPatch.morph.x, landPatch.morph.y);
    vec2 place = corner + morphGridCoordinate(grid, morph) * spacing;
    float step = spacing * (1.0 + morph);

    TerrainVertex vertex;
    vertex.position = vec3(place.x, terrainHeight(place), place.y);
    vertex.normal = terrainNormal(place, step);
    vertex.tangent = vec4(normalize(vec3(2.0 * step, terrainHeight(place + vec2(step, 0.0)) - terrainHeight(place - vec2(step, 0.0)), 0.0)), -1.0);
    vertex.uv = (place - landPatch.uv.xy) * landPatch.uv.z;
    return vertex;
}

#endif
