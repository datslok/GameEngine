#pragma once

#include "math/vec3.h"
#include "render/gpu/ray_tracing_shared.h"
#include "scene/camera.h"
#include "scene/frame_description.h"

#include <array>
#include <cstdint>

/*
* Hardware ray tracing (phase 6d), the parts that do not call the graphics API. A shader asks the GPU what a ray hits
* first (a ray query) and gets the distance, the instance, the triangle, where on it (barycentric coordinates) and whether
* it hit the front or the back. The GPU searches two levels of acceleration structure: one per mesh (the bottom level,
* a tree of boxes around its triangles, built once at upload) and one per frame (the top level, a list of instances, each
* a mesh's structure placed by a matrix, built from the frame's draws). This file packs those instances and holds the
* rules both levels share with the tests.
*/

inline constexpr std::uint32_t solidRayMask = RAY_MASK_SOLID;
inline constexpr std::uint32_t visualOnlyRayMask = RAY_MASK_VISUAL_ONLY;
inline constexpr std::uint32_t staticRayMask = RAY_MASK_STATIC;
inline constexpr std::uint32_t glassRayMask = RAY_MASK_GLASS;
inline constexpr std::uint32_t everythingRayMask = RAY_MASK_EVERYTHING;

// An instance's number has 24 bits.
inline constexpr std::uint32_t maxRayInstances = 1u << 24;

// Which rays see a drawn object: shadow casters are solid (they block light, so light-gathering rays see them); the
// rest only rays that want everything drawn (reflections, the debug view). Static shadow casters carry the static bit
// as well, for light probe relocation. Transparent ones (glass) carry only the glass bit: rays that ask for glass pass
// through each pane, tinted by it (render/gpu/transparency.h); the rest never see it.
std::uint32_t rayMaskFor(const DrawItem& draw);

/*
* One instance as the top-level structure reads it, byte for byte (Vulkan's VkAccelerationStructureInstanceKHR, checked
* there): the model matrix's top three rows, row-major; the instance number (24 bits) under the mask (8); the shader
* record offset (24 bits, unused by ray queries) under the instance flags (8); the mesh's structure's GPU address.
*/
struct GpuRayInstance {
    float transform[12];
    std::uint32_t indexAndMask = 0;
    std::uint32_t recordAndFlags = 0;
    std::uint64_t accelerationStructure = 0;
};

static_assert(sizeof(GpuRayInstance) == 64, "GpuRayInstance must match VkAccelerationStructureInstanceKHR");

// Vulkan's VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR (checked there): every hit on the instance is a candidate the
// shader decides on, as a cut-out's must be (its texel's alpha tested), instead of being taken at once.
inline constexpr std::uint32_t rayInstanceNotOpaque = 0x8;

// The draw as instance number instanceNumber (the frame's instance i, so a hit finds its GpuInstance), with its mask and
// the given flags, and not opaque if it is a cut-out or glass (each pane a candidate, tinting the light and passed over). Throws std::out_of_range for a number beyond 24 bits.
GpuRayInstance packRayInstance(const DrawItem& draw, std::uint32_t instanceNumber, std::uint64_t meshStructure, std::uint32_t flags = 0);

/*
* Whether a ray along direction hits the front of the triangle a, b, c (in the world): the side its corners turn
* anticlockwise when seen from, which faces out of a closed mesh. handedness is the instance's (-1 for a mirrored model
* matrix, whose corners turn the other way in the world while its outside stays outside).
*/
bool hitsFrontFace(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& direction, float handedness);

// What the ray tracing debug view shows, in place of the lit scene: off, each pixel's first hit's normal, its albedo
// (base colour), or whether the ray met a front (green) or a back face (red).
enum class RayDebugView {
    Off = RAY_DEBUG_OFF,
    Normals = RAY_DEBUG_NORMALS,
    Albedo = RAY_DEBUG_ALBEDO,
    Faces = RAY_DEBUG_FACES
};

inline constexpr std::array<const char*, 4> rayDebugViewNames{"Off", "Normals", "Albedo", "Faces"};

/*
* The camera for the debug view's rays, as its push constants (std430): right and up scaled by the tangents of half
* the view's width and height, forward, the camera's position with the view in w, and the screen's size. A pixel's
* ray runs from the position along forward + x right + y up for its place on screen, x and y from -1 to 1.
*/
struct GpuRayDebugView {
    float right[4];
    float up[4];
    float forward[4];
    float originAndView[4];
    float screen[4];
};

static_assert(sizeof(GpuRayDebugView) == 80, "GpuRayDebugView must match ray_debug.frag's push constants");

GpuRayDebugView packRayDebugView(const Camera& camera, RayDebugView view, std::uint32_t screenWidth, std::uint32_t screenHeight);

// The direction (not normalised) of the camera's ray through a point on screen, x and y from -1 (left, bottom) to 1.
// Mirrors cameraRayDirection in ray_tracing.glsl.
Vec3 cameraRayDirection(const Vec3& right, const Vec3& up, const Vec3& forward, float x, float y);

// The three corners' weights for a hit's barycentric coordinates (the weights of the second and third corners).
// Mirrors hitCornerWeights in ray_tracing.glsl.
Vec3 hitCornerWeights(float second, float third);
