// What every self-collision pass (cloth_self_*.comp) binds besides the cloth passes' set 0 (render/vulkan/vulkan_cloth.h,
// GpuClothSelf): the triangles and edges, who is whose neighbour, the rest shape and pieces, the pair search's hash
// table, the pairs found, and the contacts' sums and the cut's shares per point.

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

// Keep a pair found: appended in whatever order the invocations find them (the contacts and the cut do not depend on
// it), or counted as lost past the room.
void clothKeepPair(ClothSelfPair pair) {
    uint slot = atomicAdd(pairState[CLOTH_SELF_PAIR_COUNT], 1u);
    if (slot < info.pairCapacity) {
        pairs[slot] = pair;
    } else {
        atomicAdd(pairState[CLOTH_SELF_PAIRS_LOST], 1u);
    }
}

// How many pairs this search kept.
uint clothPairsKept() {
    return min(pairState[CLOTH_SELF_PAIR_COUNT], info.pairCapacity);
}

#endif
