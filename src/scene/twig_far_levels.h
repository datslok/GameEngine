#pragma once

#include "scene/mesh_clusters_shared.h"
#include "scene/twig_far_levels_shared.h"
#include "scene/twig_set.h"

#include <algorithm>
#include <cstdint>
#include <vector>

/*
* A twig set's far levels (ROADMAP phase 21 stage 3e-i, docs/research/LEAFGEOMETRY.md, Far levels above the twigs; the
* records are in scene/twig_set.h). Built on the CPU once a tree variant; their memory grows with the variants' leaves,
* not with the trees planted.
*/

// A far leaf is drawn with one row (games/forest/twigs.h's twigLevelRows past level 1; twig_far_levels_shared.h), so a
// cluster holds as many as fit a mesh cluster's limits (12).
inline constexpr std::uint32_t farLeafVertices = FAR_LEAF_VERTICES;
inline constexpr std::uint32_t farLeafTriangles = FAR_LEAF_TRIANGLES;
inline constexpr std::uint32_t farLeavesPerCluster = std::min(MESH_CLUSTER_MAX_VERTICES / farLeafVertices, MESH_CLUSTER_MAX_TRIANGLES / farLeafTriangles);

/*
* The far levels of a set whose prototypes hand over leaves (one TwigFarLeaves a prototype, in the set's order). Every
* twig's handed-over leaves in brick and twig order are ranked once for the whole set (prunedOrder), so a level keeping
* one leaf in 2^k keeps a subset of every finer one, across merges too. A brick's level 0 is its twigs' leaves exactly;
* each level after keeps half by rank, its survivors grown about their feet by sqrt(full area over kept area), until
* one cluster is left; then two nodes merge, as the bricks' median split paired them (twigBricksFirstHalf), starting a
* density past both children's last, again until one cluster, up to the root. The shapes are kept with them. Throws
* std::invalid_argument for a twig whose prototype has no far leaves given, or a leaf whose cell has no shape.
* Deterministic; cost grows with the leaves times the tree's depth.
*/
TwigFarLevels makeTwigFarLevels(const TwigSet& set, const std::vector<TwigFarLeaves>& prototypes, FarLeafShapes shapes);

// A far leaf as its twig carries a handed-over leaf: the leaf's foot and frame in the plant's, its side times the
// twig's scale and growth, its cell and number, the twig's number.
FarLeaf farLeafOf(const TwigInstance& twig, std::uint32_t twigNumber, const TwigLeafPlace& leaf, float growth);

