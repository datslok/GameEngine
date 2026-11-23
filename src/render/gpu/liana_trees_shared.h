#ifndef LIANA_TREES_SHARED_H
#define LIANA_TREES_SHARED_H

// The lianas' trees found among the planted plants (render/gpu/liana_trees.h, liana_trees.glsl, liana_trees.comp),
// numbers shared by C++ and GLSL. Only #defines of plain numbers, since GLSL includes this file too.

// Threads a workgroup of the pass, a thread a liana's anchor.
#define LIANA_TREES_GROUP_SIZE 64

// How near a planted plant's foot is to a liana's tree's to be that tree: both are planted by the same maths, so they
// agree to rounding, while trunks stand metres apart.
#define LIANA_TREE_MATCH_METRES 0.01

// A cell table's place holding no cell.
#define LIANA_CELL_EMPTY 0xFFFFFFFFu

// The pass's set: the lianas' records, the slots' plants and counts, the layers, the cells, each anchor's plant found.
#define LIANA_TREES_BINDINGS 6

// Each liana anchor's plant (BONE_NO_TREE none), two a liana, in the scene set for the lianas' vertex shaders.
#define SCENE_LIANA_TREE_PLANTS_BINDING 53

#endif
