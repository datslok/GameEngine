#include "render/gpu/volumetric_fog.h"
#include "render/gpu/atmosphere.h"
#include "render/gpu/column_major.h"
#include "render/gpu/taa.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <stdexcept>

namespace {
    constexpr float sliceStart = static_cast<float>(FOG_SLICE_START);
    constexpr float maxExponent = static_cast<float>(FOG_MAX_EXPONENT);

    /*
    * (1 - exp(-x)) / x, the average of exp(-t) over 0..x as a share of its start: the integral of an exponential along a
    * step whose exponent changes by x. Near 0 its series 1 - x / 2 + x^2 / 6, since the division loses every digit there.
    */
    float exponentialShare(float x) {
        if (std::abs(x) < 1e-3f) {
            return 1.0f - x * (0.5f - x / 6.0f);
        }

        return (1.0f - std::exp(-x)) / x;
    }
}

void visit(FogSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Volumetric fog", settings.enabled);
    visitor.field("Fog range", settings.range, {.min = static_cast<float>(FOG_MIN_RANGE), .max = static_cast<float>(FOG_MAX_RANGE),
                                                .sliderMin = 20.0f, .sliderMax = 300.0f, .unit = "m"});
    visitor.field("Fog froxels across", settings.gridWidth, {.min = FOG_MIN_GRID_SIDE, .max = FOG_MAX_GRID_SIDE, .sliderMax = 320});
    visitor.field("Fog froxels down", settings.gridHeight, {.min = FOG_MIN_GRID_SIDE, .max = FOG_MAX_GRID_SIDE, .sliderMax = 180});
    visitor.field("Fog slices", settings.slices, {.min = FOG_MIN_SLICES, .max = FOG_MAX_SLICES, .sliderMax = 128});
    visitor.field("Fog history", settings.historyWeight, {.min = 0.0f, .max = static_cast<float>(FOG_MAX_HISTORY_WEIGHT)});
    visitor.field("Fog lit by probes", settings.probeLight);
}

// Written so not a number fails too.
void validateFog(const FogSettings& settings) {
    if (!(settings.range >= static_cast<float>(FOG_MIN_RANGE) && settings.range <= static_cast<float>(FOG_MAX_RANGE))) {
        throw std::invalid_argument("Volumetric fog needs a range from FOG_MIN_RANGE to FOG_MAX_RANGE");
    }

    if (settings.gridWidth < FOG_MIN_GRID_SIDE || settings.gridWidth > FOG_MAX_GRID_SIDE || settings.gridHeight < FOG_MIN_GRID_SIDE ||
        settings.gridHeight > FOG_MAX_GRID_SIDE) {
        throw std::invalid_argument("Volumetric fog needs FOG_MIN_GRID_SIDE to FOG_MAX_GRID_SIDE froxels across and down");
    }

    if (settings.slices < FOG_MIN_SLICES || settings.slices > FOG_MAX_SLICES) {
        throw std::invalid_argument("Volumetric fog needs FOG_MIN_SLICES to FOG_MAX_SLICES slices");
    }

    if (!(settings.historyWeight >= 0.0f && settings.historyWeight <= static_cast<float>(FOG_MAX_HISTORY_WEIGHT))) {
        throw std::invalid_argument("Volumetric fog needs a history weight from 0 to FOG_MAX_HISTORY_WEIGHT");
    }
}

// The exponent is capped, so far below the base the density stays a float.
float fogDensity(const Fog& fog, float height) {
    return fog.density * std::exp(std::min((fog.baseHeight - height) / fog.scaleHeight, maxExponent));
}

/*
* Along the ray the height is originHeight + directionY t, so the density is the density at start times exp(-k (t -
* start)) with k = directionY / scaleHeight, whose integral to end is the start's density times the length times
* exponentialShare(k x length). Looking down, k is negative and the density grows along the ray; capping k x length keeps
* it a float.
*/
float fogOpticalDepth(const Fog& fog, float originHeight, float directionY, float start, float end) {
    if (!(end > start)) {
        return 0.0f;
    }

    const float length = end - start;
    const float change = std::max(directionY / fog.scaleHeight * length, -maxExponent);
    return fogDensity(fog, originHeight + directionY * start) * length * exponentialShare(change);
}

// Even in log(depth + sliceStart): depth = s ((1 + range / s)^slice - 1).
float fogSliceDepth(float slice, float range) {
    return sliceStart * (std::pow(1.0f + range / sliceStart, slice) - 1.0f);
}

float fogDepthSlice(float depth, float range) {
    return std::log(1.0f + std::max(depth, 0.0f) / sliceStart) / std::log(1.0f + range / sliceStart);
}

Vec3 fogVolumeCoordinate(const Vec3& relativePoint, const Mat4& viewProjection, float range) {
    const Vec4 clip = viewProjection * Vec4{relativePoint.x, relativePoint.y, relativePoint.z, 1.0f};
    return Vec3{clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f, fogDepthSlice(clip.w, range)};
}

/*
* Any depth between the near and far planes gives a point on the ray through the screen place (the camera is at the
* origin): halfway in the GPU's depth range. Scaled along the ray to the slice's view depth.
*/
Vec3 fogVolumePoint(const Vec3& coordinate, const Mat4& inverseViewProjection, const Vec3& viewForward, float range) {
    const Vec4 onRay = inverseViewProjection * Vec4{coordinate.x * 2.0f - 1.0f, 1.0f - coordinate.y * 2.0f, 0.5f, 1.0f};
    const Vec3 direction = Vec3{onRay.x, onRay.y, onRay.z}.normalized();
    return direction * (fogSliceDepth(coordinate.z, range) / direction.dot(viewForward));
}

Vec3 fogJitter(std::uint64_t frame) {
    const std::uint32_t index = static_cast<std::uint32_t>(frame % fogJitterFrames) + 1;
    return Vec3{halton(index, 2) - 0.5f, halton(index, 3) - 0.5f, halton(index, 5) - 0.5f};
}

Vec3 fogInScattering(const Vec3& irradiance, const Vec3& toLight, const Vec3& viewDirection, float anisotropy) {
    return irradiance * (std::numbers::pi_v<float> * henyeyGreensteinPhase(anisotropy, viewDirection.dot(toLight)));
}

Vec3 fogAmbient(const Vec3& irradianceUp, const Vec3& irradianceDown) {
    return (irradianceUp + irradianceDown) * 0.5f;
}

void fogIntegrateSlice(FogIntegration& sum, const Vec3& scattered, float extinction, float length) {
    const float depth = extinction * length;
    sum.light = sum.light + scattered * (sum.transmittance * oneMinusExp(depth));
    sum.transmittance *= std::exp(-depth);
}

FogIntegration fogFollow(const FogIntegration& front, const FogIntegration& behind) {
    return FogIntegration{front.light + behind.light * front.transmittance, front.transmittance * behind.transmittance};
}

Vec3 fogComposite(const Vec3& colour, const FogIntegration& fog) {
    return colour * fog.transmittance + fog.light;
}

Vec3 fogOverGlass(const Vec3& added, const Vec3& transmittance, const FogIntegration& fog) {
    return added * fog.transmittance + (Vec3{1.0f, 1.0f, 1.0f} - transmittance) * fog.light;
}

FogSliceRead fogSliceRead(float slice, int slices) {
    const float count = static_cast<float>(slices);
    const float half = 0.5f / count;
    return FogSliceRead{std::clamp(slice - half, half, 1.0f - half), std::clamp(slice * count, 0.0f, 1.0f)};
}

float fogHistoryWeight(const Vec3& previousCoordinate, float weight) {
    const bool inside = previousCoordinate.x >= 0.0f && previousCoordinate.x <= 1.0f && previousCoordinate.y >= 0.0f && previousCoordinate.y <= 1.0f &&
                        previousCoordinate.z >= 0.0f && previousCoordinate.z <= 1.0f;
    return inside ? weight : 0.0f;
}

std::vector<FogIntegration> integrateFogColumn(std::span<const Vec4> lighting, float range, float stretch) {
    const float slices = static_cast<float>(lighting.size());
    std::vector<FogIntegration> column;
    column.reserve(lighting.size());
    FogIntegration sum;

    for (std::size_t slice = 0; slice < lighting.size(); ++slice) {
        const float near = fogSliceDepth(static_cast<float>(slice) / slices, range);
        const float far = fogSliceDepth(static_cast<float>(slice + 1) / slices, range);
        const Vec4& froxel = lighting[slice];
        fogIntegrateSlice(sum, Vec3{froxel.x, froxel.y, froxel.z}, froxel.w, (far - near) * stretch);
        column.push_back(sum);
    }

    return column;
}

FogIntegration fogBeyondGrid(const Fog& fog, const Vec3& source, float originHeight, float directionY, float start, float end) {
    const float depth = fogOpticalDepth(fog, originHeight, directionY, start, end);
    return FogIntegration{source * oneMinusExp(depth), std::exp(-depth)};
}

GpuFog packFog(const Fog* fog, const FogSettings& settings, const Mat4& viewProjection, const Mat4& previousViewProjection, bool historyUsable,
               std::uint64_t frame, float cameraFar) {
    GpuFog packed{};

    if (fog == nullptr || !settings.enabled) {
        return packed;
    }

    writeColumnMajor(viewProjection, packed.viewProjection);
    writeColumnMajor(inverse(viewProjection).value_or(Mat4::identity()), packed.inverseViewProjection);
    writeColumnMajor(previousViewProjection, packed.previousViewProjection);

    packed.grid[0] = static_cast<float>(settings.gridWidth);
    packed.grid[1] = static_cast<float>(settings.gridHeight);
    packed.grid[2] = static_cast<float>(settings.slices);
    packed.grid[3] = std::min(settings.range, cameraFar);

    packed.medium[0] = fog->density;
    packed.medium[1] = fog->baseHeight;
    packed.medium[2] = fog->scaleHeight;
    packed.medium[3] = fog->anisotropy;

    packed.albedo[0] = fog->albedo.x;
    packed.albedo[1] = fog->albedo.y;
    packed.albedo[2] = fog->albedo.z;
    packed.albedo[3] = fog->maxDistance;

    packed.options[0] = 1.0f;
    packed.options[1] = historyUsable ? settings.historyWeight : 0.0f;
    packed.options[2] = settings.probeLight ? 1.0f : 0.0f;

    const Vec3 jitter = fogJitter(frame);
    packed.jitter[0] = jitter.x;
    packed.jitter[1] = jitter.y;
    packed.jitter[2] = jitter.z;
    return packed;
}

GpuFogPicturePass packFogPicturePass(const Mat4& inverseViewProjection, RenderSize size) {
    GpuFogPicturePass pass{};
    writeColumnMajor(inverseViewProjection, pass.inverseViewProjection);
    pass.size[0] = static_cast<float>(size.width);
    pass.size[1] = static_cast<float>(size.height);
    return pass;
}
