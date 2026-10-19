#include "render/gpu/clusters.h"

#include "math/mat4.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    // Tiles to cover a screen side, the last one cut off by the edge.
    int tilesToCover(int pixels) {
        return (pixels + clusterTilePixels - 1) / clusterTilePixels;
    }

    // The first pixel past a tile's last one, which is the screen's edge for the last tile of a row or column.
    int tileEnd(int tile, int screenSize) {
        return std::min((tile + 1) * clusterTilePixels, screenSize);
    }

    // A pixel position (from the top-left corner) as normalised device coordinates, -1 to 1. The renderer's viewport
    // flips y, so row 0 is the top of the screen, where y is 1.
    float ndcFromPixelX(float pixel, int screenWidth) {
        return 2.0f * pixel / static_cast<float>(screenWidth) - 1.0f;
    }

    float ndcFromPixelY(float pixel, int screenHeight) {
        return 1.0f - 2.0f * pixel / static_cast<float>(screenHeight);
    }

    Vec3 recordPosition(const float positionRange[4]) {
        return Vec3{positionRange[0], positionRange[1], positionRange[2]};
    }

    void setBit(std::vector<std::uint32_t>& words, int cluster, int word, int bit) {
        words[static_cast<std::size_t>(cluster * clusterWords + word)] |= 1u << bit;
    }

    bool hasBit(const std::vector<std::uint32_t>& words, int cluster, int word, int bit) {
        return (words[static_cast<std::size_t>(cluster * clusterWords + word)] & (1u << bit)) != 0;
    }
}

/*
* The tiles cover the screen, the last ones cut off by its edge. The slices' scale and bias turn the logarithm of a
* depth straight into its slice: slice = slices * log(depth / near) / log(far / near), rearranged so the shader needs one
* log, one multiply and one add. The axes come from the view matrix's rows (right, up, and backwards along the view),
* which are exactly what the projection turns the scene by.
*/
ClusterGrid makeClusterGrid(const Camera& camera, int screenWidth, int screenHeight) {
    if (screenWidth <= 0 || screenHeight <= 0) {
        throw std::invalid_argument("A cluster grid needs a screen of at least one pixel");
    }

    ClusterGrid grid;
    grid.screenWidth = screenWidth;
    grid.screenHeight = screenHeight;
    grid.tilesX = tilesToCover(screenWidth);
    grid.tilesY = tilesToCover(screenHeight);
    grid.slices = clusterDepthSlices;

    grid.nearDepth = camera.getNearPlane();
    grid.farDepth = camera.getFarPlane();
    const float logRatio = std::log(grid.farDepth / grid.nearDepth);
    grid.sliceScale = static_cast<float>(grid.slices) / logRatio;
    grid.sliceBias = -static_cast<float>(grid.slices) * std::log(grid.nearDepth) / logRatio;

    grid.tanHalfHeight = std::tan(camera.getVerticalFov() / 2.0f);
    grid.tanHalfWidth = grid.tanHalfHeight * camera.getAspectRatio();

    const Mat4 view = camera.getViewMatrix();
    grid.right = Vec3{view.values[0][0], view.values[0][1], view.values[0][2]};
    grid.up = Vec3{view.values[1][0], view.values[1][1], view.values[1][2]};
    grid.forward = Vec3{-view.values[2][0], -view.values[2][1], -view.values[2][2]};
    return grid;
}

int clusterCount(const ClusterGrid& grid) {
    return grid.tilesX * grid.tilesY * grid.slices;
}

std::size_t clusterMaskBytes(const ClusterGrid& grid) {
    return clusterMaskBytes(grid.screenWidth, grid.screenHeight);
}

std::size_t clusterMaskBytes(int screenWidth, int screenHeight) {
    const std::size_t clusters = static_cast<std::size_t>(tilesToCover(screenWidth) * tilesToCover(screenHeight) * clusterDepthSlices);
    return clusters * clusterWords * sizeof(std::uint32_t);
}

Vec3 toClusterView(const ClusterGrid& grid, const Vec3& relativePosition) {
    return Vec3{relativePosition.dot(grid.right), relativePosition.dot(grid.up), relativePosition.dot(grid.forward)};
}

// The logarithm only exists for positive depths; anything at or behind the camera belongs to the first slice.
int clusterSlice(float depth, float sliceScale, float sliceBias, int slices) {
    if (!(depth > 0.0f)) {
        return 0;
    }

    const int slice = static_cast<int>(std::floor(std::log(depth) * sliceScale + sliceBias));
    return std::clamp(slice, 0, slices - 1);
}

float sliceStartDepth(int slice, float sliceScale, float sliceBias) {
    return std::exp((static_cast<float>(slice) - sliceBias) / sliceScale);
}

int clusterIndex(int tileX, int tileY, int slice, int tilesX, int tilesY) {
    return (slice * tilesY + tileY) * tilesX + tileX;
}

int clusterOfPixel(const ClusterGrid& grid, float pixelX, float pixelY, float depth) {
    const int tileX = std::clamp(static_cast<int>(std::floor(pixelX / static_cast<float>(clusterTilePixels))), 0, grid.tilesX - 1);
    const int tileY = std::clamp(static_cast<int>(std::floor(pixelY / static_cast<float>(clusterTilePixels))), 0, grid.tilesY - 1);
    return clusterIndex(tileX, tileY, clusterSlice(depth, grid.sliceScale, grid.sliceBias, grid.slices), grid.tilesX, grid.tilesY);
}

/*
* A tile's edges are planes through the camera, so at depth z its sideways extent is the edge's device coordinate times
* z times the half-width at depth 1. Every coordinate of the frustum piece is linear in z along those edges, so its
* extremes are at the eight corners: the two edges of each axis at the slice's two depths.
*/
Aabb clusterBounds(const ClusterGrid& grid, int tileX, int tileY, int slice) {
    const float nearDepth = slice == 0 ? 0.0f : sliceStartDepth(slice, grid.sliceScale, grid.sliceBias);
    const float farDepth = sliceStartDepth(slice + 1, grid.sliceScale, grid.sliceBias);

    const float left = ndcFromPixelX(static_cast<float>(tileX * clusterTilePixels), grid.screenWidth) * grid.tanHalfWidth;
    const float right = ndcFromPixelX(static_cast<float>(tileEnd(tileX, grid.screenWidth)), grid.screenWidth) * grid.tanHalfWidth;
    const float top = ndcFromPixelY(static_cast<float>(tileY * clusterTilePixels), grid.screenHeight) * grid.tanHalfHeight;
    const float bottom = ndcFromPixelY(static_cast<float>(tileEnd(tileY, grid.screenHeight)), grid.screenHeight) * grid.tanHalfHeight;

    const float margin = clusterBoundsMargin * farDepth;
    Aabb box;
    box.minimum = Vec3{std::min(left * nearDepth, left * farDepth), std::min(bottom * nearDepth, bottom * farDepth), nearDepth};
    box.maximum = Vec3{std::max(right * nearDepth, right * farDepth), std::max(top * nearDepth, top * farDepth), farDepth};
    box.minimum = box.minimum - Vec3{margin, margin, margin};
    box.maximum = box.maximum + Vec3{margin, margin, margin};
    return box;
}

bool sphereTouchesBox(const Vec3& centre, float radius, const Aabb& box) {
    const Vec3 closest{
        std::clamp(centre.x, box.minimum.x, box.maximum.x),
        std::clamp(centre.y, box.minimum.y, box.maximum.y),
        std::clamp(centre.z, box.minimum.z, box.maximum.z),
    };
    const Vec3 offset = centre - closest;
    return offset.dot(offset) <= radius * radius;
}

/*
* Split the centre's offset from the apex into its part along the axis and its distance from the axis. Turning that
* picture by the cone's half-angle gives the signed distance from the cone's slanted edge to the centre, measured at
* right angles to the edge: cos(angle) * across - sin(angle) * along. Past the radius, the sphere is outside the cone.
* Near the apex this distance is to the edge's line extended behind the apex, which is why the sphere behind the apex
* is caught by its own test. Within 90 degrees the cone is convex, which the edge distance relies on.
*/
bool coneTouchesSphere(const Vec3& apex, const Vec3& axis, float cosOuter, float range, const Vec3& centre, float radius) {
    const Vec3 offset = centre - apex;
    const float distanceSquared = offset.dot(offset);

    if (cosOuter <= 0.0f) {
        return distanceSquared <= (range + radius) * (range + radius);
    }

    const float along = offset.dot(axis);
    const float across = std::sqrt(std::max(0.0f, distanceSquared - along * along));
    const float sinOuter = std::sqrt(std::max(0.0f, 1.0f - cosOuter * cosOuter));
    const float fromEdge = cosOuter * across - sinOuter * along;

    return fromEdge <= radius && along <= range + radius && along >= -radius;
}

bool ClusterMasks::hasPointLight(int cluster, int light) const {
    return hasBit(words, cluster, light / 32, light % 32);
}

bool ClusterMasks::hasSpotLight(int cluster, int light) const {
    return hasBit(words, cluster, clusterPointWords + light / 32, light % 32);
}

/*
* Every cluster against every light the frame uses, in cluster view space. A spotlight must pass both tests: its range
* sphere against the box (the same as a point light's), and its cone against the box's bounding sphere, which is looser
* than the box but cheap. Both are conservative, so a listed light may still give a pixel nothing, but a light that
* reaches a pixel is always listed.
*/
ClusterMasks buildClusterMasks(const ClusterGrid& grid, const GpuLights& lights) {
    ClusterMasks masks;
    masks.words.assign(static_cast<std::size_t>(clusterCount(grid) * clusterWords), 0u);

    const int pointCount = std::min({lights.counts[1], static_cast<int>(lights.points.size()), maxPointLights});
    const int spotCount = std::min({lights.counts[2], static_cast<int>(lights.spots.size()), maxSpotLights});

    for (int slice = 0; slice < grid.slices; ++slice) {
        for (int tileY = 0; tileY < grid.tilesY; ++tileY) {
            for (int tileX = 0; tileX < grid.tilesX; ++tileX) {
                const int cluster = clusterIndex(tileX, tileY, slice, grid.tilesX, grid.tilesY);
                const Aabb box = clusterBounds(grid, tileX, tileY, slice);
                const Vec3 boxCentre = box.centre();
                const float boxRadius = box.size().length() * 0.5f;

                for (int light = 0; light < pointCount; ++light) {
                    const GpuPointLight& record = lights.points[static_cast<std::size_t>(light)];
                    const Vec3 position = toClusterView(grid, recordPosition(record.positionRange));

                    if (sphereTouchesBox(position, record.positionRange[3], box)) {
                        setBit(masks.words, cluster, light / 32, light % 32);
                    }
                }

                for (int light = 0; light < spotCount; ++light) {
                    const GpuSpotLight& record = lights.spots[static_cast<std::size_t>(light)];
                    const Vec3 position = toClusterView(grid, recordPosition(record.positionRange));
                    const Vec3 axis = toClusterView(grid, recordPosition(record.directionCosOuter));
                    const float range = record.positionRange[3];

                    if (sphereTouchesBox(position, range, box) &&
                        coneTouchesSphere(position, axis, record.directionCosOuter[3], range, boxCentre, boxRadius)) {
                        setBit(masks.words, cluster, clusterPointWords + light / 32, light % 32);
                    }
                }
            }
        }
    }

    return masks;
}
