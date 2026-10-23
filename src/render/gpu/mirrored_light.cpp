#include "render/gpu/mirrored_light.h"
#include "render/gpu/clusters.h"
#include "render/gpu/column_major.h"
#include "render/gpu/light_falloff.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

void visit(MirroredLightSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Mirrored lights", settings.enabled);
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
    // mirror's step does not land (or both ends are not in front of it), there is no point of reflection.
    const std::optional<Vec3> image = planeMirrorPoint(receiver, light, surface->position, surface->frame.normal);
    std::optional<MirrorSurface> moved = image ? project(*image) : std::nullopt;

    if (moved) {
        surface = moved;
    } else if (isFlatMirror(surface->curvature)) {
        return std::nullopt;
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

        if (!draw.castsShadows || draw.bounds.radius <= 0.0f || draw.material.roughness > settings.mirrorRoughness) {
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

GpuMirroredLightPass packMirroredLightPass(const Mat4& inverseViewProjection, RenderSize size, std::span<const MirrorPair> pairs) {
    if (pairs.size() > MIRROR_MAX_PAIRS) {
        throw std::length_error("A mirrored light pass holds at most MIRROR_MAX_PAIRS pairs");
    }

    GpuMirroredLightPass pass{};
    writeColumnMajor(inverseViewProjection, pass.inverseViewProjection);
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);
    pass.size[0] = width;
    pass.size[1] = height;
    pass.size[2] = 1.0f / width;
    pass.size[3] = 1.0f / height;
    pass.counts[0] = static_cast<std::int32_t>(pairs.size());

    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const MirrorPair& pair = pairs[i];
        pass.pairs[i].light[0] = pair.lightKind;
        pass.pairs[i].light[1] = pair.slot;
        pass.pairs[i].light[2] = static_cast<std::int32_t>(pair.draw);
        pass.pairs[i].bounds[0] = pair.centre.x;
        pass.pairs[i].bounds[1] = pair.centre.y;
        pass.pairs[i].bounds[2] = pair.centre.z;
        pass.pairs[i].bounds[3] = pair.radius;
    }

    return pass;
}
