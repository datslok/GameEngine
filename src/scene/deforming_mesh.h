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

/*
* The steps of shaping, shared by deformVertices and the GPU's shape pass (deform.glsl, which mirrors them):
* - triangleAreaNormal: the triangle's normal times twice its area, so a vertex's sum weights its triangles by area.
* - triangleTangent: the triangle's direction of increasing u (Lengyel's), scaled by the sign of its UV area so a
*   mirrored UV layout still points along increasing u; none when its UVs have no area.
* - deformedNormal: a vertex's summed normal made unit, or straight up for none.
* - deformedTangent: its summed tangent made square to the normal and unit, or any direction square to it for none.
*/
Vec3 triangleAreaNormal(const Vec3& first, const Vec3& second, const Vec3& third);

struct TriangleTangent {
    Vec3 alongU;
    bool hasUvArea = false;
};

TriangleTangent triangleTangent(const Vec3& first, const Vec3& second, const Vec3& third, const Vec2& firstUv, const Vec2& secondUv,
                                const Vec2& thirdUv);
Vec3 deformedNormal(const Vec3& summed);
Vec3 deformedTangent(const Vec3& summed, const Vec3& normal);

/*
* Which triangles each vertex adds up, for shaping one render vertex at a time (the GPU's shape pass): for each Mesh
* vertex the triangles that have it as a corner (its normal), for each render vertex the triangles it is a corner of
* (its tangent), each in triangle order and once per corner, as deformVertices adds them. Compressed rows: each list
* is xTriangles[xStarts[i] .. xStarts[i + 1]].
*/
struct DeformingAdjacency {
    std::vector<std::uint32_t> sourceStarts;
    std::vector<std::uint32_t> sourceTriangles;
    std::vector<std::uint32_t> vertexStarts;
    std::vector<std::uint32_t> vertexTriangles;
};

DeformingAdjacency buildDeformingAdjacency(const DeformingTopology& topology);

// One render vertex for a shape, gathered from its own triangles: deformVertices' result for it, to the bit.
MeshVertex deformVertexAt(const DeformingTopology& topology, const DeformingAdjacency& adjacency, std::span<const Vec3> positions,
                          std::uint32_t vertex);

// The shape to draw at alpha between the last two ticks (0 the last tick's, 1 the latest); the latest when there is no
// last tick's of the same size.
std::vector<Vec3> blendedPositions(const DeformingMesh& mesh, float alpha);

/*
* A frame's deformed mesh, as the renderer needs it: the vertices to upload, their bounds (for culling) and a
* fingerprint of the shape (never 0), the same for the same positions, so a shape that holds still keeps its cached
* shadows. A cloth simulated on the GPU is shaped there (shapedOnGpu): no vertices, its bounds from its points read back
* a few frames late, grown by how far they can have moved since.
*/
struct DeformedMesh {
    MeshHandle mesh;
    std::vector<MeshVertex> vertices;
    Aabb bounds;
    std::uint64_t shape = 0;
    bool shapedOnGpu = false;
};

DeformedMesh deformMesh(const DeformingTopology& topology, const DeformingMesh& mesh, float alpha);
