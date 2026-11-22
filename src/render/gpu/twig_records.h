#pragma once

#include "render/gpu/camera_culling.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/twig_drawing_shared.h"
#include "scene/mesh_clusters.h"
#include "scene/twig_set.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

struct GpuMeshCluster; // render/gpu/mesh_cluster_culling.h, which includes this

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

// ---- Mirrored by twig_drawing.glsl ----

/*
* A twig of a placed part: the part's instance (its plant and part placed: placedInstance) with the twig's matrix after
* its own, so the prototype's frame goes to the world; the twig's turn after the part's normals (its scale is uniform,
* so its matrix's turning part serves, made unit later), its scale on the part's.
*/
GpuInstance twigInstance(const GpuInstance& part, const GpuTwig& twig);

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
* where the part is not rigid (meshClusterRigid: swaying where the view's sway reaches), else when its sphere (the
* brick's in the plant's frame, placed by the part; a twig's coarsest group's in the prototype's, placed by the twig) is
* in the frustum.
*/
bool twigBrickKept(const GpuTwigBrick& brick, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view);
bool twigKept(const GpuTwigLevel& coarsest, const GpuInstance& twig, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view);

// ---- The twig pass's steps on the CPU ----

// A twig a view draws: its number in the set, and its chosen level's clusters in the pool.
struct TwigDraw {
    std::uint32_t twig = 0;
    std::uint32_t firstCluster = 0;
    std::uint32_t clusterCount = 0;

    bool operator==(const TwigDraw&) const = default;
};

// A part's twigs as the pass reads them: its bricks and twigs, and the prototypes and levels they draw.
struct TwigTables {
    std::vector<GpuTwigBrick> bricks;
    std::vector<GpuTwig> twigs;
    std::vector<GpuTwigPrototype> prototypes;
    std::vector<GpuTwigLevel> levels;
};

/*
* What the twig pass counted (F4's TWIGS line, ROADMAP phase 21 stage 5-0): its sources' twigs tested, those kept
* (drawn), those of them kept because their part sways (meshClusterRigid false: no frustum test), and the kept ones'
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
* What the twig pass draws of a placed part (twig_cull.comp's steps): each brick kept (twigBrickKept), each of its
* twigs kept (twigKept), each drawn at its level (twigLevel), in brick and twig order. A tally, when given, gains the
* part's counts, as the pass's atomics add every part's.
*/
std::vector<TwigDraw> twigDrawsOnCpu(const TwigTables& tables, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record,
                                     const GpuCameraCull& view, TwigTally* tally = nullptr);
