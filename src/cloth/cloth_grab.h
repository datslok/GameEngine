#pragma once

#include "cloth/cloth_particles.h"
#include "math/ray.h"
#include "math/vec3.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

struct Cloth;

/*
* A hand holding one point of a cloth: an XPBD constraint pulling the point towards the target, C = |x - target|,
* solved with the stretch constraints every substep. With a compliance it is a stiff spring, so a hard yank stretches
* the hand a little rather than tearing at the cloth; let go, the point keeps the speed it had.
*/
struct ClothGrab {
    std::uint32_t particle = 0;
    Vec3 target;
};

// Where a ray meets a cloth: the corner of the hit triangle nearest the hit, the hit and its distance along the ray
// (in world units, whatever the ray direction's length).
struct ClothHit {
    std::uint32_t particle = 0;
    Vec3 point;
    float distance = 0.0f;
};

// The nearest place the ray meets the cloth as it is now, from either side, within maxDistance.
std::optional<ClothHit> raycastCloth(const Cloth& cloth, const Ray& ray, float maxDistance);

/*
* Pull the held point towards the target, XPBD's multiplier step with the multiplier starting at 0 (one pass a substep):
* delta lambda = -C / (w + compliance / seconds^2), the point moved towards the target by w delta lambda. A pinned point
* (w = 0) stays; a point already on the target is left alone.
*/
void solveGrab(ClothParticles& particles, const ClothGrab& grab, float compliance, float seconds);
