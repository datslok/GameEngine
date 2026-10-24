#pragma once

#include "ecs/world.h"
#include "math/aabb.h"
#include "math/vec3.h"
#include "scene/asset_handles.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh.h"

#include <cstdint>
#include <span>
#include <vector>

/*
* Meshes whose vertices move every tick (a cloth), while their triangles and UVs stay the same. A game adds the mesh with
* AssetManager::addDeformingMesh, draws it with a ModelRenderer part like any other, and gives the entity a
* DeformingMesh holding where each vertex is now. Each frame the engine blends the last two ticks' shapes, works out
* the normals and tangents again from the new positions, and hands the vertices to the renderer
* (FrameDescription::deformedMeshes).
*/

/*
* Component: a deforming mesh's shape, one position per vertex of its Mesh (Mesh::vertices, in the mesh's own space;
* the ModelRenderer part's matrix still places it). previousPositions is the last tick's, saved by the engine before
* each tick (savePreviousShapes), so a shape is drawn between ticks as a Transform is.
*/
struct DeformingMesh {
    MeshHandle mesh;
    std::vector<Vec3> positions;
    std::vector<Vec3> previousPositions;
};

// System: remember every deforming mesh's shape before a tick changes it.
void savePreviousShapes(World& world);

/*
* What stays fixed about a deforming mesh: its render vertices (each corner of a triangle that shares a vertex, UV and
* tangent side with another is stored once, as buildIndexedMesh does) and which of the Mesh's vertices each one is.
* Corners of one vertex with different UVs (a seam) are separate render vertices but share one normal, so the surface
* stays smooth across seams. rest is the rest shape's vertices, with its normals and tangents worked out as
* deformVertices would.
*/
struct DeformingTopology {
    IndexedMesh rest;
    std::vector<std::uint32_t> sourceVertex;
    std::size_t sourceVertexCount = 0;
};

// Throws as resolveCorners does for a mesh that cannot be drawn.
DeformingTopology buildDeformingTopology(const Mesh& mesh);

/*
* The render vertices for a shape (one position per Mesh vertex): positions placed, each vertex's normal the
* area-weighted average of its triangles' (a smooth surface, whatever normals the Mesh had), each tangent the direction
* of increasing u averaged over its triangles and made square to the normal, keeping its side (w). Normals and tangents
* that cannot be found (a squashed shape) fall back to fixed unit vectors. Throws std::invalid_argument for the wrong
* number of positions.
*/
void deformVertices(const DeformingTopology& topology, std::span<const Vec3> positions, std::vector<MeshVertex>& vertices);

// The shape to draw at alpha between the last two ticks (0 the last tick's, 1 the latest); the latest when there is no
// last tick's of the same size.
std::vector<Vec3> blendedPositions(const DeformingMesh& mesh, float alpha);

// A frame's deformed mesh, as the renderer needs it: the vertices to upload, their bounds (for culling) and a
// fingerprint of the shape (never 0), the same for the same positions, so a shape that holds still keeps its cached
// shadows.
struct DeformedMesh {
    MeshHandle mesh;
    std::vector<MeshVertex> vertices;
    Aabb bounds;
    std::uint64_t shape = 0;
};

DeformedMesh deformMesh(const DeformingTopology& topology, const DeformingMesh& mesh, float alpha);
