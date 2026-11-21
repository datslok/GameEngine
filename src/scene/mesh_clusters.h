#pragma once

#include "core/job_system.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh_clusters_shared.h"

#include <cstdint>
#include <vector>

// A cluster made from no group: it holds the mesh's own triangles.
inline constexpr std::uint32_t meshClusterNoGroup = 0xffffffffu;

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

    // Its place in the hierarchy (MeshClusterGroup): the group it was simplified with, and the group it was made from by
    // simplifying (meshClusterNoGroup for the mesh's own triangles).
    std::uint32_t group = 0;
    std::uint32_t refinedGroup = meshClusterNoGroup;
};

/*
* Clusters merged and simplified together (ROADMAP phase 21, stage 2). The coarser clusters made from them carry its
* error, the farthest (in the mesh's space) their surface strays from the mesh's own, and its sphere, which holds the
* spheres of every group its clusters came from. A view draws a cluster when its group's error, projected from the
* group's sphere, is too big and its refined group's small enough: one choice everywhere, since a group's error and
* sphere bound those of every group under it, and watertight, since borders stay locked while a group is simplified.
*/
struct MeshClusterGroup {
    Vec3 centre{};
    float radius = 0.0f;
    float error = 0.0f; // the largest float when nothing coarser was made: its clusters are drawn however far
    std::uint32_t level = 0;
};

/*
* A mesh cut into clusters: each cluster's vertices as the mesh's vertex numbers (so the mesh's own vertex buffer serves),
* and its triangles as three local corner numbers (a byte each, into its own vertex run). The clusters holding the mesh's
* own triangles come first (originalCount of them), the coarser levels after.
*/
struct MeshClusters {
    std::vector<MeshCluster> clusters;
    std::vector<std::uint32_t> vertices;
    std::vector<std::uint8_t> triangles;
    std::vector<MeshClusterGroup> groups;
    std::uint32_t originalCount = 0;
};

/*
* Whether a mesh gets cluster levels (ROADMAP phase 21, 2b-ii), as Nanite's per-mesh switch: only meshes the cut will
* draw coarser (rocks, trunks, structures; AssetManager::setClusterLevels), so the cost grows with them alone.
*/
enum class ClusterLevels { One, All };

/*
* Cut a mesh's triangles into clusters, each triangle in exactly one with its winding, then build the levels over them
* (meshoptimizer's clusterlod, Nanite's scheme): about 16 neighbouring clusters grouped, simplified to half with the
* group's border locked (normals weighed, UV seams kept), cut back into clusters, repeated until one is left or nothing
* simplifies. Coarser levels' corners are the mesh's own vertices, so its vertex buffer serves every level; about twice
* the mesh's triangles in all. Without levels (ClusterLevels::One): exactly the clusters the full detail has with them,
* all in one group never simplified (drawn however far), at about a fifth of the cost. Deterministic: the same mesh
* gives the same clusters. Cost grows with the mesh's triangles, paid once a mesh.
*/
MeshClusters buildMeshClusters(const IndexedMesh& mesh, ClusterLevels levels);

/*
* Many meshes' clusters built at once on every thread, a mesh a job and the largest first (so a big mesh does not
* finish last: zeux.io 2025, docs/research/VIRTUALGEOMETRY.md), returned in the order asked. Each mesh is built alone,
* so the result is what building them one by one gives, whichever threads run them. Cost: the meshes' triangles,
* shared over the threads.
*/
struct MeshClusterBuild {
    const IndexedMesh* mesh = nullptr;
    ClusterLevels levels = ClusterLevels::One;
};

std::vector<MeshClusters> buildMeshClusters(const std::vector<MeshClusterBuild>& meshes, JobSystem& jobs);

/*
* Whether an eye (in the mesh's own space) sees only the backs of every triangle in a cluster, so a single-sided cluster
* can be skipped whole. Exact for any transform turning the mesh without mirroring it (scaled unevenly included): the
* side of a triangle an eye is on is the sign of a volume, which such maps keep, so test the eye brought into the mesh's
* space. Mirrored by the GPU's culling (stage 1b).
*/
bool clusterFacesAway(const MeshCluster& cluster, const Vec3& eye);
