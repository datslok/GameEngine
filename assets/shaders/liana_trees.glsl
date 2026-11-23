// The lianas' trees found among the planted plants (render/gpu/liana_trees.h mirrors every function and is tested
// against it): the records liana_trees.comp reads and its pure maths.
#ifndef LIANA_TREES_GLSL
#define LIANA_TREES_GLSL

#include "render/gpu/liana_trees_shared.h"

// A planting layer as the lookup finds its plants. Must match GpuLianaTreeLayer.
struct LianaTreeLayer {
    uint firstPlant;  // slot 0's first plant of the layer
    uint plantsASlot;
    uint firstCount;  // slot 0's count of the layer
    uint countsASlot; // its set's layers
    float cellMetres; // its set's cells' size; 0 no set writes it
    uint firstCell;   // its set's table's first place
    uint side;        // and its side
    uint unused;
};

// A place of a set's cell table: the cell held there and its slot (LIANA_CELL_EMPTY none). Must match GpuLianaCell.
struct LianaCell {
    int column;
    int row;
    uint slot;
    uint unused;
};

// The column (or row) whose span, float(index) * size to that plus size, holds a place: plantingCellBox's edges, which
// the cells' writing pass takes its plants by.
int lianaTreeIndex(float place, float size) {
    int index = int(floor(place / size));
    precise float west = float(index) * size;
    precise float east = west + size;

    if (place < west) {
        index -= 1;
    } else if (place >= east) {
        index += 1;
    }

    return index;
}

// The cell, cellMetres a side, whose box holds a foot (x, z) (lianaTreeCell).
ivec2 lianaTreeCell(vec2 foot, float cellMetres) {
    return ivec2(lianaTreeIndex(foot.x, cellMetres), lianaTreeIndex(foot.y, cellMetres));
}

// An index modulo side, 0 to side - 1 below 0 too (GLSL's % leaves negative operands undefined; division truncates).
int lianaWrapped(int index, int side) {
    int remainder = index - (index / side) * side;
    return remainder < 0 ? remainder + side : remainder;
}

// A cell's place in its set's table, side a side (lianaCellPlace).
uint lianaCellPlace(ivec2 cell, uint side) {
    int across = int(side);
    return uint(lianaWrapped(cell.x, across) + across * lianaWrapped(cell.y, across));
}

// Whether a planted plant's foot is a liana's tree's: within LIANA_TREE_MATCH_METRES (lianaSameTree).
bool lianaSameTree(vec3 planted, vec3 foot) {
    vec3 apart = planted - foot;
    return dot(apart, apart) < float(LIANA_TREE_MATCH_METRES) * float(LIANA_TREE_MATCH_METRES);
}

#endif
