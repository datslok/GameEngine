#pragma once

#include "assets/image_loader.h"
#include "core/pixel.h"
#include "math/vec2.h"
#include "math/vec3.h"

#include <cstdint>
#include <vector>

/*
* Leaf clusters (docs/TREES.md, Part 2): games put a whole twig with many leaves on each foliage card, not one leaf, so
* a crown gets more leaf for fewer cards and less overdraw. No CC0 library has such a texture, so the forest composes its
* own at start-up from a single-leaf atlas (ambientCG's LeafSet024): a stem up each cell with leaves turned out from it,
* each leaf's colour, normal and roughness copied from the atlas, its normals turned with it.
*
* Images here are addressed as the textures read them: column x is u, row y is v (row 0 at v 0, the bottom), as the
* image loader's default flip makes a file's rows.
*/

// A leaf atlas as images: colour with the opacity in its alpha (sRGB), the normal and roughness maps (linear, the same
// size), and its grid of cells, one leaf each, its stem at the middle of the cell's bottom edge, pointing up (+v).
struct LeafAtlas {
    ImageData colour;
    ImageData normal;
    ImageData roughness;
    int columns = 1;
    int rows = 1;
};

/*
* A plain atlas for when the scan is missing (a fresh clone, the tests): columns x rows cells of cellTexels, each an
* ellipse of a leaf in colour, domed (its normals lean out towards its edges, so turning them shows), roughness 0.6.
*/
LeafAtlas standInLeafAtlas(int cellTexels, int columns, int rows, Pixel colour);

// A normal map's tangent-space normal (x along +u, y along +v) turned by angle radians counterclockwise in the picture.
Vec3 turnTangentNormal(const Vec3& normal, float angleRadians);

/*
* One leaf drawn into a cluster cell, in the cell's own coordinates (0..1 across and up): its stem's base, its turn
* from straight up (radians, counterclockwise), its length (a share of the cell), which atlas cell it shows, and its
* tone (a brightness factor) and warmth (red up and blue down by this share, or the reverse when negative).
*/
struct ClusterLeaf {
    Vec2 base;
    float angle = 0.0f;
    float length = 0.0f;
    int sourceCell = 0;
    float tone = 1.0f;
    float warmth = 0.0f;
};

// The clusters' atlas: 2 x 2 cells, each a different twig.
inline constexpr int clusterColumns = 2;
inline constexpr int clusterRows = 2;

// A cluster's stem: from near the middle of the cell's bottom edge up to near its top, bending a little to one side.
// Its point at a share t (0..1) of its length.
Vec2 clusterStemPoint(float t, float bend);

// A twig's stem's bend from its seed: -0.12..0.12 of the cell sideways at its tip.
float clusterStemBend(std::uint32_t seed);

/*
* A twig's leaves, in a cell's coordinates: 12-18 along its stem from a tenth of the way up, alternately left and right,
* each turned 35-70 degrees out from straight up (less towards the tip) and the last one near straight up, each
* leafShare of the cell long (70-100%, smaller towards the tip) and kept inside the cell, showing a random one of the
* atlas's sourceCells, with a tone of 0.85-1.1 and a warmth of -0.06..0.06.
*/
std::vector<ClusterLeaf> clusterLeaves(int sourceCells, float leafShare, std::uint32_t seed);

/*
* The clusters' atlas drawn from a single-leaf atlas: clusterColumns x clusterRows cells of cellTexels, each a stem and
* its leaves (clusterLeaves of its own seed), each leaf's texels sampled bilinearly from its atlas cell, its normal turned
* with it (turnTangentNormal), laid over the earlier leaves by their alpha. Texels no leaf covers keep alpha 0 over the
* leaves' mean colour, so mips and bilinear filtering do not bleed a dark edge into the cut-out's border. The result's
* grid is clusterColumns x clusterRows.
*/
LeafAtlas composeLeafClusters(const LeafAtlas& source, int cellTexels, float leafShare, std::uint32_t seed);
