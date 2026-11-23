#pragma once

#include "render/gpu/mesh_pool.h"
#include "scene/indexed_mesh.h"
#include "scene/sway_bones.h"

#include <cstdint>
#include <vector>

/*
* Trees' bones as the GPU reads them (ROADMAP phase 21 stage 3c-iii, docs/research/LEAFGEOMETRY.md, Bones on the GPU):
* each skeleton's bones in one table, and every pooled vertex's binding in a list beside the vertex pool, read by the
* vertex's place in the pool, so a mesh's levels (their own vertices) and its clusters (the mesh's) need nothing more.
* Render-only and API-free.
*/

// A bone: its pivot and parent (within its skeleton, -1 a root), its tip and radius, its chain's first bone (within its
// skeleton), length and radius, its level. Must match the shaders' SwayBoneRecord.
struct GpuSwayBone {
    float pivot[3] = {0.0f, 0.0f, 0.0f};
    std::int32_t parent = -1;
    float tip[3] = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    std::uint32_t chainStart = 0;
    float chainLength = 0.0f;
    float chainRadius = 0.0f;
    std::uint32_t level = 0;
};

static_assert(sizeof(GpuSwayBone) == 48, "GpuSwayBone must match the shaders' SwayBoneRecord");

/*
* A skeleton: its bones' run in the table, its height (its mesh's units) and its first bone's radius (the foot's); its
* chain levels' starts from firstLevel in the table's levelStarts (levelCount + 1 of them: level l's chains run from
* start l to start l + 1 in the chains); the placed table's part its trees take their lean from (set by the bones
* pass's caller, skeletonLeanParts; BONE_NO_PART none); and its reach (SwaySkeleton::reach, mesh units).
*/
struct GpuSwaySkeleton {
    std::uint32_t firstBone = 0;
    std::uint32_t boneCount = 0;
    float height = 1.0f;
    float footRadius = 0.0f;
    std::uint32_t firstLevel = 0;
    std::uint32_t levelCount = 0;
    std::uint32_t leanPart = 0xFFFFFFFFu;
    float reach = 0.0f;
};

static_assert(sizeof(GpuSwaySkeleton) == 32, "GpuSwaySkeleton must match the shaders' SwaySkeletonRecord");

// A chain (a branch's run of bones, each the one before's child): its first bone within its skeleton and how many.
struct GpuSwayChain {
    std::uint32_t firstBone = 0;
    std::uint32_t boneCount = 0;
};

static_assert(sizeof(GpuSwayChain) == 8, "GpuSwayChain must match the shaders' uvec2");

struct SwaySkeletonTables {
    std::vector<GpuSwayBone> bones;
    std::vector<GpuSwaySkeleton> skeletons;
    std::vector<GpuSwayChain> chains;
    std::vector<std::uint32_t> levelStarts;
};

/*
* The skeletons in their order, each its bones after the last's and its chains by level (the trunk's, then the
* branches on it, ...; by first bone within a level), so the bones pass poses a level's chains at once once the level
* before is done. Throws std::invalid_argument for a skeleton with no bones, more than SWAY_MOST_BONES or bones not
* parents first (swayBonesOrdered); a chain not a run from its chainStart, each bone the one before's child and of one
* level; or a chain starting on a bone not of a lower level.
*/
SwaySkeletonTables packSwaySkeletons(const std::vector<const SwaySkeleton*>& skeletons);

/*
* A mesh's bindings written into the pool's list (one word a pool vertex) at its place there, the list first grown to
* the pool's end so far with SWAY_UNBOUND; a mesh without bindings leaves its place unbound. Throws
* std::invalid_argument for bindings on a mesh not in the pools, or not one a vertex.
*/
void placeSwayBindings(std::vector<std::uint32_t>& bindings, const IndexedMesh& mesh, const MeshPlacement& placement);
