#include "render/gpu/mirrored_light.h"
#include "render/gpu/clusters.h"
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
