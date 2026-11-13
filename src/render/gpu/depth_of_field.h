#pragma once

#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/camera_finish.h"
#include "render/gpu/depth_of_field_shared.h"
#include "render/gpu/upscaling.h"

#include <array>
#include <functional>

/*
* Depth of field: a real lens brings only one distance to a sharp point on the sensor; a point nearer or farther lands
* as a disc, the circle of confusion, as wide as the aperture's cone where it meets the sensor. With a thin lens of focal
* length f and aperture A = f / N focused at S1, a point at S2 lands as a disc of diameter
*
*   c = A f / (S1 - f) |S2 - S1| / S2 = A f |u - w| / (1 - f u),   u = 1 / S1, w = 1 / S2,
*
* linear in the inverse distance (dioptres), which is why the focus eases in dioptres too. Signed here: positive behind
* the focus (the far field, at most A f u / (1 - f u) at infinity), negative in front (the near field, growing without
* bound towards the lens). Its settings in CameraSettings, off by default; the lens's focal length from the field of
* view and a 24 mm tall sensor.
*
* The blur, after the anti-aliasing at output size (before motion blur), as scatter-as-gather (Jimenez 2014): every
* point really spreads its light over its own disc, which a gather (each pixel averaging within its own radius) gets
* wrong at edges. So each pixel gathers out to the largest radius that could reach it and counts a sample only where the
* sample's own disc covers the pixel, in two layers: the far field behind (no sample reaching farther than the pixel's
* own blur, so a sharp thing in front keeps its edge) and the near field over it, with its coverage as alpha, so a
* blurred foreground turns see-through at its edges over what is behind. The passes, at half resolution (blur pixels):
*
* 1. The focus (depth_of_field_focus.comp): the inverse distance under the screen's centre (or the set one), eased.
* 2. Prepare (depth_of_field_prepare.comp): each blur pixel's light (the 2 x 2 output pixels' mean) and blur radius
*    (the nearest of the four's), and each tile's longest near-field radius.
* 3. Each tile's neighbourhood's longest near radius, of the 3 x 3 tiles round it (depth_of_field_neighbours.comp).
* 4. The gather (depth_of_field.comp): DEPTH_OF_FIELD_SAMPLES samples on rings out to the larger of the pixel's own
*    radius and the neighbourhood's near reach, into the far and near layers.
* 5. A 3 x 3 median of each layer (depth_of_field_median.comp), against lone bright samples.
* 6. The composite at output size (depth_of_field_composite.comp): the sharp pixel or the far layer by the pixel's own
*    radius, the near layer over it.
*
* These functions mirror depth_of_field.glsl and the passes, tested here without a GPU and against it by the GPU tests.
*/

// The focal length (metres) of a lens giving this vertical field of view on the 24 mm tall sensor.
float lensFocalLength(float verticalFovRadians);

// The textbook thin lens's circle of confusion on the sensor (metres, signed: positive behind the focus), for a point
// this far away along the view with the lens focused at focus (metres, beyond the focal length).
float circleOfConfusion(float distance, float focus, float focalLength, float fNumber);

// The blur radius in blur pixels per unit of A f (u - w) / (1 - f u) on the sensor: half the diameter, as a share of the
// sensor's height, times the blur grid's height (half the output's).
float depthOfFieldLensScale(float focalLength, float fNumber, float outputHeight);

// The largest inverse distance (dioptres) the lens focuses at: no nearer than DEPTH_OF_FIELD_NEAREST_FOCUS, nor than
// twice the focal length.
float nearestFocusInverse(float focalLength);

// The blur radius (blur pixels, signed as the circle of confusion) of a point at this inverse distance (0 the sky),
// focused at inverseFocus; at most DEPTH_OF_FIELD_LARGEST_RADIUS either way.
float blurRadius(float inverseDistance, float inverseFocus, float focalLength, float lensScale);

// The focus's inverse distance after seconds easing from current towards target with this time constant (seconds,
// 0 or less: straight there), as a lens motor eases it.
float easeFocus(float current, float target, float seconds, float timeConstant);

// The share of a pixel a disc of this radius (its sign ignored) covers at this distance (blur pixels): all of it until
// DEPTH_OF_FIELD_EDGE inside the edge, none from that far outside, linear between (a straight edge across a pixel).
float blurCoverage(float radius, float distance);

// Sample step of ring (1..DEPTH_OF_FIELD_RINGS) as an offset in the unit disc: 8 ring steps round the middle of the
// ring's annulus, (ring - 1/2) / DEPTH_OF_FIELD_RINGS out, odd rings turned by half a step.
Vec2 blurRingOffset(int ring, int step);

// The share of the unit disc's area one sample of this ring stands for (ring 0, the pixel's own sample, shares ring 1's
// annulus with its 8): (2 ring - 1) / (8 ring RINGS^2), and 1 / (9 RINGS^2) for rings 0 and 1, adding up to 1.
float blurRingShare(int ring);

// A sample's weight in the far layer, given its radius, the pixel's and their distance: none from the near field; a
// sample's reach no farther than the pixel's own blur (unless the pixel itself is in the near field, whose background
// is what lies behind it), so the background never spreads over a sharper thing in front.
float farSampleWeight(float sampleRadius, float pixelRadius, float distance);

// A scattering point's weight at a pixel this far away, per unit of its ring's share, out to a gather of this reach: its
// disc's coverage of the pixel (blurCoverage), its light spread over the disc's area (reach^2 / radius^2 of the reach's,
// the radius at least a pixel). Shared by the near layer and frosted glass (render/gpu/transparency.h).
float scatterWeight(float radius, float distance, float reach);

// A sample's weight in the near layer per unit of its ring's share, out to a gather of this reach: a near-field sample
// covering the pixel, its light spread over its disc's area (reach^2 / radius^2 of the reach's, the radius at least a
// pixel). Summed over the gather's samples, each times its share, it is the near field's coverage: its alpha.
float nearSampleWeight(float sampleRadius, float distance, float reach);

// The median of nine values, for the median pass.
float medianOfNine(std::array<float, 9> values);

// The composite at an output pixel with this blur radius: the sharp pixel or the far layer (all far by twice the
// in-focus radius), the near layer over it by its alpha.
Vec3 depthOfFieldComposite(Vec3 sharp, float radius, Vec3 far, Vec3 nearColour, float nearAlpha);

// A blur pixel as the prepare pass writes it: the light and the blur radius.
struct DepthOfFieldTexel {
    Vec3 colour;
    float radius = 0.0f;
};

// The prepare pass's blur pixel from its 2 x 2 output pixels: their mean light, the nearest one's radius (the
// smallest), so a blurred background never takes in a sharp edge's pixels.
DepthOfFieldTexel prepareBlurTexel(const std::array<Vec3, 4>& colours, const std::array<float, 4>& radii);

// The gather's two layers at a blur pixel.
struct DepthOfFieldLayers {
    Vec3 far;
    Vec3 nearColour;
    float nearAlpha = 0.0f;
};

using DepthOfFieldPrepared = std::function<DepthOfFieldTexel(int x, int y)>;

/*
* The gather (depth_of_field.comp) at blur pixel (x, y) of a grid this size, given its tile neighbourhood's longest
* near-field radius: the reach is the larger of that and the pixel's own radius; under DEPTH_OF_FIELD_IN_FOCUS the pixel
* is copied, else the rings' samples out to the reach plus DEPTH_OF_FIELD_EDGE (each at the nearest pixel to its place,
* kept on the grid, at its ring's distance) are weighed, each times its ring's share, into the far layer (its weights'
* mean, or the pixel's own light if none) and the near layer (its weights' mean, alpha their sum up to 1).
*/
DepthOfFieldLayers depthOfFieldAt(int x, int y, int width, int height, float nearReach, const DepthOfFieldPrepared& prepared);

// The blur grid over an output picture (half its size, rounded up), and its tiles (DEPTH_OF_FIELD_TILE a side).
RenderSize depthOfFieldGrid(RenderSize output);
RenderSize depthOfFieldTiles(RenderSize output);

// The passes' record, std430.
struct GpuDepthOfFieldPass {
    float inverseViewProjection[16]; // camera-relative, as the frame was drawn (jitter included), for view depths
    float renderSize[4];             // the depth image's size, and its inverse
    float outputSize[4];             // the picture's size, and its inverse
    float lens[4];                   // x: the focal length (m), y: the lens scale, z: the nearest focus's inverse, w: 1 to snap the focus
    float focus[4];                  // x: the set focus's inverse, y: 1 for autofocus, z: seconds since last frame, w: the time constant
    float options[4];                // x: 1 to tint the blur (near blue, far orange)
};

static_assert(sizeof(GpuDepthOfFieldPass) == 144, "GpuDepthOfFieldPass must match the depth of field passes' Pass block");

GpuDepthOfFieldPass packDepthOfFieldPass(const Mat4& inverseViewProjection, RenderSize render, RenderSize output, float verticalFovRadians,
                                         const CameraSettings& camera, float frameSeconds, bool snap);
