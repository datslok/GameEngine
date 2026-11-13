#pragma once

#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/camera_finish.h"
#include "render/gpu/motion_blur_shared.h"
#include "render/gpu/upscaling.h"

#include <cstdint>
#include <functional>

/*
* Motion blur: a real shutter stays open for part of each frame, so whatever moves across the sensor in that time
* smears along its path. The blur's length is the motion on screen over the frame times the shutter's share of it (180
* degrees: half). McGuire et al. 2012's reconstruction filter, as most games ship it, on the finished picture after
* anti-aliasing (its settings in CameraSettings, off by default):
*
* 1. Each output pixel's half-blur (pixels either way, at most a tile) from the velocity image (a moving object's motion)
*    or the camera's own motion from its depth, with its view depth; and each tile's longest (motion_blur_tiles.comp).
* 2. Each tile's neighbourhood's longest, of the 3 x 3 tiles round it (motion_blur_neighbours.comp).
* 3. Each pixel gathers MOTION_BLUR_SAMPLES samples along its neighbourhood's motion (motion_blur.comp), each weighed by
*    depth: a sample in front counts where its own blur reaches the pixel, one behind where the pixel's blur reaches it,
*    and two blurred alike count together. So a moving thing in front smears over the background, and a still thing in
*    front keeps its edge. A neighbourhood slower than half a pixel is copied as it is.
*
* These functions mirror motion_blur.glsl and the passes, tested here without a GPU and against it by the GPU tests.
*/

// The motion a pixel blurs by (NDC, this frame's minus last frame's): the velocity texel's, or the camera's where it
// holds TAA_NO_MOTION; with cameraMotionBlur off, only what the object adds to the camera's.
Vec2 pixelMotion(Vec2 velocityTexel, Vec2 cameraMotionNdc, bool cameraMotionBlur);

// The half-blur in output pixels: the NDC motion in pixels (y down) times the shutter's share, halved, and no longer
// than MOTION_BLUR_TILE.
Vec2 motionBlurHalfVector(Vec2 ndcMotion, Vec2 outputSize, float shutterShare);

// The longer of two motions, the first on a tie.
Vec2 largerMotion(Vec2 a, Vec2 b);

// McGuire's cone: how surely a sample this far away lies within a blur of this speed (1 at the pixel, 0 at the speed).
float motionBlurCone(float distance, float speed);

// McGuire's cylinder: 1 within the blur's reach, 0 past it, with a narrow smooth edge (5% either side).
float motionBlurCylinder(float distance, float speed);

// How surely a point at view depth a lies at or in front of one at depth b: 1 in front or level, 0 more than
// MOTION_BLUR_DEPTH_SOFTNESS of b behind it, linear between.
float motionBlurNearer(float a, float b);

// A sample's weight at this distance from the pixel, given both view depths and speeds (half-blurs, at least
// MOTION_BLUR_LEAST_MOTION): in front and reaching the pixel, behind and reached by it, or blurred alike.
float motionBlurSampleWeight(float distance, float pixelDepth, float sampleDepth, float pixelSpeed, float sampleSpeed);

// Sample index's place along the neighbourhood's motion, -1..1, the noise (0..1) moving every sample alike.
float motionBlurOffset(int index, float noise);

// What the tiles pass prepares at an output pixel: its half-blur and its view depth.
struct MotionBlurTexel {
    Vec2 half;
    float viewDepth = 0.0f;
};

using MotionBlurPicture = std::function<Vec3(int x, int y)>;
using MotionBlurPrepared = std::function<MotionBlurTexel(int x, int y)>;

/*
* The gather (motion_blur.comp) at output pixel (x, y) of a picture this size, given its tile neighbourhood's longest
* half-blur and the pixel's noise (interleavedGradientNoise): the pixel weighed by one over its own speed, plus the
* samples weighed by motionBlurSampleWeight, each at the nearest pixel to its place (kept on the picture).
*/
Vec3 motionBlurAt(int x, int y, int width, int height, Vec2 neighbourMax, float noise, const MotionBlurPicture& picture, const MotionBlurPrepared& prepared);

// The tiles' grid over an output picture: MOTION_BLUR_TILE pixels a tile, rounded up.
RenderSize motionBlurTiles(RenderSize output);

// The passes' record, std430.
struct GpuMotionBlurPass {
    float inverseViewProjection[16]; // camera-relative, as the frame was drawn (jitter included), for view depths
    float reprojection[16];          // this frame's clip space (without the jitter) to last frame's, for the camera's motion
    float renderSize[4];             // the velocity and depth images' size, and its inverse
    float outputSize[4];             // the picture's size, and its inverse
    float options[4];                // x: the shutter's share of the frame, y: 1 with the camera's motion blurred
};

static_assert(sizeof(GpuMotionBlurPass) == 176, "GpuMotionBlurPass must match the motion blur passes' Pass block");

GpuMotionBlurPass packMotionBlurPass(const Mat4& inverseViewProjection, const Mat4& reprojection, RenderSize render, RenderSize output,
                                     const CameraSettings& camera);
