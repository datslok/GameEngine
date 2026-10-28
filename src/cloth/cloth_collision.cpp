#include "cloth/cloth_collision.h"

#include "cloth/cloth.h"
#include "cloth/cloth_shared.h"
#include "math/closest_points.h"
#include "math/transform.h"
#include "physics/collider.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace {
    // Closer than this to a centre or segment there is no direction out of it (squared; cloth/cloth_shared.h).
    constexpr float noDirection = static_cast<float>(CLOTH_NO_DIRECTION);

    const Vec3 straightUp{0.0f, 1.0f, 0.0f};

    // Out of a ball of the given radius round a centre.
    SurfacePoint outOfBall(const Vec3& centre, float radius, const Vec3& point) {
        const Vec3 away = point - centre;
        const float length = away.length();
        if (length * length < noDirection) {
            return SurfacePoint{-radius, straightUp};
        }
        return SurfacePoint{length - radius, away * (1.0f / length)};
    }

    /*
    * Outside: from the closest point of the box. Inside: through the face nearest the point, the first of the six in
    * order (-x, +x, -y, +y, -z, +z) when two are as near.
    */
    SurfacePoint outOfBox(const Aabb& box, const Vec3& point) {
        if (!box.contains(point)) {
            const Vec3 away = point - closestPointInBox(point, box);
            const float length = away.length();
            return SurfacePoint{length, away * (1.0f / length)};
        }

        const float depths[6]{point.x - box.minimum.x, box.maximum.x - point.x, point.y - box.minimum.y,
                              box.maximum.y - point.y, point.z - box.minimum.z, box.maximum.z - point.z};
        const Vec3 normals[6]{Vec3{-1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f},
                              Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        const int nearest = static_cast<int>(std::min_element(std::begin(depths), std::end(depths)) - std::begin(depths));
        return SurfacePoint{-depths[nearest], normals[nearest]};
    }
}

SurfacePoint surfacePoint(const Shape& shape, const Vec3& point) {
    return std::visit([&](const auto& held) {
        using Held = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<Held, Sphere>) {
            return outOfBall(held.centre, held.radius, point);
        } else if constexpr (std::is_same_v<Held, Capsule>) {
            return outOfBall(closestPointOnSegment(point, held.a, held.b), held.radius, point);
        } else {
            return outOfBox(held, point);
        }
    }, shape);
}

namespace {
    // One body's push and friction on a point (solveContacts).
    void contact(Vec3& position, const Vec3& previous, const SurfacePoint& surface, float thickness, float staticFriction, float kineticFriction) {
        const float push = thickness - surface.distance;
        if (push <= 0.0f) {
            return;
        }

        position = position + surface.normal * push;

        const Vec3 moved = position - previous;
        const Vec3 slide = moved - surface.normal * surface.normal.dot(moved);
        const float slideLength = slide.length();
        if (slideLength < staticFriction * push) {
            position = position - slide;
        } else if (slideLength > 0.0f) {
            position = position - slide * std::min(kineticFriction * push / slideLength, 1.0f);
        }
    }
}

void solveContacts(ClothParticles& particles, const std::vector<Shape>& shapes, const ClothSettings& settings, const ClothFields& fields,
                   std::span<const std::uint8_t> sewing) {
    for (std::size_t index = 0; index < particles.size(); ++index) {
        if (particles.inverseMasses[index] == 0.0f) {
            continue;
        }

        const bool beingSewn = !sewing.empty() && sewing[index] != 0;
        const float staticFriction = beingSewn ? 0.0f : settings.staticFriction;
        const float kineticFriction = beingSewn ? 0.0f : settings.kineticFriction;
        Vec3& position = particles.positions[index];
        const Vec3& previous = particles.previousPositions[index];
        for (const Shape& shape : shapes) {
            contact(position, previous, surfacePoint(shape, position), settings.thickness, staticFriction, kineticFriction);
        }
        for (const std::shared_ptr<const SignedDistanceField>& field : fields) {
            contact(position, previous, surfacePoint(*field, position), settings.thickness, staticFriction, kineticFriction);
        }
    }
}

SurfacePoint surfacePoint(const SignedDistanceField& field, const Vec3& point) {
    return fieldSurface(sampleField(field, point));
}

SurfacePoint fieldSurface(const FieldPoint& sampled) {
    const float slope = sampled.gradient.length();
    if (slope * slope < noDirection) {
        return SurfacePoint{sampled.distance, straightUp};
    }
    return SurfacePoint{sampled.distance, sampled.gradient * (1.0f / slope)};
}

ClothFields worldDistanceFields(const World& world) {
    ClothFields fields;
    world.each<DistanceFieldCollider>([&](Entity, const DistanceFieldCollider& collider) {
        if (collider.field) {
            fields.push_back(collider.field);
        }
    });
    return fields;
}

std::vector<Shape> worldColliderShapes(const World& world) {
    std::vector<Shape> shapes;
    world.each<Collider, Transform>([&](Entity, const Collider& collider, const Transform& transform) {
        shapes.push_back(worldShape(collider.shape, transform.getMatrix()));
    });
    return shapes;
}
