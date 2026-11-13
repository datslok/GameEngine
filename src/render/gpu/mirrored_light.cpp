#include "render/gpu/mirrored_light.h"
#include "core/srgb.h"
#include "render/gpu/brdf.h"
#include "render/gpu/clusters.h"
#include "render/gpu/planar_reflection.h"
#include "render/gpu/column_major.h"
#include "render/gpu/light_falloff.h"
#include "math/angles.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

void visit(MirroredLightSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Mirrored lights", settings.enabled);
    visitor.field("Mirrored light bounce", settings.bounce);
    visitor.field("Mirror roughness", settings.mirrorRoughness, {.min = 0.0f, .max = 1.0f});
    visitor.field("Image lights", settings.imageLights);
}

// Written so not a number fails too.
void validateMirroredLights(const MirroredLightSettings& settings) {
    if (!(settings.mirrorRoughness >= 0.0f && settings.mirrorRoughness <= 1.0f)) {
        throw std::invalid_argument("Mirrored lights need a mirror roughness from 0 to 1");
    }
}

// Duff et al. 2017, "Building an Orthonormal Basis, Revisited": the sign picks the half of the sphere, so the
// division never comes near zero.
TangentFrame tangentFrame(const Vec3& normal) {
    const float sign = normal.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + normal.z);
    const float b = normal.x * normal.y * a;
    const Vec3 tangent{1.0f + sign * normal.x * normal.x * a, sign * b, -sign * normal.x};
    const Vec3 bitangent{b, sign + normal.y * normal.y * a, -normal.y};
    return TangentFrame{normal, tangent, bitangent};
}

float tangentDeterminant(const TangentMatrix& matrix) {
    return matrix.xx * matrix.yy - matrix.xy * matrix.yx;
}

/*
* Over a triangle the interpolated normal N changes linearly with the point, so a move v in its plane, written as
* a edge1 + b edge2 (least squares through the edges' Gram matrix, which also drops any part of v off the triangle's
* plane), changes N by a normalChange1 + b normalChange2. The unit normal's change is that over N's length, less its
* part along the normal, which the tangents do not see.
*/
TangentMatrix triangleCurvature(const Vec3& edge1, const Vec3& edge2, const Vec3& normalChange1, const Vec3& normalChange2, float normalLength,
                                const TangentFrame& frame) {
    const float e11 = edge1.dot(edge1);
    const float e12 = edge1.dot(edge2);
    const float e22 = edge2.dot(edge2);
    const float gram = e11 * e22 - e12 * e12;

    if (!(gram > 0.0f && normalLength > 0.0f)) {
        return TangentMatrix{};
    }

    const auto normalChange = [&](const Vec3& move) {
        const float along1 = edge1.dot(move);
        const float along2 = edge2.dot(move);
        const float a = (e22 * along1 - e12 * along2) / gram;
        const float b = (e11 * along2 - e12 * along1) / gram;
        return (normalChange1 * a + normalChange2 * b) * (1.0f / normalLength);
    };

    const Vec3 alongTangent = normalChange(frame.tangent);
    const Vec3 alongBitangent = normalChange(frame.bitangent);
    const float twist = 0.5f * (frame.tangent.dot(alongBitangent) + frame.bitangent.dot(alongTangent));
    return TangentMatrix{frame.tangent.dot(alongTangent), twist, twist, frame.bitangent.dot(alongBitangent)};
}

namespace {
    // The two unit vectors from the point of reflection and how far each end is.
    struct MirrorLegs {
        Vec3 toReceiver;
        Vec3 toLight;
        float receiverDistance;
        float lightDistance;
    };

    MirrorLegs mirrorLegs(const Vec3& receiver, const Vec3& light, const Vec3& position) {
        const Vec3 toReceiver = receiver - position;
        const Vec3 toLight = light - position;
        const float receiverDistance = toReceiver.length();
        const float lightDistance = toLight.length();
        return MirrorLegs{toReceiver * (1.0f / receiverDistance), toLight * (1.0f / lightDistance), receiverDistance, lightDistance};
    }

    // u . P v, where P = (I - wx wx^T) / dx + (I - wL wL^T) / dL: how the half vector changes (as -P) as the point moves.
    float legsProjection(const MirrorLegs& legs, const Vec3& u, const Vec3& v) {
        const float alongBoth = u.dot(v);
        return (alongBoth - legs.toReceiver.dot(u) * legs.toReceiver.dot(v)) / legs.receiverDistance +
               (alongBoth - legs.toLight.dot(u) * legs.toLight.dot(v)) / legs.lightDistance;
    }
}

Vec2 mirrorConstraint(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface) {
    const MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    const Vec3 halfway = legs.toReceiver + legs.toLight;
    return Vec2{halfway.dot(surface.frame.tangent), halfway.dot(surface.frame.bitangent)};
}

/*
* c_i = h . t_i. Moving the point by d in the tangent plane changes h by -P d, and tilts each tangent towards the
* normal by -(t_i . S d) n, which h (mostly along the normal) sees as -(h . n)(t_i . S d); the tangents' turn within
* the plane changes c only to second order where c is small.
*/
TangentMatrix mirrorJacobian(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface) {
    const MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    const float alongNormal = (legs.toReceiver + legs.toLight).dot(surface.frame.normal);
    const Vec3& t = surface.frame.tangent;
    const Vec3& b = surface.frame.bitangent;
    const TangentMatrix& s = surface.curvature;
    return TangentMatrix{-legsProjection(legs, t, t) - alongNormal * s.xx, -legsProjection(legs, t, b) - alongNormal * s.xy,
                         -legsProjection(legs, b, t) - alongNormal * s.yx, -legsProjection(legs, b, b) - alongNormal * s.yy};
}

std::optional<Vec3> mirrorNewtonTarget(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface) {
    const TangentMatrix jacobian = mirrorJacobian(receiver, light, surface);
    const float det = tangentDeterminant(jacobian);

    if (!(std::abs(det) > 0.0f)) {
        return std::nullopt;
    }

    const Vec2 constraint = mirrorConstraint(receiver, light, surface);
    const float stepTangent = -(jacobian.yy * constraint.x - jacobian.xy * constraint.y) / det;
    const float stepBitangent = -(jacobian.xx * constraint.y - jacobian.yx * constraint.x) / det;

    if (!std::isfinite(stepTangent) || !std::isfinite(stepBitangent)) {
        return std::nullopt;
    }

    return surface.position + surface.frame.tangent * stepTangent + surface.frame.bitangent * stepBitangent;
}

// The image of the light is as far behind the plane as the light is in front; the line from the receiver to it
// crosses the plane after the share of its length the receiver's height is of both heights.
std::optional<Vec3> planeMirrorPoint(const Vec3& receiver, const Vec3& light, const Vec3& point, const Vec3& normal) {
    const float receiverHeight = (receiver - point).dot(normal);
    const float lightHeight = (light - point).dot(normal);

    if (!(receiverHeight > 0.0f && lightHeight > 0.0f)) {
        return std::nullopt;
    }

    const Vec3 image = light - normal * (2.0f * lightHeight);
    return receiver + (image - receiver) * (receiverHeight / (receiverHeight + lightHeight));
}

float mirrorSpreadArea(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface) {
    const MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    const float incidence = legs.toReceiver.dot(surface.frame.normal);
    const float squared = legs.receiverDistance * legs.lightDistance;
    return squared * squared * std::abs(tangentDeterminant(mirrorJacobian(receiver, light, surface))) / (incidence * incidence);
}

bool isFlatMirror(const TangentMatrix& curvature) {
    const float flat = static_cast<float>(MIRROR_FLAT_CURVATURE);
    return std::abs(curvature.xx) < flat && std::abs(curvature.xy) < flat && std::abs(curvature.yx) < flat && std::abs(curvature.yy) < flat;
}

float mirrorIncidence(const Vec3& receiver, const MirrorSurface& surface) {
    return (receiver - surface.position).normalized().dot(surface.frame.normal);
}

// In lighting.glsl's order: the window squared, then one division.
Vec3 mirroredIrradiance(const Vec3& intensity, const Vec3& reflectance, float spreadArea, float pathLength, float range, float sourceRadius) {
    const float window = rangeWindow(pathLength, range);
    return intensity * reflectance * (window * window / (spreadArea + sourceRadius * sourceRadius));
}

bool mirrorPairMayReach(const Vec3& receiver, const Vec3& centre, float radius, const Vec3& light, float range) {
    const float shortest = std::max((receiver - centre).length() - radius, 0.0f) + std::max((light - centre).length() - radius, 0.0f);
    return shortest < range;
}

std::optional<MirrorPath> solveMirrorPath(const Vec3& receiver, const Vec3& light, const Vec3& seed, const MirrorProjection& project) {
    std::optional<MirrorSurface> surface = project(seed);

    if (!surface) {
        return std::nullopt;
    }

    // Exact for a flat mirror, and a good start on a curved one: kept only if it lands on the surface. Where a flat
    // mirror's step does not land, the point of reflection is off the mirror, on its plane; where both ends are not in
    // front of it, there is none, nor for a receiver on the plane itself (the mirror's own surface, which its reflection
    // cannot light).
    const std::optional<Vec3> image = planeMirrorPoint(receiver, light, surface->position, surface->frame.normal);
    std::optional<MirrorSurface> moved = image ? project(*image) : std::nullopt;
    const bool flat = isFlatMirror(surface->curvature);

    if (moved && flat && moved->frame.normal.dot(surface->frame.normal) < static_cast<float>(MIRROR_SAME_PLANE_COSINE)) {
        moved.reset();
    }

    if (moved) {
        surface = moved;
    } else if (flat) {
        const bool offPlane = (receiver - surface->position).dot(surface->frame.normal) > static_cast<float>(MIRROR_RAY_OFFSET);
        return image && offPlane ? std::optional<MirrorPath>{MirrorPath{MirrorSurface{*image, surface->frame, surface->curvature}, 0, false}}
                                 : std::nullopt;
    }

    for (int steps = 0;; ++steps) {
        if (mirrorConstraint(receiver, light, *surface).length() <= static_cast<float>(MIRROR_SOLVE_TOLERANCE)) {
            const bool inFront = mirrorIncidence(receiver, *surface) > 0.0f && (light - surface->position).dot(surface->frame.normal) > 0.0f;
            return inFront ? std::optional<MirrorPath>{MirrorPath{*surface, steps}} : std::nullopt;
        }

        if (steps == MIRROR_MAX_STEPS) {
            return std::nullopt;
        }

        std::optional<Vec3> target = mirrorNewtonTarget(receiver, light, *surface);

        if (!target) {
            return std::nullopt;
        }

        std::optional<MirrorSurface> next = project(*target);

        for (int halving = 0; halving < MIRROR_BACKTRACKS && !next; ++halving) {
            target = (surface->position + *target) * 0.5f;
            next = project(*target);
        }

        if (!next) {
            return std::nullopt;
        }

        surface = next;
    }
}

float mirrorRoughnessFade(float roughness, float limit) {
    return 1.0f - smoothStep(limit, limit + static_cast<float>(MIRROR_ROUGHNESS_FADE), roughness);
}

bool usesMirrorFootprint(float roughness) {
    return roughness > static_cast<float>(MIRROR_FOOTPRINT_ROUGHNESS);
}

// A share u of the way through the radii (stratified: sample index of count, the stratum placing it in its share) is
// the radius sqrt(-2 ln(1 - u)), which inverts the chi distribution with two degrees of freedom; the angle spreads the
// samples evenly round, turned by the frame's turn. Uniform u and angle make each sample exactly normal.
Vec2 gaussianSamplePoint(int index, int count, float turn, float stratum) {
    const float share = (static_cast<float>(index) + stratum) / static_cast<float>(count);
    const float radius = std::sqrt(-2.0f * std::log(1.0f - share));
    const float angle = fullTurnRadians * (turn + static_cast<float>(index) / static_cast<float>(count));
    return Vec2{radius * std::cos(angle), radius * std::sin(angle)};
}

/*
* The precision P = J^T J / s^2 + I / R^2 and its Cholesky factor L (P = L L^T): steps d = L^-T z for standard normal z
* have covariance P^-1. The determinant comes from det(A + cI) = det A + c tr A + c^2 (for 2 x 2), with
* det(J^T J) = det(J)^2, rather than from L's last entry, where a near-singular J would cancel away its digits.
*/
MirrorFootprint mirrorFootprint(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface, float alpha, float emitterRadius, float mirrorSize) {
    const MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    const float along = (legs.toReceiver + legs.toLight).dot(surface.frame.normal);
    const float sourceSpread = emitterRadius / legs.lightDistance;
    const float spread = along * along * alpha * alpha * 0.5f + 0.25f * sourceSpread * sourceSpread;
    const float inverseSizeSquared = 1.0f / (mirrorSize * mirrorSize);

    const TangentMatrix j = mirrorJacobian(receiver, light, surface);
    const float squaredXX = (j.xx * j.xx + j.yx * j.yx) / spread;
    const float squaredXY = (j.xx * j.xy + j.yx * j.yy) / spread;
    const float squaredYY = (j.xy * j.xy + j.yy * j.yy) / spread;
    const float det = tangentDeterminant(j);
    const float precisionXX = squaredXX + inverseSizeSquared;
    const float precisionDet = det * det / (spread * spread) + inverseSizeSquared * (squaredXX + squaredYY) + inverseSizeSquared * inverseSizeSquared;

    const float l11 = std::sqrt(precisionXX);
    const float l21 = squaredXY / l11;
    const float l22 = std::sqrt(precisionDet / precisionXX);
    return MirrorFootprint{1.0f / l11, -l21 / (l11 * l22), 1.0f / l22, 1.0f / (spread * std::sqrt(precisionDet)), inverseSizeSquared};
}

Vec2 footprintOffset(const MirrorFootprint& footprint, const Vec2& standardNormal) {
    return Vec2{footprint.offsetXX * standardNormal.x + footprint.offsetXY * standardNormal.y, footprint.offsetYY * standardNormal.y};
}

float footprintWeight(const MirrorFootprint& footprint, const Vec2& offset) {
    return footprint.weightScale * std::exp(0.5f * offset.dot(offset) * footprint.inverseSizeSquared);
}

float footprintLight(const MirrorSurface& centre, const MirrorFootprint& footprint, int samples, float turn, float stratum, const MirrorProjection& project,
                     const MirrorLightReach& reach) {
    float sum = 0.0f;

    for (int sample = 0; sample < samples; ++sample) {
        const Vec2 offset = footprintOffset(footprint, gaussianSamplePoint(sample, samples, turn, stratum));
        const std::optional<MirrorSurface> landed = project(centre.position + centre.frame.tangent * offset.x + centre.frame.bitangent * offset.y);

        if (!landed) {
            continue;
        }

        const float lit = reach(*landed);

        if (lit > 0.0f) {
            sum += footprintWeight(footprint, offset) * lit;
        }
    }

    return sum / static_cast<float>(samples);
}

// In mirroredIrradiance's order: the window squared, then the rest.
Vec3 roughMirroredIrradiance(const Vec3& intensity, const Vec3& reflectance, const Vec3& receiver, const Vec3& light, const MirrorSurface& surface,
                             float meanWeight, float range) {
    const MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    const float along = (legs.toReceiver + legs.toLight).dot(surface.frame.normal);
    const float window = rangeWindow(legs.receiverDistance + legs.lightDistance, range);
    const float distances = legs.receiverDistance * legs.lightDistance;
    return intensity * reflectance * (window * window * along * along * meanWeight / (4.0f * distances * distances));
}

std::vector<MirrorPair> chooseMirrorPairs(const GpuLights& lights, std::span<const DrawItem> draws, const Vec3& cameraOrigin,
                                          const MirroredLightSettings& settings) {
    struct Candidate {
        float gap;
        MirrorPair pair;
    };

    std::vector<Candidate> candidates;

    const auto position = [](const float* positionRange) { return Vec3{positionRange[0], positionRange[1], positionRange[2]}; };

    for (std::size_t index = 0; index < draws.size(); ++index) {
        const DrawItem& draw = draws[index];

        if (!draw.castsShadows || isTransparent(draw.material) || draw.bounds.radius <= 0.0f || mirrorRoughnessFade(draw.material.roughness, settings.mirrorRoughness) <= 0.0f) {
            continue;
        }

        const Vec3 centre = draw.bounds.centre - cameraOrigin;
        const float radius = draw.bounds.radius;
        const auto add = [&](int kind, std::size_t slot, const Vec3& light) {
            const float gap = std::max((centre - light).length() - radius, 0.0f);
            candidates.push_back(Candidate{gap, MirrorPair{kind, static_cast<int>(slot), static_cast<std::uint32_t>(index), centre, radius}});
        };

        for (std::size_t slot = 0; slot < lights.points.size() && slot < lights.fastPoints.size(); ++slot) {
            const GpuPointLight& point = lights.points[slot];
            const Vec3 light = position(point.positionRange);

            if (lights.fastPoints[slot] && (centre - light).length() - radius < point.positionRange[3]) {
                add(MIRROR_POINT_LIGHT, slot, light);
            }
        }

        for (std::size_t slot = 0; slot < lights.spots.size() && slot < lights.fastSpots.size(); ++slot) {
            const GpuSpotLight& spot = lights.spots[slot];
            const Vec3 light = position(spot.positionRange);
            const Vec3 axis{spot.directionCosOuter[0], spot.directionCosOuter[1], spot.directionCosOuter[2]};

            if (lights.fastSpots[slot] && coneTouchesSphere(light, axis, spot.directionCosOuter[3], spot.positionRange[3], centre, radius)) {
                add(MIRROR_SPOT_LIGHT, slot, light);
            }
        }
    }

    // Nearest first; ties in a fixed order, so the same frame always picks the same pairs.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return std::tie(a.gap, a.pair.lightKind, a.pair.slot, a.pair.draw) < std::tie(b.gap, b.pair.lightKind, b.pair.slot, b.pair.draw);
    });

    std::vector<MirrorPair> pairs;

    for (std::size_t i = 0; i < candidates.size() && i < MIRROR_MAX_PAIRS; ++i) {
        pairs.push_back(candidates[i].pair);
    }

    return pairs;
}

GpuMirrorPairs packMirrorPairs(std::span<const MirrorPair> pairs, const MirroredLightSettings& settings) {
    if (pairs.size() > MIRROR_MAX_PAIRS) {
        throw std::length_error("A frame holds at most MIRROR_MAX_PAIRS mirror pairs");
    }

    GpuMirrorPairs packed{};
    packed.counts[0] = static_cast<std::int32_t>(pairs.size());
    packed.options[0] = settings.mirrorRoughness;

    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const MirrorPair& pair = pairs[i];
        packed.pairs[i].light[0] = pair.lightKind;
        packed.pairs[i].light[1] = pair.slot;
        packed.pairs[i].light[2] = static_cast<std::int32_t>(pair.draw);
        packed.pairs[i].bounds[0] = pair.centre.x;
        packed.pairs[i].bounds[1] = pair.centre.y;
        packed.pairs[i].bounds[2] = pair.centre.z;
        packed.pairs[i].bounds[3] = pair.radius;
    }

    return packed;
}

GpuMirroredLightPass packMirroredLightPass(const Mat4& inverseViewProjection, RenderSize size) {
    GpuMirroredLightPass pass{};
    writeColumnMajor(inverseViewProjection, pass.inverseViewProjection);
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    pass.size[0] = width;
    pass.size[1] = height;
    pass.size[2] = 1.0f / width;
    pass.size[3] = 1.0f / height;
    return pass;
}

namespace {
    // A metal draw's flat faces may be mirrors: metallic at least this.
    constexpr float leastMirrorMetallic = 0.5f;

    // A point light's image's cone: its inner cone takes in the window's corners with this to spare (radians, against
    // rounding), its outer this much wider (the window cuts the edge exactly, so the soft cone edge is never seen), and
    // no wider than a shadow tile's perspective holds well.
    constexpr float imageConeSpare = 1e-3f;
    constexpr float imageConeEdge = 0.02f;
    constexpr float widestImageCone = 1.4f;

    float component(const Vec3& vector, int axis) {
        return axis == 0 ? vector.x : axis == 1 ? vector.y : vector.z;
    }

    Vec3 unitAxis(int axis) {
        return Vec3{axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f};
    }

    Vec3 timesEach(const Vec3& a, const Vec3& b) {
        return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
    }

    // The window's corners and the radius of the circle round them.
    float windowRadius(const LightWindow& window) {
        return std::sqrt(window.halfU * window.halfU + window.halfV * window.halfV);
    }

    // Light of the source's kind turned into its image's: the position mirrored, the colour times the reflectance, the
    // intensity times the roughness's fade, not fast, the glowing size widened by the facets' spread (twice the facets'
    // tilt, GGX's alpha) over the source's distance from the mirror.
    PlacedSpotLight imageBase(const Vec3& position, const SpotLight& light, float fade, float shadowFade, const LightWindow& window, const Material& mirror,
                              float roughnessLimit) {
        const MirrorPlane plane{window.normal, window.offset};
        PlacedSpotLight image;
        image.position = reflectAcrossPlane(position, plane);
        image.light = light;
        image.light.colour = timesEach(light.colour, mirrorReflectance(mirror));
        image.light.intensity = light.intensity * mirrorRoughnessFade(mirror.roughness, roughnessLimit);
        image.light.fast = false;
        const float toMirror = std::max(window.normal.dot(position) - window.offset, 0.0f);
        image.light.emitterRadius = light.emitterRadius + 2.0f * mirror.roughness * mirror.roughness * toMirror;
        image.fade = fade;
        image.shadowFade = shadowFade;
        image.window = window;
        return image;
    }
}

// A face square to its mesh's axes is an axis-aligned rectangle there: the two other axes' extents are its edges.
std::optional<LightWindow> mirrorFaceWindow(const FlatFace& face, const Mat4& model) {
    int across = 0;

    for (int axis = 1; axis < 3; ++axis) {
        if (std::abs(component(face.normal, axis)) > std::abs(component(face.normal, across))) {
            across = axis;
        }
    }

    if (std::abs(component(face.normal, across)) < 0.999f * face.normal.length()) {
        return std::nullopt;
    }

    const int first = (across + 1) % 3;
    const int second = (across + 2) % 3;
    const Vec3 size = face.bounds.size();
    const Vec3 edgeU = transformDirection(model, unitAxis(first) * (0.5f * component(size, first)));
    const Vec3 edgeV = transformDirection(model, unitAxis(second) * (0.5f * component(size, second)));
    const Vec3 normal = transformDirection(normalMatrix(model), face.normal).normalized();

    if (!(edgeU.length() > 0.0f) || !(edgeV.length() > 0.0f) || !(normal.length() > 0.0f)) {
        return std::nullopt;
    }

    LightWindow window;
    window.normal = normal;
    window.centre = transformPoint(model, face.bounds.centre());
    window.offset = normal.dot(window.centre);
    window.halfU = edgeU.length();
    window.halfV = edgeV.length();
    window.axisU = edgeU * (1.0f / window.halfU);
    window.axisV = edgeV * (1.0f / window.halfV);
    return window;
}

bool throughMirrorWindow(const LightWindow& window, const Vec3& image, const Vec3& point, Vec3& crossing) {
    const float imageSide = window.normal.dot(image) - window.offset;
    const float pointSide = window.normal.dot(point) - window.offset;

    if (!(imageSide < 0.0f && pointSide > 0.0f)) {
        return false;
    }

    crossing = image + (point - image) * (imageSide / (imageSide - pointSide));
    const Vec3 fromCentre = crossing - window.centre;
    return std::abs(fromCentre.dot(window.axisU)) <= window.halfU && std::abs(fromCentre.dot(window.axisV)) <= window.halfV;
}

Vec3 mirrorReflectance(const Material& material) {
    SurfaceProperties surface;
    surface.baseColour = Vec3{srgbToLinear(static_cast<float>(material.colour.r) / 255.0f), srgbToLinear(static_cast<float>(material.colour.g) / 255.0f),
                              srgbToLinear(static_cast<float>(material.colour.b) / 255.0f)};
    surface.metallic = material.metallic;
    surface.roughness = material.roughness;
    return reflectanceAtNormal(surface);
}

// Reflection keeps angles, so the beam's direction is reflected and its cone kept.
PlacedSpotLight imageOfSpotLight(const PlacedSpotLight& source, int sourceSlot, const LightWindow& window, const Material& mirror, float roughnessLimit) {
    PlacedSpotLight image = imageBase(source.position, source.light, source.fade, source.shadowFade, window, mirror, roughnessLimit);
    const Vec3 beam = source.light.direction;
    image.light.direction = beam - window.normal * (2.0f * window.normal.dot(beam));
    image.window->sourceIsSpot = true;
    image.window->sourceSlot = sourceSlot;
    return image;
}

// A point light shines every way, so its image only needs a cone round the window as seen from the image.
PlacedSpotLight imageOfPointLight(const PlacedPointLight& source, int sourceSlot, const LightWindow& window, const Material& mirror, float roughnessLimit) {
    SpotLight light;
    light.colour = source.light.colour;
    light.intensity = source.light.intensity;
    light.range = source.light.range;
    light.sourceRadius = source.light.sourceRadius;
    light.emitterRadius = source.light.emitterRadius;
    light.castsShadows = source.light.castsShadows;
    light.priority = source.light.priority;
    PlacedSpotLight image = imageBase(source.position, light, source.fade, source.shadowFade, window, mirror, roughnessLimit);
    const Vec3 axis = (window.centre - image.position).normalized();
    float widest = 0.0f;

    for (const float u : {-1.0f, 1.0f}) {
        for (const float v : {-1.0f, 1.0f}) {
            const Vec3 corner = window.centre + window.axisU * (window.halfU * u) + window.axisV * (window.halfV * v);
            widest = std::max(widest, std::acos(std::clamp((corner - image.position).normalized().dot(axis), -1.0f, 1.0f)));
        }
    }

    image.light.direction = axis;
    image.light.innerAngle = std::min(widest + imageConeSpare, widestImageCone);
    image.light.outerAngle = image.light.innerAngle + imageConeEdge;
    image.window->sourceIsSpot = false;
    image.window->sourceSlot = sourceSlot;
    return image;
}

std::vector<PlacedSpotLight> chooseImageLights(const FrameLighting& lighting, std::span<const DrawItem> draws, const MirroredLightSettings& settings) {
    if (!settings.imageLights) {
        return {};
    }

    struct Candidate {
        float score;
        bool isSpot;
        std::size_t slot;
        std::size_t draw;
        LightWindow window;
    };

    std::vector<Candidate> candidates;

    for (std::size_t index = 0; index < draws.size(); ++index) {
        const DrawItem& draw = draws[index];
        const float fade = mirrorRoughnessFade(draw.material.roughness, settings.mirrorRoughness);

        if (!draw.castsShadows || isTransparent(draw.material) || draw.material.metallic < leastMirrorMetallic || fade <= 0.0f) {
            continue;
        }

        const Vec3 reflectance = mirrorReflectance(draw.material);
        const float reflects = fade * (0.2126f * reflectance.x + 0.7152f * reflectance.y + 0.0722f * reflectance.z);

        for (const FlatFace& face : draw.flatFaces) {
            const std::optional<LightWindow> window = mirrorFaceWindow(face, draw.model);

            if (!window) {
                continue;
            }

            const float area = 4.0f * window->halfU * window->halfV;
            const float radius = windowRadius(*window);

            // The light reaching the face: intensity times the face's solid angle seen from the light (its area, times its
            // cosine to the light, over the distance squared), times what the mirror reflects.
            const auto consider = [&](bool isSpot, std::size_t slot, const Vec3& position, const Vec3& colour, float intensity) {
                const float inFront = window->normal.dot(position) - window->offset;
                const float distance = (window->centre - position).length();

                if (!(inFront > 0.0f) || !(distance > 0.0f)) {
                    return;
                }

                const float brightest = std::max({colour.x, colour.y, colour.z});
                const float score = brightest * intensity * area * (inFront / distance) / (distance * distance) * reflects;
                candidates.push_back(Candidate{score, isSpot, slot, index, *window});
            };

            for (std::size_t slot = 0; slot < lighting.pointLights.size(); ++slot) {
                const PlacedPointLight& placed = lighting.pointLights[slot];

                if ((window->centre - placed.position).length() - radius < placed.light.range) {
                    consider(false, slot, placed.position, placed.light.colour, placed.light.intensity);
                }
            }

            for (std::size_t slot = 0; slot < lighting.spotLights.size(); ++slot) {
                const PlacedSpotLight& placed = lighting.spotLights[slot];

                // Never an image of an image: one bounce only.
                if (!placed.window && coneTouchesSphere(placed.position, placed.light.direction.normalized(), std::cos(placed.light.outerAngle),
                                                        placed.light.range, window->centre, radius)) {
                    consider(true, slot, placed.position, placed.light.colour, placed.light.intensity);
                }
            }
        }
    }

    // Brightest first; ties in a fixed order, so the same frame always picks the same images.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }

        return std::tie(a.isSpot, a.slot, a.draw) < std::tie(b.isSpot, b.slot, b.draw);
    });

    std::vector<PlacedSpotLight> images;

    for (std::size_t i = 0; i < candidates.size() && static_cast<int>(i) < MIRROR_IMAGE_LIGHTS; ++i) {
        const Candidate& chosen = candidates[i];
        const Material& mirror = draws[chosen.draw].material;
        const int slot = static_cast<int>(chosen.slot);
        images.push_back(chosen.isSpot ? imageOfSpotLight(lighting.spotLights[chosen.slot], slot, chosen.window, mirror, settings.mirrorRoughness)
                                       : imageOfPointLight(lighting.pointLights[chosen.slot], slot, chosen.window, mirror, settings.mirrorRoughness));
    }

    return images;
}
