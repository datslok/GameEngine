#pragma once

#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/bone_poses.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "render/gpu/mesh_pool.h"
#include "render/gpu/placed_drawing.h"
#include "render/gpu/screen_reflections.h"
#include "render/gpu/twig_records.h"
#include "scene/mesh_clusters.h"
#include "scene/sway_bones.h"

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
    std::uint32_t level = 0;              // the level of the group it was simplified with (MeshClusterGroup::level)
    // Its vertices' own bones (meshClusterBones): up to MESH_CLUSTER_MOST_BONES, 16 bits each, low half first.
    std::uint32_t bones[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    std::uint32_t unused[2] = {};
};

static_assert(sizeof(GpuMeshCluster) == 128, "GpuMeshCluster must match mesh_cluster_culling.glsl's MeshCluster");

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

inline constexpr std::uint32_t meshClusterMostBones = MESH_CLUSTER_MOST_BONES;
inline constexpr std::uint32_t meshClusterNoBone = MESH_CLUSTER_NO_BONE;
inline constexpr std::uint32_t meshClusterManyBones = MESH_CLUSTER_MANY_BONES;
static_assert(swayMostBones <= meshClusterManyBones, "A bone must never read as a cluster's empty or many slot");

/*
* bindings: the mesh's (IndexedMesh::swayBindings, one a vertex, or none), each cluster's bones taken from them
* (meshClusterBones). Throws std::invalid_argument for bindings not one a vertex the clusters name.
*/
PackedMeshClusters packMeshClusters(const MeshClusters& built, std::uint32_t meshFirstVertex, std::uint32_t firstClusterVertex,
                                    std::uint32_t firstTriangleWord, std::span<const SwayBinding> bindings = {});

/*
* A cluster's bones (ROADMAP phase 21 stage 3c-iv-a, docs/research/LEAFGEOMETRY.md, Bounds from the poses): its
* vertices' own bones (each blends with its parent, which the bone's pose names), ascending, up to
* MESH_CLUSTER_MOST_BONES, the rest MESH_CLUSTER_NO_BONE; MESH_CLUSTER_MANY_BONES first when there are more (its tree's
* move bounds it); all MESH_CLUSTER_NO_BONE without bindings. Packed two to a word, the first low. Throws
* std::invalid_argument as packMeshClusters.
*/
std::array<std::uint32_t, 4> meshClusterBones(const MeshClusters& built, const MeshCluster& cluster, std::span<const SwayBinding> bindings);

// A packed cluster's bone in a slot (0 to MESH_CLUSTER_MOST_BONES - 1).
std::uint32_t meshClusterBone(const GpuMeshCluster& cluster, std::uint32_t slot);

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
* Whether the draw's shape is its mesh's, moved only by its matrix and (for vertices bound to bones) its bones: not a
* terrain patch, an impostor's card, ground-following, a liana, or cards thinned in this view (the entry's bit).
*/
bool meshClusterShapeFixed(const GpuInstance& instance, std::uint32_t entry);

// Whether the draw sways by the wind's per-vertex sway: it sways (CULL_SWAYS) where the view's sway reaches.
bool meshClusterSways(const GpuCullRecord& record, const GpuCameraCull& view);

/*
* How a cluster stands this frame (ROADMAP phase 21 stage 3c-iv-b; docs/research/LEAFGEOMETRY.md, Bounds from the
* poses): bound to bones (its mesh's vertices, or its twig), so never moved by the per-vertex sway (a bound vertex
* whose tree is not posed stands still); posed when its tree is, with its sphere in the world (sphere) holding every
* vertex as drawn, its leaves' flutter included. Mirrored by mesh_cluster_culling.glsl's MeshClusterPose.
*/
struct MeshClusterPose {
    bool bound = false;
    bool posed = false;
    std::array<float, 4> sphere{};
};

/*
* A posed thing's pose: its sphere in its mesh's frame as its bones carry it, placed by placedBy, grown by the farthest
* its leaves flutter (windFlutterReach of flutter, its instance's sway's, and its tree's lean).
*/
MeshClusterPose meshClusterPosed(const float sphere[4], const GpuInstance& placedBy, float flutter, const Vec2& lean);

// Whether the view keeps every cluster of the draw: its shape not its mesh's, or swaying per vertex and not bound to bones.
bool meshClusterSeenWhole(const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view,
                          const MeshClusterPose& pose);

// Whether a cluster's vertices are bound to bones: its first bone slot not empty (a bone or MESH_CLUSTER_MANY_BONES).
bool meshClusterBound(const GpuMeshCluster& cluster);

/*
* A bound cluster's sphere in its mesh's frame as its tree's bones carry it this frame (tree posed, pool its poses):
* each of its bones' and their parents' images of its sphere merged (windBlendedSphere); with too many bones, its
* sphere grown by the tree's move.
*/
Vec4 meshClusterBonesSphere(const GpuMeshCluster& cluster, std::span<const GpuBonePose> pool, const GpuBoneTree& tree);

// The sphere the view tests a cluster by: its pose's when posed, else its own in the world.
std::array<float, 4> meshClusterCullSphere(const GpuMeshCluster& cluster, const GpuInstance& instance, const MeshClusterPose& pose);

/*
* What the view makes of the cluster (MESH_CLUSTER_HIDDEN, _SEEN, _SPHERE): hidden out of the cut (any draw); seen whole
* (meshClusterSeenWhole); else hidden out of the frustum (meshClusterCullSphere) or, single-sided, not mirrored, not
* posed (a posed cluster's triangles turn and bend: its cone does not hold) and not in a shadow view (CAMERA_CULL_SHADOW:
* the light sees the sides the eye does not), facing away; else seen with its sphere holding.
*/
std::uint32_t meshClusterVisibility(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                                    const GpuCameraCull& view, const MeshClusterPose& pose = {});

// Whether the view draws the cluster, occlusion aside: its visibility not hidden.
bool meshClusterKept(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                     const GpuCameraCull& view, const MeshClusterPose& pose = {});

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

// A box holding nothing yet (least past most), for meshClusterBoxTake to grow.
MeshClusterScreenBox meshClusterEmptyBox();

// The box grown to hold a clip-space point's pixel and depth; false (the box untouched) when the point is within
// MESH_CLUSTER_SMALLEST_W of the camera's plane or behind it, so nothing it is part of can be bounded on screen.
bool meshClusterBoxTake(MeshClusterScreenBox& box, const Vec4& clip, Vec2 size);

/*
* A far leaf's screen box (ROADMAP phase 21 stage 3e-v-b), from its vertices as the mesh shader placed them: a leaf is
* their hull, so their box bounds it and their nearest depth its nearest. That depth is taken nearer by
* MESH_CLUSTER_LEAF_DEPTH_MARGIN, since the main pass tests a leaf against a pyramid holding its own depth, rasterised
* from the same vertices but rounded apart. Not on screen when a vertex cannot be projected.
*/
MeshClusterScreenBox farLeafScreenBox(std::span<const Vec4> clips, Vec2 size);

// The pixels the box covers, clamped to the screen (x y first, z w last); empty (first past last) when it is off screen.
std::array<int, 4> meshClusterBoxPixels(const std::array<float, 4>& box, int width, int height);

// The pyramid level where the pixels span at most two cells each way: their extent's power of two, within the levels.
std::uint32_t meshClusterPyramidLevel(const std::array<int, 4>& pixels, std::uint32_t levels);

// Whether the pyramid (farthest depths) hides the sphere: every cell its box covers at its level farther than its
// nearest point. Read by the passes in mesh_cluster_pass.glsl (meshClusterHidden).
bool meshClusterHidden(const DepthPyramid& pyramid, const std::array<float, 4>& sphere, const Mat4& viewProjection);

// Whether the pyramid hides a screen box (of the pyramid's size): every cell it covers at its level farther than its
// nearest depth; never one off screen. Read by mesh_cluster_pyramid.glsl (meshClusterBoxHiddenIn).
bool meshClusterBoxHidden(const DepthPyramid& pyramid, const MeshClusterScreenBox& box);

// ---- Sizes ----

// The room for pairs at first (8 MB); grown to what a frame needed when it overflows.
inline constexpr std::uint32_t smallestMeshClusterRoom = 1u << 20;

/*
* At least the pairs the camera can test this frame, before the GPU says: each group's candidates (an upper bound on
* its drawn instances) times its clusters, and each style's padding to a block. Cost grows with the groups and segments.
*/
std::uint64_t meshClusterPairBound(const CameraCullFrame& table);

/*
* At least the placed plants' pairs a view can test (phase 21, 2f): each of its items' block's plants (placedGroupSize)
* times the most a plant of the block's layer can test, its kind's parts' every level slot's clusters (a plant draws at
* most two slots of a part, while fading). Cost grows with the view's items and the table's kinds.
*/
std::uint64_t placedMeshClusterPairBound(const PlacedTable& table, std::span<const GpuPlacedBlock> blocks, std::span<const GpuPlacedItem> items,
                                         const GpuPlacedView& view);

// ---- The passes' steps on the CPU ----

/*
* The camera's placed plants as the passes read them (phase 21, 2f; render/gpu/placed_drawing.h): its commands by placed
* group (first instances from placedInstanceBase), the placed groups (their clusters), every view's entries, each part's
* instance and record in its plant's frame, and the slots' plants. Empty without placed plants.
*/
struct MeshClusterPlacedTables {
    std::span<const GpuDrawCommand> commands;
    std::span<const GpuPlacedGroup> groups;
    std::span<const GpuPlacedEntry> entries;
    std::span<const GpuInstance> partInstances;
    std::span<const GpuPlacedPart> parts;
    std::span<const GpuPlacedPlant> plants;
};

/*
* The camera's twigs as the passes read them (phase 21 stage 3b-iii; render/gpu/twig_records.h): each region's entries
* written by the twig pass (region r's entries draw r + 1 clusters each; a count may pass the room, the rest lost), the
* room a region has (region r's entries from r times it), the entries and the twigs. The last region (twigFarRegion)
* holds far leaf clusters, one an entry, its first cluster the far cluster's number with twigFarClusterBit (stage
* 3e-iii-b); the far clusters and whether each placed part is bound to bones (its skeleton, GpuPlacedPart::unused[1])
* go with them. Empty without twigs.
*/
struct MeshClusterTwigTables {
    std::array<std::uint32_t, TWIG_REGIONS> counts{};
    std::uint32_t room = 0;
    std::span<const GpuTwigEntry> entries;
    std::span<const GpuTwig> twigs;
    std::span<const GpuTwigFarCluster> farClusters;
};

/*
* What the passes read: the camera's commands by group (its culling's, unpacked), the drawing order (meshClusterDrawOrder:
* a number past the groups is a placed group's, less their count; from twigGroupBase a twig region's), the groups (their
* clusters), the frame's draw list, instances, records and clusters, the placed plants, the twigs and the bones.
*/
struct MeshClusterTables {
    std::span<const GpuDrawCommand> commands;
    std::span<const std::uint32_t> drawOrder;
    std::span<const GpuCullGroup> groups;
    std::span<const std::uint32_t> drawList;
    std::span<const GpuInstance> instances;
    std::span<const GpuCullRecord> records;
    std::span<const GpuMeshCluster> clusters;
    MeshClusterPlacedTables placed{};
    MeshClusterTwigTables twigs{};
    BoneCullTables bones{};
};

/*
* The order the passes walk: each style's run of the draws' pooled groups, then its run of the placed groups, numbered
* after the draws' drawGroups, then, in twigStyle's run (none below 0), the twig regions (twigGroupBase + 0 to
* TWIG_REGIONS - 1, the far region last); every place pooled. Cost grows with the groups.
*/
CullDrawOrder meshClusterDrawOrder(const CullDrawOrder& draws, std::uint32_t drawGroups, const CullDrawOrder& placed, int twigStyle = -1);

// The most pairs the twig regions can hold: each region's room times its clusters (1 + 2 + ... + TWIG_MOST_LEVEL_CLUSTERS,
// and 1 for the far region).
std::uint64_t twigMeshClusterPairBound(std::uint32_t room);

/*
* What a pair's place draws: its instance, its entry's bits (thinned, fade, with its draw's or plant's number) and its
* record; a placed plant's number (BONE_NO_TREE for a frame's draw), and a twig's part (placed by its plant) and number
* (in twigs.twigs; meshClusterNoTwig otherwise); a far leaf cluster's number (meshClusterNoFar otherwise: it is drawn as
* its part, its leaves in the plant's frame) and whether its part is bound to bones.
*/
inline constexpr std::uint32_t meshClusterNoTwig = MESH_CLUSTER_NO_TWIG;
inline constexpr std::uint32_t meshClusterNoFar = 0xffffffffu;

struct MeshClusterDrawn {
    GpuInstance instance{};
    std::uint32_t entry = 0;
    GpuCullRecord record{};
    std::uint32_t plant = boneNoTree;
    GpuInstance part{};
    std::uint32_t twig = meshClusterNoTwig;
    std::uint32_t farCluster = meshClusterNoFar;
    bool partBound = false;
};

/*
* A far leaf cluster as the cluster passes test it (mirrored by twig_drawing.glsl): its sphere (own and group alike),
* its leaves' vertices and triangles, always in the cut (own error 0, nothing coarser: its node chose its level), never
* culled by facing, bound to no bone (twigSpherePose bounds it by its tree's move).
*/
GpuMeshCluster twigFarMeshCluster(const GpuTwigFarCluster& far);

// A pair's cluster: the pool's, or a far cluster's stand-in when its number carries twigFarClusterBit.
GpuMeshCluster meshClusterOfPair(const MeshClusterTables& tables, std::uint32_t cluster);

/*
* The instance at a place the passes and mesh shaders read (a pair's entry): a frame's draw through the draw list, or,
* from placedInstanceBase, a placed entry's part placed by its plant (placedInstance, placedCullRecord), as the vertex
* shaders' drawnObject does. Mirrored by mesh_cluster_pass.glsl's meshClusterDrawnAt.
*/
MeshClusterDrawn meshClusterDrawnAt(const MeshClusterTables& tables, std::uint32_t place);

/*
* How a pair's cluster stands (MeshClusterPose), as mesh_cluster_pass.glsl's meshClusterPoseAt finds it: a twig's by its
* binding and tree (twigPose on its cluster's own sphere); a placed plant's bound cluster by its tree (boneCullTree,
* meshClusterBonesSphere, meshClusterPosed); a frame's draw's bound cluster stands still.
*/
MeshClusterPose meshClusterPoseAt(const MeshClusterTables& tables, const MeshClusterDrawn& drawn, const GpuMeshCluster& cluster);

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
// its sphere (posed, when posed) within a millimetre of a plane, its cone's test within 1e-4), where the GPU and the CPU
// may each be right.
bool nearAMeshClusterEdge(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                          const GpuCameraCull& view, const MeshClusterPose& pose = {});

// The same for a pair of the tables: its cluster and what its place draws.
bool nearAMeshClusterEdge(const MeshClusterTables& tables, const GpuMeshClusterItem& item, const GpuCameraCull& view);
