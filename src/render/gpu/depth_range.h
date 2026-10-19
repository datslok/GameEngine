#pragma once

#include "render/gpu/depth_range_shared.h"

#include <cstdint>
#include <optional>

/*
* How far away the surfaces on screen are, for sample distribution shadow maps (Lauritzen 2011): the sun's cascades are
* fitted to the farthest surface the camera actually sees instead of always reaching the shadow distance, so in a room
* every texel lands on something visible. A compute pass (depth_range.comp) reads the depth image after the main pass
* and keeps the nearest and farthest surface depth; this is its CPU mirror, and how the result is read.
*
* Depth is reversed-Z (near 1, far 0), so the farthest surface has the smallest depth, and pixels left at the far depth
* (0) are sky, never a surface. Depths are positive floats, whose bits read as unsigned integers sort in the same order
* as the numbers (the exponent sits above the mantissa), so the GPU keeps both ends with integer atomics on the bits.
*/

inline constexpr std::uint32_t depthRangeGroupSide = DEPTH_RANGE_GROUP_SIDE;
inline constexpr std::uint32_t depthRangeTexelsPerSide = DEPTH_RANGE_TEXELS_PER_SIDE;
inline constexpr std::uint32_t noSurfaceFarthestBits = DEPTH_RANGE_NO_SURFACE_FARTHEST;

// The bits of the farthest (smallest) and nearest (largest) surface depth seen; as made, no surface at all. The same
// two words the pass writes, in the same order.
struct ScreenDepthRange {
    std::uint32_t farthestBits = noSurfaceFarthestBits;
    std::uint32_t nearestBits = 0;
};

static_assert(sizeof(ScreenDepthRange) == 8, "ScreenDepthRange must match the depth_range.comp result");

// Whether a stored depth is a surface rather than the sky (the far depth, where nothing was drawn).
bool isSurfaceDepth(float depth);

// The range widened to hold this depth, if it is a surface.
ScreenDepthRange includeDepth(ScreenDepthRange range, float depth);

// The range of everything in both.
ScreenDepthRange combineDepthRanges(const ScreenDepthRange& a, const ScreenDepthRange& b);

bool hasSurfaces(const ScreenDepthRange& range);

// The two ends as depths. Only for a range with surfaces.
float farthestSurfaceDepth(const ScreenDepthRange& range);
float nearestSurfaceDepth(const ScreenDepthRange& range);

// The farthest surface's distance along the view, for the camera's near and far planes it was drawn with; none when the
// range holds no surface.
std::optional<float> farthestSurfaceDistance(const ScreenDepthRange& range, float nearPlane, float farPlane);
