#pragma once

#include "render/gpu/camera_culling.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/twig_drawing_shared.h"
#include "math/vec2.h"
#include "math/vec4.h"
#include "scene/mesh_clusters.h"
#include "scene/twig_far_levels_shared.h"
#include "scene/twig_set.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

struct GpuMeshCluster;   // render/gpu/mesh_cluster_culling.h, which includes this
struct MeshClusterPose;  // the same
struct BoneCullTables;   // render/gpu/bone_poses.h
struct GpuBoneTree;      // the same
struct GpuBonePose;      // the same
struct WindBonePose;     // render/gpu/wind_bones.h

/*
* A twig set's records as the GPU's twig pass reads them (ROADMAP phase 21 stage 3b-iii; scene/twig_set.h). Render-only
* and API-free, so tests can check the packing.
*/

/*
* One twig: the 3 x 4 matrix taking its prototype's frame into the plant's (rows; columns across, towardsTip and facing
* times its scale, then its foot), its lean and its prototype.
*/
struct GpuTwig {
    float rows[3][4] = {};
    float lean[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t prototype = 0;
};

static_assert(sizeof(GpuTwig) == 64, "GpuTwig must match twig_drawing.glsl's Twig");

// A brick: its sphere in the plant's frame (centre, radius) and its twigs' run.
struct GpuTwigBrick {
    float sphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t firstTwig = 0;
    std::uint32_t twigCount = 0;
    std::uint32_t unused[2] = {0, 0};
};

static_assert(sizeof(GpuTwigBrick) == 32, "GpuTwigBrick must match twig_drawing.glsl's TwigBrick");

/*
* A prototype's level, as its given cluster levels made it (buildGivenClusterLevels): its clusters' run in the cluster
* pool, and its group's sphere (prototype frame) and error, the error past which the next level takes over (the
* coarsest's the largest float), and its clusters' triangles (for the pass's tally). A level is chosen where the level
* before's error is small enough and its own is not.
*/
struct GpuTwigLevel {
    float sphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float error = 0.0f;
    std::uint32_t firstCluster = 0;
    std::uint32_t clusterCount = 0;
    std::uint32_t triangleCount = 0;
};

static_assert(sizeof(GpuTwigLevel) == 32, "GpuTwigLevel must match twig_drawing.glsl's TwigLevel");

// A twig's record from its instance.
GpuTwig packTwig(const TwigInstance& twig);

// A twig's binding as the twigs' bindings list holds it, one word a twig beside its record (packSwayBinding), or
// SWAY_UNBOUND for a twig riding on no bone.
std::uint32_t packTwigBinding(const TwigInstance& twig);

// A brick's record.
GpuTwigBrick packTwigBrick(const TwigBrick& brick);

/*
* A prototype's levels from its clusters (a chain, as buildGivenClusterLevels makes: group k level k's clusters, which
* run together), its clusters from firstPoolCluster in the pool. Throws std::invalid_argument for clusters that are not
* such a chain, more than TWIG_MOST_LEVELS levels or a level of more than TWIG_MOST_LEVEL_CLUSTERS clusters.
*/
std::vector<GpuTwigLevel> packTwigLevels(const MeshClusters& clusters, std::uint32_t firstPoolCluster);

/*
* The same levels from the pool's records of a prototype's clusters (the renderer's view of them: packMeshClusters keeps
* each cluster's group's level, sphere and error), its clusters from firstPoolCluster. Throws as the other does.
*/
std::vector<GpuTwigLevel> packTwigLevels(std::span<const GpuMeshCluster> records, std::uint32_t firstPoolCluster);

// A prototype's levels in the level table: from firstLevel, levelCount of them.
struct GpuTwigPrototype {
    std::uint32_t firstLevel = 0;
    std::uint32_t levelCount = 0;
    std::uint32_t unused[2] = {0, 0};
};

static_assert(sizeof(GpuTwigPrototype) == 16, "GpuTwigPrototype must match twig_drawing.glsl's TwigPrototype");

/*
* A twig a view draws, as the twig pass writes it for the cluster passes and mesh shaders (an entry from
* TWIG_INSTANCE_BASE, in its region by its level's cluster count): its part's placed entry (the camera's place in the
* placed entries), its twig's number in the twig table, its level's first cluster in the pool.
*/
struct GpuTwigEntry {
    std::uint32_t placedEntry = 0;
    std::uint32_t twig = 0;
    std::uint32_t firstCluster = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuTwigEntry) == 16, "GpuTwigEntry must match twig_drawing.glsl's uvec4");

inline constexpr std::uint32_t twigInstanceBase = TWIG_INSTANCE_BASE;
inline constexpr std::uint32_t twigGroupBase = TWIG_GROUP_BASE;
inline constexpr std::uint32_t twigMostLevelClusters = TWIG_MOST_LEVEL_CLUSTERS;
inline constexpr std::uint32_t twigMostLevels = TWIG_MOST_LEVELS;
static_assert(TWIG_BRICK_SIZE == twigBrickSize, "The twig pass's workgroup is a brick");
static_assert(TWIG_BRICK_LEVEL == twigBrickLevel, "Twigs of a set with far levels stop where its bricks take over");
static_assert(TWIG_FAR_NO_NODE == twigFarNoNode, "The far tree's no node is the shaders'");
inline constexpr std::uint32_t twigFarRegion = TWIG_FAR_REGION;
inline constexpr std::uint32_t twigRegions = TWIG_REGIONS;
inline constexpr std::uint32_t twigFarClusterBit = TWIG_FAR_CLUSTER_BIT;
static_assert(TWIG_REGION_WORDS >= TWIG_REGIONS && TWIG_REGION_WORDS % 4 == 0, "The region counts fill whole uvec4s");

/*
* A far leaf's record as the GPU holds it (scene/twig_set.h's FarLeaf; ROADMAP phase 21 stage 3e-ii), 28 B: its foot
* (plant's frame), its frame as the unit quaternion taking x, y and z to across, towardsTip and facing (four snorm16,
* x and y in the first word's low and high halves, z and w in the second's), a word of its side (a half float, low),
* its cell (bits 16-23) and its number (bits 24-31), and its twig's number.
*/
struct GpuFarLeaf {
    float foot[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t turn[2] = {0, 0};
    std::uint32_t sideCellNumber = 0;
    std::uint32_t twig = 0;
};

static_assert(sizeof(GpuFarLeaf) == 28, "GpuFarLeaf must match twig_record.glsl's FarLeaf");

// An atlas cell's shape (scene/twig_set.h's FarLeafShape) with the fold and curl every leaf shares: base, tip, the
// row's height and left half width; its right half width, the fold's raise (its tangent), the curl, unused; its texture
// rectangle (u and v low, u and v high).
struct GpuFarLeafShape {
    float shape[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float right = 0.0f;
    float raise = 0.0f;
    float curl = 0.0f;
    float unused = 0.0f;
    float uvRect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuFarLeafShape) == 48, "GpuFarLeafShape must match twig_record.glsl's FarLeafShape");

// A far leaf's record from its CPU record. Throws std::invalid_argument for a cell or number past 255.
GpuFarLeaf packFarLeaf(const FarLeaf& leaf);

// A cell's shape record, with every leaf's fold (radians) and curl.
GpuFarLeafShape packFarLeafShape(const FarLeafShape& shape, float foldRadians, float curl);

/*
* A far node as the twig pass reads it (scene/twig_set.h's TwigFarNode; ROADMAP phase 21 stage 3e-iii): its sphere in
* the plant's frame, its levels' run in the far level table, its first brick in the brick table (the brick whose
* workgroup draws it) and its parent in the node table (TWIG_FAR_NO_NODE for the root).
*/
struct GpuTwigFarNode {
    float sphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t firstLevel = 0;
    std::uint32_t levelCount = 0;
    std::uint32_t firstBrick = 0;
    std::uint32_t parent = TWIG_FAR_NO_NODE;
};

static_assert(sizeof(GpuTwigFarNode) == 32, "GpuTwigFarNode must match twig_drawing.glsl's TwigFarNode");

// A far node's level: its clusters' run in the far cluster table, its error (metres, plant's frame) and its leaves.
struct GpuTwigFarLevel {
    std::uint32_t firstCluster = 0;
    std::uint32_t clusterCount = 0;
    float error = 0.0f;
    std::uint32_t leafCount = 0;
};

static_assert(sizeof(GpuTwigFarLevel) == 16, "GpuTwigFarLevel must match twig_drawing.glsl's TwigFarLevel");

/*
* A far leaf cluster (ROADMAP phase 21 stage 3e-iii-b): up to farLeavesPerCluster of a level's leaves in a row, the
* sphere holding every vertex the mesh shader makes of them (plant's frame), their run in the far leaf table, and their
* set's first cell shape.
*/
struct GpuTwigFarCluster {
    float sphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t firstLeaf = 0;
    std::uint32_t leafCount = 0;
    std::uint32_t firstShape = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuTwigFarCluster) == 32, "GpuTwigFarCluster must match twig_drawing.glsl's TwigFarCluster");

/*
* A twig set as the twig pass reads it: its bricks' run in the brick table, its bricks' far nodes from firstNode (brick
* k's node firstNode + k; TWIG_FAR_NO_NODE for a set without far levels) and its cells' shapes from firstShape.
*/
struct GpuTwigSet {
    std::uint32_t firstBrick = 0;
    std::uint32_t brickCount = 0;
    std::uint32_t firstNode = TWIG_FAR_NO_NODE;
    std::uint32_t firstShape = 0;
};

static_assert(sizeof(GpuTwigSet) == 16, "GpuTwigSet must match twig_pass.glsl's uvec4");

// ---- Mirrored by twig_record.glsl ----

// A far leaf's vertex as the mesh shader makes it: in the plant's frame, its normal unit, its texture coordinate's
// whole part its leaf's number (as a twig prototype's, numberedLeafU), its tangent across the leaf.
struct FarLeafVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec4 tangent;
};

/*
* Vertex (0 to FAR_LEAF_VERTICES - 1: base, the row's left edge, midrib and right edge, tip) of a far leaf, made as
* games/forest/leaf_clusters.h's appendModelledLeaf makes a one-row leaf: its frame from the record's quaternion,
* each point raised by the fold and dropped by the curl, its normal its triangles' facings summed (area weighted).
*/
FarLeafVertex farLeafVertex(const GpuFarLeaf& leaf, const GpuFarLeafShape& shape, std::uint32_t vertex);

// A far leaf's triangle (0 to FAR_LEAF_TRIANGLES - 1) as its three vertices, wound as appendModelledLeaf winds them.
std::array<std::uint32_t, 3> farLeafTriangle(std::uint32_t triangle);

// ---- Mirrored by twig_drawing.glsl ----

/*
* A twig of a placed part: the part's instance (its plant and part placed: placedInstance) with the twig's matrix after
* its own, so the prototype's frame goes to the world; the twig's turn after the part's normals (its scale is uniform,
* so its matrix's turning part serves, made unit later), its scale on the part's.
*/
GpuInstance twigInstance(const GpuInstance& part, const GpuTwig& twig);

// A twig carried whole by a rigid pose in the plant's frame (its bones' windRigidPose at its foot): the pose after its
// matrix, so twigInstance places it where its vertices are drawn (ROADMAP phase 21 stage 3c-iv-b).
GpuTwig twigPosed(const GpuTwig& twig, const WindBonePose& pose);

/*
* The level a twig draws (the cluster cut on its prototype's chain, meshClusterInCut's test on whole levels): the level
* whose own error (the level before's group's; none for the finest) fits the view's pixel error and whose group's does
* not, or that nothing coarser was made from. Each group holds the one before and errors rise, so exactly one level fits;
* the last if rounding left none.
*/
std::uint32_t twigLevel(const GpuTwigLevel* levels, std::uint32_t levelCount, const GpuInstance& twig, const GpuCameraCull& view);

// One step of it (the shaders' twig pass walks the levels with it): whether the twig draws level, given the level before
// (ignored for the finest).
bool twigLevelDrawn(const GpuTwigLevel& before, bool finest, const GpuTwigLevel& level, const GpuInstance& twig, const GpuCameraCull& view);

/*
* Whether a view keeps a brick or a twig of a placed part (its record placed in the world, its entry's bits): kept whole
* where the part's shape is not its mesh's or it sways unbound (meshClusterSeenWhole); else when its sphere is in the
* frustum: pose's when posed (twigBrickPose, twigPose), else the brick's in the plant's frame placed by the part, a
* twig's coarsest group's in the prototype's placed by the twig.
*/
bool twigBrickKept(const GpuTwigBrick& brick, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view,
                   const MeshClusterPose& pose);
bool twigKept(const GpuTwigLevel& coarsest, const GpuInstance& twig, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view,
              const MeshClusterPose& pose);

/*
* How a brick stands this frame (ROADMAP phase 21 stage 3c-iv-b): bound when its part is moved by bones; posed when its
* tree is too (tree: boneCullTree's), its sphere grown by the tree's move (every twig of it, carried at its foot, moves
* no farther), placed by the part, padded by the leaves' flutter (meshClusterPosed).
*/
MeshClusterPose twigBrickPose(const GpuTwigBrick& brick, const GpuInstance& part, bool partBound, const GpuBoneTree& tree);

// The same for any sphere in the plant's frame (a brick's, a far node's, a far leaf cluster's: each holds leaves carried
// by twigs, so none moves farther than the tree's move).
MeshClusterPose twigSpherePose(const float sphere[4], const GpuInstance& part, bool partBound, const GpuBoneTree& tree);

/*
* How a twig stands: bound by its binding (not SWAY_UNBOUND); posed when its tree is, its coarsest group's sphere placed
* by the twig carried by its rigid pose (windRigidPose of its two bones at its foot, twigPosed), padded by the flutter.
*/
MeshClusterPose twigPose(const GpuTwigLevel& coarsest, const GpuInstance& part, const GpuTwig& twig, std::uint32_t binding, std::span<const GpuBonePose> pool,
                         const GpuBoneTree& tree);

/*
* Whether a far node's level fits the view (ROADMAP phase 21 stage 3e-iii): its error on the node's sphere, placed by the
* part, within the view's pixel error. A parent's sphere holds its children's and its errors are never less than
* theirs, so where a node's finest level fits, every node below it fits too: the nodes that fit are a run from each
* brick up its chain, the same top for every brick under it.
*/
bool twigFarLevelFits(const GpuTwigFarNode& node, const GpuTwigFarLevel& level, const GpuInstance& part, const GpuCameraCull& view);

// Whether a view keeps a far node: as twigBrickKept, its sphere in the plant's frame placed by the part (posed by pose).
bool twigFarNodeKept(const GpuTwigFarNode& node, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view,
                     const MeshClusterPose& pose);

// ---- The twig pass's steps on the CPU ----

// A twig a view draws: its number in the set, and its chosen level's clusters in the pool.
struct TwigDraw {
    std::uint32_t twig = 0;
    std::uint32_t firstCluster = 0;
    std::uint32_t clusterCount = 0;

    bool operator==(const TwigDraw&) const = default;
};

// A far node a view draws: its number in the node table, its chosen level's in the level table, and that level's clusters.
struct TwigFarDraw {
    std::uint32_t node = 0;
    std::uint32_t level = 0;
    std::uint32_t firstCluster = 0;
    std::uint32_t clusterCount = 0;

    bool operator==(const TwigFarDraw&) const = default;
};

// Twigs as the pass reads them: bricks and twigs, the prototypes and levels they draw, and the far nodes, their levels,
// leaves and cells' shapes (empty for sets without far levels).
struct TwigTables {
    std::vector<GpuTwigBrick> bricks;
    std::vector<GpuTwig> twigs;
    std::vector<GpuTwigPrototype> prototypes;
    std::vector<GpuTwigLevel> levels;
    std::vector<GpuTwigFarNode> farNodes;
    std::vector<GpuTwigFarLevel> farLevels;
    std::vector<GpuTwigFarCluster> farClusters;
    std::vector<GpuFarLeaf> farLeaves;
    std::vector<GpuFarLeafShape> farShapes;
};

// Every set's records in one table each, as the twig pass reads them; the prototypes and levels left to the caller (they
// come from the cluster pool).
struct PackedTwigSets {
    std::vector<GpuTwigSet> sets;
    TwigTables tables;
    std::vector<std::uint32_t> bindings; // a twig's packTwigBinding, by its number in the twig table
};

/*
* Sets packed one after another: each brick's twig run, each far node's levels, first brick and parent, and each far
* leaf's twig moved past the sets before (a leaf's twig names its binding, lean and tone, so it must be the table's
* number, not the set's), each twig's prototype its mesh's place in prototypeMeshes. Each far level's leaves cut into
* clusters of farLeavesPerCluster in order, each with the sphere of its leaves' vertices (farLeafVertex: the box's
* middle, out to the farthest). Sets sharing their cells' shapes share one run of them. Throws std::invalid_argument for
* a prototype not in prototypeMeshes. Cost grows with the sets' twigs and far records.
*/
PackedTwigSets packTwigSets(std::span<const TwigSet* const> sets, std::span<const MeshHandle> prototypeMeshes);

/*
* The node a brick's workgroup stands for (the far tree's cut, one choice a brick): TWIG_FAR_NO_NODE when the brick's
* own node's finest level does not fit (its twigs are drawn), else the highest node up its chain whose finest level fits.
* Every brick under that node finds it, so it is drawn once, by its first brick.
*/
std::uint32_t twigFarNodeDrawn(const TwigTables& tables, std::uint32_t brickNode, const GpuInstance& part, const GpuCameraCull& view);

// The level a far node draws: its coarsest that fits (its levels' errors rise), its finest when none does.
std::uint32_t twigFarLevel(const TwigTables& tables, const GpuTwigFarNode& node, const GpuInstance& part, const GpuCameraCull& view);

/*
* What the twig pass counted (F4's TWIGS line, ROADMAP phase 21 stage 5-0): its sources' twigs tested, those kept
* (drawn), those of them that move (posed by their bones, or kept whole as their part sways unbound), and the kept ones'
* twigs and triangles at each level. The pass writes it as TWIG_TALLY_WORDS words (readTwigTally).
*/
struct TwigTally {
    std::uint32_t tested = 0;
    std::uint32_t kept = 0;
    std::uint32_t swaying = 0;
    std::array<std::uint32_t, TWIG_MOST_LEVELS> levelTwigs{};
    std::array<std::uint32_t, TWIG_MOST_LEVELS> levelTriangles{};

    bool operator==(const TwigTally&) const = default;
};

TwigTally readTwigTally(std::span<const std::uint32_t, TWIG_TALLY_WORDS> words);

/*
* What the twig pass draws of a placed part (twig_cull.comp's steps), its tables one set's (brick k's far node k, when it
* has far nodes). Each brick makes one choice: with far nodes, the node it stands for (twigFarNodeDrawn), drawn at its
* level (twigFarLevel) by its first brick where kept (twigFarNodeKept), into farDraws when given; else its twigs: the
* brick kept (twigBrickKept), each of its twigs kept (twigKept), each drawn at its level (twigLevel; no coarser than
* TWIG_BRICK_LEVEL in a set with far nodes, so the bricks take over from exactly the leaves the twigs showed), in brick
* and twig order. A tally, when given, gains the part's counts, as the pass's atomics add every part's. bones, when
* given: the frame's (the entry's plant's tree poses the bricks of a part bound to bones, partBound, and its bound twigs);
* without, nothing is posed.
*/
std::vector<TwigDraw> twigDrawsOnCpu(const TwigTables& tables, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record,
                                     const GpuCameraCull& view, TwigTally* tally = nullptr, const BoneCullTables* bones = nullptr, bool partBound = false,
                                     std::vector<TwigFarDraw>* farDraws = nullptr);
