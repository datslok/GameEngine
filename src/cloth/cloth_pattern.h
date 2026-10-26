#pragma once

#include "cloth/cloth_seams.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/mesh.h"

#include <cstdint>
#include <vector>

/*
* Sewing patterns: garments cut from flat pieces of fabric and sewn together, as a dressmaker's paper pattern draws
* them. Each piece is a closed outline in the plane, in metres, made of named stretches (edges: a hem, a side seam, an
* armhole), each a polyline from its first point to its last, the next edge starting where it ends. Outlines run
* counterclockwise seen from the fabric's outside (+z), so the fabric lies on each edge's left.
*
* A seam sews one piece's edge to another's (or the same piece's). Two outlines that both run counterclockwise run
* opposite ways along the edge they share once sewn, as the faces of any surface do, so a seam joins the first edge's
* point k to the second's point n - k. Sewn edges should be the same length, as a pattern cutter makes them; their
* points are spaced evenly along each, so a little difference is shared out along the seam.
*/
struct PatternEdge {
    std::vector<Vec2> points;
};

struct PatternPiece {
    std::vector<PatternEdge> edges;
};

struct PatternSeam {
    std::uint32_t firstPiece = 0;
    std::uint32_t firstEdge = 0;
    std::uint32_t secondPiece = 0;
    std::uint32_t secondEdge = 0;
};

struct Pattern {
    std::vector<PatternPiece> pieces;
    std::vector<PatternSeam> seams;
};

// How long an edge is along its polyline.
float edgeLength(const PatternEdge& edge);

// segments + 1 points evenly spaced along the edge's polyline, its first and last points exactly its own.
std::vector<Vec2> resampleEdge(const PatternEdge& edge, int segments);

/*
* A pattern cut into cloth: every piece's points in one mesh, laid flat in z = 0 side by side along x (a cutting
* layout: the first piece where its outline is, each next one a little to the right of the last, so no two overlap),
* the rest shape a cloth made from it keeps.
*/
struct PatternMesh {
    Mesh mesh;                                                   // UVs the flat positions, in metres
    std::vector<Vec2> flatPositions;                             // each point where it lies on its own piece's outline
    std::vector<std::uint32_t> pieces;                           // each point's piece
    std::vector<std::vector<std::vector<std::uint32_t>>> edgePoints; // [piece][edge]: its points along it, ends included
    std::vector<std::vector<ClothStitch>> seams;                 // each seam's stitches, in order along its first edge
};

/*
* Each piece cut into near-equilateral triangles about spacing apart (metres): its edges' points spaced evenly at no
* more than spacing (sewn edges with as many points as each other), a triangular lattice inside kept at least 0.6 of a
* spacing from the outline, the Delaunay triangulation of them all (math/delaunay.h), and the triangles outside the
* outline dropped. The lattice is fixed in the pattern's plane, the same for every piece.
*
* Equilateral triangles because they are the same every way: a grid's one diagonal a square makes cloth stiffer along
* that diagonal than across it. Throws std::invalid_argument for a spacing that is not finite and positive, a piece that
* does not close, runs clockwise or has fewer than three edges' worth of points, a seam on an edge or piece that does
* not exist or on an edge already sewn, or a point that would be in two stitches (a corner where two sewn edges meet);
* std::logic_error for an outline too sharp for the spacing to keep its edges.
*/
PatternMesh meshPattern(const Pattern& pattern, float spacing);

// How the cloth is sewn: every stitch, the hinges across each seam (seamHinges) from the flat rest shape, and its pieces' outlines for its tethers.
ClothSewing clothSewing(const PatternMesh& meshed);

/*
* A point of a piece wrapped round an upright cylinder of radius round the axis through axisFoot: the piece's x runs
* round the circle (its lengths kept: a cylinder is flat cut open), its middle line (x = 0) facing radians round from
* +z towards +x, its y straight up from axisFoot. The piece's outside faces outwards. How garment software lays panels
* round a body before sewing them.
*/
Vec3 wrapOnCylinder(const Vec2& flat, const Vec3& axisFoot, float radius, float facing);
