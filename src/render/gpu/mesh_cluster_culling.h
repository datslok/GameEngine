#pragma once

#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "render/gpu/mesh_pool.h"
#include "render/gpu/screen_reflections.h"
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

    // The cut (stage 2c), in the mesh's space: the group it was made from (its error, 0 for the mesh's own triangles, and
    // sphere) and the group it was simplified with (its sphere, and the error of what was made from it: above
    // MESH_CLUSTER_NEVER_COARSER when nothing was).
    float ownError = 0.0f;
    float ownSphere[4] = {};
    float parentSphere[4] = {};
    float parentError = 0.0f;
    std::uint32_t unused[3] = {};
};

static_assert(sizeof(GpuMeshCluster) == 112, "GpuMeshCluster must match mesh_cluster_culling.glsl's MeshCluster");

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

// A sphere in the mesh's space (xyz centre, w radius) placed in the world: its centre moved, its radius by the longest
// a unit can stretch.
std::array<float, 4> meshClusterPlacedSphere(const float sphere[4], const GpuInstance& instance);

/*
* How many pixels a group's error (mesh units) covers in the view: stretched as far as the model stretches anything,
* over the distance from the eye to the group's placed sphere (at least MESH_CLUSTER_NEAREST_ERROR_DISTANCE), times the
* view's pixels a metre away (view.levels.eye's w); distance-free in an orthographic view. One function for a cluster's
* own error and its parent's, so the two sides of a choice round alike.
*/
float meshClusterErrorPixels(const float sphere[4], float error, const GpuInstance& instance, const GpuCameraCull& view);

/*
* Whether the cut draws the cluster (Nanite's, ROADMAP phase 21, 2c): its own error fits the view's pixel error
* (view.levels.pixelError) and its parent's does not (or nothing coarser was made). A group's error and sphere bound
* those of every group under it, so exactly one cluster on every path from the mesh's own triangles to the coarsest
* is drawn: watertight, since group borders stay locked while simplified.
*/
bool meshClusterInCut(const GpuMeshCluster& cluster, const GpuInstance& instance, const GpuCameraCull& view);

/*
* Whether the draw's vertices are where its mesh has them, moved only by its matrix: not a terrain patch, an impostor's
* card, ground-following, a liana, cards thinned in this view (the entry's bit), or swaying where the view's sway reaches.
*/
bool meshClusterRigid(const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view);

// What the view makes of the cluster (MESH_CLUSTER_HIDDEN, _SEEN, _SPHERE): hidden out of the cut (any draw); seen whole
// for a draw not rigid; else hidden out of the frustum or, single-sided and not mirrored, facing away; else seen with
// its sphere holding.
std::uint32_t meshClusterVisibility(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                                    const GpuCameraCull& view);

// Whether the view draws the cluster, occlusion aside: its visibility not hidden.
bool meshClusterKept(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                     const GpuCameraCull& view);

// A style's survivors as mesh shader workgroups: a row of up to MESH_CLUSTER_TASK_WIDTH, then as many rows as it takes
// (x y z, VkDrawMeshTasksIndirectCommandEXT); none for none.
std::array<std::uint32_t, 3> meshClusterTaskGroups(std::uint32_t count);

// A workgroup's survivor within its style's run; past the run's count in the last row, none.
std::uint32_t meshClusterTaskIndex(std::uint32_t x, std::uint32_t y);

// The mesh shaders' buffers by GPU address, in GpuFrameData::meshClusterBuffers's order.
struct MeshClusterBuffers {
    std::uint64_t items = 0;
    std::uint64_t totals = 0;
    std::uint64_t clusters = 0;
    std::uint64_t clusterVertices = 0;
    std::uint64_t triangleWords = 0;
    std::uint64_t vertices = 0;
};

// Each address as its low and high words, as frame_data.glsl's meshClusterBuffers reads them.
void packMeshClusterBuffers(const MeshClusterBuffers& buffers, std::uint32_t words[12]);

// The most survivors a list holds: a style's run in one draw stays within every GPU's 2^22 mesh shader workgroups.
inline constexpr std::uint32_t largestMeshClusterRoom = 1u << 22;

// ---- Occlusion (two passes: docs/research/OCCLUSION.md), mirrored by mesh_cluster_culling.glsl ----

/*
* What the passes test occlusion with (std430, mesh_cluster_pass.glsl's Occlusion): last frame's camera-relative
* view-projection taking points measured from this frame's camera (the early list's, against last frame's pyramid), this
* frame's as the depth was drawn (jittered; the late list's), this frame's camera, and the pyramid (its width, height,
* levels and MESH_CLUSTER_OCCLUSION_* flags).
*/
struct GpuMeshClusterOcclusion {
    float previous[16] = {};
    float current[16] = {};
    float origin[4] = {};
    std::uint32_t pyramid[4] = {};
};

static_assert(sizeof(GpuMeshClusterOcclusion) == 160, "GpuMeshClusterOcclusion must match mesh_cluster_pass.glsl's Occlusion");

// The view's occlusion on the CPU: its matrices and camera, whether it is tested and last frame's pyramid is there to
// test against, last frame's pyramid (both lists read it: the late list's candidates are what it hid) and this frame's
// (from the early list's depth; the late list's), and the list being made.
struct MeshClusterOcclusionView {
    Mat4 previous;
    Mat4 current;
    Vec3 origin;
    bool on = false;
    bool history = false;
    const DepthPyramid* lastPyramid = nullptr;
    const DepthPyramid* thisPyramid = nullptr;
    std::uint32_t list = MESH_CLUSTER_EARLY;
};

GpuMeshClusterOcclusion packMeshClusterOcclusion(const MeshClusterOcclusionView& view, RenderSize pyramid, std::uint32_t levels);

/*
* A sphere (xyz centre measured from the camera, w radius) on screen through a camera-relative view-projection: the
* pixels its box's eight corners land in, from the top-left (x y least, z w most), and the nearest depth of any corner
* (reversed-Z: the largest). Not on screen when a corner is within MESH_CLUSTER_SMALLEST_W of the camera's plane or
* behind it (it cannot be bounded so; never hidden).
*/
struct MeshClusterScreenBox {
    std::array<float, 4> box{};
    float nearestDepth = 0.0f;
    bool onScreen = false;
};

MeshClusterScreenBox meshClusterScreenBox(const std::array<float, 4>& sphere, const Mat4& viewProjection, Vec2 size);

// The pixels the box covers, clamped to the screen (x y first, z w last); empty (first past last) when it is off screen.
std::array<int, 4> meshClusterBoxPixels(const std::array<float, 4>& box, int width, int height);

// The pyramid level where the pixels span at most two cells each way: their extent's power of two, within the levels.
std::uint32_t meshClusterPyramidLevel(const std::array<int, 4>& pixels, std::uint32_t levels);

// Whether the pyramid (farthest depths) hides the sphere: every cell its box covers at its level farther than its
// nearest point. Read by the passes in mesh_cluster_pass.glsl (meshClusterHidden).
bool meshClusterHidden(const DepthPyramid& pyramid, const std::array<float, 4>& sphere, const Mat4& viewProjection);

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
    std::uint32_t candidates = 0; // the late list's: the pairs the early list's occlusion test hid
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
* place plus those before it in the block, kept within room; the runs from their styles' first blocks (from the list's
* own first). With occlusion, the list it says: the early list keeps what last frame's pyramid does not hide (all, with
* no history); the late list what it hid that this frame's pyramid does not.
*/
MeshClusterList cullMeshClustersOnCpu(const MeshClusterTables& tables, const GpuCameraCull& view, std::uint32_t room,
                                      const MeshClusterOcclusionView* occlusion = nullptr);

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

// cluster-compare's reply: the comparison, the pairs tested and kept, and whether they agree (differences only near edges;
// with occlusion, the GPU's lists may lack pairs, hidden, but never add any not near an edge).
std::string describeMeshClusterComparison(const MeshClusterComparison& comparison, std::uint32_t tested, std::uint32_t kept, bool occlusion = false);

// Whether the cluster's choice is within rounding of changing (its own or parent's error within 1e-4 of the pixel error,
// its sphere within a millimetre of a plane, its cone's test within 1e-4), where the GPU and the CPU may each be right.
bool nearAMeshClusterEdge(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                          const GpuCameraCull& view);
