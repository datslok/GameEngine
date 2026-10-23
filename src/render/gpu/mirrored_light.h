#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/mirrored_light_shared.h"
#include "render/gpu/upscaling.h"
#include "scene/frame_description.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

/*
* Mirrored lights: light that reaches a surface by way of one reflection off a smooth shiny one, a flashlight shone
* into a mirror lighting the floor it points at. A caustic: neither direct lighting (straight lines only) nor the light
* probes (which see a mirror's own dull diffuse colour) carry it.
*
* Found by manifold next event estimation (Hanika, Droske and Fascione 2015). For a lit point x and a light at L, the
* light arrives by way of the point y on the shiny surface where the law of reflection holds: the half vector
* h = unit(x - y) + unit(L - y) lies along the surface's normal there, so its two tangential components are zero (the
* constraint). Newton's method slides y over the surface until they are, a ray from x through each new guess putting it
* back onto the surface (and proving the way from x to it clear). The first step is exact for a flat mirror: the point
* where the line from x to L's image behind the tangent plane crosses it.
*
* The brightness: the light's intensity I sends I dOmega into the solid angle dOmega round its ray to y, which lands on
* an area dA at x (across the ray), so the irradiance is I dOmega / dA. Through the constraint's Jacobian J (implicit
* differentiation), dA / dOmega = dx^2 dL^2 |det J| / cos^2 theta, the spread area: for a flat mirror (dx + dL)^2, the
* image light's squared distance; a convex mirror spreads the beam (more), a concave one focuses it (less, 0 at the
* focus). It takes the place of distance^2 in the lights' falloff, 1 / (area + sourceRadius^2), the source's size
* keeping a focus finite as it does a light's own neighbourhood: a surface cannot be lit brighter than by the source
* filling its whole sky.
*
* Rough mirrors (brushed steel, polished stone) reflect a light from a patch round that point, its footprint: the facets
* there are tilted by the roughness, so some of them send the light on to x. Treating the facets' slopes as a Gaussian
* (Beckmann's, as wide as GGX's alpha), the light is I F a^2 / (4 dx^2 dL^2) times the integral over the mirror of the
* slopes' density at each point's half vector, a = h.n. Near the point of reflection the half vector's tilt is J d / a for
* a step d, so the footprint is a Gaussian on the mirror with covariance s^2 (J^T J)^-1 (s^2 = a^2 alpha^2 / 2 plus the
* bulb's size), and on a mirror bigger than it the integral is exactly the sharp answer: a rough mirror only softens the
* reflection's edges (the mirror's own, a spotlight's cone, shadows). A few points of the footprint each frame, a ray to
* each proving it on the mirror and in the light, estimate the share that is (importance sampling, with the footprint
* held to the mirror's size so a focus stays finite); temporal anti-aliasing averages the frames. Gaussian slopes drop
* GGX's long tail, which matters little up to the roughness this is used for.
*
* The pass runs between the depth prepass and the main pass, on GPUs with ray queries (mirrored_light.comp), and leaves
* each pixel the light arriving by way of the mirrors and its direction, which the main pass shades as one more light.
* The search itself (mirrored_light_search.glsl) is shared: the light probes' rays and traced reflections add it at
* their hits, and the reflection probe's traced capture at its texels, so the lit spot bounces and shows in reflections.
* This is the API-free part, mirrored by assets/shaders/mirrored_light.glsl and checked against it by the GPU tests.
*/

struct MirroredLightSettings {
    // On GPUs with ray queries: fast lights (the flashlight) reflected off smooth shiny surfaces light what they point at.
    // On by default (the Ultra preset's, the user's choice on Oct 23 2026; off below Ultra); ignored without ray queries
    // or without the depth prepass.
    bool enabled = true;

    // Whether the light probes bounce it on (ddgi_mirror.comp): the lit spot lights the room round it. Costs a ray and
    // the search for each fast probe ray's hit near a fast light (about 2 ms at 2560 x 1440 on the Ultra preset's 1024
    // fast rays a probe, a quarter of that on High).
    bool bounce = true;

    // Surfaces up to this roughness reflect lights, fading out over MIRROR_ROUGHNESS_FADE more; past
    // MIRROR_FOOTPRINT_ROUGHNESS each point's light comes from a footprint, which softens the reflected spot's edges.
    float mirrorRoughness = 0.25f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(MirroredLightSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a mirror roughness outside 0..1.
void validateMirroredLights(const MirroredLightSettings& settings);

// A unit normal and two unit tangents across it, all at right angles (Duff et al. 2017: no branch on the normal, and no
// break where a helper axis would line up with it).
struct TangentFrame {
    Vec3 normal;
    Vec3 tangent;
    Vec3 bitangent;
};

TangentFrame tangentFrame(const Vec3& normal);

// A 2 x 2 matrix in a tangent frame: xy is row tangent, column bitangent.
struct TangentMatrix {
    float xx = 0.0f;
    float xy = 0.0f;
    float yx = 0.0f;
    float yy = 0.0f;
};

float tangentDeterminant(const TangentMatrix& matrix);

/*
* How a smoothed triangle's normal turns as a point moves over it (the shape operator, dn = S dy), in frame's tangents:
* from its two edges and the changes of its interpolated (not normalised) normal along them, whose length at the point
* is normalLength. Positive for a surface curving away from its normal (convex, a ball's 1 / radius); zero for a flat
* triangle. Made symmetric, as a surface's shape operator is.
*/
TangentMatrix triangleCurvature(const Vec3& edge1, const Vec3& edge2, const Vec3& normalChange1, const Vec3& normalChange2, float normalLength,
                                const TangentFrame& frame);

// A point on a shiny surface: where it is, its frame, and how it curves there.
struct MirrorSurface {
    Vec3 position;
    TangentFrame frame;
    TangentMatrix curvature;
};

// The constraint at a point of the surface: the half vector's components along its tangent and bitangent.
Vec2 mirrorConstraint(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface);

// The constraint's change as the point moves over the surface by a step in its tangents (both tangential components).
TangentMatrix mirrorJacobian(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface);

// Newton's next guess: the point plus the tangential step that zeroes the constraint to first order. None where the
// Jacobian is singular (a focus).
std::optional<Vec3> mirrorNewtonTarget(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface);

// The point of reflection off the plane through a point with this unit normal: where the line from the receiver to the
// light's image behind the plane crosses it. None unless both are in front of the plane.
std::optional<Vec3> planeMirrorPoint(const Vec3& receiver, const Vec3& light, const Vec3& point, const Vec3& normal);

// dx^2 dL^2 |det J| / cos^2 theta at a point of reflection: what the light's squared distance is for a direct light.
float mirrorSpreadArea(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface);

// Whether a surface curves less than MIRROR_FLAT_CURVATURE every way there, so the flat mirror step is exact.
bool isFlatMirror(const TangentMatrix& curvature);

// The cosine of the angle of incidence at a point of reflection: negative with the receiver behind the surface.
float mirrorIncidence(const Vec3& receiver, const MirrorSurface& surface);

// The window that ends a light at its range (lighting.glsl's rangeWindow), applied to the unfolded path, times the
// intensity (the cone's share included) and the mirror's reflectance, over the spread area plus the source's size squared.
Vec3 mirroredIrradiance(const Vec3& intensity, const Vec3& reflectance, float spreadArea, float pathLength, float range, float sourceRadius);

// Whether a light can reach a receiver by way of anything in a sphere: the shortest such path is within its range.
bool mirrorPairMayReach(const Vec3& receiver, const Vec3& centre, float radius, const Vec3& light, float range);

// The surface where a ray from the receiver towards a point first meets the mirror, or none where it meets something
// else first or misses it: the pass traces it; the tests intersect shapes.
using MirrorProjection = std::function<std::optional<MirrorSurface>(const Vec3& towards)>;

// What the search found: the point of reflection, the Newton steps it took after the first, and whether the point is on
// the mirror; off it, it is on a flat mirror's plane past its edge, where only a rough mirror's footprint gives light.
struct MirrorPath {
    MirrorSurface surface;
    int steps = 0;
    bool onMirror = true;
};

/*
* The search, as mirrored_light_search.glsl runs it: the ray towards seed puts the first guess on the surface, the flat
* mirror step moves it (kept only if it lands on the surface; on a flat surface, where it is exact, a point off the
* mirror if it does not), then Newton steps until the constraint is within the tolerance, each step halved while its
* point falls off the surface. None if it never lands, does not converge, or the receiver or the light is behind the
* surface at the end.
*/
std::optional<MirrorPath> solveMirrorPath(const Vec3& receiver, const Vec3& light, const Vec3& seed, const MirrorProjection& project);

// The share of a surface's mirrored light kept at its roughness: all of it up to the setting's limit, fading smoothly
// to none over MIRROR_ROUGHNESS_FADE more.
float mirrorRoughnessFade(float roughness, float limit);

// Whether a surface this rough reflects a light from a footprint rather than from its one point of reflection.
bool usesMirrorFootprint(float roughness);

// Point index of count of a standard 2D normal (mean 0, variance 1 each way), for a frame's turn and stratum (0..1):
// stratified in radius, spread evenly round, each point over all frames exactly normal.
Vec2 gaussianSamplePoint(int index, int count, float turn, float stratum);

/*
* A rough mirror's footprint round a point of reflection, to sample (importance sampling) and weigh. It is drawn from a
* Gaussian with precision J^T J / s^2 + I / R^2: the true footprint held to the mirror's size R, so the long way of a
* focus's footprint stays on the mirror. offset* is the upper triangular square root of its covariance (the inverse of
* the precision's Cholesky factor, transposed), tangent row first; a point's weight, the true density's ratio to the
* drawn one, is weightScale exp(|d|^2 / 2R^2).
*/
struct MirrorFootprint {
    float offsetXX = 0.0f;
    float offsetXY = 0.0f;
    float offsetYY = 0.0f;
    float weightScale = 0.0f;
    float inverseSizeSquared = 0.0f;
};

// The footprint for facets of GGX width alpha (above 0), a light whose glowing part is emitterRadius across (the bulb,
// as for soft shadows: not sourceRadius, the falloff's dial, often tuned far larger), on a mirror of this size (its
// bounding sphere's radius).
MirrorFootprint mirrorFootprint(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface, float alpha, float emitterRadius, float mirrorSize);

// A standard normal pair's step from the point of reflection along the surface's tangent and bitangent.
Vec2 footprintOffset(const MirrorFootprint& footprint, const Vec2& standardNormal);

// The weight of a step: on a mirror much bigger than the footprint, 1 / |det J| for every step.
float footprintWeight(const MirrorFootprint& footprint, const Vec2& offset);

// How much of the light reaches a point on the mirror from there: the spotlight's cone, 0 where a shadow ray is blocked.
using MirrorLightReach = std::function<float(const MirrorSurface& at)>;

/*
* The footprint's samples for one frame, as mirrored_light_search.glsl takes them: each step from the point of
* reflection, a ray from the receiver towards it (project) proving it on the mirror, and the light reaching it there;
* the mean of their weights times that light. The integral of the slopes' density over the lit mirror.
*/
float footprintLight(const MirrorSurface& centre, const MirrorFootprint& footprint, int samples, float turn, float stratum, const MirrorProjection& project,
                     const MirrorLightReach& reach);

// A rough mirror's light: intensity (the cone's share not included: footprintLight has it) times reflectance and the
// range's window on the unfolded path, times (h.n)^2 / (4 dx^2 dL^2) times the footprint's mean weight.
Vec3 roughMirroredIrradiance(const Vec3& intensity, const Vec3& reflectance, const Vec3& receiver, const Vec3& light, const MirrorSurface& surface,
                             float meanWeight, float range);

// A light and a shiny surface that may light the frame through it: the light's kind and slot in the light buffers, the
// draw (the frame's instance), and the draw's bounding sphere measured from the camera.
struct MirrorPair {
    int lightKind = MIRROR_POINT_LIGHT;
    int slot = 0;
    std::uint32_t draw = 0;
    Vec3 centre;
    float radius = 0.0f;
};

/*
* Each fast light (lights.fastPoints, lights.fastSpots) with each shadow-casting draw not past the setting's roughness
* and its fade whose bounds its range (and a spotlight's cone) reach, nearest first (the gap between the light and the
* sphere), at most MIRROR_MAX_PAIRS. The lights' positions are measured from the camera at cameraOrigin, as the draws'
* are not.
*/
std::vector<MirrorPair> chooseMirrorPairs(const GpuLights& lights, std::span<const DrawItem> draws, const Vec3& cameraOrigin,
                                          const MirroredLightSettings& settings);

// One pair (std430): x the light's kind, y its slot, z the draw; the bounds measured from the camera.
struct GpuMirrorPair {
    std::int32_t light[4]{};
    float bounds[4]{};
};

// The frame's pairs (std430, scene set 1, binding 24, read by everything that runs the search): their count, the
// setting's roughness limit, then the pairs.
struct GpuMirrorPairs {
    std::int32_t counts[4]{};
    float options[4]{};
    GpuMirrorPair pairs[MIRROR_MAX_PAIRS];
};

// The pass record (std430, a storage buffer): the camera-relative view-projection's inverse (jitter included), and the
// render size and its inverse.
struct GpuMirroredLightPass {
    float inverseViewProjection[16];
    float size[4];
};

static_assert(sizeof(GpuMirrorPair) == 32, "GpuMirrorPair must match the shader's MirrorPair");
static_assert(sizeof(GpuMirrorPairs) == 32 + 32 * MIRROR_MAX_PAIRS, "GpuMirrorPairs must match mirrored_light_search.glsl's MirrorPairs");
static_assert(sizeof(GpuMirroredLightPass) == 80, "GpuMirroredLightPass must match mirrored_light.comp's Pass");

// Throws std::length_error for more than MIRROR_MAX_PAIRS pairs.
GpuMirrorPairs packMirrorPairs(std::span<const MirrorPair> pairs, const MirroredLightSettings& settings);

GpuMirroredLightPass packMirroredLightPass(const Mat4& inverseViewProjection, RenderSize size);
