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
* The pass runs between the depth prepass and the main pass, on GPUs with ray queries (mirrored_light.comp), and leaves
* each pixel the light arriving by way of the mirrors and its direction, which the main pass shades as one more light.
* This is the API-free part, mirrored by assets/shaders/mirrored_light.glsl and checked against it by the GPU tests.
*/

struct MirroredLightSettings {
    // On GPUs with ray queries: fast lights (the flashlight) reflected off smooth shiny surfaces light what they point at.
    // Off by default, as every ray-traced feature; ignored without ray queries or without the depth prepass.
    bool enabled = false;

    // Surfaces up to this roughness reflect lights, as perfect mirrors: rougher ones would spread the reflected spot.
    float mirrorRoughness = 0.1f;
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

// What the search found: the point of reflection, and the Newton steps it took after the first.
struct MirrorPath {
    MirrorSurface surface;
    int steps = 0;
};

/*
* The search, as mirrored_light.comp runs it: the ray towards seed puts the first guess on the surface, the flat
* mirror step moves it (kept only if it lands on the surface; on a flat surface, where it is exact, none if it does
* not), then Newton steps until the constraint is within the tolerance, each step halved while its point falls off the
* surface. None if it never lands, does not converge, or the receiver or the light is behind the surface at the end.
*/
std::optional<MirrorPath> solveMirrorPath(const Vec3& receiver, const Vec3& light, const Vec3& seed, const MirrorProjection& project);

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
* Each fast light (lights.fastPoints, lights.fastSpots) with each shadow-casting draw no rougher than the setting whose
* bounds its range (and a spotlight's cone) reach, nearest first (the gap between the light and the sphere), at most
* MIRROR_MAX_PAIRS. The lights' positions are measured from the camera at cameraOrigin, as the draws' are not.
*/
std::vector<MirrorPair> chooseMirrorPairs(const GpuLights& lights, std::span<const DrawItem> draws, const Vec3& cameraOrigin,
                                          const MirroredLightSettings& settings);

// One pair in the pass record (std430): x the light's kind, y its slot, z the draw; the bounds measured from the camera.
struct GpuMirrorPair {
    std::int32_t light[4]{};
    float bounds[4]{};
};

// The pass record (std430, a storage buffer): the camera-relative view-projection's inverse (jitter included), the
// render size and its inverse, the pairs' count, and the pairs.
struct GpuMirroredLightPass {
    float inverseViewProjection[16];
    float size[4];
    std::int32_t counts[4];
    GpuMirrorPair pairs[MIRROR_MAX_PAIRS];
};

static_assert(sizeof(GpuMirrorPair) == 32, "GpuMirrorPair must match the shader's MirrorPair");
static_assert(sizeof(GpuMirroredLightPass) == 96 + 32 * MIRROR_MAX_PAIRS, "GpuMirroredLightPass must match mirrored_light.comp's Pass");

// Throws std::length_error for more than MIRROR_MAX_PAIRS pairs.
GpuMirroredLightPass packMirroredLightPass(const Mat4& inverseViewProjection, RenderSize size, std::span<const MirrorPair> pairs);
