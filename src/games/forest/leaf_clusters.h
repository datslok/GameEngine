#pragma once

#include "assets/image_loader.h"
#include "core/pixel.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <vector>

/*
* Leaf clusters (docs/research/TREES.md, Part 2): games put a whole twig with many leaves on each foliage card, not one leaf, so
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

/*
* One cell's outline (for appendOutlinedLeafCard): the cell's square clipped by four pairs of parallel cuts (across, up
* and the two diagonals) that just hold every texel whose alpha is at least alphaCutoff, each pushed marginTexels further
* out (the mips' blur reaches past a leaf's edge), in the cell's own units (0..1 across and up, row 0 at v 0),
* counterclockwise: at most twelve corners, usually eight. The forest's twigs cover 14-20% of their cells, their
* outlines 28-30%, so about a third of each card's pixels are drawn. Empty for a cell with nothing over the cutoff.
*/
std::vector<Vec2> clusterOutline(const ImageData& image, std::uint32_t column, std::uint32_t row, std::uint32_t cellTexels,
                                 std::uint8_t alphaCutoff, float marginTexels);

/*
* A modelled leaf's outline (docs/research/LEAFGEOMETRY.md; ROADMAP phase 21 stage 3a), in its atlas cell's units (0..1
* across and up, the midrib up the middle at 0.5, the stem at the bottom): its base and tip on the midrib, and rows
* across it between them, each its height and its half widths to the left and right of the midrib. Leaves grow along
* the midrib, so rows across it follow them closely in few corners, and a bite or a lopsided leaf is one side narrower.
*/
struct LeafRow {
    float up = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
};

struct LeafShape {
    float base = 0.0f;
    float tip = 1.0f;
    std::vector<LeafRow> rows;
};

/*
* One atlas cell's leaf traced from its alpha (at least alphaCutoff a leaf) as rowCount rows: the base and tip at the
* lowest and highest covered texels, each row's widths the covered extent on its texel row, everything pulled insetTexels
* inside the alpha's edge (the mips' blur and the scan's background stay off the leaf). The rows' heights are the ones,
* on a grid of steps between base and tip, whose shape best matches the leaf's covered area (the most overlap for the
* least union, row by row). An empty shape (no rows, base = tip = 0) for a cell with nothing over the cutoff.
*/
LeafShape traceLeafShape(const ImageData& image, std::uint32_t column, std::uint32_t row, std::uint32_t cellTexels,
                         std::uint8_t alphaCutoff, int rowCount, float insetTexels);

// The shape's area, in the cell's units squared.
float leafShapeArea(const LeafShape& shape);

// Whether a point of the cell (its units) lies inside the shape.
bool insideLeafShape(const LeafShape& shape, const Vec2& point);

/*
* Where a modelled leaf goes: its stem's foot (the cell's bottom middle), the way to its tip and its facing (square to
* each other), its cell's side in metres (the shape's units times this), how far each half is raised about the midrib
* (fold, radians) and how far its tip drops behind its plane (curl, a share of the side), and its atlas cell's texture
* rectangle.
*/
struct ModelledLeafPlace {
    Vec3 foot;
    Vec3 towardsTip;
    Vec3 facing;
    float side = 1.0f;
    float foldRadians = 0.0f;
    float curl = 0.0f;
    Vec2 uvMin;
    Vec2 uvMax;
};

/*
* A leaf as opaque geometry: its base, each row's left edge, midrib and right edge, and its tip, joined in triangles
* (4 a row: 12 for three rows), wound counterclockwise seen from its facing, textured from its atlas cell, each vertex's
* normal the mean of its triangles' (the fold shows as a soft ridge). Nothing for a shape with no rows.
*/
void appendModelledLeaf(IndexedMesh& mesh, const LeafShape& shape, const ModelledLeafPlace& place);
