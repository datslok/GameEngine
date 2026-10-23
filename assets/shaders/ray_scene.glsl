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

// The renderer's passes have the bindless textures and the frame's scene as sets 0 and 1, so a hit can look up its
// material: a cut-out's alpha, a double-sided surface's sides.
#define RAY_SCENE_MATERIALS
#include "materials.glsl"
#include "instances.glsl"
#include "alpha_mask.glsl"
#include "render/gpu/material_shared.h"
#endif

layout(set = RAY_SCENE_SET, binding = 0) uniform accelerationStructureEXT rayScene;

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

#ifdef RAY_SCENE_MATERIALS
// The glass and blended surfaces' light (render/gpu/transparency.h).
#include "transparency.glsl"

// Glass is seen through, never solid: blended (its alpha a coverage) or transmitting (render/gpu/transparency.h).
bool isTransparentMaterial(MaterialData material) {
    return (material.extra.y & MATERIAL_BLEND) != 0u || material.transmission.x > 0.0;
}

// Whether a candidate hit lands on a cut-out's solid texel rather than a hole (render/gpu/alpha_mask.h). Only cut-outs
// and glass give candidates (their instances are not opaque); glass is never solid, so rays that see it (those asking
// for everything) pass through it. The full-size texel is read, as rays have no screen footprint.
bool candidateIsSolid(rayQueryEXT query) {
    InstanceData instance = instances[rayQueryGetIntersectionInstanceCustomIndexEXT(query, false)];
    MaterialData material = materials[instance.material.x];

    if (isTransparentMaterial(material)) {
        return false;
    }

    vec2 uv = meshSurfaceAt(instance.material.y, rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                            rayQueryGetIntersectionBarycentricsEXT(query, false)).uv;
    float alpha = textureLod(textures[nonuniformEXT(material.textureSlots.x)], uv, 0.0).a * material.baseColour.a;
    return !isCutAway(alpha, material.emissive.w);
}

// The glass a ray may meet (GpuGlassBounds): x the spheres held, y 1 when every ray must be traced.
layout(std430, set = 1, binding = 26) readonly buffer GlassBounds {
    uvec4 glassCounts;
    vec4 glassSpheres[TRANSPARENCY_GLASS_BOUNDS_LIMIT];
};

// Whether a segment may meet glass by the bounds (glassMayLieBetween in transparency.cpp).
bool glassMayLieBetween(vec3 from, vec3 direction, float segmentLength) {
    if (glassCounts.y != 0u) {
        return true;
    }

    for (uint i = 0u; i < glassCounts.x; ++i) {
        if (segmentMeetsSphere(from, direction, segmentLength, glassSpheres[i].xyz, glassSpheres[i].w)) {
            return true;
        }
    }

    return false;
}

/*
* The light one glass candidate lets through towards a light along direction: nothing changed by a face turned away from
* the light (glassFaceCounts), else its coverage, transmission and colour at the hit (full-size texels) and the angle to
* its smoothed normal.
*/
vec3 candidateGlassTransmittance(rayQueryEXT query, vec3 direction) {
    InstanceData instance = instances[rayQueryGetIntersectionInstanceCustomIndexEXT(query, false)];
    MaterialData material = materials[instance.material.x];

    if (!glassFaceCounts(rayQueryGetIntersectionFrontFaceEXT(query, false), (material.extra.y & MATERIAL_DOUBLE_SIDED) != 0u)) {
        return vec3(1.0);
    }

    MeshSurface surface = meshSurfaceAt(instance.material.y, rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                                        rayQueryGetIntersectionBarycentricsEXT(query, false));
    vec4 albedo = textureLod(textures[nonuniformEXT(material.textureSlots.x)], surface.uv, 0.0) * material.baseColour;
    float metallic = clamp(material.surfaceParameters.x * textureLod(textures[nonuniformEXT(material.textureSlots.y)], surface.uv, 0.0).b, 0.0, 1.0);
    float transmission = material.transmission.x * textureLod(textures[nonuniformEXT(material.extra.z)], surface.uv, 0.0).r;
    float coverage = (material.extra.y & MATERIAL_BLEND) != 0u ? albedo.a : 1.0;
    vec3 normal = mat3(instance.normalMatrix) * surface.normal;
    float normalLength = length(normal);
    float cosine = normalLength > 0.0 ? dot(normal, direction) / normalLength : 1.0;
    return glassLightTransmittance(coverage, transmission, metallic, albedo.rgb, cosine);
}

/*
* The share of light (per channel) that passes the glass between a point and a light along direction within reach: the
* product over every pane the ray crosses, in any order (none is confirmed, so the query walks them all), stopping once
* nothing passes. Rays the glass's bounds rule out are not traced. 1 without glass.
*/
vec3 glassTransmittance(vec3 from, vec3 direction, float reach) {
    if (!glassMayLieBetween(from, direction, reach)) {
        return vec3(1.0);
    }

    vec3 passing = vec3(1.0);
    rayQueryEXT query;
    rayQueryInitializeEXT(query, rayScene, gl_RayFlagsNoneEXT, RAY_MASK_GLASS, from, 0.0, direction, reach);

    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            passing *= candidateGlassTransmittance(query, direction);

            if (all(lessThanEqual(passing, vec3(0.0)))) {
                rayQueryTerminateEXT(query);
            }
        }
    }

    return passing;
}
#endif

// Runs a query to its nearest solid hit (or, with gl_RayFlagsTerminateOnFirstHitEXT, any solid hit): a hit in a
// cut-out's hole, or on glass, is passed over. Every ray query in the renderer but glassTransmittance's goes through here.
void traceRayQuery(rayQueryEXT query) {
    while (rayQueryProceedEXT(query)) {
#ifdef RAY_SCENE_MATERIALS
        if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionTriangleEXT && candidateIsSolid(query)) {
            rayQueryConfirmIntersectionEXT(query);
        }
#endif
    }
}

// Whether the committed hit's surface faces the ray: its front, or either side of a double-sided one (a leaf or a fence
// is a surface from both sides, not the inside of something).
bool committedHitFacesRay(rayQueryEXT query) {
    if (rayQueryGetIntersectionFrontFaceEXT(query, true)) {
        return true;
    }

#ifdef RAY_SCENE_MATERIALS
    InstanceData instance = instances[rayQueryGetIntersectionInstanceCustomIndexEXT(query, true)];
    return (materials[instance.material.x].extra.y & MATERIAL_DOUBLE_SIDED) != 0u;
#else
    return false;
#endif
}

// Whether nothing solid lies between a point and a light along toLight within reach: the first hit anywhere settles it.
bool reachesLight(vec3 from, vec3 toLight, float reach) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, rayScene, gl_RayFlagsTerminateOnFirstHitEXT, RAY_MASK_SOLID, from, 0.0, toLight, reach);
    traceRayQuery(query);
    return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

#ifdef RAY_SCENE_MATERIALS
// The share of a light (per channel) reaching a point: none past anything solid, else what the glass on the way passes.
vec3 lightThrough(vec3 from, vec3 toLight, float reach) {
    return reachesLight(from, toLight, reach) ? glassTransmittance(from, toLight, reach) : vec3(0.0);
}
#endif

#endif
