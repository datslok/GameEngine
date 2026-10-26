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

#endif
