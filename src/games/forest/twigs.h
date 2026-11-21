#pragma once

#include "core/settings_visitor.h"
#include "games/forest/leaf_clusters.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh_clusters.h"

#include <cstdint>
#include <vector>

/*
* Twig prototypes (ROADMAP phase 21 stage 3a, docs/research/LEAFGEOMETRY.md): a few twigs of modelled, opaque leaves, which
* the trees will place along their limbs as instances (assemblies) in place of today's cut-out twig cards. Each twig lays
* its leaves out as the cards' pictures do (clusterLeaves), each leaf traced from its LeafSet024 cell (traceLeafShape)
* and built as geometry (appendModelledLeaf), tilted a little out of the twig's plane; a thin stem up the middle.
*
* Each twig has its own levels: each coarser level keeps half the leaves of the one below by a fixed rank (prunedOrder),
* the survivors grown so the twig keeps its leaf area, and drawn with fewer rows (the level's error grows long before a
* leaf's outline could show). A level's error is how far its grown leaves stray past their own size, the most of any.
*/

// How the twigs' leaves are built (F5 Forest, Twigs; start-up: the prototypes are built once).
struct ForestTwigSettings {
    // How many different twigs, and the nearest leaves' rows across the midrib (4 triangles a row: 3 rows, 12, the
    // user's pick over 2 or 4, Nov 21 2026); the next level draws at most 2 rows, the coarser ones 1.
    int prototypes = 8;
    int leafRows = 3;

    // Each leaf half raised about the midrib (degrees), and the tip's drop behind the leaf's plane (a share of its
    // cell): real leaves are never flat, and a folded one never turns into an edge-on line.
    float foldDegrees = 15.0f;
    float curl = 0.08f;

    // The most each leaf is turned out of the twig's plane, about its own width and its length (degrees), at random.
    float tiltDegrees = 15.0f;

    // How far inside the scan's alpha edge the outline is pulled (texels of the leaf atlas).
    float insetTexels = 1.0f;

    // The stem's radius at its foot (metres), thinning to a third at its tip.
    float stemRadius = 0.006f;
};

void visit(ForestTwigSettings& settings, SettingsVisitor& visitor);

// An atlas's leaves traced at every row count up to the most asked for: byRows[rows - 1][cell].
struct TracedLeaves {
    int columns = 1;
    int rows = 1;
    std::vector<std::vector<LeafShape>> byRows;
};

// Every cell of a leaf atlas traced from its colour's alpha at the forest's leaf cutoff, at 1 to mostRows rows.
TracedLeaves traceAtlasLeaves(const LeafAtlas& atlas, int mostRows, float insetTexels);

// The rows a twig level's leaves are drawn with: leafRows at level 0, at most 2 at level 1, 1 beyond.
int twigLevelRows(int leafRows, int level);

/*
* One level of a twig: its leaves (one mesh, the leaf material's), its stem (the finest level only; empty beyond), which
* of the twig's leaves it keeps (their numbers in clusterLeaves' order), how much the kept leaves grew (their scale), and
* its error in metres (0 for the finest; never less than the finer level's).
*/
struct TwigLevel {
    IndexedMesh leaves;
    IndexedMesh stem;
    std::vector<std::uint32_t> kept;
    float growth = 1.0f;
    float error = 0.0f;
};

// A twig, its levels from the finest until one leaf is left. Its frame: the stem's foot near the origin, the stem up
// +y, the leaves' plane facing +z, a cell of the cards' pictures twigSize metres a side.
struct TwigPrototype {
    std::vector<TwigLevel> levels;
};

/*
* A twig's leaves as one mesh for the cluster cut (ROADMAP phase 21 stage 3b-i): every level's vertices, the finest's
* triangles as its indices, and the coarser levels given with their errors (AssetManager::setGivenClusterLevels).
*/
struct TwigLeafMesh {
    IndexedMesh mesh;
    std::vector<GivenClusterLevel> coarser;
};

TwigLeafMesh twigLeafMesh(const TwigPrototype& twig);

// One twig: leaves laid out as clusterLeaves(cells, leafShare, seed) lays out a card's, in a twig twigSize metres tall.
TwigPrototype buildTwig(const TracedLeaves& leaves, const ForestTwigSettings& settings, float twigSize, float leafShare, std::uint32_t seed);

// The settings' prototypes, each from its own seed out of this one.
std::vector<TwigPrototype> buildTwigPrototypes(const TracedLeaves& leaves, const ForestTwigSettings& settings, float twigSize, float leafShare,
                                               std::uint32_t seed);
