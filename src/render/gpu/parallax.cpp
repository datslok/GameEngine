#include "render/gpu/parallax.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <cmath>

Vec3 toTangentFrame(const Vec3& direction, const Vec3& normal, const Vec4& tangent) {
    const Vec3 unitNormal = normal.normalized();
    const Vec3 along{tangent.x, tangent.y, tangent.z};
    const Vec3 unitTangent = (along - unitNormal * unitNormal.dot(along)).normalized();
    const Vec3 bitangent = unitNormal.cross(unitTangent) * tangent.w;
    return Vec3{direction.dot(unitTangent), direction.dot(bitangent), direction.dot(unitNormal)};
}

int parallaxSteps(float viewCosine, int mostSteps) {
    const int most = std::max(mostSteps, parallaxFewestSteps);
    const float fewest = static_cast<float>(parallaxFewestSteps);
    const float steps = static_cast<float>(most) + (fewest - static_cast<float>(most)) * std::clamp(viewCosine, 0.0f, 1.0f);
    return std::clamp(static_cast<int>(std::floor(steps + 0.5f)), parallaxFewestSteps, most);
}

Vec2 parallaxDelta(const Vec3& viewTangent, float depth) {
    const float scale = -depth / std::max(viewTangent.z, parallaxSmallestViewCosine);
    return Vec2{viewTangent.x * scale, viewTangent.y * scale};
}

Vec2 parallaxLightDelta(const Vec3& lightTangent, float depth) {
    return parallaxDelta(lightTangent, -depth);
}

float parallaxGap(float t, float height) {
    return 1.0f - t - height;
}

float parallaxCrossing(float gapAbove, float gapBelow) {
    const float span = gapAbove - gapBelow;
    return span > 0.0f ? std::clamp(gapAbove / span, 0.0f, 1.0f) : 1.0f;
}

float parallaxDepthPixels(float depth, const Vec2& uvStepAcross, const Vec2& uvStepDown) {
    const float footprint = std::max(uvStepAcross.length(), uvStepDown.length());
    return depth / std::max(footprint, 1e-12f);
}

float parallaxFade(float depthPixels) {
    return smoothStep(parallaxFadeNonePixels, parallaxFadeFullPixels, depthPixels);
}

float parallaxShadowSample(float terrainHeight, float rayHeight, float climbed) {
    if (terrainHeight <= rayHeight) {
        return 1.0f;
    }

    return std::clamp(1.0f - (terrainHeight - rayHeight) / climbed * parallaxShadowSoftness, 0.0f, 1.0f);
}

/*
* Each step's t is worked out from its number rather than summed, so the last is exactly 1, the bottom, where every
* ray's gap is zero or less: the loop always stops by then.
*/
ParallaxHit parallaxSearch(const ParallaxHeight& heightAt, const Vec2& uv, const Vec3& viewTangent, float depth, int steps) {
    float height = heightAt(uv);

    if (height >= 1.0f) {
        return ParallaxHit{uv, height};
    }

    const Vec2 delta = parallaxDelta(viewTangent, depth);
    float previousT = 0.0f;
    float previousGap = parallaxGap(0.0f, height);
    float t = 1.0f;

    for (int step = 1; step <= steps; ++step) {
        t = static_cast<float>(step) / static_cast<float>(steps);
        height = heightAt(uv + delta * t);
        const float gap = parallaxGap(t, height);

        if (gap <= 0.0f) {
            t = previousT + (t - previousT) * parallaxCrossing(previousGap, gap);
            break;
        }

        previousT = t;
        previousGap = gap;
    }

    return ParallaxHit{uv + delta * t, 1.0f - t};
}

float parallaxShadow(const ParallaxHeight& heightAt, const Vec2& hitUv, float hitHeight, const Vec3& lightTangent, float depth, int steps) {
    if (lightTangent.z <= 0.0f || hitHeight >= 1.0f) {
        return 1.0f;
    }

    const Vec2 delta = parallaxLightDelta(lightTangent, depth);
    float visible = 1.0f;

    for (int step = 1; step <= steps; ++step) {
        const float climbed = (1.0f - hitHeight) * static_cast<float>(step) / static_cast<float>(steps);
        visible = std::min(visible, parallaxShadowSample(heightAt(hitUv + delta * climbed), hitHeight + climbed, climbed));
    }

    return visible;
}
