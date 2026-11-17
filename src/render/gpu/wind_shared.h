#ifndef WIND_SHARED_H
#define WIND_SHARED_H

// The wind force field's fixed sizes (render/gpu/wind.h, wind.glsl, wind_field.comp), shared by C++ and GLSL.

// Cells a side of each cascade's window round the camera, and the cascades (near and far).
#define WIND_CELLS 64
#define WIND_CASCADES 2

// The categories of springs (Horizon Zero Dawn's four): trees, plants, grass, and hanging things.
#define WIND_CATEGORIES 4
#define WIND_CATEGORY_TREES 0
#define WIND_CATEGORY_PLANTS 1
#define WIND_CATEGORY_GRASS 2
#define WIND_CATEGORY_HANGING 3

/*
* The field's springs: the trees', plants' and grass's, then the hanging things' in three bands (short, medium, long),
* since a hanging chain swings slower the longer it is; each hanging vertex blends the two bands round its own swing.
* The hanging category's own sway reads the short band.
*/
#define WIND_HANGING_BANDS 3
#define WIND_SPRINGS 6
#define WIND_SPRING_HANGING_SHORT 3

/*
* A hanging chain's slowest swing (render/gpu/wind.h, windChainHertz): its period is WIND_CHAIN_PERIOD sqrt(L / g)
* (2 pi over half the first zero of the Bessel function J0, 2.4048 / 2), its length floored at the shortest swing (m).
*/
#define WIND_GRAVITY 9.81
#define WIND_CHAIN_PERIOD 5.2255
#define WIND_SHORTEST_SWING 0.05

// The field pass's workgroup side (an 8 x 8 square of cells, one layer).
#define WIND_GROUP_SIDE 8

// The longest step the springs take in one frame (seconds): a frame slower than this moves them by this much, so a
// hitch never throws them.
#define WIND_LONGEST_STEP 0.1

// The most radians of a spring's swing in one substep, and the most substeps a frame: semi-implicit Euler stays near
// the true swing well inside its limit of 2.
#define WIND_SUBSTEP_TURN 0.5
#define WIND_MOST_SUBSTEPS 8

// How much slower gusts change shape than they travel, and the sideways turbulence's lattice offset from the gusts'.
#define WIND_GUST_EVOLUTION 0.25
#define WIND_TURBULENCE_OFFSET 37.0

// Swaying (render/gpu/wind.h, windSwayOffset). A sway's category for things that stand still.
#define WIND_CATEGORY_STILL -1

// A hanging vertex's anchor codes (scene/wind.h): one that never moves, and its own object's sway at the vertex.
#define WIND_ANCHOR_FIXED 65535
#define WIND_ANCHOR_OWN 65534

// The most lean (radians) a soft limit lets a spring give, however hard it blows.
#define WIND_MOST_LEAN 0.5

// The cells over which a place passes from one cascade's window to the next coarser one's, before its edge's cell.
#define WIND_BLEND_CELLS 8.0

// Branches bob by this share of the lean times their distance from the trunk, this often (Hz), their phase changing
// every so many metres. (Leaves' flutter is each category's own, WindSettings::flutters.)
#define WIND_BRANCH_SHARE 0.2
#define WIND_BRANCH_HERTZ 0.9
#define WIND_BRANCH_METRES 4.0

#endif
