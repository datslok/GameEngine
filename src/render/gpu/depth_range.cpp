#include "render/gpu/depth_range.h"

#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/soft_shadow.h"

#include <algorithm>
#include <bit>

bool isSurfaceDepth(float depth) {
    return depth > gpuFarDepth;
}

ScreenDepthRange includeDepth(ScreenDepthRange range, float depth) {
    if (!isSurfaceDepth(depth)) {
        return range;
    }

    const std::uint32_t bits = std::bit_cast<std::uint32_t>(depth);
    range.farthestBits = std::min(range.farthestBits, bits);
    range.nearestBits = std::max(range.nearestBits, bits);
    return range;
}

ScreenDepthRange combineDepthRanges(const ScreenDepthRange& a, const ScreenDepthRange& b) {
    return ScreenDepthRange{std::min(a.farthestBits, b.farthestBits), std::max(a.nearestBits, b.nearestBits)};
}

bool hasSurfaces(const ScreenDepthRange& range) {
    return range.nearestBits != 0;
}

float farthestSurfaceDepth(const ScreenDepthRange& range) {
    return std::bit_cast<float>(range.farthestBits);
}

float nearestSurfaceDepth(const ScreenDepthRange& range) {
    return std::bit_cast<float>(range.nearestBits);
}

// The camera's projection is a perspective one turned to reversed-Z like a spotlight's shadow view, so the same
// formula turns its depth back into distance along the view (linearShadowDepth).
std::optional<float> farthestSurfaceDistance(const ScreenDepthRange& range, float nearPlane, float farPlane) {
    if (!hasSurfaces(range)) {
        return std::nullopt;
    }

    return linearShadowDepth(farthestSurfaceDepth(range), nearPlane, farPlane);
}
