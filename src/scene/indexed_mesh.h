#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/mesh.h"

#include <cstdint>
#include <type_traits>
#include <vector>

/*
* One corner as the renderer sees it: everything needed to shade it, already resolved.
* This is also the exact GPU vertex layout (the pipeline reads it with offsetof), so it must stay tightly packed floats.
*/
struct MeshVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

static_assert(std::is_standard_layout_v<MeshVertex>, "MeshVertex is uploaded as raw bytes");
static_assert(sizeof(MeshVertex) == 8 * sizeof(float), "MeshVertex must have no padding");

/*
* The render format: each unique corner stored once, and triangles as three indices into it.
* A corner shared by several triangles is uploaded and transformed once instead of once per triangle.
*/
struct IndexedMesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

/*
* Convert the import format into the render format.
* Each corner gets its final normal (from the file, or the triangle's face normal) and UV (or 0,0), and corners that are identical in every component share one vertex.
* Throws for empty meshes, non-finite positions, w != 1, or triangles that reference missing vertices.
*/
IndexedMesh buildIndexedMesh(const Mesh& mesh);
