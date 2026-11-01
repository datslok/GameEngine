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

/*
* The hand's way through one tick, from where it was (start) to where it gets to (end), holding particle; each substep
* holds the point at its share of the way (slidPinPosition), as pins slide. The hand moves no faster than
* ClothSettings::handSpeed: a target that jumps (the mouse moved fast between two frames) is reached over several ticks,
* since a stiff hand pulled its point 16 cm in one substep, crushing the fabric round it until self-collision's cut let
* edges through.
*/
struct ClothHandMove {
    std::uint32_t particle = 0;
    Vec3 start;
    Vec3 end;
};

/*
* This tick's way (none without a grab): from where the last tick's ended, or, for a grab new or of another point,
* from the held point itself (positions), so taking hold never yanks; towards the grab's target, no further than speed
* (m/s) times seconds, stopping on it.
*/
std::optional<ClothHandMove> moveClothHand(const std::optional<ClothHandMove>& last, const std::optional<ClothGrab>& grab,
                                           const std::vector<Vec3>& positions, float speed, float seconds);

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
