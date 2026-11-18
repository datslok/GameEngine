#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/occlusion_culling_shared.h"
#include "render/gpu/screen_reflections.h"
#include "render/gpu/upscaling.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

/*
* Occlusion culling: not drawing what hides behind other things (research: docs/research/OCCLUSION.md). The depth the
* prepass drew is reduced into a pyramid whose every texel keeps the farthest depth below it (DepthReduction::Farthest),
* so an object whose nearest point is farther than that texel is behind everything there. Each object's bounding sphere
* becomes a rectangle of pixels and a nearest depth; the pyramid level where the rectangle covers at most 2 x 2 texels
* answers with four reads. Conservative: a sphere crossing the near plane, unknown bounds, or any texel showing sky (far)
* keeps the object.
*
* First stage (occlusion-measure): the camera's draws are only counted (OcclusionCounts, F4's HIDDEN line), drawing is
* unchanged. This is the API-free part, mirrored by assets/shaders/occlusion_culling.glsl.
*/

inline constexpr int occlusionRectMargin = OCCLUSION_RECT_MARGIN;
inline constexpr std::uint32_t occlusionGroupSize = OCCLUSION_GROUP_SIZE;
inline constexpr std::uint32_t occlusionMaxGroups = OCCLUSION_MAX_GROUPS;

// Pixels from (minX, minY) to (maxX, maxY), both corners included, counted from the top-left; empty when a min passes its max.
struct PixelRect {
    int minX = 0;
    int minY = 0;
    int maxX = -1;
    int maxY = -1;

    bool empty() const;
};

/*
* What the test needs of the camera: its turn (the camera-relative view: positions measured from the camera, turned but
* not moved), its lens (the projection's x and y scales), its near plane, the screen in pixels, and its depth as a
* function of distance in front: depth = depthConstant + depthPerInverseDistance / distance (the GPU's reversed-Z).
*/
struct OcclusionView {
    Mat4 view;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float nearPlane = 0.1f;
    float depthConstant = 0.0f;
    float depthPerInverseDistance = 0.0f;
    RenderSize size;
};

// From the camera-relative view and the projection in the GPU's depth range (toGpuDepthRange); throws
// std::invalid_argument for a projection that is not a perspective one.
OcclusionView makeOcclusionView(const Mat4& relativeView, const Mat4& gpuProjection, float nearPlane, RenderSize size);

/*
* The pixels a sphere (centre measured from the camera) covers, from its exact tangent planes (Mara & McGuire 2013),
* widened by occlusionRectMargin and clamped to the screen (empty when off it). None when the sphere reaches the near
* plane or behind it: it cannot be judged.
*/
std::optional<PixelRect> sphereRect(const OcclusionView& view, const Vec3& relativeCentre, float radius);

// The GPU depth of the sphere's nearest point (reversed-Z: the largest of any point on it).
float sphereNearestDepth(const OcclusionView& view, const Vec3& relativeCentre, float radius);

// The pyramid level (below levels) at which the rectangle touches at most 2 x 2 texels: its longer side rounded up to
// a power of two.
std::uint32_t occlusionLevel(const PixelRect& rect, std::uint32_t levels);

// Whether everything the farthest-depth pyramid holds under the rectangle is nearer than nearestDepth.
bool hiddenInPyramid(const DepthPyramid& pyramid, const PixelRect& rect, float nearestDepth);

// The whole test for one sphere: false for unknown bounds (radius below 0), at the near plane, or off the screen.
bool sphereHidden(const OcclusionView& view, const DepthPyramid& pyramid, const Vec3& relativeCentre, float radius);

// The pieces of the shader's maths, one number at a time as the GLSL has them (the GPU tests check these).
std::optional<PixelRect> sphereRectFromView(const Vec3& viewCentre, float radius, float scaleX, float scaleY, float nearPlane, RenderSize size);
float nearestDepthFromView(float viewZ, float radius, float depthConstant, float depthPerInverseDistance);

// What the camera's draws would lose, counting each instance once and its mesh's triangles (the chosen level's).
struct OcclusionCounts {
    std::size_t tested = 0;
    std::size_t hidden = 0;
    std::size_t testedTriangles = 0;
    std::size_t hiddenTriangles = 0;
};

// The counting pass's result, as the GPU adds into it. Must match occlusion_count.comp's Counts.
struct GpuOcclusionCounts {
    std::uint32_t tested = 0;
    std::uint32_t hidden = 0;
    std::uint32_t testedTriangles = 0;
    std::uint32_t hiddenTriangles = 0;
};

static_assert(sizeof(GpuOcclusionCounts) == 16, "GpuOcclusionCounts must match occlusion_count.comp's Counts");

OcclusionCounts unpackOcclusionCounts(const GpuOcclusionCounts& counts);

// A batch of the camera's draws: its run of draw list entries and its mesh's triangles. Must match occlusion_count.comp.
struct GpuOcclusionBatch {
    std::uint32_t firstEntry = 0;
    std::uint32_t count = 0;
    std::uint32_t triangles = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuOcclusionBatch) == 16, "GpuOcclusionBatch must match occlusion_count.comp's Batch");

std::vector<GpuOcclusionBatch> packOcclusionBatches(const std::vector<DrawBatch>& batches,
                                                    const std::function<std::uint32_t(MeshHandle)>& trianglesOf);

// The counting pass's push constants. Must match occlusion_count.comp's Pass.
struct GpuOcclusionPass {
    float view[16];  // the camera-relative view, column-major
    float lens[4];   // scaleX, scaleY, near plane, depthConstant
    float origin[4]; // xyz: the camera's position in the world (instances are in the world), w: depthPerInverseDistance
    std::int32_t sizes[4]; // the screen's width and height, the pyramid's levels, the batches
};

static_assert(sizeof(GpuOcclusionPass) == 112, "GpuOcclusionPass must match occlusion_count.comp's push constants");

GpuOcclusionPass packOcclusionPass(const OcclusionView& view, const Vec3& cameraOrigin, std::uint32_t levels, std::uint32_t batches);
