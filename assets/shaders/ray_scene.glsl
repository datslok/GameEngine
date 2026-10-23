#ifndef RAY_SCENE_GLSL
#define RAY_SCENE_GLSL

/*
* The ray tracing set (render/vulkan/vulkan_ray_tracing.h): the frame's top-level acceleration structure, and every
* mesh's vertex and index buffers by GPU address, so a hit can rebuild the surface it struck. Declarations and the
* vertex reads only; the maths is in ray_tracing.glsl. The set is 2 for the renderer's passes (after the bindless
* textures and the frame's scene); a shader that binds it elsewhere defines RAY_SCENE_SET first.
*/
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

#include "ray_tracing.glsl"

#ifndef RAY_SCENE_SET
#define RAY_SCENE_SET 2
#endif

layout(set = RAY_SCENE_SET, binding = 0) uniform accelerationStructureEXT rayScene;

// Whether nothing solid lies between a point and a light along toLight within reach: the first hit anywhere settles it.
bool reachesLight(vec3 from, vec3 toLight, float reach) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, rayScene, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, RAY_MASK_SOLID, from, 0.0, toLight, reach);

    while (rayQueryProceedEXT(query)) {
    }

    return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

// A mesh's buffers by address: MeshVertex as floats (MESH_VERTEX_FLOATS each, no padding, so read as a float array
// rather than a struct, whose vec3s std430 would pad) and three 32-bit indices per triangle. Addresses are kept as two
// 32-bit halves (GL_EXT_buffer_reference_uvec2), so shaders need no 64-bit integers.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer MeshFloats {
    float values[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer MeshIndices {
    uint values[];
};

struct MeshAddresses {
    uvec2 vertices;
    uvec2 indices;
};

layout(std430, set = RAY_SCENE_SET, binding = 1) readonly buffer MeshTable {
    MeshAddresses meshes[];
};

// The surface where a ray met a triangle, in the mesh's own coordinates: its normal (interpolated, not normalised) and
// texture coordinates.
struct MeshSurface {
    vec3 normal;
    vec2 uv;
};

vec3 meshVec3(MeshFloats floats, uint vertex, uint offset) {
    uint first = vertex * MESH_VERTEX_FLOATS + offset;
    return vec3(floats.values[first], floats.values[first + 1], floats.values[first + 2]);
}

vec2 meshVec2(MeshFloats floats, uint vertex, uint offset) {
    uint first = vertex * MESH_VERTEX_FLOATS + offset;
    return vec2(floats.values[first], floats.values[first + 1]);
}

// The triangle's corners blended by the hit's barycentric coordinates.
MeshSurface meshSurfaceAt(uint mesh, uint triangle, vec2 barycentrics) {
    MeshFloats floats = MeshFloats(meshes[mesh].vertices);
    MeshIndices indices = MeshIndices(meshes[mesh].indices);
    uvec3 corners = uvec3(indices.values[triangle * 3], indices.values[triangle * 3 + 1], indices.values[triangle * 3 + 2]);
    vec3 weights = hitCornerWeights(barycentrics.x, barycentrics.y);

    MeshSurface surface;
    surface.normal = meshVec3(floats, corners.x, MESH_VERTEX_NORMAL) * weights.x + meshVec3(floats, corners.y, MESH_VERTEX_NORMAL) * weights.y +
                     meshVec3(floats, corners.z, MESH_VERTEX_NORMAL) * weights.z;
    surface.uv = meshVec2(floats, corners.x, MESH_VERTEX_UV) * weights.x + meshVec2(floats, corners.y, MESH_VERTEX_UV) * weights.y +
                 meshVec2(floats, corners.z, MESH_VERTEX_UV) * weights.z;
    return surface;
}

// A triangle's corners in the mesh's own coordinates: their positions and normals, for how a smoothed surface curves.
struct MeshTriangle {
    vec3 positions[3];
    vec3 normals[3];
};

MeshTriangle meshTriangleAt(uint mesh, uint triangle) {
    MeshFloats floats = MeshFloats(meshes[mesh].vertices);
    MeshIndices indices = MeshIndices(meshes[mesh].indices);
    MeshTriangle corners;

    for (uint corner = 0u; corner < 3u; ++corner) {
        uint vertex = indices.values[triangle * 3u + corner];
        corners.positions[corner] = meshVec3(floats, vertex, MESH_VERTEX_POSITION);
        corners.normals[corner] = meshVec3(floats, vertex, MESH_VERTEX_NORMAL);
    }

    return corners;
}

#endif
