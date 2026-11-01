// What every self-collision pass (cloth_self_*.comp) binds besides the cloth passes' set 0 (render/vulkan/vulkan_cloth.h,
// GpuClothSelf): the triangles and edges, who is whose neighbour, the rest shape and pieces, the pair search's hash
// table, the pairs found, the contacts' sums and the cut's shares per point, and untangling's crossings found and the
// CPU's analysis of them (GpuClothTangles).

#ifndef CLOTH_SELF_PASS_GLSL
#define CLOTH_SELF_PASS_GLSL

#include "cloth_pass.glsl"

// Must match render/gpu/cloth_gpu.h's GpuClothSelfInfo.
struct ClothSelfInfo {
    uint triangleCount;
    uint edgeCount;
    uint bucketCount;
    uint scanBlocks;
    uint pairCapacity;
    float meanRestEdge;
    uint pieced;
    uint unused;
};

layout(std430, set = 1, binding = 0) readonly buffer ClothSelfTriangles {
    uvec4 selfTriangles[]; // corners in xyz
};

layout(std430, set = 1, binding = 1) readonly buffer ClothSelfEdges {
    uvec2 selfEdges[];
};

// Each point's representative (stitched points count as one), and the representatives one edge from each.
layout(std430, set = 1, binding = 2) readonly buffer ClothSelfJoined {
    uint joined[];
};

layout(std430, set = 1, binding = 3) readonly buffer ClothSelfNeighbourStarts {
    uint neighbourStarts[];
};

layout(std430, set = 1, binding = 4) readonly buffer ClothSelfNeighbours {
    uint neighbours[];
};

layout(std430, set = 1, binding = 5) readonly buffer ClothSelfRest {
    vec4 restPositions[];
};

layout(std430, set = 1, binding = 6) readonly buffer ClothSelfPieces {
    uint pieceOf[];
};

layout(std430, set = 1, binding = 7) readonly buffer ClothSelfInfoBuffer {
    ClothSelfInfo info;
};

// Where the points were when the pairs were found: the cut measures each pair's plane from there.
layout(std430, set = 1, binding = 8) buffer ClothSelfPairStart {
    vec4 pairStart[];
};

// The hash table: each bucket's count (then a cursor while scattering), where each starts, the scan's block totals,
// the items bucket by bucket (triangles numbered first, then edges), and the items too large for it.
layout(std430, set = 1, binding = 9) buffer ClothSelfBucketCounts {
    uint bucketCounts[];
};

layout(std430, set = 1, binding = 10) buffer ClothSelfBucketStarts {
    uint bucketStarts[];
};

layout(std430, set = 1, binding = 11) buffer ClothSelfScanBlocks {
    uint scanBlocks[];
};

layout(std430, set = 1, binding = 12) buffer ClothSelfEntries {
    uint entries[];
};

layout(std430, set = 1, binding = 13) buffer ClothSelfLargeItems {
    uint largeItems[];
};

layout(std430, set = 1, binding = 14) buffer ClothSelfPairs {
    ClothSelfPair pairs[];
};

// CLOTH_SELF_PAIR_COUNT, CLOTH_SELF_PAIRS_LOST, CLOTH_SELF_LARGE_COUNT, then the pairs' dispatch sizes.
layout(std430, set = 1, binding = 15) buffer ClothSelfState {
    uint pairState[];
};

// Each point's contact moves summed in whole units (xyz) and how many (w), and the least share of its motion the cut
// keeps, as a float's bits (shares are never negative, so their bits order as the numbers do).
layout(std430, set = 1, binding = 16) buffer ClothSelfPushes {
    ivec4 pushes[];
};

layout(std430, set = 1, binding = 17) buffer ClothSelfShares {
    uint shares[];
};

// Each crossing the search found (an edge through a triangle: edge, triangle), up to CLOTH_SELF_CROSSING_ROOM, read
// back for the CPU's analysis.
layout(std430, set = 1, binding = 18) buffer ClothSelfCrossings {
    uvec2 crossings[];
};

// The CPU's analysis of the crossings a few frames back (GpuClothTangles): each point's part, each part's flags with
// its size above them, and the relations between parts behind their header (x how many, y whether any tangle).
layout(std430, set = 1, binding = 19) readonly buffer ClothTanglePartOf {
    uint partOf[];
};

layout(std430, set = 1, binding = 20) readonly buffer ClothTanglePartFlags {
    uint partFlags[];
};

layout(std430, set = 1, binding = 21) readonly buffer ClothTangleRelations {
    uvec4 tangleHeader;
    uvec4 relations[]; // low part, high part, through (1 when both are inside), unused
};

// The pair search's cell for a point (clothSelfCellSize, cellOf).
ivec3 clothSelfCell(vec3 point, float cellSize) {
    return ivec3(floor(point / cellSize));
}

// The hash table's bucket for a cell (Teschner et al. 2003's primes), the table a power of two.
uint clothSelfBucket(ivec3 cell) {
    uvec3 bits = uvec3(cell);
    return ((bits.x * 73856093u) ^ (bits.y * 19349663u) ^ (bits.z * 83492791u)) & (info.bucketCount - 1u);
}

float clothSelfCellSize(float reach) {
    return max(info.meanRestEdge + 2.0 * reach, 1e-4);
}

// A point's representative's neighbours hold another's (nearInCloth).
bool clothNearInCloth(uint first, uint second) {
    uint one = joined[first];
    uint other = joined[second];
    if (one == other) {
        return true;
    }
    uint low = neighbourStarts[one];
    uint high = neighbourStarts[one + 1u];
    while (low < high) {
        uint middle = (low + high) / 2u;
        if (neighbours[middle] < other) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return low < neighbourStarts[one + 1u] && neighbours[low] == other;
}

// Whether any of the first count points shares a representative with any of the other points (own features).
bool clothShareJoined(uvec4 points, int firstCount) {
    for (int one = 0; one < firstCount; ++one) {
        for (int other = firstCount; other < 4; ++other) {
            if (joined[points[one]] == joined[points[other]]) {
                return true;
            }
        }
    }
    return false;
}

// clothPairExcused: neighbours either across pieces or already within the thickness at rest (restGap).
bool clothPairExcused(uvec4 points, int firstCount, float restGap, float thickness) {
    bool near = false;
    bool acrossPieces = false;
    for (int one = 0; one < firstCount; ++one) {
        for (int other = firstCount; other < 4; ++other) {
            near = near || clothNearInCloth(points[one], points[other]);
            acrossPieces = acrossPieces || (info.pieced != 0u && pieceOf[points[one]] != pieceOf[points[other]]);
        }
    }
    return near && (acrossPieces || restGap < thickness);
}

// The four points' places now (the positions) and at rest.
void clothPairPlaces(uvec4 points, out vec3 now[4], out vec3 rest[4]) {
    for (int index = 0; index < 4; ++index) {
        now[index] = positions[points[index]].xyz;
        rest[index] = restPositions[points[index]].xyz;
    }
}

// The relation between two parts (searching the sorted relations): 0 none, 1 crossed, 2 passed through each other.
uint clothPartsRelated(uint one, uint other) {
    if (one == other) {
        return 0u;
    }
    uvec2 wanted = uvec2(min(one, other), max(one, other));
    uint low = 0u;
    uint high = min(tangleHeader.x, uint(CLOTH_TANGLE_RELATION_ROOM));
    while (low < high) {
        uint middle = (low + high) / 2u;
        uvec2 at = relations[middle].xy;
        if (at.x < wanted.x || (at.x == wanted.x && at.y < wanted.y)) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low < min(tangleHeader.x, uint(CLOTH_TANGLE_RELATION_ROOM)) && all(equal(relations[low].xy, wanted))) {
        return relations[low].z != 0u ? 2u : 1u;
    }
    return 0u;
}

// One part for a side (points from to to), or none when its points are in different parts or the part is not inside.
uint clothSidePart(uvec4 points, int from, int to) {
    uint part = partOf[points[from]];
    for (int index = from + 1; index < to; ++index) {
        if (partOf[points[index]] != part) {
            return CLOTH_NO_STITCH;
        }
    }
    return (partFlags[part] & uint(CLOTH_PART_INSIDE)) != 0u ? part : CLOTH_NO_STITCH;
}

// pairTangle: Through between two inside parts that passed through each other, Free within a part folded through itself
// or between parts one crossed, Apart otherwise; Apart outright with no tangle at all.
uint clothPairTangle(uvec4 points, uint kind) {
    if (tangleHeader.y == 0u) {
        return CLOTH_TANGLE_APART;
    }
    int firstPoints = kind == CLOTH_PAIR_POINT_TRIANGLE ? 1 : 2;
    uint first = clothSidePart(points, 0, firstPoints);
    uint second = clothSidePart(points, firstPoints, 4);
    if (first != CLOTH_NO_STITCH && second != CLOTH_NO_STITCH && clothPartsRelated(first, second) == 2u) {
        return CLOTH_TANGLE_THROUGH;
    }
    for (int index = 0; index < firstPoints; ++index) {
        uint one = partOf[points[index]];
        for (int other = firstPoints; other < 4; ++other) {
            uint another = partOf[points[other]];
            if ((one == another && (partFlags[one] & uint(CLOTH_PART_LOOPED)) != 0u) || clothPartsRelated(one, another) != 0u) {
                return CLOTH_TANGLE_FREE;
            }
        }
    }
    return CLOTH_TANGLE_APART;
}

// Store a pair (appended in whatever order the invocations find them: the contacts and the cut do not depend on it), or
// count it as lost past the room.
void clothStorePair(ClothSelfPair pair) {
    uint slot = atomicAdd(pairState[CLOTH_SELF_PAIR_COUNT], 1u);
    if (slot < info.pairCapacity) {
        pairs[slot] = pair;
    } else {
        atomicAdd(pairState[CLOTH_SELF_PAIRS_LOST], 1u);
    }
}

// Keep a pair found (markPairTangles): with untangling on, its tangle marked and a Through pair's normal turned round.
void clothKeepPair(ClothSelfPair pair) {
    if (ticks[tickIndex].untangle != 0u) {
        pair.tangle = clothPairTangle(pair.points, pair.kind);
        if (pair.tangle == CLOTH_TANGLE_THROUGH) {
            pair.normalGap.xyz = -pair.normalGap.xyz;
        }
    }
    clothStorePair(pair);
}

// A crossing found (an edge through a triangle): recorded for the CPU's analysis, and, where the analysis has the
// edge's ends in different parts, its inside end pushed back across the triangle (clothCrossingPairs): a Through pair
// against it, its normal turned towards the edge's other end.
void clothKeepCrossing(uint edge, uint triangle) {
    uint slot = atomicAdd(pairState[CLOTH_SELF_CROSSING_COUNT], 1u);
    if (slot < uint(CLOTH_SELF_CROSSING_ROOM)) {
        crossings[slot] = uvec2(edge, triangle);
    }

    uvec2 ends = selfEdges[edge];
    uint one = partOf[ends.x];
    uint other = partOf[ends.y];
    if (one == other) {
        return;
    }
    uint oneSize = partFlags[one] >> uint(CLOTH_PART_SIZE_SHIFT);
    uint otherSize = partFlags[other] >> uint(CLOTH_PART_SIZE_SHIFT);
    bool firstInside = oneSize < otherSize || (oneSize == otherSize && one < other);
    uint inside = firstInside ? ends.x : ends.y;
    uint outside = firstInside ? ends.y : ends.x;

    uvec4 corners = selfTriangles[triangle];
    uvec4 points = uvec4(inside, corners.xyz);
    vec3 at[4] = vec3[4](positions[inside].xyz, positions[corners.x].xyz, positions[corners.y].xyz, positions[corners.z].xyz);
    ClothSelfPair pair = clothMakePair(CLOTH_PAIR_POINT_TRIANGLE, points, at);
    pair.normalGap.xyz = clothCrossingNormal(at[1], at[2], at[3], positions[outside].xyz);
    pair.tangle = CLOTH_TANGLE_THROUGH;
    clothStorePair(pair);
}

// How many pairs this search kept.
uint clothPairsKept() {
    return min(pairState[CLOTH_SELF_PAIR_COUNT], info.pairCapacity);
}

#endif
