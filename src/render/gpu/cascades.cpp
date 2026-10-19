#include "render/gpu/cascades.h"

#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/shader_shared.h"

#include <algorithm>
#include <cmath>
#include <limits>

/*
* Evenly spaced splits waste the far cascades' texels on little screen; logarithmic ones (each cascade reaching the same
* multiple further than the last) match texels to the screen but squeeze the first cascade into the last metre or two.
* The ends are set exactly, so the last cascade reaches the shadow distance to the bit.
*/
std::vector<float> cascadeSplits(float nearDistance, float farDistance, int count, float lambda) {
    const int cascades = std::max(count, 1);
    const float nearest = std::max(nearDistance, 1e-4f);
    const float farthest = std::max(farDistance, nearest * 1.001f);
    std::vector<float> splits(static_cast<std::size_t>(cascades) + 1);

    for (int i = 0; i <= cascades; ++i) {
        const float share = static_cast<float>(i) / static_cast<float>(cascades);
        const float logarithmic = nearest * std::pow(farthest / nearest, share);
        const float even = nearest + (farthest - nearest) * share;
        splits[static_cast<std::size_t>(i)] = lambda * logarithmic + (1.0f - lambda) * even;
    }

    splits.front() = nearest;
    splits.back() = farthest;
    return splits;
}

/*
* A corner at distance d along the view is d * t off the axis, where t is the length of (tan of half the width, tan of
* half the height). A centre on the axis at distance z is as far from the near corners as from the far ones when
* (z - n)^2 + (n t)^2 = (f - z)^2 + (f t)^2, so z = (f + n)(1 + t^2) / 2. Beyond the far end (a wide view, or a thin
* slice far out), the far corners alone decide, and the far end's centre is the best place. Only n, f and t enter the
* radius, so it is the same however the camera is placed and turned.
*/
BoundingSphere cascadeSphere(const Camera& camera, float nearDistance, float farDistance) {
    const float tanHalfHeight = std::tan(camera.getVerticalFov() * 0.5f);
    const float tanHalfWidth = tanHalfHeight * camera.getAspectRatio();
    const float spreadSquared = tanHalfWidth * tanHalfWidth + tanHalfHeight * tanHalfHeight;

    float along = 0.5f * (farDistance + nearDistance) * (1.0f + spreadSquared);
    float radius = 0.0f;

    if (along >= farDistance) {
        along = farDistance;
        radius = farDistance * std::sqrt(spreadSquared);
    }
    else {
        const float beyond = farDistance - along;
        radius = std::sqrt(beyond * beyond + farDistance * farDistance * spreadSquared);
    }

    return BoundingSphere{camera.getPosition() + camera.getForward() * along, radius};
}

namespace {
    float ladderDistance(int step) {
        return std::pow(shadowDistanceStep, static_cast<float>(step));
    }

    // The lowest step reaching at least this far (the first, 1 m, for anything nearer). Corrected by a step either way
    // after the logarithm, so rounding never leaves a distance just short of its step.
    int stepReaching(float distance) {
        if (!(distance > 1.0f)) {
            return 0;
        }

        int step = static_cast<int>(std::ceil(std::log(distance) / std::log(shadowDistanceStep)));

        while (step > 0 && ladderDistance(step - 1) >= distance) {
            --step;
        }

        while (ladderDistance(step) < distance) {
            ++step;
        }

        return step;
    }
}

ShadowDistanceFit fitShadowDistance(const ShadowDistanceFit& previous, std::optional<float> farthestSurface) {
    if (!farthestSurface) {
        return ShadowDistanceFit{};
    }

    const int needed = stepReaching(*farthestSurface * shadowDistanceMargin);

    if (previous.step && needed < *previous.step && needed > *previous.step - shadowDistanceShrinkSteps) {
        return previous;
    }

    return ShadowDistanceFit{needed};
}

float fittedShadowDistance(const ShadowDistanceFit& fit) {
    return fit.step ? ladderDistance(*fit.step) : std::numeric_limits<float>::infinity();
}

Vec3 shadowViewUp(const Vec3& forward) {
    return std::abs(forward.normalized().y) > 0.99f ? Vec3{0.0f, 0.0f, -1.0f} : Vec3{0.0f, 1.0f, 0.0f};
}

/*
* Parallel rays need no perspective: a box looking along the light, as wide as the sphere. The box's centre is snapped to
* whole texels, measured across the light's direction: as the sphere moves with the camera, the box jumps a whole texel
* at a time, so every shadow edge falls on the same texels instead of crawling and shimmering.
*/
CascadeBox cascadeBox(const Vec3& lightDirection, const BoundingSphere& sphere, float casterReach, std::uint32_t tileSize) {
    const Vec3 along = lightDirection.lengthSquared() > 1e-12f ? lightDirection.normalized() : Vec3{0.0f, -1.0f, 0.0f};
    const float radius = sphere.radius;

    // The light's orientation only: a view at the origin looking along the light.
    const Mat4 rotation = Mat4::lookAt(Vec3{0.0f, 0.0f, 0.0f}, along, shadowViewUp(along));
    const Vec4 centreInLight = rotation * Vec4{sphere.centre.x, sphere.centre.y, sphere.centre.z, 1.0f};

    const float texel = 2.0f * radius / static_cast<float>(tileSize);
    const float snappedX = std::round(centreInLight.x / texel) * texel;
    const float snappedY = std::round(centreInLight.y / texel) * texel;

    // The view looks down -z, so a point's distance along the light is -z.
    const float centreDistance = -centreInLight.z;
    const Mat4 box = Mat4::orthographic(
        snappedX - radius, snappedX + radius,
        snappedY - radius, snappedY + radius,
        centreDistance - radius - casterReach, centreDistance + radius
    );

    return CascadeBox{toGpuDepthRange(box) * rotation, 2.0f * radius + casterReach, texel};
}

float cascadeEdgeDistance(const Vec3& projected, float depthRangeOverRadius) {
    const float across = 1.0f - std::max(std::abs(projected.x), std::abs(projected.y));
    const float fromFarSide = projected.z * depthRangeOverRadius;
    const float fromNearSide = (1.0f - projected.z) * depthRangeOverRadius;
    return std::min(across, std::min(fromFarSide, fromNearSide));
}

float cascadeBlendWeight(float edgeDistance, float blendBand) {
    if (blendBand <= 0.0f) {
        return edgeDistance > 0.0f ? 1.0f : 0.0f;
    }

    return std::clamp(edgeDistance / blendBand, 0.0f, 1.0f);
}

bool cascadeDitherKeeps(float weight, float noise) {
    return noise < weight;
}

float sunShadowBias(float baseOffset, float nDotL) {
    const float facing = std::clamp(nDotL, 0.0f, 1.0f);
    const float slope = std::sqrt(1.0f - facing * facing) / std::max(facing, 1e-6f);
    return baseOffset * (1.0f + std::min(slope, maxSunBiasSlope));
}
