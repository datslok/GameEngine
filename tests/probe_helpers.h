#pragma once

#include "math/vec3.h"
#include "render/gpu/ddgi.h"

#include <vector>

// Helpers shared by the light probe tests (ddgi_test, probe_cascades_test).

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
