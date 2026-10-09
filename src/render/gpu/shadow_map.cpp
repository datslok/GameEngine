#include "render/gpu/shadow_map.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/light_uniforms.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace {
    // Soft shadows read the depth texels around a point (3x3), and filtering touches one more. Views are made slightly
    // wider than needed, so every point they are meant to cover stays this many texels inside its tile.
    constexpr float edgeMarginTexels = 2.0f;

    // How far behind a directional light's box (towards the light) objects still cast shadows into it.
    constexpr float directionalCasterReach = 50.0f;

    // Shadow acne fix: points are moved off their surface along the normal by this many texels. It is the only bias (the
    // shadow pass has no hardware depth bias), so it is a little larger than usual: 1.5 left faint stripes on faces the
    // light grazes. Measured in texels, it shrinks and grows with each tile's size.
    constexpr float normalOffsetTexels = 2.5f;

    // Even with every light at its largest size cut down to the smallest (directional lights never shrink), all tiles fit.
    constexpr std::uint32_t areaInSmallestTiles(std::uint32_t size) {
        return (size / smallestShadowTileSize) * (size / smallestShadowTileSize);
    }

    constexpr std::uint32_t atlasCapacity = (shadowAtlasWidth / smallestShadowTileSize) * (shadowAtlasHeight / smallestShadowTileSize);

    static_assert(maxShadowedDirectionalLights * areaInSmallestTiles(largestShadowTileSize) +
                  maxShadowedSpotLights + maxShadowedPointLights * 6 <= atlasCapacity,
                  "the smallest shadow tiles for every shadowed light must fit in the atlas");

    // Widen a view (given as tan of its half-angle) so its contents stay edgeMarginTexels from the edge of its tile.
    float withEdgeMargin(float halfWidth, std::uint32_t tileSize) {
        return halfWidth / (1.0f - 2.0f * edgeMarginTexels / static_cast<float>(tileSize));
    }

    // A square perspective camera's projection, in the GPU's depth range.
    Mat4 squarePerspective(float halfWidth, float farPlane) {
        const float fieldOfView = 2.0f * std::atan(halfWidth);
        return toGpuDepthRange(Mat4::perspective(fieldOfView, 1.0f, shadowNearPlane, std::max(farPlane, 2.0f * shadowNearPlane)));
    }

    // Any up direction works for a shadow view, as long as it is not parallel to the way the view looks.
    Vec3 upFor(const Vec3& forward) {
        return std::abs(forward.normalized().y) > 0.99f ? Vec3{0.0f, 0.0f, -1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    }

    // One texel of a perspective view covers 2 * distance * halfWidth / tileSize of world, so the offset is stored per unit of distance.
    float perspectiveOffset(float halfWidth, std::uint32_t tileSize) {
        return normalOffsetTexels * 2.0f * halfWidth / static_cast<float>(tileSize);
    }

    // A spotlight's cone as tan of its half-angle. Cones wider than about 160 degrees are clamped, because a perspective
    // view cannot reach 180.
    float spotHalfWidth(float outerAngle) {
        return std::tan(std::clamp(outerAngle, 0.01f, 1.4f));
    }

    enum class ShadowKind {
        Directional,
        Spot,
        Point
    };

    // One light's claim on the atlas: tileCount squares of one size, and where they were placed.
    struct ShadowRequest {
        ShadowKind kind;
        std::size_t slot;
        int tileCount;
        std::uint32_t size;
        std::vector<ShadowTile> tiles;
    };

    /*
    * While the requests need more room than the atlas has, halve the tiles of the least important light that can still
    * shrink: requests are in order of importance (directional lights, then spotlights and point lights in slot order),
    * so the last ones lose detail first. Directional boxes keep their size, since their texel snapping is built on it.
    */
    void shrinkToFit(std::vector<ShadowRequest>& requests) {
        std::uint32_t used = 0;

        for (const ShadowRequest& request : requests) {
            used += static_cast<std::uint32_t>(request.tileCount) * areaInSmallestTiles(request.size);
        }

        while (used > atlasCapacity) {
            const auto shrinkable = std::find_if(requests.rbegin(), requests.rend(), [](const ShadowRequest& request) {
                return request.kind != ShadowKind::Directional && request.size > smallestShadowTileSize;
            });

            // The static_assert above guarantees everything fits at the smallest size.
            if (shrinkable == requests.rend()) {
                throw std::logic_error("Shadow tiles do not fit in the atlas");
            }

            const std::uint32_t saved = areaInSmallestTiles(shrinkable->size) - areaInSmallestTiles(shrinkable->size / 2);
            used -= static_cast<std::uint32_t>(shrinkable->tileCount) * saved;
            shrinkable->size /= 2;
        }
    }

    /*
    * Places squares in the atlas, which starts as a grid of the largest tiles. A request takes the smallest free square
    * that fits, splitting a bigger one into four quarters as often as needed (a quadtree). Squares must be asked for
    * largest first: then the quarters of a split square are always used up before another is split, so nothing is
    * wasted and everything fits whenever the total area does.
    */
    class AtlasPacker {
    public:
        AtlasPacker() {
            for (std::uint32_t y = 0; y < shadowAtlasHeight; y += largestShadowTileSize) {
                for (std::uint32_t x = 0; x < shadowAtlasWidth; x += largestShadowTileSize) {
                    freeSquares.push_back(ShadowTile{Mat4::identity(), x, y, largestShadowTileSize});
                }
            }
        }

        ShadowTile place(std::uint32_t size) {
            std::size_t best = freeSquares.size();

            for (std::size_t i = 0; i < freeSquares.size(); ++i) {
                if (freeSquares[i].size >= size && (best == freeSquares.size() || freeSquares[i].size < freeSquares[best].size)) {
                    best = i;
                }
            }

            if (best == freeSquares.size()) {
                throw std::logic_error("Shadow atlas is full");
            }

            ShadowTile square = freeSquares[best];
            freeSquares.erase(freeSquares.begin() + static_cast<std::ptrdiff_t>(best));

            // Keep the top-left quarter and free the other three, until the square is the size asked for.
            while (square.size > size) {
                square.size /= 2;
                freeSquares.push_back(ShadowTile{Mat4::identity(), square.x + square.size, square.y, square.size});
                freeSquares.push_back(ShadowTile{Mat4::identity(), square.x, square.y + square.size, square.size});
                freeSquares.push_back(ShadowTile{Mat4::identity(), square.x + square.size, square.y + square.size, square.size});
            }

            return square;
        }

    private:
        std::vector<ShadowTile> freeSquares;
    };

    void addTile(ShadowPlan& plan, ShadowTile tile, const Mat4& matrix, float offsetPerDistance, float fixedOffset) {
        tile.matrix = matrix;
        plan.tiles.push_back(tile);

        ShadowTileData data{};

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                data.matrix[column * 4 + row] = matrix.values[row][column];
            }
        }

        data.offset[0] = offsetPerDistance;
        data.offset[1] = fixedOffset;

        data.rect[0] = static_cast<float>(tile.x) / static_cast<float>(shadowAtlasWidth);
        data.rect[1] = static_cast<float>(tile.y) / static_cast<float>(shadowAtlasHeight);
        data.rect[2] = static_cast<float>(tile.size) / static_cast<float>(shadowAtlasWidth);
        data.rect[3] = static_cast<float>(tile.size) / static_cast<float>(shadowAtlasHeight);
        plan.tileData.push_back(data);
    }
}

/*
* A view of half-width h reaching range r is 2 * h * r across at its far end. Rounding up to a power of two keeps every
* tile a whole quarter, sixteenth, ... of the largest one, which is what lets the atlas pack them without gaps.
*/
std::uint32_t shadowTileSizeFor(float halfWidth, float range) {
    const float texelsNeeded = 2.0f * halfWidth * range / shadowTexelTarget;
    std::uint32_t size = smallestShadowTileSize;

    while (static_cast<float>(size) < texelsNeeded && size < largestShadowTileSize) {
        size *= 2;
    }

    return size;
}

/*
* The cube face a direction pierces is the one for its largest component, so ties at edges and corners go to the earlier face.
*/
int pointShadowFace(const Vec3& fromLight) {
    const float x = std::abs(fromLight.x);
    const float y = std::abs(fromLight.y);
    const float z = std::abs(fromLight.z);

    if (x >= y && x >= z) {
        return fromLight.x > 0.0f ? 0 : 1;
    }

    if (y >= z) {
        return fromLight.y > 0.0f ? 2 : 3;
    }

    return fromLight.z > 0.0f ? 4 : 5;
}

/*
* A square camera at the light, a little wider than 90 degrees. The shader looks points up with these same matrices,
* so the up direction only needs to be consistent.
*/
Mat4 pointShadowFaceMatrix(const Vec3& lightPosition, int face, float farPlane, std::uint32_t tileSize) {
    static const Vec3 forwards[6] = {
        Vec3{1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f},
        Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f},
        Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f}
    };

    const Vec3 forward = forwards[face];
    const Mat4 view = Mat4::lookAt(lightPosition, lightPosition + forward, upFor(forward));

    return squarePerspective(withEdgeMargin(1.0f, tileSize), farPlane) * view;
}

/*
* A square camera at the light looking down the beam. Its half-width reaches the outer cone, since nothing outside it is lit.
*/
Mat4 spotShadowMatrix(const Vec3& lightPosition, const Vec3& direction, float outerAngle, float range, std::uint32_t tileSize) {
    const Mat4 view = Mat4::lookAt(lightPosition, lightPosition + direction.normalized(), upFor(direction));

    return squarePerspective(withEdgeMargin(spotHalfWidth(outerAngle), tileSize), range) * view;
}

/*
* Parallel rays need no perspective: a box, looking along the light, centred halfway along the stretch of the camera's
* view that gets shadows. The box reaches back towards the light so tall things outside the view still cast into it.
* Its centre is snapped to whole shadow texels, measured across the light's direction: as the camera moves, the box
* jumps a whole texel at a time, so every shadow edge falls on the same texels instead of crawling and shimmering.
* Directional lights always get the largest tile.
*/
Mat4 directionalShadowMatrix(const Vec3& direction, const Camera& camera) {
    const Vec3 along = direction.lengthSquared() > 1e-12f ? direction.normalized() : Vec3{0.0f, -1.0f, 0.0f};
    const float radius = directionalShadowDistance * 0.5f;
    const Vec3 centre = camera.getPosition() + camera.getForward() * radius;

    // The light's orientation only: a view at the origin looking along the light.
    const Mat4 rotation = Mat4::lookAt(Vec3{0.0f, 0.0f, 0.0f}, along, upFor(along));
    const Vec4 centreInLight = rotation * Vec4{centre.x, centre.y, centre.z, 1.0f};

    const float texel = 2.0f * radius / static_cast<float>(largestShadowTileSize);
    const float snappedX = std::round(centreInLight.x / texel) * texel;
    const float snappedY = std::round(centreInLight.y / texel) * texel;

    // The view looks down -z, so a point's distance along the light is -z.
    const float centreDistance = -centreInLight.z;
    const Mat4 box = Mat4::orthographic(
        snappedX - radius, snappedX + radius,
        snappedY - radius, snappedY + radius,
        centreDistance - radius - directionalCasterReach, centreDistance + radius
    );

    return toGpuDepthRange(box) * rotation;
}

/*
* Each shadow-casting light, up to the limit for its kind and in the shader's slot order, asks for tiles sized by its
* reach; a light whose castsShadows is off keeps its slot but gets none. If the atlas would overflow, the least
* important lights' tiles shrink. Tiles are then placed largest first (so they pack without gaps), but numbered in
* request order, so the numbering does not depend on sizes.
*/
ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera) {
    const FrameLighting selected = selectDrawableLights(lighting);
    ShadowPlan plan{};

    std::fill(&plan.uniforms.pointTiles[0][0], &plan.uniforms.pointTiles[0][0] + 64, -1);
    std::fill(&plan.uniforms.spotTiles[0][0], &plan.uniforms.spotTiles[0][0] + 8, -1);
    std::fill(plan.uniforms.directionalTiles, plan.uniforms.directionalTiles + 4, -1);

    plan.uniforms.atlasTexel[0] = 1.0f / static_cast<float>(shadowAtlasWidth);
    plan.uniforms.atlasTexel[1] = 1.0f / static_cast<float>(shadowAtlasHeight);

    std::vector<ShadowRequest> requests;

    for (std::size_t slot = 0; slot < selected.directionalLights.size(); ++slot) {
        if (selected.directionalLights[slot].castsShadows && static_cast<int>(slot) < maxShadowedDirectionalLights) {
            requests.push_back(ShadowRequest{ShadowKind::Directional, slot, 1, largestShadowTileSize, {}});
        }
    }

    int shadowedSpots = 0;

    for (std::size_t slot = 0; slot < selected.spotLights.size() && shadowedSpots < maxShadowedSpotLights; ++slot) {
        const SpotLight& light = selected.spotLights[slot].light;

        if (light.castsShadows) {
            requests.push_back(ShadowRequest{ShadowKind::Spot, slot, 1, shadowTileSizeFor(spotHalfWidth(light.outerAngle), light.range), {}});
            ++shadowedSpots;
        }
    }

    int shadowedPoints = 0;

    for (std::size_t slot = 0; slot < selected.pointLights.size() && shadowedPoints < maxShadowedPointLights; ++slot) {
        const PointLight& light = selected.pointLights[slot].light;

        if (light.castsShadows) {
            requests.push_back(ShadowRequest{ShadowKind::Point, slot, 6, shadowTileSizeFor(1.0f, light.range), {}});
            ++shadowedPoints;
        }
    }

    shrinkToFit(requests);

    // Place the biggest first; ties keep request order.
    std::vector<std::size_t> placingOrder(requests.size());
    std::iota(placingOrder.begin(), placingOrder.end(), std::size_t{0});
    std::stable_sort(placingOrder.begin(), placingOrder.end(), [&](std::size_t a, std::size_t b) {
        return requests[a].size > requests[b].size;
    });

    AtlasPacker packer;

    for (std::size_t index : placingOrder) {
        for (int tile = 0; tile < requests[index].tileCount; ++tile) {
            requests[index].tiles.push_back(packer.place(requests[index].size));
        }
    }

    for (const ShadowRequest& request : requests) {
        const std::int32_t firstTile = static_cast<std::int32_t>(plan.tiles.size());

        if (request.kind == ShadowKind::Directional) {
            // A box texel is the same size everywhere, so its offset is fixed rather than growing with distance.
            const DirectionalLight& light = selected.directionalLights[request.slot];
            plan.uniforms.directionalTiles[request.slot] = firstTile;
            addTile(plan, request.tiles[0], directionalShadowMatrix(light.direction, camera), 0.0f,
                    normalOffsetTexels * directionalShadowDistance / static_cast<float>(largestShadowTileSize));
        }
        else if (request.kind == ShadowKind::Spot) {
            const PlacedSpotLight& placed = selected.spotLights[request.slot];
            plan.uniforms.spotTiles[request.slot / 4][request.slot % 4] = firstTile;
            addTile(plan, request.tiles[0],
                    spotShadowMatrix(placed.position, placed.light.direction, placed.light.outerAngle, placed.light.range, request.size),
                    perspectiveOffset(withEdgeMargin(spotHalfWidth(placed.light.outerAngle), request.size), request.size), 0.0f);
        }
        else {
            const PlacedPointLight& placed = selected.pointLights[request.slot];
            plan.uniforms.pointTiles[request.slot / 4][request.slot % 4] = firstTile;

            for (int face = 0; face < 6; ++face) {
                addTile(plan, request.tiles[static_cast<std::size_t>(face)],
                        pointShadowFaceMatrix(placed.position, face, placed.light.range, request.size),
                        perspectiveOffset(withEdgeMargin(1.0f, request.size), request.size), 0.0f);
            }
        }
    }

    return plan;
}
