#ifndef SCENE_PLANTING_SHARED_H
#define SCENE_PLANTING_SHARED_H

// Planting from maps (scene/planting.h and its GPU twin, planting.glsl). Only #defines of plain numbers, since GLSL
// includes this file too. Decimals are read the same way on both sides (to a double, then to a float).

// Roberts' R2 sequence's steps (1 / g and 1 / g^2 for the plastic number g) in 32-bit fixed point: the ordered dither.
#define PLANTING_DITHER_STEP_COLUMN 3242174889u
#define PLANTING_DITHER_STEP_ROW 2447445414u

// A random number's 24 bits to 0..1: times 2^-24.
#define PLANTING_UNIT_STEP 5.9604644775390625e-8

// A full turn in radians: a plant's turn is this times a random number.
#define PLANTING_FULL_TURN 6.283185307179586

// Each dither cell's own random numbers: this many, the first PLANTING_DRAWS_USED placing its plant in the cell, then
// its scale, turn, kind and pick.
#define PLANTING_DRAWS_PER_CELL 8
#define PLANTING_DRAWS_USED 2
#define PLANTING_DRAW_SCALE 0
#define PLANTING_DRAW_TURN 1
#define PLANTING_DRAW_KIND 2
#define PLANTING_DRAW_PICK 3

// The ring of points round a trunk its lowest ground is read at: eight, a quarter turn's diagonal sqrt(1/2).
#define PLANTING_RING_POINTS 8
#define PLANTING_RING_DIAGONAL 0.7071067811865476

// The most kinds a layer has (its size table), and the most chosen by shares (a cumulative share before each but the last).
#define PLANTING_MOST_KINDS 32
#define PLANTING_MOST_SHARED_KINDS 5

// A cover layer's map byte a lattice cell (PlantingCoverMap): 0 nothing, else 1 + its kind times this many turns plus
// its quarter turns; a quarter turn in radians.
#define PLANTING_COVER_TURNS 4
#define PLANTING_QUARTER_TURN 1.5707963267948966

// The stored maps' steps: clearance and caps in decimetres, the path's signed distance in centimetres.
#define PLANTING_DECIMETRE 0.1
#define PLANTING_CENTIMETRE 0.01

// The maps' limits: the farthest clearance told apart (metres), a cap texel no sight line passes over, the height read
// there (metres), caps stored as decimetres above this many metres down, and the farthest path distance stored (cm).
#define PLANTING_FARTHEST_CLEARANCE 60.0
#define PLANTING_NO_CAP 65535u
#define PLANTING_UNCAPPED 1e9
#define PLANTING_CAP_FLOOR_METRES 1000.0
#define PLANTING_FARTHEST_PATH 65535.0

#endif
