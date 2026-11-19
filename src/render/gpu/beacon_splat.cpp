#include "render/gpu/beacon_splat.h"

#include "render/gpu/beacon_splat_shared.h"
#include "render/gpu/stars.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
    constexpr float nearestBeaconDistance = static_cast<float>(BEACON_NEAREST_DISTANCE);
}

std::vector<GpuBeacon> packBeacons(std::span<const PlacedBeacon> beacons, const Vec3& cameraOrigin) {
    std::vector<GpuBeacon> packed;
    packed.reserve(beacons.size());

    for (const PlacedBeacon& beacon : beacons) {
        const Vec3 relative = beacon.position - cameraOrigin;
        packed.push_back(GpuBeacon{{relative.x, relative.y, relative.z, beacon.lensRadius}, {beacon.light.x, beacon.light.y, beacon.light.z, 0.0f}});
    }

    return packed;
}

float beaconLensPixels(float lensRadius, float distance, float pixelSolidAngle) {
    return lensRadius / std::max(distance, nearestBeaconDistance) / std::sqrt(pixelSolidAngle);
}

float beaconSplatRadiance(float intensity, float distance, float pixelSolidAngle) {
    const float far = std::max(distance, nearestBeaconDistance);
    return intensity / (far * far * pixelSolidAngle);
}

/*
* The mix runs from a lens one pixel across (radius 1) to two, so the tent, exact for a point, gives way to the disc only
* once the lens is big enough for the disc's sum to be close to 1.
*/
float beaconSplatWeight(const Vec2& offset, float lensPixels) {
    const float toDisc = std::clamp(lensPixels - 1.0f, 0.0f, 1.0f);
    const float radius = std::max(lensPixels, 1.0f);
    const float cover = std::clamp(radius + 0.5f - offset.length(), 0.0f, 1.0f);
    const float disc = cover / (std::numbers::pi_v<float> * radius * radius);
    return (1.0f - toDisc) * starSplatWeight(offset) + toDisc * disc;
}

float beaconExtentPixels(float lensPixels) {
    return std::max(lensPixels, 1.0f) + 1.0f;
}
