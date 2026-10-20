#pragma once

#include "math/ray.h"
#include "math/vec3.h"
#include "physics/static_level.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/probe_relocation.h"

#include <optional>
#include <vector>

// Helpers shared by the light probe tests (ddgi_test, probe_cascades_test, light_lab_test).

/*
* A relocation ray against a level's triangles, as ddgi_relocate.comp traces it: hits nearer than
* probeRelocationMinDistance are skipped (the ray starts that far along), the distance is negative for a back face (a
* triangle seen from behind its winding, the inside of a solid) and probeRayMiss for nothing.
*/
inline float traceRelocationRay(const StaticLevel& level, const Vec3& origin, const Vec3& direction) {
    const Ray ray{origin + direction * probeRelocationMinDistance, direction};
    const std::optional<LevelHit> hit = raycast(level, ray, probeRayMiss);

    if (!hit) {
        return probeRayMiss;
    }

    const std::array<Vec3, 3> corners = level.getTriangle(hit->triangle);
    const Vec3 outward = (corners[1] - corners[0]).cross(corners[2] - corners[0]);
    const float distance = hit->hit.distance + probeRelocationMinDistance;
    return outward.dot(direction) > 0.0f ? -distance : distance;
}

// One texel's cosine-weighted mean of a set of rays, as the update computes it.
inline Vec3 irradianceEstimate(const Vec3& texel, const std::vector<Vec3>& directions, const std::vector<Vec3>& radiance) {
    Vec3 sum{0.0f, 0.0f, 0.0f};
    float weights = 0.0f;

    for (std::size_t i = 0; i < directions.size(); ++i) {
        const float weight = irradianceRayWeight(texel, directions[i]);
        sum = sum + radiance[i] * weight;
        weights += weight;
    }

    return sum * (1.0f / weights);
}
