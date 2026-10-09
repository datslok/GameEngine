#include "render/gpu/shadow_map.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/light_uniforms.h"

#include <algorithm>
#include <cmath>

namespace {
    // Soft shadows read the depth texels around a point (3x3), and filtering touches one more. Views are made slightly
    // wider than needed, so every point they are meant to cover stays this many texels inside its tile.
    constexpr float edgeMarginTexels = 2.0f;

    // How far behind a directional light's box (towards the light) objects still cast shadows into it.
    constexpr float directionalCasterReach = 50.0f;

    // Shadow acne fix: points are moved off their surface along the normal by this many texels.
    constexpr float normalOffsetTexels = 1.5f;

    float tileSize() {
        return static_cast<float>(shadowTileSize);
    }

    // Widen a view (given as tan of its half-angle) so its contents stay edgeMarginTexels from the edge.
    float withEdgeMargin(float halfWidth) {
        return halfWidth / (1.0f - 2.0f * edgeMarginTexels / tileSize());
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
    float perspectiveOffset(float halfWidth) {
        return normalOffsetTexels * 2.0f * halfWidth / tileSize();
    }

    void addTile(ShadowPlan& plan, const Mat4& matrix, float offsetPerDistance, float fixedOffset) {
        const std::size_t tile = plan.tileMatrices.size();
        plan.tileMatrices.push_back(matrix);

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                plan.uniforms.tileMatrices[tile][column * 4 + row] = matrix.values[row][column];
            }
        }

        plan.uniforms.tileOffsets[tile][0] = offsetPerDistance;
        plan.uniforms.tileOffsets[tile][1] = fixedOffset;
    }
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
Mat4 pointShadowFaceMatrix(const Vec3& lightPosition, int face, float farPlane) {
    static const Vec3 forwards[6] = {
        Vec3{1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f},
        Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f},
        Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f}
    };

    const Vec3 forward = forwards[face];
    const Mat4 view = Mat4::lookAt(lightPosition, lightPosition + forward, upFor(forward));

    return squarePerspective(withEdgeMargin(1.0f), farPlane) * view;
}

/*
* A square camera at the light looking down the beam. Its half-width reaches the outer cone, since nothing outside it is lit.
* Cones wider than about 160 degrees are clamped, because a perspective view cannot reach 180.
*/
Mat4 spotShadowMatrix(const Vec3& lightPosition, const Vec3& direction, float outerAngle, float range) {
    const float halfAngle = std::clamp(outerAngle, 0.01f, 1.4f);
    const Mat4 view = Mat4::lookAt(lightPosition, lightPosition + direction.normalized(), upFor(direction));

    return squarePerspective(withEdgeMargin(std::tan(halfAngle)), range) * view;
}

/*
* Parallel rays need no perspective: a box, looking along the light, centred halfway along the stretch of the camera's
* view that gets shadows. The box reaches back towards the light so tall things outside the view still cast into it.
* Its centre is snapped to whole shadow texels, measured across the light's direction: as the camera moves, the box
* jumps a whole texel at a time, so every shadow edge falls on the same texels instead of crawling and shimmering.
*/
Mat4 directionalShadowMatrix(const Vec3& direction, const Camera& camera) {
    const Vec3 along = direction.lengthSquared() > 1e-12f ? direction.normalized() : Vec3{0.0f, -1.0f, 0.0f};
    const float radius = directionalShadowDistance * 0.5f;
    const Vec3 centre = camera.getPosition() + camera.getForward() * radius;

    // The light's orientation only: a view at the origin looking along the light.
    const Mat4 rotation = Mat4::lookAt(Vec3{0.0f, 0.0f, 0.0f}, along, upFor(along));
    const Vec4 centreInLight = rotation * Vec4{centre.x, centre.y, centre.z, 1.0f};

    const float texel = 2.0f * radius / tileSize();
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
* Tiles go to point lights first (six each), then spotlights, then directional lights, each up to its limit, in the
* order the shader's light slots use. A light whose castsShadows is off keeps its slot but gets no tiles.
*/
ShadowPlan planShadows(const FrameLighting& lighting, const Camera& camera) {
    const FrameLighting selected = selectDrawableLights(lighting);
    ShadowPlan plan{};

    std::fill(&plan.uniforms.pointTiles[0][0], &plan.uniforms.pointTiles[0][0] + 16, -1);
    std::fill(plan.uniforms.spotTiles, plan.uniforms.spotTiles + 4, -1);
    std::fill(plan.uniforms.directionalTiles, plan.uniforms.directionalTiles + 4, -1);

    plan.uniforms.atlasTexel[0] = 1.0f / (tileSize() * static_cast<float>(shadowAtlasColumns));
    plan.uniforms.atlasTexel[1] = 1.0f / (tileSize() * static_cast<float>(shadowAtlasRows));

    int shadowedPoints = 0;

    for (std::size_t slot = 0; slot < selected.pointLights.size(); ++slot) {
        const PlacedPointLight& placed = selected.pointLights[slot];

        if (!placed.light.castsShadows || shadowedPoints == maxShadowedPointLights) {
            continue;
        }

        plan.uniforms.pointTiles[slot / 4][slot % 4] = static_cast<std::int32_t>(plan.tileMatrices.size());

        for (int face = 0; face < 6; ++face) {
            addTile(plan, pointShadowFaceMatrix(placed.position, face, placed.light.range), perspectiveOffset(withEdgeMargin(1.0f)), 0.0f);
        }

        ++shadowedPoints;
    }

    for (std::size_t slot = 0; slot < selected.spotLights.size(); ++slot) {
        const PlacedSpotLight& placed = selected.spotLights[slot];

        if (!placed.light.castsShadows || static_cast<int>(slot) >= maxShadowedSpotLights) {
            continue;
        }

        const float halfWidth = withEdgeMargin(std::tan(std::clamp(placed.light.outerAngle, 0.01f, 1.4f)));
        plan.uniforms.spotTiles[slot] = static_cast<std::int32_t>(plan.tileMatrices.size());
        addTile(plan, spotShadowMatrix(placed.position, placed.light.direction, placed.light.outerAngle, placed.light.range), perspectiveOffset(halfWidth), 0.0f);
    }

    for (std::size_t slot = 0; slot < selected.directionalLights.size(); ++slot) {
        const DirectionalLight& light = selected.directionalLights[slot];

        if (!light.castsShadows || static_cast<int>(slot) >= maxShadowedDirectionalLights) {
            continue;
        }

        // A box texel is the same size everywhere, so its offset is fixed rather than growing with distance.
        plan.uniforms.directionalTiles[slot] = static_cast<std::int32_t>(plan.tileMatrices.size());
        addTile(plan, directionalShadowMatrix(light.direction, camera), 0.0f, normalOffsetTexels * directionalShadowDistance / tileSize());
    }

    return plan;
}
