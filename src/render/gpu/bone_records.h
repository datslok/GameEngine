#pragma once

#include "render/gpu/bone_records_shared.h"
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

inline constexpr std::uint32_t swayUnbound = SWAY_UNBOUND;
inline constexpr std::size_t swayMostBones = SWAY_MOST_BONES;

// A binding in one word: its bone in the low 16 bits, its weight as a 16-bit unsigned normalized number above them.
// Throws std::invalid_argument for a bone outside 0..SWAY_MOST_BONES - 1.
std::uint32_t packSwayBinding(const SwayBinding& binding);

// A packed binding back (its weight rounded to 1 / 65535).
SwayBinding unpackSwayBinding(std::uint32_t packed);

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

// A skeleton: its bones' run in the table, its height (its mesh's units) and its first bone's radius (the foot's).
struct GpuSwaySkeleton {
    std::uint32_t firstBone = 0;
    std::uint32_t boneCount = 0;
    float height = 1.0f;
    float footRadius = 0.0f;
};

static_assert(sizeof(GpuSwaySkeleton) == 16, "GpuSwaySkeleton must match the shaders' SwaySkeletonRecord");

struct SwaySkeletonTables {
    std::vector<GpuSwayBone> bones;
    std::vector<GpuSwaySkeleton> skeletons;
};

/*
* The skeletons in their order, each its bones after the last's. Throws std::invalid_argument for a skeleton with no
* bones, more than SWAY_MOST_BONES or bones not parents first (swayBonesOrdered).
*/
SwaySkeletonTables packSwaySkeletons(const std::vector<const SwaySkeleton*>& skeletons);

/*
* A mesh's bindings written into the pool's list (one word a pool vertex) at its place there, the list first grown to
* the pool's end so far with SWAY_UNBOUND; a mesh without bindings leaves its place unbound. Throws
* std::invalid_argument for bindings on a mesh not in the pools, or not one a vertex.
*/
void placeSwayBindings(std::vector<std::uint32_t>& bindings, const IndexedMesh& mesh, const MeshPlacement& placement);
