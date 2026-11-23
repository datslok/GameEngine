#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/bone_poses_shared.h"
#include "render/gpu/bone_records.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/placed_drawing.h"
#include "render/gpu/planting_records.h"
#include "render/gpu/wind_bones.h"
#include "scene/frame_description.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/*
* The bones pass (ROADMAP phase 21 stage 3c-iii-b, docs/research/LEAFGEOMETRY.md, The bones pass): each frame every tree
* a swaying view draws (the camera, the swaying shadow tiles; trees are placed plants) is posed once, this frame and
* last frame, into a pool. bones_claim.comp: a workgroup a (swaying view, group of a part moved by bones), a thread an
* entry, claims its plant with a frame stamp (one word pair a held plant: no clearing), the first to claim it taking a
* tree record and its bones' room in the pool. bones_pose.comp: a workgroup a tree, its lean from the trees' spring at
* its foot, then its chains a level at a time (a thread a chain, bone by bone), barriers between levels, so each bone is
* posed once. Render-only and API-free: the functions below mirror bone_poses.glsl, and the passes' steps on the CPU
* serve the tests and bones-compare.
*/

inline constexpr std::uint32_t boneMostTrees = BONE_MOST_TREES;
inline constexpr std::uint32_t boneNoPose = BONE_NO_POSE;
inline constexpr std::uint32_t boneNoTree = BONE_NO_TREE;
inline constexpr std::uint32_t boneNoPart = BONE_NO_PART;

/*
* A posed tree: its plant (in the slots' plants), its skeleton, its first pose in the pool (two a bone, this frame's
* then last frame's; BONE_NO_POSE when the pool was full: it stands still), the farthest it moves anything bound to it
* this frame (boneTreeMove, mesh units; for the culling's bounds), and its lean in its plant's frame this frame and last
* (for bones-compare); the pose pass writes the move and leans. Must match bone_poses.glsl's BoneTree.
*/
struct GpuBoneTree {
    std::uint32_t plant = 0;
    std::uint32_t skeleton = 0;
    std::uint32_t firstPose = boneNoPose;
    float move = 0.0f;
    float lean[2] = {0.0f, 0.0f};
    float previousLean[2] = {0.0f, 0.0f};
};

static_assert(sizeof(GpuBoneTree) == 32, "GpuBoneTree must match bone_poses.glsl's BoneTree");

/*
* A bone's pose in the pool: its turn (x y z w) and shift, in its mesh's units, and its parent within its skeleton (-1 a
* root), so a vertex finds both poses it blends without the bone table. Must match bone_poses.glsl's BonePose.
*/
struct GpuBonePose {
    float turn[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float shift[3] = {0.0f, 0.0f, 0.0f};
    std::int32_t parent = -1;
};

static_assert(sizeof(GpuBonePose) == 32, "GpuBonePose must match bone_poses.glsl's BonePose");

GpuBonePose packBonePose(const WindBonePose& pose, std::int32_t parent);
WindBonePose unpackBonePose(const GpuBonePose& pose);

/*
* The part each of a table's skeletons takes its trees' lean from (its spring's bend): the first part bound to it
* (GpuPlacedPart::unused[1]) whose draw sways as a tree; BONE_NO_PART where none does. plantedParts: the frame's
* planted parts the table's draws index.
*/
std::vector<std::uint32_t> skeletonLeanParts(const PlacedTable& table, std::span<const DrawItem> plantedParts);

// The groups of the parts moved by bones (each such part's every level slot), in part order.
std::vector<std::uint32_t> boneGroups(const PlacedTable& table);

// ---- Mirrored by bone_poses.glsl ----

// A lean along the ground (x, z) in a plant's own frame: the world's turned back by the plant's turn about up.
Vec2 windLeanInPlant(const Vec2& lean, float turnRadians);

// Whether a tree is posed: its foot nearer the eye than the sway's fade end (past it nothing sways).
bool boneTreeWanted(const Vec3& foot, const Vec3& eye, float fadeEnd);

// A tree's first pose, two a bone from twice the bones asked for before it, if all its bones fit the pool; else BONE_NO_POSE.
std::uint32_t boneTreeFirstPose(std::uint32_t askedBefore, std::uint32_t boneCount, std::uint32_t poolBones);

/*
* The pool for the frames after one that asked for bonesAsked: pool while they fit; else the next power of two at or
* above them (room to spare, so a view a little denser does not grow it again), held under most (the cap, at least
* pool). Never smaller than pool: a pool stays as big as the densest view yet asked.
*/
std::uint32_t boneGrownPool(std::uint32_t pool, std::uint32_t bonesAsked, std::uint32_t most);

// A tree's seed for its branches' phases: its plant's pick's bits (fixed for the plant wherever its slot is).
std::uint32_t boneTreeSeed(const GpuPlacedPlant& plant);

// The stamp after a frame's: the next, never 0 (the plant table's cleared value and the draws' "nothing posed").
std::uint32_t boneNextStamp(std::uint32_t stamp);

/*
* The tree a plant's word pair (claimStamp, claimTree) holds for a draw this frame: claimTree when the frame that claimed
* it is this one, else BONE_NO_TREE. frameStamp 0 (the passes did not run) matches nothing, though the table starts
* cleared to 0.
*/
std::uint32_t boneClaimedTree(std::uint32_t claimStamp, std::uint32_t claimTree, std::uint32_t frameStamp);

// A bone's place in the pool: its tree's first pose + 2 bone, last frame's (previous) after it.
std::uint32_t bonePoseSlot(std::uint32_t firstPose, std::uint32_t bone, bool previous);

// What a bound vertex blends: its bone's pose, its parent's (the rest pose for a root), its own weight.
struct BoneVertexPoses {
    WindBonePose own;
    WindBonePose parent;
    float weight = 1.0f;
};

// A vertex's poses (binding: packSwayBinding's, not SWAY_UNBOUND) from its tree's (firstPose) in the pool, this frame's or last's.
BoneVertexPoses boneVertexPoses(std::span<const GpuBonePose> pool, std::uint32_t firstPose, std::uint32_t binding, bool previous);

/*
* What the camera's culling reads of the bones (ROADMAP phase 21 stage 3c-iv-b; bone_cull_data.glsl): each held
* plant's word pair (stamp, tree), the trees, the pool, each twig's binding (packTwigBinding), and the frame's stamp (0
* when the bones pass did not run: nothing is posed).
*/
struct BoneCullTables {
    std::span<const std::uint32_t> plantTrees;
    std::span<const GpuBoneTree> trees;
    std::span<const GpuBonePose> poses;
    std::span<const std::uint32_t> twigBindings;
    std::uint32_t stamp = 0;
};

/*
* The tree posing a plant this frame, as the draws find it (boneDrawnBy): claimed with this frame's stamp and given room
* in the pool; else a tree with firstPose BONE_NO_POSE (its bound vertices stand still). A plant past the table is none.
*/
GpuBoneTree boneCullTree(const BoneCullTables& tables, std::uint32_t plant);

// The plant table the claims leave (for cluster-compare, from read-back trees): each tree's plant's pair (stamp, tree), plants' pairs long.
std::vector<std::uint32_t> bonePlantTreesOf(std::span<const GpuBoneTree> trees, std::uint32_t stamp, std::size_t plants);

// ---- The passes' steps on the CPU ----

// A plant to pose and the skeleton its parts are moved by.
struct BoneWanted {
    std::uint32_t plant = 0;
    std::uint32_t skeleton = 0;

    bool operator==(const BoneWanted&) const = default;
};

/*
* What the claim pass takes: every plant a swaying view (its number among the placed views) draws a part moved by bones
* of, its foot within reach (boneTreeWanted), each once, by plant. commands: every view's, groups a view (as the placed
* passes write them); entries: every view's.
*/
std::vector<BoneWanted> boneWantedPlants(const PlacedTable& table, std::span<const GpuDrawCommand> commands, std::span<const GpuPlacedEntry> entries,
                                         std::span<const std::uint32_t> swayingViews, std::span<const GpuPlacedPlant> plants, const Vec3& eye,
                                         float fadeEnd);

// The claims in an order (the GPU's is its atomics'): each a tree, its bones' room in the pool if they fit, the bones asked and the trees left still.
struct BoneClaims {
    std::vector<GpuBoneTree> trees;
    std::uint32_t bonesAsked = 0;
    std::uint32_t still = 0;
};

// Throws std::invalid_argument for a skeleton past the table's. Trees past BONE_MOST_TREES are not claimed.
BoneClaims claimBoneTrees(std::span<const BoneWanted> wanted, std::span<const GpuSwaySkeleton> skeletons, std::uint32_t poolBones);

/*
* A tree's poses (by bone, within its skeleton) as the pose pass makes them: level by level, each level's chains bone
* by bone, each bone's own turn (windBoneOwnTurn) carried by its parent's pose. tree: the skeleton's height and foot
* radius with the plant's scale and seed. Equal to windTreePoses for a skeleton whose bones are parents first.
*/
std::vector<WindBonePose> boneTreePoses(const SwaySkeletonTables& tables, std::uint32_t skeleton, const WindTreeShape& tree, const Vec2& lean,
                                        float seconds, const WindBoneMotion& motion);

/*
* A tree's move this frame as the pose pass makes it (ROADMAP phase 21 stage 3c-iv-a): windTreeMove of its bones'
* largest windPoseMoves (poses by bone, within its skeleton) and its skeleton's reach, in its mesh's units.
*/
float boneTreeMove(const SwaySkeletonTables& tables, std::uint32_t skeleton, std::span<const WindBonePose> poses);

// A tree's shape for its poses: its skeleton's height and foot radius, its plant's scale and seed.
WindTreeShape boneTreeShape(const GpuSwaySkeleton& skeleton, const GpuPlacedPlant& plant);

// ---- bones-compare ----

/*
* A frame's trees and poses read back against the CPU's: the plants it should have posed (boneWantedPlants over its
* read-back lists, by plant), those the GPU claimed, still for want of room, missing, extra (claimed though not wanted),
* claimed twice; and every posed tree's poses against boneTreePoses with the leans the GPU wrote (the springs stay on
* the GPU), the worst difference in turn (quaternion components), shift and move (mesh units).
*/
struct BoneComparison {
    std::size_t wanted = 0;
    std::size_t claimed = 0;
    std::size_t still = 0;
    std::size_t missing = 0;
    std::size_t extra = 0;
    std::size_t twice = 0;
    std::size_t posesChecked = 0;
    std::size_t wrongParents = 0;
    float worstTurn = 0.0f;
    float worstShift = 0.0f;
    float worstMove = 0.0f;
};

BoneComparison compareBonePoses(std::span<const BoneWanted> wanted, std::span<const GpuBoneTree> trees, std::span<const GpuBonePose> poses,
                                const SwaySkeletonTables& tables, std::span<const GpuPlacedPlant> plants, const WindBoneMotion& motion, float seconds,
                                float previousSeconds);

// The comparison in a line: agrees (turns within 1e-4, shifts and moves within 1e-3 units, every parent right) or DIFFERS, with the counts.
std::string describeBoneComparison(const BoneComparison& comparison);
