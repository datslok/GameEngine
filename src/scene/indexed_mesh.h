#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "scene/mesh.h"

#include <cstdint>
#include <type_traits>
#include <vector>

/*
* One corner as the renderer sees it: everything needed to shade it, already resolved.
* This is also the exact GPU vertex layout (the pipeline reads it with offsetof), so it must stay tightly packed floats.
*
* The tangent is the surface's direction of increasing u, and with the normal it carries a normal map's picture over the
* surface. Its w is +1 or -1: the bitangent, "up the picture", is w * cross(normal, tangent), so a mirrored picture
* needs no third vector of its own.
*/
struct MeshVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec4 tangent;
};

static_assert(std::is_standard_layout_v<MeshVertex>, "MeshVertex is uploaded as raw bytes");
static_assert(sizeof(MeshVertex) == 12 * sizeof(float), "MeshVertex must have no padding");

/*
* The render format: each unique corner stored once, and triangles as three indices into it.
* A corner shared by several triangles is uploaded and transformed once instead of once per triangle.
*/
struct IndexedMesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

/*
* Every triangle corner with everything resolved, three per triangle in order, not yet shared: the normal (from the file,
* or the triangle's face normal), the UV (or 0,0) and the tangent (from the file when every corner has one, else
* generated with MikkTSpace). Throws for empty meshes, non-finite positions, w != 1, or triangles that reference missing
* vertices.
*/
std::vector<MeshVertex> resolveCorners(const Mesh& mesh);

/*
* Convert the import format into the render format: resolveCorners, then corners that are identical in every component
* share one vertex.
*/
IndexedMesh buildIndexedMesh(const Mesh& mesh);
