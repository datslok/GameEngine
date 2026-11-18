#include "render/gpu/occlusion_culling.h"
#include "math/vec4.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

bool PixelRect::empty() const {
    return minX > maxX || minY > maxY;
}

/*
* A perspective projection's depth row, after toGpuDepthRange, gives clip z = g22 z + g23 and clip w = -z for a point at
* view z (negative in front), so depth = -g22 + g23 / distance: two numbers stand for the whole row.
*/
OcclusionView makeOcclusionView(const Mat4& relativeView, const Mat4& gpuProjection, float nearPlane, RenderSize size) {
    if (gpuProjection.values[3][2] != -1.0f || gpuProjection.values[3][3] != 0.0f) {
        throw std::invalid_argument("Occlusion culling needs a perspective projection");
    }

    OcclusionView view;
    view.view = relativeView;
    view.scaleX = gpuProjection.values[0][0];
    view.scaleY = gpuProjection.values[1][1];
    view.nearPlane = nearPlane;
    view.depthConstant = -gpuProjection.values[2][2];
    view.depthPerInverseDistance = gpuProjection.values[2][3];
    view.size = size;
    return view;
}

/*
* Mara & McGuire 2013 (as in Kapoulkine's niagara): in the plane of the view axis and x (then y), the two lines from the
* eye touching the sphere's circle give its extent exactly, x / distance at the tangent points = (v cx -+ r d) / (v d +- r cx)
* with v = sqrt(cx^2 + d^2 - r^2). Both denominators are positive while the sphere is past the near plane. Through the
* lens scales these are the edges in normalized device coordinates, then pixels as TAA's ndcToPixel counts them (y down).
* Clamped in floats before rounding, so a sphere far off to the side never overflows an int; clamped further out than
* the margin reaches, so a sphere off the screen stays off it once widened.
*/
std::optional<PixelRect> sphereRectFromView(const Vec3& viewCentre, float radius, float scaleX, float scaleY, float nearPlane, RenderSize size) {
    const float distance = -viewCentre.z;

    if (distance - radius <= nearPlane) {
        return std::nullopt;
    }

    const float lessRadius = distance * distance - radius * radius;
    const float alongX = std::sqrt(viewCentre.x * viewCentre.x + lessRadius);
    const float alongY = std::sqrt(viewCentre.y * viewCentre.y + lessRadius);
    const float lowX = (alongX * viewCentre.x - radius * distance) / (alongX * distance + radius * viewCentre.x);
    const float highX = (alongX * viewCentre.x + radius * distance) / (alongX * distance - radius * viewCentre.x);
    const float lowY = (alongY * viewCentre.y - radius * distance) / (alongY * distance + radius * viewCentre.y);
    const float highY = (alongY * viewCentre.y + radius * distance) / (alongY * distance - radius * viewCentre.y);

    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    const float beyond = 2.0f + static_cast<float>(occlusionRectMargin);
    const auto onScreen = [beyond](float pixel, float extent) { return std::clamp(pixel, -beyond, extent + beyond); };
    const float left = onScreen((lowX * scaleX + 1.0f) * 0.5f * width, width);
    const float right = onScreen((highX * scaleX + 1.0f) * 0.5f * width, width);
    const float top = onScreen((1.0f - highY * scaleY) * 0.5f * height, height);
    const float bottom = onScreen((1.0f - lowY * scaleY) * 0.5f * height, height);

    PixelRect rect;
    rect.minX = std::max(static_cast<int>(std::floor(left)) - occlusionRectMargin, 0);
    rect.minY = std::max(static_cast<int>(std::floor(top)) - occlusionRectMargin, 0);
    rect.maxX = std::min(static_cast<int>(std::floor(right)) + occlusionRectMargin, static_cast<int>(size.width) - 1);
    rect.maxY = std::min(static_cast<int>(std::floor(bottom)) + occlusionRectMargin, static_cast<int>(size.height) - 1);
    return rect;
}

// Depth depends only on the distance along the view axis, so the sphere's nearest point there is its nearest depth.
float nearestDepthFromView(float viewZ, float radius, float depthConstant, float depthPerInverseDistance) {
    return depthConstant + depthPerInverseDistance / (-viewZ - radius);
}

namespace {
    Vec3 inView(const OcclusionView& view, const Vec3& relativeCentre) {
        const Vec4 turned = view.view * Vec4{relativeCentre.x, relativeCentre.y, relativeCentre.z, 1.0f};
        return Vec3{turned.x, turned.y, turned.z};
    }
}

std::optional<PixelRect> sphereRect(const OcclusionView& view, const Vec3& relativeCentre, float radius) {
    return sphereRectFromView(inView(view, relativeCentre), radius, view.scaleX, view.scaleY, view.nearPlane, view.size);
}

float sphereNearestDepth(const OcclusionView& view, const Vec3& relativeCentre, float radius) {
    return nearestDepthFromView(inView(view, relativeCentre).z, radius, view.depthConstant, view.depthPerInverseDistance);
}

/*
* A run of n pixels touches at most two cells of 2^k pixels when n <= 2^k, wherever it starts; one level finer it would
* touch at least two whenever n > 2^(k-1), so this is the finest level that answers with four reads.
*/
std::uint32_t occlusionLevel(const PixelRect& rect, std::uint32_t levels) {
    const int longest = std::max(rect.maxX - rect.minX + 1, rect.maxY - rect.minY + 1);
    std::uint32_t level = 0;

    while (level + 1 < levels && (1 << level) < longest) {
        ++level;
    }

    return level;
}

/*
* The cells under the rectangle at its level: a texel's cell there is pyramidCell's (halved level times, the far edge's
* leftovers in the last cell), which covers exactly the texels the pyramid reduced into it. Hidden only when the
* object's nearest point is strictly farther than the farthest thing in every cell.
*/
bool hiddenInPyramid(const DepthPyramid& pyramid, const PixelRect& rect, float nearestDepth) {
    if (rect.empty()) {
        return false;
    }

    const std::uint32_t level = occlusionLevel(rect, static_cast<std::uint32_t>(pyramid.levels.size()));
    const int width = static_cast<int>(pyramid.sizes[level].width);
    const int height = static_cast<int>(pyramid.sizes[level].height);
    float farthest = gpuNearDepth;

    for (int y = pyramidCell(rect.minY, level, height); y <= pyramidCell(rect.maxY, level, height); ++y) {
        for (int x = pyramidCell(rect.minX, level, width); x <= pyramidCell(rect.maxX, level, width); ++x) {
            farthest = fartherDepth(farthest, pyramid.at(level, x, y));
        }
    }

    return nearestDepth < farthest;
}

bool sphereHidden(const OcclusionView& view, const DepthPyramid& pyramid, const Vec3& relativeCentre, float radius) {
    if (radius < 0.0f) {
        return false;
    }

    const std::optional<PixelRect> rect = sphereRect(view, relativeCentre, radius);

    if (!rect || rect->empty()) {
        return false;
    }

    return hiddenInPyramid(pyramid, *rect, sphereNearestDepth(view, relativeCentre, radius));
}

OcclusionCounts unpackOcclusionCounts(const GpuOcclusionCounts& counts) {
    return OcclusionCounts{
        .tested = counts.tested,
        .hidden = counts.hidden,
        .testedTriangles = counts.testedTriangles,
        .hiddenTriangles = counts.hiddenTriangles,
    };
}

std::vector<GpuOcclusionBatch> packOcclusionBatches(const std::vector<DrawBatch>& batches,
                                                    const std::function<std::uint32_t(MeshHandle)>& trianglesOf) {
    std::vector<GpuOcclusionBatch> packed;
    packed.reserve(batches.size());

    for (const DrawBatch& batch : batches) {
        packed.push_back(GpuOcclusionBatch{batch.firstInstance, batch.instanceCount, trianglesOf(batch.mesh), 0});
    }

    return packed;
}

GpuOcclusionPass packOcclusionPass(const OcclusionView& view, const Vec3& cameraOrigin, std::uint32_t levels, std::uint32_t batches) {
    GpuOcclusionPass pass{};
    writeColumnMajor(view.view, pass.view);
    pass.lens[0] = view.scaleX;
    pass.lens[1] = view.scaleY;
    pass.lens[2] = view.nearPlane;
    pass.lens[3] = view.depthConstant;
    pass.origin[0] = cameraOrigin.x;
    pass.origin[1] = cameraOrigin.y;
    pass.origin[2] = cameraOrigin.z;
    pass.origin[3] = view.depthPerInverseDistance;
    pass.sizes[0] = static_cast<std::int32_t>(view.size.width);
    pass.sizes[1] = static_cast<std::int32_t>(view.size.height);
    pass.sizes[2] = static_cast<std::int32_t>(levels);
    pass.sizes[3] = static_cast<std::int32_t>(batches);
    return pass;
}
