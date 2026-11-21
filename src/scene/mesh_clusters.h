#pragma once

#include "math/vec3.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh_clusters_shared.h"

#include <cstdint>
#include <vector>

/*
* A small piece of a mesh, culled on its own (ROADMAP phase 21, stage 1). Its sphere holds its vertices (for the frustum
* and depth tests); its cone holds its triangles' facings, so an eye can be found to see none of their fronts:
* clusterFacesAway. Its vertices and triangles are runs in MeshClusters' lists.
*/
struct MeshCluster {
    Vec3 centre{};
    float radius = 0.0f;

    // The cone of facing (meshoptimizer's): every triangle faces away from an eye when the direction from the eye to the
    // apex lies within coneCutoff (a cosine) of the axis. A cutoff of 1 or more never culls (facings spread too wide).
    Vec3 coneApex{};
    float coneCutoff = 1.0f;
    Vec3 coneAxis{};

    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t firstTriangle = 0;
    std::uint32_t triangleCount = 0;
};

/*
* A mesh cut into clusters: each cluster's vertices as the mesh's vertex numbers (so the mesh's own vertex buffer serves),
* and its triangles as three local corner numbers (a byte each, into its own vertex run).
*/
struct MeshClusters {
    std::vector<MeshCluster> clusters;
    std::vector<std::uint32_t> vertices;
    std::vector<std::uint8_t> triangles;
};

/*
* Cut a mesh's triangles into clusters (meshoptimizer's builder, which grows each cluster from triangles near it and of
* like facing, so cones stay narrow and spheres small), each triangle in exactly one with its winding, then reorder each
* cluster's corners for the GPU's vertex reuse. Deterministic: the same mesh gives the same clusters. Cost grows with the
* mesh's triangles, paid once a mesh.
*/
MeshClusters buildMeshClusters(const IndexedMesh& mesh);

/*
* Whether an eye (in the mesh's own space) sees only the backs of every triangle in a cluster, so a single-sided cluster
* can be skipped whole. Exact for any transform turning the mesh without mirroring it (scaled unevenly included): the
* side of a triangle an eye is on is the sign of a volume, which such maps keep, so test the eye brought into the mesh's
* space. Mirrored by the GPU's culling (stage 1b).
*/
bool clusterFacesAway(const MeshCluster& cluster, const Vec3& eye);
