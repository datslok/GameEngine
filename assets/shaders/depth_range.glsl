#ifndef DEPTH_RANGE_GLSL
#define DEPTH_RANGE_GLSL

// The nearest and farthest surface depth on screen, for fitting the sun's cascades (render/gpu/depth_range.h mirrors
// it). Reversed-Z: the farthest surface has the smallest depth, and the far depth itself is sky. Positive floats' bits
// sort as unsigned integers in the numbers' order, so both ends are kept as bits and combined with integer min and max.

#include "render/gpu/depth_shared.h"
#include "render/gpu/depth_range_shared.h"

struct DepthRange {
    uint farthestBits;
    uint nearestBits;
};

DepthRange emptyDepthRange() {
    return DepthRange(DEPTH_RANGE_NO_SURFACE_FARTHEST, 0u);
}

bool isSurfaceDepth(float depth) {
    return depth > GPU_FAR_DEPTH;
}

DepthRange includeDepth(DepthRange range, float depth) {
    if (!isSurfaceDepth(depth)) {
        return range;
    }

    uint bits = floatBitsToUint(depth);
    return DepthRange(min(range.farthestBits, bits), max(range.nearestBits, bits));
}

#endif
