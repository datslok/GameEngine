#pragma once

#include "math/aabb.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "scene/card_thinning.h"
#include "scene/mesh.h"
#include "scene/wind.h"

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

    // Something hanging (scene/wind.h, HangingVertex): each vertex's anchors, share and arm, one a vertex, or none at all
    // for everything else. Whatever appends vertices to a hanging mesh appends their entries too (appendHanging).
    std::vector<HangingVertex> hanging{};

    // Cards thinned one by one (scene/card_thinning.h): how, and each vertex's card (its anchor and rank), one a vertex,
    // or none at all for everything else.
    CardThinning thinning{};
    std::vector<ThinningCard> thinningCards{};
};

/*
* A mesh's first vertexCount vertices and indexCount indices as a mesh of their own (a level of detail sharing its
* mesh's first vertices, MeshLevel::sharesVertices); its records stay with the mesh, whose thinning shapes it. Throws
* std::invalid_argument for more than the mesh has, or a triangle reaching past the kept vertices.
*/
IndexedMesh meshPrefix(const IndexedMesh& mesh, std::size_t vertexCount, std::size_t indexCount);

// A mesh made kept cards first (scene/card_thinning.h) cut to its kept cards: the vertices before the first card ranked to
// go and the triangles before the first reaching past them (meshPrefix); the whole mesh when nothing is thinned.
IndexedMesh thinnedLevel(const IndexedMesh& mesh);

/*
* The hanging entries for the vertices from firstVertex on, each entry: a mesh that had none yet gets the default (its
* own sway, no arm) for its earlier vertices first, so the list stays one a vertex. Nothing for a mesh with no entries
* when none are asked for (hanging null).
*/
void appendHanging(IndexedMesh& mesh, std::size_t firstVertex, const HangingVertex* hanging);

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

// The smallest box holding every vertex (a zero box for none), as meshBounds gives a Mesh's.
Aabb indexedMeshBounds(const IndexedMesh& mesh);
