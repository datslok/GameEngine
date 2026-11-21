#pragma once

#include "math/vec3.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "render/gpu/mesh_pool.h"
#include "scene/mesh_clusters.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

/*
* Mesh clusters culled a view on the GPU (ROADMAP phase 21, stage 1b; docs/research/VIRTUALGEOMETRY.md): after the
* camera's culling has chosen its draws, each drawn instance's clusters are tested one by one (the frustum, and facing),
* and the survivors listed in drawing order as (draw list entry, cluster) pairs, a run a style. Render-only and API-free:
* the records and maths, mirrored by mesh_cluster_culling.glsl, and the passes' steps on the CPU, for tests and for
* cluster-compare.
*/

inline constexpr std::uint32_t meshClusterGroupSize = MESH_CLUSTER_GROUP_SIZE;
static_assert(MESH_CLUSTER_STYLES == CAMERA_CULL_STYLES, "A run of mesh clusters for each of the camera's styles");

// A cluster in the pools. Must match mesh_cluster_culling.glsl's MeshCluster.
struct GpuMeshCluster {
    float sphere[4] = {};          // xyz centre, w radius, in the mesh's space
    float cone[4] = {0, 0, 0, 1};  // xyz apex, w cutoff (1 or more: never culled by facing)
    float axis[3] = {};
    std::uint32_t firstVertex = 0;       // its vertices' run in the cluster vertex pool
    std::uint32_t firstTriangleWord = 0; // its triangles' run in the triangle pool, four corner bytes a word
    std::uint32_t counts = 0;            // vertices, then triangles from MESH_CLUSTER_TRIANGLE_SHIFT
    std::uint32_t meshFirstVertex = 0;   // its mesh's first vertex in the vertex pool
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuMeshCluster) == 64, "GpuMeshCluster must match mesh_cluster_culling.glsl's MeshCluster");

// A pair the camera draws: the draw list entry's place (the instance index the vertex shaders read) and the cluster's number.
// Must match mesh_cluster_culling.glsl's MeshClusterItem.
struct GpuMeshClusterItem {
    std::uint32_t entry = 0;
    std::uint32_t cluster = 0;

    bool operator==(const GpuMeshClusterItem&) const = default;
};

static_assert(sizeof(GpuMeshClusterItem) == 8, "GpuMeshClusterItem must match mesh_cluster_culling.glsl's MeshClusterItem");

/*
* A mesh's clusters as the pools hold them: its records (numbered from firstCluster), their vertices as the vertex
* pool's numbers (the mesh's first vertex added), and their triangles' corner bytes packed four to a word, each cluster's
* from a word of its own. firstClusterVertex and firstTriangleWord: where the runs go in their pools.
*/
struct PackedMeshClusters {
    std::vector<GpuMeshCluster> clusters;
    std::vector<std::uint32_t> vertices;
    std::vector<std::uint32_t> triangleWords;
};

PackedMeshClusters packMeshClusters(const MeshClusters& built, std::uint32_t meshFirstVertex, std::uint32_t firstClusterVertex,
                                    std::uint32_t firstTriangleWord);

// The words a cluster's triangles take (three corner bytes each, four to a word).
std::uint32_t meshClusterTriangleWords(std::uint32_t triangles);

// ---- Mirrored by mesh_cluster_culling.glsl ----

// The longest a unit vector can come out of the model's turning part: its longest column.
float meshClusterLargestScale(const GpuInstance& instance);

// The cluster's sphere in the world (xyz centre, w radius).
std::array<float, 4> meshClusterWorldSphere(const GpuMeshCluster& cluster, const GpuInstance& instance);

// The model's turning part's determinant (below 0 mirrors, 0 flattens).
float meshClusterDeterminant(const GpuInstance& instance);

// A point in the world in the mesh's space (the determinant checked positive by the caller).
Vec3 meshClusterEyeInMesh(const GpuInstance& instance, const Vec3& eye, float determinant);

// Every triangle's back faces the eye (in the mesh's space): clusterFacesAway on the record's cone.
bool meshClusterFacesAway(const GpuMeshCluster& cluster, const Vec3& eye);

/*
* Whether the draw's vertices are where its mesh has them, moved only by its matrix: not a terrain patch, an impostor's
* card, ground-following, a liana, cards thinned in this view (the entry's bit), or swaying where the view's sway reaches.
*/
bool meshClusterRigid(const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view);

// Whether the view draws the cluster: all of a draw not rigid; else its sphere in the frustum and, single-sided and not
// mirrored, not facing away.
bool meshClusterKept(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                     const GpuCameraCull& view);

// ---- Sizes ----

// The room for pairs at first (8 MB); grown to what a frame needed when it overflows.
inline constexpr std::uint32_t smallestMeshClusterRoom = 1u << 20;

/*
* At least the pairs the camera can test this frame, before the GPU says: each group's candidates (an upper bound on
* its drawn instances) times its clusters, and each style's padding to a block. Cost grows with the groups and segments.
*/
std::uint64_t meshClusterPairBound(const CameraCullFrame& table);

// ---- The passes' steps on the CPU ----

// What the passes read: the camera's commands by group (its culling's, unpacked), the drawing order, the groups
// (their clusters), and the frame's draw list, instances, records and clusters.
struct MeshClusterTables {
    std::span<const GpuDrawCommand> commands;
    std::span<const std::uint32_t> drawOrder;
    std::span<const GpuCullGroup> groups;
    std::span<const std::uint32_t> drawList;
    std::span<const GpuInstance> instances;
    std::span<const GpuCullRecord> records;
    std::span<const GpuMeshCluster> clusters;
};

// A list culled: its pairs in drawing order (at most the room), each style's run of them, the pairs tested and the
// survivors (all of them, past the room too).
struct MeshClusterList {
    std::vector<GpuMeshClusterItem> items;
    std::array<std::uint32_t, MESH_CLUSTER_STYLES> runFirst{};
    std::array<std::uint32_t, MESH_CLUSTER_STYLES> runCount{};
    std::uint32_t tested = 0;
    std::uint32_t kept = 0;
    std::uint32_t triangles = 0; // the written survivors' triangles
};

/*
* The pairs to test: each style's run of the drawing order (view.runFirst, runCount), each group's instances times its
* clusters, instance by instance; each style from a block's first. Returns each place's end (its pairs' end) and fills
* work's MESH_CLUSTER_WORK_* words, as mesh_cluster_count.comp does.
*/
std::vector<std::uint32_t> meshClusterWorkEnds(const MeshClusterTables& tables, const GpuCameraCull& view,
                                               std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS>& work);

// The pair at a place in the work (the first place whose end is past it), or none in a style's padding.
struct MeshClusterPair {
    bool real = false;
    std::uint32_t entry = 0;
    std::uint32_t cluster = 0;
};

MeshClusterPair meshClusterPairAt(const MeshClusterTables& tables, std::span<const std::uint32_t> ends, std::uint32_t pair);

/*
* The list as the passes make it: the work, each block's survivors, their prefix sum, each survivor at its block's first
* place plus those before it in the block, kept within room; the runs from their styles' first blocks.
*/
MeshClusterList cullMeshClustersOnCpu(const MeshClusterTables& tables, const GpuCameraCull& view, std::uint32_t room);

// ---- cluster-compare ----

// The GPU's list against the CPU's, pair by pair (as sets: a pair near an edge moves the places after it).
struct MeshClusterComparison {
    std::size_t entries = 0;    // the CPU's
    std::size_t matching = 0;   // in both
    std::size_t missing = 0;    // the CPU's, not the GPU's
    std::size_t extra = 0;      // the GPU's, not the CPU's
    std::size_t nearAnEdge = 0; // of missing and extra, pairs near an edge (nearAMeshClusterEdge)
};

MeshClusterComparison compareMeshClusterLists(std::span<const GpuMeshClusterItem> gpu, std::span<const GpuMeshClusterItem> cpu,
                                              const std::function<bool(const GpuMeshClusterItem&)>& nearEdge);

// cluster-compare's reply: the comparison, the pairs tested and kept, and whether they agree (differences only near edges).
std::string describeMeshClusterComparison(const MeshClusterComparison& comparison, std::uint32_t tested, std::uint32_t kept);

// Whether the cluster's choice is within rounding of changing (its sphere within a millimetre of a plane, its cone's
// test within 1e-4), where the GPU and the CPU may each be right.
bool nearAMeshClusterEdge(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                          const GpuCameraCull& view);
