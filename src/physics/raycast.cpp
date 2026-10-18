#include "physics/raycast.h"

#include "math/closest_points.h"

#include <algorithm>
#include <cmath>
#include <variant>

namespace {
    // Below this a direction, a triangle's determinant or a ray's sideways motion counts as zero.
    constexpr float tiny = 1e-12f;

    /*
    * Work with a unit direction, so every t below is a distance in world units, whatever length the caller's direction had.
    */
    std::optional<Vec3> unitDirection(const Ray& ray) {
        if (ray.direction.lengthSquared() <= tiny) {
            return std::nullopt;
        }
        return ray.direction.normalized();
    }

    /*
    * A ray that starts inside a shape hits it at once. The normal opposes the ray, as if it had just come through a wall.
    */
    RayHit hitAtStart(const Ray& ray, const Vec3& direction) {
        return RayHit{0.0f, ray.origin, direction * -1.0f};
    }

    /*
    * Where a ray from outside first enters a ball: solve |origin + t * direction - centre|^2 = radius^2 for the smaller t.
    * With a unit direction the quadratic is t^2 + 2bt + c = 0, so t = -b - sqrt(b^2 - c).
    */
    std::optional<float> enterBall(const Vec3& origin, const Vec3& direction, const Vec3& centre, float radius) {
        const Vec3 fromCentre = origin - centre;
        const float b = fromCentre.dot(direction);
        const float c = fromCentre.lengthSquared() - radius * radius;
        const float discriminant = b * b - c;

        // Pointing away (b > 0) from outside (c > 0), or passing beside it.
        if ((c > 0.0f && b > 0.0f) || discriminant < 0.0f) {
            return std::nullopt;
        }

        return std::max(0.0f, -b - std::sqrt(discriminant));
    }

    /*
    * A hit on a rounded shape: the normal points from the nearest point of its core (centre or segment) out to the surface.
    */
    std::optional<RayHit> roundedHit(const Ray& ray, const Vec3& direction, float distance, const Vec3& coreA, const Vec3& coreB, float maxDistance) {
        if (distance > maxDistance) {
            return std::nullopt;
        }
        const Vec3 point = ray.origin + direction * distance;
        const Vec3 normal = (point - closestPointOnSegment(point, coreA, coreB)).normalized();
        return RayHit{distance, point, normal};
    }
}

std::optional<RayHit> raycast(const Ray& ray, const Sphere& sphere, float maxDistance) {
    const std::optional<Vec3> direction = unitDirection(ray);
    if (!direction) {
        return std::nullopt;
    }

    if ((ray.origin - sphere.centre).lengthSquared() <= sphere.radius * sphere.radius) {
        return hitAtStart(ray, *direction);
    }

    const std::optional<float> distance = enterBall(ray.origin, *direction, sphere.centre, sphere.radius);
    if (!distance) {
        return std::nullopt;
    }
    return roundedHit(ray, *direction, *distance, sphere.centre, sphere.centre, maxDistance);
}

/*
* A capsule is a cylinder around its segment with a ball on each end. The ray enters through the cylinder's side (if it
* gets there between the ends) or through one of the balls; the first of these is the hit.
*/
std::optional<RayHit> raycast(const Ray& ray, const Capsule& capsule, float maxDistance) {
    const std::optional<Vec3> direction = unitDirection(ray);
    if (!direction) {
        return std::nullopt;
    }

    const Vec3 nearestOnCore = closestPointOnSegment(ray.origin, capsule.a, capsule.b);
    if ((ray.origin - nearestOnCore).lengthSquared() <= capsule.radius * capsule.radius) {
        return hitAtStart(ray, *direction);
    }

    std::optional<float> best;
    const auto consider = [&best](std::optional<float> distance) {
        if (distance && (!best || *distance < *best)) {
            best = distance;
        }
    };

    consider(enterBall(ray.origin, *direction, capsule.a, capsule.radius));
    consider(enterBall(ray.origin, *direction, capsule.b, capsule.radius));

    // The side: drop the motion along the axis, leaving a circle problem in the plane across it.
    const Vec3 axis = capsule.b - capsule.a;
    const float axisLengthSquared = axis.lengthSquared();
    if (axisLengthSquared > tiny) {
        const Vec3 fromA = ray.origin - capsule.a;
        const Vec3 sidewaysDirection = *direction - axis * (direction->dot(axis) / axisLengthSquared);
        const Vec3 sidewaysOffset = fromA - axis * (fromA.dot(axis) / axisLengthSquared);
        const float a = sidewaysDirection.lengthSquared();

        // A ray along the axis never meets the side; only the end balls can stop it.
        if (a > tiny) {
            const float b = sidewaysOffset.dot(sidewaysDirection);
            const float c = sidewaysOffset.lengthSquared() - capsule.radius * capsule.radius;
            const float discriminant = b * b - a * c;

            if (discriminant >= 0.0f) {
                const float distance = (-b - std::sqrt(discriminant)) / a;
                const float alongAxis = (fromA + *direction * distance).dot(axis) / axisLengthSquared;

                if (distance >= 0.0f && alongAxis >= 0.0f && alongAxis <= 1.0f) {
                    consider(distance);
                }
            }
        }
    }

    if (!best) {
        return std::nullopt;
    }
    return roundedHit(ray, *direction, *best, capsule.a, capsule.b, maxDistance);
}

/*
* The slab method: on each axis the ray is between the box's two planes for one stretch of t. The ray is inside the box
* where all three stretches overlap, so it enters at the latest entry and must do so before the earliest exit. The axis
* that gave the latest entry is the face it came through.
*/
std::optional<RayHit> raycast(const Ray& ray, const Aabb& box, float maxDistance) {
    const std::optional<Vec3> direction = unitDirection(ray);
    if (!direction) {
        return std::nullopt;
    }

    if ((closestPointInBox(ray.origin, box) - ray.origin).lengthSquared() == 0.0f) {
        return hitAtStart(ray, *direction);
    }

    float entry = -unlimitedRayDistance;
    float exit = unlimitedRayDistance;
    int entryAxis = 0;

    for (int index = 0; index < 3; ++index) {
        const float origin = ray.origin[index];
        const float step = (*direction)[index];

        // Moving parallel to this pair of faces: it is either always between them or never.
        if (std::abs(step) <= tiny) {
            if (origin < box.minimum[index] || origin > box.maximum[index]) {
                return std::nullopt;
            }
            continue;
        }

        const float toMinimum = (box.minimum[index] - origin) / step;
        const float toMaximum = (box.maximum[index] - origin) / step;
        const float axisEntry = std::min(toMinimum, toMaximum);

        if (axisEntry > entry) {
            entry = axisEntry;
            entryAxis = index;
        }
        exit = std::min(exit, std::max(toMinimum, toMaximum));
    }

    if (entry < 0.0f || entry > exit || entry > maxDistance) {
        return std::nullopt;
    }

    // The face's outward normal: along the entry axis, against the ray's motion on it.
    const Vec3 axes[3] = {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
    const float outward = (*direction)[entryAxis] > 0.0f ? -1.0f : 1.0f;

    return RayHit{entry, ray.origin + *direction * entry, axes[entryAxis] * outward};
}

/*
* std::visit calls the lambda with the shape's real type, and overload resolution picks the matching raycast above.
*/
std::optional<RayHit> raycast(const Ray& ray, const Shape& shape, float maxDistance) {
    return std::visit([&](const auto& held) { return raycast(ray, held, maxDistance); }, shape);
}

/*
* Moller-Trumbore: write the hit point both as origin + t * direction and as a weighted mix of the triangle's corners,
* and solve the 3x3 system for t and the two weights with Cramer's rule. The point is on the triangle when both weights
* and their sum lie in 0..1. Each triangle that hits nearer than the best so far replaces it.
*/
std::optional<RayHit> raycast(const Ray& ray, const TriangleMesh& mesh, float maxDistance) {
    const std::optional<Vec3> direction = unitDirection(ray);
    if (!direction) {
        return std::nullopt;
    }

    std::optional<RayHit> best;
    float reach = maxDistance;

    for (std::size_t corner = 0; corner + 2 < mesh.indices.size(); corner += 3) {
        const Vec3& first = mesh.positions[mesh.indices[corner]];
        const Vec3 edge1 = mesh.positions[mesh.indices[corner + 1]] - first;
        const Vec3 edge2 = mesh.positions[mesh.indices[corner + 2]] - first;

        const Vec3 p = direction->cross(edge2);
        const float determinant = edge1.dot(p);

        // Zero when the ray runs along the triangle's plane, or the triangle has no area.
        if (std::abs(determinant) <= tiny * edge1.lengthSquared() * edge2.lengthSquared()) {
            continue;
        }

        const float inverse = 1.0f / determinant;
        const Vec3 fromFirst = ray.origin - first;
        const float u = fromFirst.dot(p) * inverse;
        if (u < 0.0f || u > 1.0f) {
            continue;
        }

        const Vec3 q = fromFirst.cross(edge1);
        const float v = direction->dot(q) * inverse;
        if (v < 0.0f || u + v > 1.0f) {
            continue;
        }

        const float distance = edge2.dot(q) * inverse;
        if (distance < 0.0f || distance > reach) {
            continue;
        }

        Vec3 normal = edge1.cross(edge2).normalized();
        if (normal.dot(*direction) > 0.0f) {
            normal = normal * -1.0f;
        }

        best = RayHit{distance, ray.origin + *direction * distance, normal};
        reach = distance;
    }

    return best;
}
