#include "render/gpu/ambient_occlusion.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/screen_reflections.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

void visit(AmbientOcclusionSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Ambient occlusion", settings.enabled);
    visitor.field("Occlusion radius", settings.radius, {.min = 0.01f, .max = 100.0f, .sliderMin = 0.05f, .sliderMax = 3.0f, .unit = "m"});
    visitor.field("Occlusion falloff", settings.falloff, {.min = 0.01f, .max = 1.0f});
    visitor.field("Occlusion slices", settings.slices, {.min = 1, .max = AO_MAX_SLICES});
    visitor.field("Occlusion steps", settings.steps, {.min = 1, .max = AO_MAX_STEPS});
    visitor.field("Occlusion bounces", settings.multipleBounces);
    visitor.field("Specular occlusion", settings.specularOcclusion);
}

// Written so not a number fails too.
void validateAmbientOcclusion(const AmbientOcclusionSettings& settings) {
    if (!(settings.radius > 0.0f)) {
        throw std::invalid_argument("Ambient occlusion needs a radius above 0");
    }

    if (!(settings.falloff > 0.0f && settings.falloff <= 1.0f)) {
        throw std::invalid_argument("Ambient occlusion needs a falloff above 0 and at most 1");
    }

    if (settings.slices < 1 || settings.slices > AO_MAX_SLICES || settings.steps < 1 || settings.steps > AO_MAX_STEPS) {
        throw std::invalid_argument("Ambient occlusion needs 1 to AO_MAX_SLICES slices and 1 to AO_MAX_STEPS steps");
    }
}

float occluderWeight(float distance, float radius, float falloff) {
    const float range = falloff * radius;
    return std::clamp((radius - distance) / range, 0.0f, 1.0f);
}

/*
* The slice's plane holds the way to the camera v and the direction d; its axis is at right angles to both. The normal
* projected into the plane has an angle from v, signed towards d's side (the part of d at right angles to v).
*/
AoSlice aoSlice(const Vec3& normal, const Vec3& toCamera, const Vec3& direction) {
    const Vec3 across = direction - toCamera * direction.dot(toCamera);
    const Vec3 axis = across.cross(toCamera).normalized();
    const Vec3 projected = normal - axis * normal.dot(axis);
    const float length = projected.length();

    if (!(length > 0.0f)) {
        return AoSlice{axis, 0.0f, 0.0f};
    }

    const float cosine = std::clamp(projected.dot(toCamera) / length, 0.0f, 1.0f);
    const float sign = across.dot(projected) < 0.0f ? -1.0f : 1.0f;
    return AoSlice{axis, sign * std::acos(cosine), length};
}

float lowHorizonCosine(const AoSlice& slice, int side) {
    const float halfTurn = 0.5f * std::numbers::pi_v<float>;
    return std::cos(side == 0 ? slice.normalAngle + halfTurn : slice.normalAngle - halfTurn);
}

float horizonCosine(const Vec3& offset, const Vec3& toCamera) {
    return offset.dot(toCamera) / offset.length();
}

/*
* Side 0's horizon is at +acos(cosine0) from the way to the camera, side 1's at -acos(cosine1); each is kept within a
* right angle of the normal (a horizon below the surface hides nothing). Each side's arc from the way to the camera
* integrates cos(theta - n) |sin theta| to (cos n + 2 h sin n - cos(2 h - n)) / 4.
*/
float sliceVisibility(const AoSlice& slice, float horizonCosine0, float horizonCosine1) {
    const float halfTurn = 0.5f * std::numbers::pi_v<float>;
    const float n = slice.normalAngle;
    const float h0 = n + std::max(-std::acos(std::clamp(horizonCosine1, -1.0f, 1.0f)) - n, -halfTurn);
    const float h1 = n + std::min(std::acos(std::clamp(horizonCosine0, -1.0f, 1.0f)) - n, halfTurn);
    const float cosine = std::cos(n);
    const float sine = std::sin(n);
    const float arc0 = (cosine + 2.0f * h0 * sine - std::cos(2.0f * h0 - n)) * 0.25f;
    const float arc1 = (cosine + 2.0f * h1 * sine - std::cos(2.0f * h1 - n)) * 0.25f;
    return slice.projectedLength * (arc0 + arc1);
}

// Each side's miss: how far the pixel's point is from where its two neighbours on that side, carried on, put it.
Vec3 surfaceStep(const Vec3& centre, const Vec3& before, const Vec3& beforeNext, const Vec3& after, const Vec3& afterNext) {
    const Vec3 fromBefore = centre - before;
    const Vec3 toAfter = after - centre;

    if (!(fromBefore.dot(fromBefore) > 0.0f)) {
        return toAfter;
    }

    if (!(toAfter.dot(toAfter) > 0.0f)) {
        return fromBefore;
    }

    const Vec3 missBefore = centre - (before * 2.0f - beforeNext);
    const Vec3 missAfter = centre - (after * 2.0f - afterNext);
    return missAfter.dot(missAfter) < missBefore.dot(missBefore) ? toAfter : fromBefore;
}

Vec3 depthNormal(const Vec3& centre, const Vec3& across, const Vec3& downwards) {
    const Vec3 normal = downwards.cross(across);
    const float length = normal.length();

    if (!(length > 0.0f)) {
        return (centre * -1.0f).normalized();
    }

    const Vec3 unit = normal * (1.0f / length);
    return unit.dot(centre) > 0.0f ? unit * -1.0f : unit;
}

float aoScreenRadius(float radius, float footprint, float viewDistance) {
    return std::min(radius / (footprint * viewDistance), static_cast<float>(AO_MAX_SCREEN_RADIUS));
}

float aoSampleDistance(int step, int count, float noise, float screenRadius) {
    const float share = (static_cast<float>(step) + noise) / static_cast<float>(count);
    return std::max(share * share * screenRadius, static_cast<float>(AO_MIN_SAMPLE_PIXELS));
}

// Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate Indirect Occlusion", their equation 10.
Vec3 multiBounceOcclusion(float visibility, const Vec3& albedo) {
    const auto channel = [visibility](float albedo) {
        const float a = static_cast<float>(AO_BOUNCE_A_SLOPE) * albedo + static_cast<float>(AO_BOUNCE_A_OFFSET);
        const float b = static_cast<float>(AO_BOUNCE_B_SLOPE) * albedo + static_cast<float>(AO_BOUNCE_B_OFFSET);
        const float c = static_cast<float>(AO_BOUNCE_C_SLOPE) * albedo + static_cast<float>(AO_BOUNCE_C_OFFSET);
        return std::max(visibility, ((visibility * a + b) * visibility + c) * visibility);
    };
    return Vec3{channel(albedo.x), channel(albedo.y), channel(albedo.z)};
}

// Lagarde and de Rousiers 2014, "Moving Frostbite to Physically Based Rendering", their listing 26.
float specularOcclusion(float nDotV, float visibility, float roughness) {
    return std::clamp(std::pow(nDotV + visibility, std::exp2(-16.0f * roughness - 1.0f)) - 1.0f + visibility, 0.0f, 1.0f);
}

float surfaceVisibility(float screenVisibility, float occlusionTexel, float occlusionStrength) {
    return std::min(screenVisibility, 1.0f + occlusionStrength * (occlusionTexel - 1.0f));
}

float aoBlurWeight(float centreDistance, float neighbourDistance) {
    return std::max(1.0f - std::abs(neighbourDistance - centreDistance) / (static_cast<float>(AO_BLUR_DEPTH_SHARE) * centreDistance), 0.0f);
}

float ambientOcclusionAt(int x, int y, const AoFrame& frame, const AmbientOcclusionSettings& settings, float sliceNoise, float stepNoise,
                         const AoDepthImage& depthAt) {
    const int width = static_cast<int>(frame.size.width);
    const int height = static_cast<int>(frame.size.height);
    const Vec2 size{static_cast<float>(width), static_cast<float>(height)};
    const float depth = depthAt(x, y);

    if (depth == gpuFarDepth) {
        return 1.0f;
    }

    const Vec2 pixel{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
    const Vec3 position = positionAtPixel(frame.inverseViewProjection, pixel, depth, size);
    const float screenRadius = aoScreenRadius(settings.radius, frame.footprint, viewDistanceAtPixel(frame.inverseViewProjection, pixel, depth, size));

    if (screenRadius < static_cast<float>(AO_MIN_SAMPLE_PIXELS)) {
        return 1.0f;
    }

    // The neighbours' points, clamped onto the screen (a neighbour off it lands on a texel already read, and its side's
    // step gives way: the nearest one off screen is the pixel itself); the sky, at no finite point, counts as the pixel.
    const auto neighbour = [&](int dx, int dy) {
        const int nx = std::clamp(x + dx, 0, width - 1);
        const int ny = std::clamp(y + dy, 0, height - 1);
        const float neighbourDepth = depthAt(nx, ny);
        return neighbourDepth == gpuFarDepth ? position : positionAtPixel(frame.inverseViewProjection, Vec2{nx + 0.5f, ny + 0.5f}, neighbourDepth, size);
    };
    const Vec3 across = surfaceStep(position, neighbour(-1, 0), neighbour(-2, 0), neighbour(1, 0), neighbour(2, 0));
    const Vec3 downwards = surfaceStep(position, neighbour(0, -1), neighbour(0, -2), neighbour(0, 1), neighbour(0, 2));
    const Vec3 normal = depthNormal(position, across, downwards);
    const Vec3 toCamera = (position * -1.0f).normalized();

    float visibility = 0.0f;

    for (int slice = 0; slice < settings.slices; ++slice) {
        const float angle = std::numbers::pi_v<float> * (static_cast<float>(slice) + sliceNoise) / static_cast<float>(settings.slices);
        const Vec2 onScreen{std::cos(angle), std::sin(angle)};
        const Vec3 direction = positionAtPixel(frame.inverseViewProjection, pixel + onScreen, depth, size) - position;
        const AoSlice plane = aoSlice(normal, toCamera, direction);
        float horizons[2] = {lowHorizonCosine(plane, 0), lowHorizonCosine(plane, 1)};

        for (int step = 0; step < settings.steps; ++step) {
            const float distance = aoSampleDistance(step, settings.steps, stepNoise, screenRadius);

            for (int side = 0; side < 2; ++side) {
                const Vec2 sample = pixel + onScreen * (side == 0 ? distance : -distance);
                const int sx = static_cast<int>(std::floor(sample.x));
                const int sy = static_cast<int>(std::floor(sample.y));

                if (sx < 0 || sy < 0 || sx >= width || sy >= height) {
                    continue;
                }

                const float sampleDepth = depthAt(sx, sy);

                if (sampleDepth == gpuFarDepth) {
                    continue;
                }

                const Vec3 offset = positionAtPixel(frame.inverseViewProjection, Vec2{sx + 0.5f, sy + 0.5f}, sampleDepth, size) - position;
                const float weight = occluderWeight(offset.length(), settings.radius, settings.falloff);
                const float low = lowHorizonCosine(plane, side);
                horizons[side] = std::max(horizons[side], low + (horizonCosine(offset, toCamera) - low) * weight);
            }
        }

        visibility += sliceVisibility(plane, horizons[0], horizons[1]);
    }

    return std::clamp(visibility / static_cast<float>(settings.slices), 0.0f, 1.0f);
}

GpuAmbientOcclusionPass packAmbientOcclusionPass(const Mat4& inverseViewProjection, const Mat4& viewProjection, RenderSize size,
                                                 const AmbientOcclusionSettings& settings, std::uint32_t frame) {
    GpuAmbientOcclusionPass pass{};
    writeColumnMajor(inverseViewProjection, pass.inverseViewProjection);
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    pass.size[0] = width;
    pass.size[1] = height;
    pass.size[2] = 1.0f / width;
    pass.size[3] = 1.0f / height;
    pass.lens[0] = pixelFootprint(viewProjection, height);
    pass.options[0] = settings.radius;
    pass.options[1] = settings.falloff;
    pass.options[2] = static_cast<float>(settings.slices);
    pass.options[3] = static_cast<float>(settings.steps);
    pass.frame[0] = static_cast<std::int32_t>(frame);
    return pass;
}
