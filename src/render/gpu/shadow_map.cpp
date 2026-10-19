#include "render/gpu/shadow_map.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/light_uniforms.h"
#include "math/frustum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
    // Soft shadows read the depth texels around a point (3x3), and filtering touches one more. Views are made slightly
    // wider than needed, so every point they are meant to cover stays this many texels inside its tile.
    constexpr float edgeMarginTexels = 2.0f;

    // Even with every light at its largest size cut down to the smallest (a sun's cascades only to the standard size), all
    // tiles fit.
    constexpr std::uint32_t areaInSmallestTiles(std::uint32_t size) {
        return (size / smallestShadowTileSize) * (size / smallestShadowTileSize);
    }

    constexpr std::uint32_t capacityOf(const ShadowAtlasSize& atlas) {
        return (atlas.width / smallestShadowTileSize) * (atlas.height / smallestShadowTileSize);
    }

    // The atlas for cascades of the given size: the usual one while they fit in a quarter of it (2048 and below), else
    // one grown by the rows their extra room takes, with squares as large as a cascade.
    constexpr ShadowAtlasSize atlasForCascades(std::uint32_t cascadeSize) {
        if (cascadeSize <= largestShadowTileSize) {
            return ShadowAtlasSize{};
        }

        const std::uint32_t extraRows = maxShadowCascades * cascadeSize * cascadeSize / shadowAtlasWidth;
        return ShadowAtlasSize{shadowAtlasWidth, shadowAtlasHeight + extraRows, cascadeSize};
    }

    constexpr std::uint32_t otherShadowsAtTheirSmallest = maxShadowedSpotLights + maxShadowedPointLights * 6;

    static_assert(maxShadowedDirectionalLights * maxShadowCascades * areaInSmallestTiles(standardShadowTileSize) +
                  otherShadowsAtTheirSmallest <= capacityOf(ShadowAtlasSize{}),
                  "the smallest shadow tiles for every shadowed light must fit in the atlas");

    static_assert(maxShadowedDirectionalLights * maxShadowCascades * areaInSmallestTiles(4096) +
                  otherShadowsAtTheirSmallest <= capacityOf(atlasForCascades(4096)),
                  "Ultra cascades must fit at full size beside every other shadow at its smallest");

    // Widen a view (given as tan of its half-angle) so its contents stay edgeMarginTexels from the edge of its tile.
    float withEdgeMargin(float halfWidth, std::uint32_t tileSize) {
        return halfWidth / (1.0f - 2.0f * edgeMarginTexels / static_cast<float>(tileSize));
    }

    // A square perspective camera's projection, in the GPU's depth range.
    Mat4 squarePerspective(float halfWidth, float farPlane) {
        const float fieldOfView = 2.0f * std::atan(halfWidth);
        return toGpuDepthRange(Mat4::perspective(fieldOfView, 1.0f, shadowNearPlane, std::max(farPlane, 2.0f * shadowNearPlane)));
    }

    // One texel of a perspective view covers 2 * distance * halfWidth / tileSize of world, so the offset is stored per unit of distance.
    float perspectiveOffset(float halfWidth, std::uint32_t tileSize, float normalOffsetTexels) {
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

    // One light's claim on the atlas: tileCount squares of one size, and where they were placed. For a point light,
    // facesInView says which of its six faces get one of them.
    struct ShadowRequest {
        ShadowKind kind;
        std::size_t slot;
        int tileCount;
        std::uint32_t size;
        std::vector<ShadowTile> tiles;
        std::array<bool, 6> facesInView{};
        Entity entity{};
    };

    /*
    * A cube face can only shadow a pixel on screen if its view overlaps the camera's. Tested with the face's widest
    * version (the margin around a face grows as its tile shrinks), so a point the shader could look up in it is never
    * left out.
    */
    std::array<bool, 6> pointFacesInView(const Vec3& lightPosition, float range, const Frustum& cameraView) {
        std::array<bool, 6> inView{};

        for (int face = 0; face < 6; ++face) {
            const Mat4 widest = pointShadowFaceMatrix(lightPosition, face, range, smallestShadowTileSize);
            inView[static_cast<std::size_t>(face)] = Frustum::fromClipMatrix(widest, ClipDepth::OneToZero).mayIntersect(cameraView);
        }

        return inView;
    }

    /*
    * While the requests need more room than the atlas has, halve the tiles of the least important light that can still
    * shrink: requests are in order of importance (the sun, then spotlights and point lights in slot order), so the last
    * ones lose detail first. The sun's cascades shrink only when nothing else can, and never below the standard size:
    * they are its shadows everywhere on screen, and every point light's faces at their smallest leave them that much room.
    */
    void shrinkToFit(std::vector<ShadowRequest>& requests, std::uint32_t atlasCapacity) {
        std::uint32_t used = 0;

        for (const ShadowRequest& request : requests) {
            used += static_cast<std::uint32_t>(request.tileCount) * areaInSmallestTiles(request.size);
        }

        while (used > atlasCapacity) {
            auto shrinkable = std::find_if(requests.rbegin(), requests.rend(), [](const ShadowRequest& request) {
                return request.kind != ShadowKind::Directional && request.size > smallestShadowTileSize;
            });

            if (shrinkable == requests.rend()) {
                shrinkable = std::find_if(requests.rbegin(), requests.rend(), [](const ShadowRequest& request) {
                    return request.kind == ShadowKind::Directional && request.size > standardShadowTileSize;
                });
            }

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
    * The name a tile keeps from frame to frame, so it keeps its square: the light's entity and the face. Lights are
    * reordered between frames as their importance changes, so the slot is only used for lights without an entity
    * (directional lights, and lights made by hand in tests).
    */
    ShadowTileKey tileKey(const ShadowRequest& request, int face) {
        const bool hasEntity = !(request.entity == Entity{});
        const std::uint32_t number = hasEntity ? 0 : static_cast<std::uint32_t>(request.slot);
        return ShadowTileKey{static_cast<std::uint32_t>(request.kind), request.entity, number * 6 + static_cast<std::uint32_t>(face)};
    }

    // What soft shadows need to know about the perspective tile just added (see ShadowTileData::lens). The far plane is
    // the one squarePerspective really used.
    void setLens(ShadowPlan& plan, float halfWidth, float farPlane, std::uint32_t tileSize) {
        float* lens = plan.tileData.back().lens;
        lens[0] = shadowNearPlane;
        lens[1] = std::max(farPlane, 2.0f * shadowNearPlane);
        lens[2] = 2.0f * halfWidth / static_cast<float>(tileSize);
    }

    // drawMatrix is the world-space view the shadow pass draws with; shaderMatrix the same view for points measured from
    // the camera, which is how the main shader sees them. reach is the sphere the light lights (see ShadowTile).
    void addTile(ShadowPlan& plan, const ShadowAtlasSize& atlas, ShadowTile tile, const Mat4& drawMatrix, const Mat4& shaderMatrix, float offsetPerDistance,
                 float fixedOffset, const BoundingSphere& reach) {
        tile.matrix = drawMatrix;
        tile.reach = reach;
        plan.tiles.push_back(tile);

        ShadowTileData data{};

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                data.matrix[column * 4 + row] = shaderMatrix.values[row][column];
            }
        }

        data.offset[0] = offsetPerDistance;
        data.offset[1] = fixedOffset;

        data.rect[0] = static_cast<float>(tile.x) / static_cast<float>(atlas.width);
        data.rect[1] = static_cast<float>(tile.y) / static_cast<float>(atlas.height);
        data.rect[2] = static_cast<float>(tile.size) / static_cast<float>(atlas.width);
        data.rect[3] = static_cast<float>(tile.size) / static_cast<float>(atlas.height);
        plan.tileData.push_back(data);
    }
}

/*
* A view of half-width h reaching range r is 2 * h * r across at its far end. Rounding up to a power of two keeps every
* tile a whole quarter, sixteenth, ... of the largest one, which is what lets the atlas pack them without gaps.
*/
std::uint32_t shadowTileSizeFor(float halfWidth, float range, float texelTarget, std::uint32_t largestSize) {
    const float texelsNeeded = 2.0f * halfWidth * range / texelTarget;
    std::uint32_t size = smallestShadowTileSize;

    while (static_cast<float>(size) < texelsNeeded && size < largestSize) {
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
    const Mat4 view = Mat4::lookAt(lightPosition, lightPosition + forward, shadowViewUp(forward));

    return squarePerspective(withEdgeMargin(1.0f, tileSize), farPlane) * view;
}

/*
* A square camera at the light looking down the beam. Its half-width reaches the outer cone, since nothing outside it is lit.
*/
Mat4 spotShadowMatrix(const Vec3& lightPosition, const Vec3& direction, float outerAngle, float range, std::uint32_t tileSize) {
    const Mat4 view = Mat4::lookAt(lightPosition, lightPosition + direction.normalized(), shadowViewUp(direction));

    return squarePerspective(withEdgeMargin(spotHalfWidth(outerAngle), tileSize), range) * view;
}

/*
* Each shadow-casting light, up to the limit for its kind and in the shader's slot order, asks for tiles sized by its
* reach; a light whose castsShadows is off keeps its slot but gets none, and a point light's faces that cannot be seen
* get none (see pointFacesInView). If the atlas would overflow, the least important lights' tiles shrink. The layout then
* gives each tile a square, the same one as last frame when it can (see ShadowAtlasLayout). Tiles are numbered in
* request order, so the numbering does not depend on sizes or places.
*/
ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera, const ShadowSettings& settings, float fittedSunDistance) {
    ShadowAtlasLayout freshLayout{shadowAtlasSizeFor(settings)};
    return planShadows(lighting, camera, freshLayout, settings, fittedSunDistance);
}

ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera, ShadowAtlasLayout& layout, const ShadowSettings& settings,
                       float fittedSunDistance) {
    const ShadowAtlasSize& atlas = layout.size();

    if (atlas != shadowAtlasSizeFor(settings)) {
        throw std::invalid_argument("planShadows: the atlas layout is not for the atlas these settings need");
    }

    const FrameLighting selected = selectDrawableLights(lighting);
    const Vec3 origin = camera.getPosition();
    ShadowPlan plan;
    plan.directional.resize(selected.directionalLights.size());
    plan.spots.resize(selected.spotLights.size());
    plan.points.resize(selected.pointLights.size());

    std::vector<ShadowRequest> requests;

    for (std::size_t slot = 0; slot < selected.directionalLights.size(); ++slot) {
        if (selected.directionalLights[slot].castsShadows && static_cast<int>(slot) < maxShadowedDirectionalLights) {
            const int cascades = std::clamp(settings.cascadeCount, 1, maxShadowCascades);
            requests.push_back(ShadowRequest{ShadowKind::Directional, slot, cascades, cascadeTilePixels(settings.cascadeResolution), {}});
        }
    }

    int shadowedSpots = 0;

    for (std::size_t slot = 0; slot < selected.spotLights.size() && shadowedSpots < maxShadowedSpotLights; ++slot) {
        const SpotLight& light = selected.spotLights[slot].light;

        if (light.castsShadows) {
            const std::uint32_t size = shadowTileSizeFor(spotHalfWidth(light.outerAngle), light.range, settings.spotTexelTarget, largestShadowTileSize);
            ShadowRequest request{ShadowKind::Spot, slot, 1, size, {}};
            request.entity = selected.spotLights[slot].entity;
            requests.push_back(request);
            ++shadowedSpots;
        }
    }

    const Frustum cameraView = Frustum::fromClipMatrix(camera.getProjectionMatrix() * camera.getViewMatrix(), ClipDepth::NegativeOneToOne);
    int shadowedPoints = 0;

    for (std::size_t slot = 0; slot < selected.pointLights.size() && shadowedPoints < maxShadowedPointLights; ++slot) {
        const PlacedPointLight& placed = selected.pointLights[slot];

        if (placed.light.castsShadows) {
            const std::uint32_t size = shadowTileSizeFor(1.0f, placed.light.range, settings.texelTarget, standardShadowTileSize);
            ShadowRequest request{ShadowKind::Point, slot, 0, size, {}};
            request.facesInView = pointFacesInView(placed.position, placed.light.range, cameraView);
            request.tileCount = static_cast<int>(std::count(request.facesInView.begin(), request.facesInView.end(), true));
            request.entity = placed.entity;
            requests.push_back(request);
            ++shadowedPoints;
        }
    }

    shrinkToFit(requests, capacityOf(atlas));

    // Every tile asks the layout for a square by its lasting name, so it gets last frame's square back when it can.
    std::vector<ShadowSquareRequest> squareRequests;

    for (const ShadowRequest& request : requests) {
        for (int face = 0; face < 6; ++face) {
            const bool wanted = request.kind == ShadowKind::Point ? request.facesInView[static_cast<std::size_t>(face)] : face < request.tileCount;

            if (wanted) {
                squareRequests.push_back(ShadowSquareRequest{tileKey(request, face), request.size});
            }
        }
    }

    const std::vector<AtlasSquare> squares = layout.place(squareRequests);
    std::size_t nextSquare = 0;

    for (ShadowRequest& request : requests) {
        for (int tile = 0; tile < request.tileCount; ++tile) {
            const AtlasSquare& square = squares[nextSquare++];
            request.tiles.push_back(ShadowTile{Mat4::identity(), square.x, square.y, square.size, BoundingSphere{}});
        }
    }

    for (const ShadowRequest& request : requests) {
        const std::int32_t firstTile = static_cast<std::int32_t>(plan.tiles.size());

        if (request.kind == ShadowKind::Directional) {
            // The view from the camera's near plane to the shadow distance, cut into slices, each in its own box around
            // its bounding sphere (see cascades.h). A box texel is the same size everywhere, so its offset is fixed
            // rather than growing with distance. The box is snapped to texels in world space, so the shader's version
            // is the same box shifted to the camera, rather than one snapped around the camera, which would crawl as it moves.
            const DirectionalLight& light = selected.directionalLights[request.slot];
            plan.directional[request.slot] = LightShadow{firstTile, 1.0f, std::tan(light.angularRadius), request.tileCount};

            const float shadowDistance = std::min({settings.directionalDistance, camera.getFarPlane(), fittedSunDistance});
            const std::vector<float> splits = cascadeSplits(camera.getNearPlane(), shadowDistance, request.tileCount, settings.cascadeSplitLambda);

            for (int cascade = 0; cascade < request.tileCount; ++cascade) {
                const std::size_t index = static_cast<std::size_t>(cascade);
                const BoundingSphere sphere = cascadeSphere(camera, splits[index], splits[index + 1]);
                const CascadeBox box = cascadeBox(light.direction, sphere, settings.directionalCasterReach, request.size);

                addTile(plan, atlas, request.tiles[index], box.matrix, box.matrix * Mat4::translation(origin.x, origin.y, origin.z), 0.0f,
                        settings.normalOffsetTexels * box.texelSize, BoundingSphere{});

                float* lens = plan.tileData.back().lens;
                lens[1] = box.depthRange;
                lens[2] = box.texelSize;
                lens[3] = sphere.radius;
            }
        }
        else if (request.kind == ShadowKind::Spot) {
            const PlacedSpotLight& placed = selected.spotLights[request.slot];
            plan.spots[request.slot] = LightShadow{firstTile, placed.shadowFade, placed.light.emitterRadius};

            const float halfWidth = withEdgeMargin(spotHalfWidth(placed.light.outerAngle), request.size);
            // Built again from the light's offset from the camera, so the subtraction happens once, on positions, before
            // any matrix (see camera_relative.h).
            addTile(plan, atlas, request.tiles[0],
                    spotShadowMatrix(placed.position, placed.light.direction, placed.light.outerAngle, placed.light.range, request.size),
                    spotShadowMatrix(placed.position - origin, placed.light.direction, placed.light.outerAngle, placed.light.range, request.size),
                    perspectiveOffset(halfWidth, request.size, settings.normalOffsetTexels), 0.0f,
                    BoundingSphere{placed.position, placed.light.range});
            setLens(plan, halfWidth, placed.light.range, request.size);
        }
        else {
            const PlacedPointLight& placed = selected.pointLights[request.slot];
            plan.points[request.slot] = LightShadow{firstTile, placed.shadowFade, placed.light.emitterRadius};

            // Faces out of view keep their place in the list, so the shader still finds face f at firstTile + f, but get
            // an empty square: nothing is drawn into it, and the shader counts it as lit.
            std::size_t placedFaces = 0;

            for (int face = 0; face < 6; ++face) {
                const ShadowTile tile = request.facesInView[static_cast<std::size_t>(face)]
                    ? request.tiles[placedFaces++]
                    : ShadowTile{Mat4::identity(), 0, 0, 0, BoundingSphere{}};

                addTile(plan, atlas, tile, pointShadowFaceMatrix(placed.position, face, placed.light.range, request.size),
                        pointShadowFaceMatrix(placed.position - origin, face, placed.light.range, request.size),
                        perspectiveOffset(withEdgeMargin(1.0f, request.size), request.size, settings.normalOffsetTexels), 0.0f,
                        BoundingSphere{placed.position, placed.light.range});
                setLens(plan, withEdgeMargin(1.0f, request.size), placed.light.range, request.size);
            }
        }
    }

    return plan;
}

/*
* Both lists come from the same selectDrawableLights, so slot i of the plan is slot i of the lights. A sun's shadow never
* fades (it has no rival for its tiles), so only its tiles and its size go across.
*/
void attachShadows(GpuLights& lights, const ShadowPlan& plan) {
    if (plan.directional.size() != lights.directional.size() || plan.spots.size() != lights.spots.size() ||
        plan.points.size() != lights.points.size()) {
        throw std::invalid_argument("attachShadows: the shadow plan and the lights must come from the same lights");
    }

    for (std::size_t slot = 0; slot < plan.directional.size(); ++slot) {
        lights.directional[slot].shadowTile = plan.directional[slot].firstTile;
        lights.directional[slot].cascadeCount = plan.directional[slot].cascades;
        lights.directional[slot].tanAngularRadius = plan.directional[slot].emitterRadius;
    }

    for (std::size_t slot = 0; slot < plan.spots.size(); ++slot) {
        lights.spots[slot].shadowTile = plan.spots[slot].firstTile;
        lights.spots[slot].shadowStrength = plan.spots[slot].strength;
        lights.spots[slot].emitterRadius = plan.spots[slot].emitterRadius;
    }

    for (std::size_t slot = 0; slot < plan.points.size(); ++slot) {
        lights.points[slot].firstShadowTile = plan.points[slot].firstTile;
        lights.points[slot].shadowStrength = plan.points[slot].strength;
        lights.points[slot].emitterRadius = plan.points[slot].emitterRadius;
    }
}

std::uint32_t cascadeTilePixels(CascadeResolution resolution) {
    switch (resolution) {
        case CascadeResolution::Pixels1024:
            return 1024;
        case CascadeResolution::Pixels4096:
            return 4096;
        case CascadeResolution::Pixels2048:
            break;
    }

    return 2048;
}

ShadowAtlasSize shadowAtlasSizeFor(const ShadowSettings& settings) {
    return atlasForCascades(cascadeTilePixels(settings.cascadeResolution));
}

namespace {
    constexpr std::array<const char*, 2> cascadeTransitionNames{"Blend", "Dither"};
    constexpr std::array<const char*, 3> cascadeResolutionNames{"1024", "2048", "4096"};
}

void visit(ShadowSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Texel target", settings.texelTarget, {.min = 0.001f, .sliderMin = 0.005f, .sliderMax = 0.2f, .unit = "m"});
    visitor.field("Spot texel target", settings.spotTexelTarget, {.min = 0.001f, .sliderMin = 0.005f, .sliderMax = 0.2f, .unit = "m"});
    visitor.field("Directional distance", settings.directionalDistance, {.min = 1.0f, .sliderMax = 1000.0f, .unit = "m"});
    visitor.field("Directional caster reach", settings.directionalCasterReach, {.min = 0.0f, .sliderMax = 1000.0f, .unit = "m"});
    visitor.field("Fit cascades to depth", settings.fitCascadesToDepth);
    visitor.field("Cascades", settings.cascadeCount, {.min = 1, .max = maxShadowCascades});
    visitor.field("Cascade split blend", settings.cascadeSplitLambda, {.min = 0.0f, .max = 1.0f, .unit = "(0 even, 1 logarithmic)"});
    visitor.field("Cascade blend band", settings.cascadeBlendBand, {.min = 0.0f, .max = 0.5f, .unit = "of a radius"});
    visitor.choice("Cascade transition", settings.cascadeTransition, cascadeTransitionNames);
    visitor.choice("Cascade resolution", settings.cascadeResolution, cascadeResolutionNames);
    visitor.field("Normal offset", settings.normalOffsetTexels, {.min = 0.0f, .sliderMax = 6.0f, .unit = "texels"});
}
