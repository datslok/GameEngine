#ifndef RENDER_GPU_PLANTING_RECORDS_SHARED_H
#define RENDER_GPU_PLANTING_RECORDS_SHARED_H

// The planting cells' records (render/gpu/planting_records.h and planting_cells.glsl). Only #defines of plain numbers,
// since GLSL includes this file too.

// Threads a workgroup: the candidates' and spacing passes a lattice cell each, the writing pass a cell's box in rows.
#define PLANTING_GROUP_SIZE 64
#define PLANTING_WRITE_GROUP_SIZE 256

// The most earlier layers a layer keeps away from.
#define PLANTING_MOST_KEEP_AWAYS 4

// The maps' header's flags: which maps are there (an empty map reads as outside).
#define PLANTING_HAS_ECOTOPES 1u
#define PLANTING_HAS_GAPS 2u
#define PLANTING_HAS_PATH 4u
#define PLANTING_HAS_CLEARANCE 8u

// A cap's record in the maps' words: reach, origin x and z, columns, rows, its heights' first word, two unused.
#define PLANTING_CAP_WORDS 8

// The pass's own set: the maps, the layers, the ground's heights, the frame's jobs, the candidates, the slots' plants,
// their counts, and the problems seen.
#define PLANTING_MAPS_BINDING 0
#define PLANTING_LAYERS_BINDING 1
#define PLANTING_HEIGHTS_BINDING 2
#define PLANTING_JOBS_BINDING 3
#define PLANTING_CANDIDATES_BINDING 4
#define PLANTING_PLANTS_BINDING 5
#define PLANTING_COUNTS_BINDING 6
#define PLANTING_PROBLEMS_BINDING 7
#define PLANTING_BINDINGS 8

// What the CPU would have thrown for, counted instead: a layer read past what an earlier one planted, a plant wider
// than every canopy cap, a cell's plants past its slot's room.
#define PLANTING_PROBLEM_READ_PAST 0
#define PLANTING_PROBLEM_NO_CAP 1
#define PLANTING_PROBLEM_FULL 2
#define PLANTING_PROBLEMS 3

#endif
