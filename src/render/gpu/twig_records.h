#pragma once

#include "scene/mesh_clusters.h"
#include "scene/twig_set.h"

#include <cstdint>
#include <vector>

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
* coarsest's the largest float). A level is chosen where the level before's error is small enough and its own is not.
*/
struct GpuTwigLevel {
    float sphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float error = 0.0f;
    std::uint32_t firstCluster = 0;
    std::uint32_t clusterCount = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuTwigLevel) == 32, "GpuTwigLevel must match twig_drawing.glsl's TwigLevel");

// A twig's record from its instance.
GpuTwig packTwig(const TwigInstance& twig);

// A brick's record.
GpuTwigBrick packTwigBrick(const TwigBrick& brick);

/*
* A prototype's levels from its clusters (a chain, as buildGivenClusterLevels makes: group k level k's clusters, which
* run together), its clusters from firstPoolCluster in the pool. Throws std::invalid_argument for clusters that are not
* such a chain.
*/
std::vector<GpuTwigLevel> packTwigLevels(const MeshClusters& clusters, std::uint32_t firstPoolCluster);
