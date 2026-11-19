#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/beacon.h"

#include <span>
#include <vector>

/*
* Drawing beacons (scene/beacon.h), the parts that do not call the graphics API; mirrored by assets/shaders/
* beacon_splat.glsl and checked against it by the GPU tests. A beacon is drawn as the stars are (render/gpu/stars.h):
* the light reaching the eye, E = I / d^2, shared between the pixels round where it falls by weights summing to 1, so it
* keeps its energy at any distance instead of fading or growing; while its lens is under a pixel the weights are the
* star's tent, and as the lens grows past one and two pixels across they turn into its disc, which then shows the lens's
* own luminance, I / (pi r^2). Fogged at its own point and hidden by what stands in front of it (beacon_splat.vert/.frag).
*/

/*
* One beacon as the pass reads it (std430, a storage buffer): where it is measured from the camera, its lens's radius in
* w; its light (colour times intensity times level) in rgb.
*/
struct GpuBeacon {
    float positionAndLens[4];
    float light[4];
};

static_assert(sizeof(GpuBeacon) == 32, "GpuBeacon must match Beacon in beacon_splat.vert");

std::vector<GpuBeacon> packBeacons(std::span<const PlacedBeacon> beacons, const Vec3& cameraOrigin);

// The lens's radius in pixels: its angular radius (lensRadius / distance) over a pixel's angular size, sqrt(pixel's solid angle).
float beaconLensPixels(float lensRadius, float distance, float pixelSolidAngle);

/*
* The radiance a pixel would show if it held all of a beacon's light: E / the pixel's solid angle, E = intensity / d^2
* (distance kept above a millimetre). Times a pixel's weight, it is that pixel's share.
*/
float beaconSplatRadiance(float intensity, float distance, float pixelSolidAngle);

/*
* A pixel's share of a beacon's light by its centre's offset from the beacon in pixels: the star's tent (starSplatWeight)
* for a lens under a pixel across, the lens's disc (its pixel-wide antialiased edge at the radius, over pi R^2) from two,
* mixed between. Sums to 1 over the pixels exactly for the tent, within a few per cent for the disc.
*/
float beaconSplatWeight(const Vec2& offset, float lensPixels);

// How far from the beacon, in pixels, its square must reach to hold every pixel with a share: the tent's one, or the disc's edge.
float beaconExtentPixels(float lensPixels);
