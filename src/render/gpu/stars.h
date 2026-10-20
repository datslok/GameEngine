#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ray_tracing.h"
#include "scene/stars.h"

#include <span>
#include <vector>

/*
* Drawing the stars (scene/stars.h), the parts that do not call the graphics API; mirrored by assets/shaders/stars.glsl
* and checked against it by the GPU tests. Each star is far smaller than a pixel, so it is drawn as a splat a pixel either
* side of where it falls, its light shared between the pixels by a tent (bilinear) weight that always sums to 1 whatever
* its place inside a pixel: a star crossing pixel borders as the camera turns keeps its brightness instead of flickering.
* Its light (an illuminance, like a directional light's pi times its value) over a pixel's solid angle is the radiance
* that pixel shows.
*/

/*
* One star as the star pass reads it (std430, a storage buffer): its direction in the equatorial frame and magnitude,
* its colour of luminance 1.
*/
struct GpuStar {
    float direction[3];
    float magnitude = 0.0f;
    float colour[3];
    float unused = 0.0f;
};

static_assert(sizeof(GpuStar) == 32, "GpuStar must match Star in stars.vert");

std::vector<GpuStar> packStars(std::span<const Star> stars);

/*
* The star pass's push constants: the camera's rays (as the sky pass takes them, ray_tracing.h) and the rotation from the
* equatorial frame to the world, its rows in xyz, with the brightness and contrast in the first two rows' w.
*/
struct GpuStarView {
    GpuRayDebugView camera;
    float toWorld[3][4];
};

static_assert(sizeof(GpuStarView) == 128, "GpuStarView must match stars.vert's push constants and fit the 128 bytes every GPU allows");

GpuStarView packStarView(const GpuRayDebugView& camera, const StarrySky& sky);

// A star's light: brightness 10^(-0.4 contrast magnitude) (scene/stars.h, StarrySky).
float starLight(float magnitude, float brightness, float contrast);

/*
* Where a unit direction falls on screen, x and y from -1 to 1 as cameraRayDirection takes them (right and up scaled by
* the tangents of half the view, forward a unit vector): the inverse of that ray, x = (d . right) / (|right|^2 (d . forward)),
* likewise y. Only for directions in front (d . forward > 0).
*/
Vec2 starScreenPosition(const Vec3& right, const Vec3& up, const Vec3& forward, const Vec3& direction);

/*
* A pixel's share of a star's light by its centre's offset from the star in pixels: (1 - |x|)(1 - |y|) within a pixel
* either way, 0 beyond. Summed over the four pixels a star can touch it is exactly 1 for any offset.
*/
float starSplatWeight(const Vec2& offset);

/*
* The solid angle (steradians) a pixel spans at cosOffAxis from the view's axis, for a screen of width by height pixels:
* (2 |right| / width) (2 |up| / height) cos^3, since a pixel off axis is further away (1 / cos^2) and seen slanted (cos).
*/
float pixelSolidAngle(const Vec3& right, const Vec3& up, float width, float height, float cosOffAxis);
