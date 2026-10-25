#include "cloth/cloth_grab.h"

#include "cloth/cloth.h"
#include "cloth/cloth_shared.h"
#include "physics/raycast.h"

namespace {
    // Closer than this the point is on the target, with no direction to pull along (cloth/cloth_shared.h).
    constexpr float onTarget = static_cast<float>(CLOTH_ON_TARGET);
}

std::optional<ClothHit> raycastCloth(const Cloth& cloth, const Ray& ray, float maxDistance) {
    const std::vector<Vec3>& positions = cloth.particles.positions;
    std::optional<ClothHit> nearest;

    for (const std::array<std::uint32_t, 3>& triangle : cloth.triangles) {
        const std::optional<RayHit> hit = raycastTriangle(ray, positions[triangle[0]], positions[triangle[1]], positions[triangle[2]], maxDistance);
        if (!hit || (nearest && hit->distance >= nearest->distance)) {
            continue;
        }

        std::uint32_t closest = triangle[0];
        for (std::uint32_t corner : triangle) {
            if ((positions[corner] - hit->point).lengthSquared() < (positions[closest] - hit->point).lengthSquared()) {
                closest = corner;
            }
        }
        nearest = ClothHit{closest, hit->point, hit->distance};
    }
    return nearest;
}

void solveGrab(ClothParticles& particles, const ClothGrab& grab, float compliance, float seconds) {
    const float weight = particles.inverseMasses.at(grab.particle);
    if (weight == 0.0f) {
        return;
    }

    Vec3& point = particles.positions[grab.particle];
    const Vec3 away = point - grab.target;
    const float distance = away.length();
    if (distance < onTarget) {
        return;
    }

    const float multiplier = -distance / (weight + compliance / (seconds * seconds));
    point = point + away * (weight * multiplier / distance);
}
