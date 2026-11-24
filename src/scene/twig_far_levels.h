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
// The error percentile at which a level waits for its worst leaf.
inline constexpr float farErrorWorstPercentile = 100.0f;

inline constexpr std::uint32_t farLeavesPerCluster = std::min(MESH_CLUSTER_MAX_VERTICES / farLeafVertices, MESH_CLUSTER_MAX_TRIANGLES / farLeafTriangles);

/*
* The far levels of a set whose prototypes hand over leaves (one TwigFarLeaves a prototype, in the set's order). Every
* twig's handed-over leaves in brick and twig order are ranked once for the whole set (prunedOrder), so a level keeping
* one leaf in 2^k keeps a subset of every finer one, across merges too. A brick's level 0 is its twigs' leaves exactly;
* each level after keeps half by rank, its survivors grown about their feet by sqrt(full area over kept area), until
* one cluster is left; then two nodes merge, as the bricks' median split paired them (twigBricksFirstHalf), starting a
* density past both children's last, again until one cluster, up to the root. Each level's leaves are in their feet's
* median split order (math/median_split.h), so its runs of farLeavesPerCluster are clusters of neighbours. The shapes
* are kept with them. Throws std::invalid_argument for a twig whose prototype has no far leaves given, a leaf whose
* cell has no shape, or an error percentile outside (0, 100]. Deterministic; cost grows with the leaves times the
* tree's depth.
*
* A level's error is its leaves' at errorPercentile (nearest rank, math/percentile.h; stage 3e-iv): a brick's finest
* its leaves' twig errors (the twig level's times the twig's scale, once a leaf), each coarser level its grown leaves'
* strays past their finest sides, never under the level before's; a merged node's floor its children's last. At 100 a
* level waits for its worst leaf (a 1.5-times twig's), so bricks stayed at level 0 where most of their twigs had gone
* on to levels 3-4; lower lets a level stand for its typical leaf, its largest leaves straying a little past a pixel.
*/
TwigFarLevels makeTwigFarLevels(const TwigSet& set, const std::vector<TwigFarLeaves>& prototypes, FarLeafShapes shapes, float errorPercentile);

// A far leaf as its twig carries a handed-over leaf: the leaf's foot and frame in the plant's, its side times the
// twig's scale and growth, its cell and number, the twig's number.
FarLeaf farLeafOf(const TwigInstance& twig, std::uint32_t twigNumber, const TwigLeafPlace& leaf, float growth);

