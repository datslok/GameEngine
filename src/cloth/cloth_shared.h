#ifndef CLOTH_CLOTH_SHARED_H
#define CLOTH_CLOTH_SHARED_H

// The cloth solver's thresholds, shared by the CPU solver (src/cloth) and its GPU port (cloth.glsl). Only #defines of
// plain numbers, since GLSL includes this file too; C++ reads them as typed constants.

// Below this length an edge has no direction to push along.
#define CLOTH_SHORTEST_EDGE 1e-9

// Below this a hinge's edge, or a triangle's doubled area, squared, is taken as squashed.
#define CLOTH_SQUASHED_HINGE 1e-12

// Closer than this, squared, to a centre or segment there is no direction out of it.
#define CLOTH_NO_DIRECTION 1e-12

// Closer than this a held point is on the hand's target, with no direction to pull along.
#define CLOTH_ON_TARGET 1e-9

// A 3 x 3 block whose determinant is below this share of its largest diagonal cubed is taken as singular.
#define CLOTH_SINGULAR_BLOCK 1e-12

// The most anchors a point is tethered to, its nearest, and a spare tether slot's anchor.
#define CLOTH_TETHERS_PER_POINT 4
#define CLOTH_NO_ANCHOR 4294967295u

// Self-collision's motion cut (Planar-DAT): a point moving towards a dividing plane stops this share of the way there
// (gamma_r; the paper gives none, NVIDIA Newton uses 0.85).
#define CLOTH_CUT_RELAXATION 0.85

// Self-collision's kinds of pair: a point and a triangle, two edges.
#define CLOTH_PAIR_POINT_TRIANGLE 0
#define CLOTH_PAIR_EDGE_EDGE 1

// Self-contacts' moves are summed as whole units of this many a metre (nanometres), so any order adds up to the same
// bits: integer sums do not depend on their order as floating-point sums do. Each move is clamped to the largest first
// (m), so a sum of hundreds stays within 32 bits.
#define CLOTH_SELF_MOVE_SCALE 1e9
#define CLOTH_SELF_LARGEST_MOVE 0.02

#endif
