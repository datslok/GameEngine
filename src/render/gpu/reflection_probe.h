#pragma once

#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/ray_tracing.h"
#include "render/gpu/reflection_probe_shared.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

/*
* The reflection probe: a 360-degree picture of the surroundings taken where the camera is, for the reflections the
* screen cannot give (render/gpu/screen_reflections.h). It is a cube of six square pictures, one per face, each a camera
* looking along one axis with a 90-degree view, keeping the light and how far away each surface is.
*
* A picture taken from one point is wrong seen from anywhere else (parallax): a shiny point P elsewhere wants what its
* mirror ray meets, not what the probe sees in the same direction. The stored distances fix it (distance impostors,
* Szirmay-Kalos et al. 2005): the ray P + R t meets a surface where it first goes behind what the probe saw in the
* direction of P + R t, which a march along the ray finds. Rough surfaces read a blurred copy: each smaller level
* of the cube averages the light over a rougher surface's lobe of directions (prefiltering, Karis 2013).
*
* The cube follows Vulkan's addressing (the faces +X, -X, +Y, -Y, +Z, -Z, and where a direction lands on each), so the
* shaders read it as a samplerCube. This is the API-free part, mirrored by assets/shaders/reflection_probe.glsl.
*/

inline constexpr int cubeFaceCount = 6;

// One face's camera: the way it looks, and the directions its picture's columns (s) and rows (t, down) run.
struct CubeFaceAxes {
    Vec3 forward;
    Vec3 right; // s grows along it
    Vec3 up;    // t shrinks along it (rows count down the picture)
};

CubeFaceAxes cubeFaceAxes(int face);

// Where a direction lands on the cube, as Vulkan picks it: the face of its largest component, and s, t in 0..1.
struct CubeTexel {
    int face = 0;
    Vec2 st;
};

CubeTexel cubeTexelOf(const Vec3& direction);

// The direction (not normalised) through a point s, t of a face: the inverse of cubeTexelOf.
Vec3 cubeTexelDirection(int face, Vec2 st);

/*
* A face's view-projection for points measured from the probe, reversed-Z from near to far: its picture comes out as
* the cube stores it. The cube's faces are mirror images of an ordinary camera's (a determinant of -1), so triangles
* turn the other way round and the capture culls front faces.
*/
Mat4 cubeFaceViewProjection(int face, float nearPlane, float farPlane);

// A face's camera rays, for drawing the sky over its background (sky.frag), from origin on a square of this size.
GpuRayDebugView packCubeFaceRays(int face, const Vec3& origin, std::uint32_t size);

// The distance from the probe to what a texel saw, from its depth along the face's axis (view distance) and its unit
// direction: the axis distance over the direction's largest component.
float probeRadialDistance(float viewDistance, const Vec3& unitDirection);

// How far the ray's point at t (position + direction t, measured from the probe) is behind the distance the probe saw
// in its direction: negative while it is in front.
float parallaxMismatch(const Vec3& position, const Vec3& direction, float t, float storedDistance);

/*
* The probe's distances on the CPU, as the capture and the distance passes leave them on the GPU: level 0 the distance
* at each texel's centre, each level above the nearest of the 2x2 texels below it (a pyramid like screen-space
* reflections' depth pyramid), so one texel of a level says how near anything is over its whole patch of directions.
*/
struct ProbeDistanceCube {
    std::uint32_t size = 0;
    std::vector<std::array<std::vector<float>, 6>> levels; // [level][face][y * side + x]

    // The texel a direction falls in at a level, as a samplerCube reads it with nearest filtering.
    float at(const Vec3& direction, std::uint32_t level) const;
};

// A cube of this size (a power of two) from the distance a scene has in each direction, with its whole pyramid.
ProbeDistanceCube buildProbeDistanceCube(std::uint32_t size, const std::function<float(const Vec3&)>& distance);

// The angle one texel at a face's centre spans on a cube of this size: (pi / 2) / size.
float probeTexelAngle(std::uint32_t size);

// The pyramid's level whose texels are PROBE_CONE_MARGIN times as wide as a cone of this angle (radians), on a cube
// of this size, so the texels that hold the cone's two ends cover all of it between (twice for a face's corners,
// where texels are narrower, and twice for an arc crossing a texel's corner); at most the top level.
std::uint32_t probeConeLevel(float angle, std::uint32_t size, std::uint32_t levels);

// The direction from the probe to the ray's point at t (normalised); the ray's own direction while t is negative.
Vec3 parallaxDirection(const Vec3& position, const Vec3& direction, float t);

// How far a crossing is trusted, from the distances the probe saw on its near and far side: fully while they agree
// within PROBE_MATCH_SHARE of the farther, not at all from PROBE_MISMATCH_SHARE (an edge the probe saw past).
float parallaxConfidence(float nearSide, float farSide);

// The search's result: the direction to read the probe in, and how far it is trusted.
struct ParallaxLookup {
    Vec3 direction;
    float confidence = 0.0f;
};

/*
* The whole search, as reflections.comp runs it, reading the distances' pyramid through distanceAt (a unit direction
* from the probe and a level), for a capture reaching far, on a cube of this size and levels: the ray leaves its
* surface until it is in front of what the probe saw; then it marches, each step checked against the cone of the
* probe's directions it sweeps, so nothing the probe saw nearer (a wall seen edge-on) is stepped over; the first step
* where it goes behind what the probe saw is narrowed down by halving. Nothing found trusts nothing.
*/
ParallaxLookup searchProbe(const Vec3& position, const Vec3& direction, float far, std::uint32_t size, std::uint32_t levels,
                           const std::function<float(const Vec3&, std::uint32_t)>& distanceAt);

// The prefiltered cube's levels for a face of this size: down to PROBE_SMALLEST_LEVEL_SIZE texels, at least one.
std::uint32_t probeLevels(std::uint32_t size);

// The roughness a level is prefiltered for (0 for the sharp top level, 1 for the smallest) and the level to read for
// a roughness, in between: linear in roughness.
float probeLevelRoughness(std::uint32_t level, std::uint32_t levels);
float probeRoughnessLevel(float roughness, std::uint32_t levels);

/*
* The level of the captured cube a prefilter sample reads (filtered importance sampling, Colbert and Krivanek 2007): as
* wide as the solid angle its share of the samples stands for, 1 / (samples pdf), against one texel's at the top level,
* 4 pi / (6 size^2); half log2 of the ratio, since a level halves the side and quarters the solid angle, plus one level,
* which keeps the sum smooth.
*/
float prefilterSourceLevel(float pdf, std::uint32_t samples, std::uint32_t size);

/*
* One prefiltered texel looking along a unit direction, for a roughness: the light read through sample (a unit
* direction and the captured cube's level) over samples directions of the GGX lobe, each weighted by n.l. Karis's split
* sum takes the normal and the view along the direction itself, which is what lets one cube serve every view.
*/
Vec3 prefilterTexel(const Vec3& direction, float roughness, std::uint32_t samples, std::uint32_t sourceSize,
                    const std::function<Vec3(const Vec3&, float)>& sample);

// The stand-in swapped for the probe's light by its confidence: what the reflection pass hands on to screen-space
// reflections as the light they replace.
Vec3 blendedStandIn(const Vec3& standIn, const Vec3& specularAlbedo, const Vec3& light, float confidence);

// The prefilter pass's push constants (probe_prefilter.comp): the level written (its size, number and roughness) and
// the captured cube's top size and levels.
struct GpuPrefilterPass {
    float target[4]; // x: the level's size, y: the level, z: its roughness, w: unused
    float source[4]; // x: the captured cube's size, y: its levels
};

static_assert(sizeof(GpuPrefilterPass) == 32, "GpuPrefilterPass must match probe_prefilter.comp's push constants");

GpuPrefilterPass packPrefilterPass(std::uint32_t size, std::uint32_t level, std::uint32_t levels, std::uint32_t sourceSize,
                                   std::uint32_t sourceLevels);
