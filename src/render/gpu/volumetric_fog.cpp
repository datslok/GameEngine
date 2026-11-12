#include "render/gpu/volumetric_fog.h"
#include "core/hash.h"
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

Vec3 fogFroxelTurn(const FogFroxel& froxel) {
    const std::array<std::uint32_t, 3> hashed =
        pcg3d({static_cast<std::uint32_t>(froxel[0]), static_cast<std::uint32_t>(froxel[1]), static_cast<std::uint32_t>(froxel[2])});
    constexpr float share = 1.0f / 16777216.0f;
    return Vec3{static_cast<float>(hashed[0] >> 8u) * share, static_cast<float>(hashed[1] >> 8u) * share, static_cast<float>(hashed[2] >> 8u) * share};
}

// The jitter is the Halton point less a half: the point turned, wrapped into 0..1 (x - floor(x), GLSL's fract), less a half.
Vec3 fogFroxelJitter(const Vec3& frameJitter, const FogFroxel& froxel) {
    const Vec3 turn = fogFroxelTurn(froxel);
    const auto wrapped = [](float jitter, float turn) {
        const float value = jitter + 0.5f + turn;
        return value - std::floor(value) - 0.5f;
    };
    return Vec3{wrapped(frameJitter.x, turn.x), wrapped(frameJitter.y, turn.y), wrapped(frameJitter.z, turn.z)};
}

Vec3 fogFroxelCoordinate(const FogFroxel& froxel, const FogFroxel& size, const Vec3& offset) {
    const auto along = [](int place, int count, float offset) {
        return std::clamp((static_cast<float>(place) + 0.5f + offset) / static_cast<float>(count), 0.0f, 1.0f);
    };
    return Vec3{along(froxel[0], size[0], offset.x), along(froxel[1], size[1], offset.y), along(froxel[2], size[2], offset.z)};
}

Vec3 fogHistoryCoordinate(const FogFroxel& froxel, const FogFroxel& size, const Mat4& inverseViewProjection, const Mat4& previousViewProjection,
                          const Vec3& viewForward, float range) {
    const Vec3 middle = fogVolumePoint(fogFroxelCoordinate(froxel, size, Vec3{}), inverseViewProjection, viewForward, range);
    return fogVolumeCoordinate(middle, previousViewProjection, range);
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

Vec3 fogRemoved(const Vec3& seen, const FogIntegration& fog) {
    const float letThrough = std::max(fog.transmittance, static_cast<float>(FOG_REMOVAL_MIN_TRANSMITTANCE));
    const Vec3 surface = (seen - fog.light) * (1.0f / letThrough);
    return Vec3{std::max(surface.x, 0.0f), std::max(surface.y, 0.0f), std::max(surface.z, 0.0f)};
}

Vec3 fogOverGlass(const Vec3& added, const Vec3& transmittance, const FogIntegration& fog) {
    return added * fog.transmittance + (Vec3{1.0f, 1.0f, 1.0f} - transmittance) * fog.light;
}

FogSliceRead fogSliceRead(float slice, int slices) {
    const float count = static_cast<float>(slices);
    const float half = 0.5f / count;
    return FogSliceRead{std::clamp(slice - half, half, 1.0f - half), std::clamp(slice * count, 0.0f, 1.0f)};
}

FogMedium fogSliceMedium(const FogIntegration& before, const FogIntegration& after, float length) {
    if (!(before.transmittance > 0.0f) || !(length > 0.0f)) {
        return FogMedium{};
    }

    const float through = std::clamp(after.transmittance / before.transmittance, 0.0f, 1.0f);
    const Vec3 sliceLight = (after.light - before.light) * (1.0f / before.transmittance);
    const Vec3 light{std::max(sliceLight.x, 0.0f), std::max(sliceLight.y, 0.0f), std::max(sliceLight.z, 0.0f)};
    const float extinction = -std::log(std::max(through, std::exp(-static_cast<float>(FOG_MAX_EXPONENT)))) / length;

    // Light per metre: the slice's light S (1 - T) is S extinction per metre over its length; nearly clear, its light
    // over its length (the same limit, without dividing by nothing).
    const float opaque = 1.0f - through;
    return FogMedium{opaque > 1e-4f ? light * (extinction / opaque) : light * (1.0f / length), extinction};
}

FogMedium mixFogMedium(const FogMedium& a, const FogMedium& b, float t) {
    return FogMedium{a.light + (b.light - a.light) * t, a.extinction + (b.extinction - a.extinction) * t};
}

FogIntegration fogThroughMedium(const FogMedium& medium, float length) {
    const float depth = medium.extinction * length;
    const float through = std::exp(-depth);
    return FogIntegration{depth > 1e-4f ? medium.light * ((1.0f - through) / medium.extinction) : medium.light * length, through};
}

FogSurfaceRead fogSurfaceRead(float slice, int slices, float range) {
    const float count = static_cast<float>(slices);
    const float place = slice * count;

    if (place < 1.0f || place >= count) {
        return FogSurfaceRead{};
    }

    const float index = std::floor(place);
    const float front = (place - 1.0f) / count;
    return FogSurfaceRead{true, static_cast<int>(index) - 1, place - index, front, fogSliceDepth(slice, range) - fogSliceDepth(front, range)};
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
               bool probesRead, std::uint64_t frame, float cameraFar) {
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
    packed.options[2] = settings.probeLight && probesRead ? 1.0f : 0.0f;

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
