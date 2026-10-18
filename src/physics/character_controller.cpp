#include "physics/character_controller.h"

#include "math/angles.h"
#include "math/transform.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace {
    // Pushes shallower than this are touching, not overlapping, and are left alone.
    constexpr float touchingDepth = 1e-5f;

    // How far below the feet to look when deciding whether the character stands on something.
    constexpr float groundProbeDistance = 0.01f;

    // Settling onto the ground drops in steps of at most this, so it meets the nearest surface first.
    constexpr float descentIncrement = 0.05f;

    // However fast the character moves, one tick never splits into more substeps than this.
    constexpr int maxSubsteps = 64;

    // Below this length a vector counts as zero.
    constexpr float negligible = 1e-6f;

    Vec3 acrossGround(const Vec3& vector) {
        return Vec3{vector.x, 0.0f, vector.z};
    }

    // Moves from towards to by at most maxChange (a vector version of stepping a number towards a target).
    Vec3 moveTowards(const Vec3& from, const Vec3& to, float maxChange) {
        const Vec3 difference = to - from;
        const float distance = difference.length();
        if (distance <= maxChange || distance <= negligible) {
            return to;
        }
        return from + difference * (maxChange / distance);
    }

    // A contact normal further than this from its triangle's face normal is on an edge or corner, not on the face.
    constexpr float faceContactCosine = 0.999f;

    // An edge counts as a ledge to step onto only if its push points at least this much upwards (not a wall's side edge).
    constexpr float leastStepNormalY = 0.05f;

    /*
    * Whether a contact holds the character up (ground) rather than blocking it (wall or ceiling). A face is ground when its
    * slope is walkable: normal.y = cos(angle from straight up) at least cos(maxSlope). An edge is ground when it faces
    * upwards at all and is no higher than stepHeight above the feet: a ledge low enough to step onto. When a round body
    * bumps a ledge of height h, the push comes from the edge towards the ball's centre, at angle acos((r - h) / r) from
    * up, often steeper than any walkable slope; counting the edge as ground lets the capsule ride up over it, the way a
    * low kerb is already climbed. A steep ramp is still a wall, because walking into it touches its face.
    */
    bool supports(const CharacterControllerSettings& settings, const Contact& found, const Vec3& feet) {
        if (found.normal.y >= std::cos(radiansFromDegrees(settings.maxSlopeDegrees))) {
            return true;
        }
        const bool onEdge = found.normal.dot(found.faceNormal) < faceContactCosine;
        return onEdge && found.normal.y >= leastStepNormalY && found.point.y - feet.y <= settings.stepHeight;
    }

    /*
    * Push the capsule out of the level, one contact at a time, deepest first, asking again after each push (pushing out
    * of one wall can push into another, as in a corner). Ground is pushed out of straight up, by the height that clears
    * the same depth (depth / normal.y), so standing on a slope does not slide: the kinematic stand-in for static friction.
    * Anything else pushes along its normal and takes away the velocity going into it, which is a frictionless, perfectly
    * inelastic collision: what is left slides along the surface. While walking, a steep slope underfoot is treated as a
    * wall and pushes only sideways, so walking into it cannot climb it.
    */
    void pushOut(const CharacterControllerSettings& settings, Vec3& feet, Vec3& velocity, const StaticLevel& level, bool walking) {

        for (int pass = 0; pass < settings.pushOutPasses; ++pass) {
            const std::vector<Contact> found = contacts(level, controllerCapsule(settings, feet));
            const Contact* deepest = nullptr;
            for (const Contact& candidate : found) {
                if (candidate.depth > touchingDepth && (!deepest || candidate.depth > deepest->depth)) {
                    deepest = &candidate;
                }
            }
            if (!deepest) {
                break;
            }

            if (supports(settings, *deepest, feet)) {
                feet.y += deepest->depth / deepest->normal.y;
                velocity.y = std::max(velocity.y, 0.0f);
                continue;
            }

            Vec3 normal = deepest->normal;
            float distance = deepest->depth;
            if (walking && normal.y > 0.0f) {
                const Vec3 sideways = acrossGround(normal);
                const float sidewaysLength = sideways.length();
                if (sidewaysLength > negligible) {
                    // Moving x sideways reduces the overlap along the true normal by x * sidewaysLength.
                    normal = sideways / sidewaysLength;
                    distance = deepest->depth / sidewaysLength;
                }
            }

            feet = feet + normal * distance;
            const float into = velocity.dot(normal);
            if (into < 0.0f) {
                velocity = velocity - normal * into;
            }
        }
    }

    /*
    * The most upward ground just under the feet, if there is any: the contacts of the capsule lowered a little that hold
    * it up (a walkable face, or the edge of a low ledge). It only looks; nothing moves.
    */
    std::optional<Vec3> groundUnder(const CharacterControllerSettings& settings, const Vec3& feet, const StaticLevel& level) {
        const Vec3 lowered = feet - Vec3{0.0f, groundProbeDistance, 0.0f};

        std::optional<Vec3> ground;
        for (const Contact& found : contacts(level, controllerCapsule(settings, lowered))) {
            if (supports(settings, found, feet) && (!ground || found.normal.y > ground->y)) {
                ground = found.normal;
            }
        }
        return ground;
    }

    // Whether the capsule overlaps the level more than just touching.
    bool overlapsLevel(const CharacterControllerSettings& settings, const Vec3& feet, const StaticLevel& level) {
        for (const Contact& found : contacts(level, controllerCapsule(settings, feet))) {
            if (found.depth > touchingDepth) {
                return true;
            }
        }
        return false;
    }

    // Where a descent onto the ground ended, and the ground's normal there.
    struct Landing {
        Vec3 feet;
        Vec3 groundNormal;
    };

    /*
    * Lower the character at most maxDrop onto walkable ground, in small steps, pushing out as soon as it first overlaps
    * something. Dropping the whole way at once and then pushing out would sink it into everything nearby at once (the
    * ground and the edge of the kerb beside it), and push-out deals with the deepest first: often that edge, seen from
    * below as a wall, which shoves the character sideways. Nothing if it lands on no walkable ground.
    */
    std::optional<Landing> descend(const CharacterControllerSettings& settings, Vec3 feet, Vec3 velocity, const StaticLevel& level,
                                   float maxDrop) {
        // Even when it already seems to stand on something, it steps down once and pushes out, which leaves it exactly
        // touching the surface instead of hovering a little above it.
        float dropped = 0.0f;
        while (dropped < maxDrop) {
            const float step = std::min(descentIncrement, maxDrop - dropped);
            feet.y -= step;
            dropped += step;
            if (overlapsLevel(settings, feet, level)) {
                pushOut(settings, feet, velocity, level, true);
                const std::optional<Vec3> ground = groundUnder(settings, feet, level);
                if (!ground) {
                    return std::nullopt;
                }
                return Landing{feet, *ground};
            }
        }

        // Down the whole way without overlapping anything: only a surface it now just touches can hold it.
        if (const std::optional<Vec3> ground = groundUnder(settings, feet, level)) {
            return Landing{feet, *ground};
        }
        return std::nullopt;
    }

    /*
    * The horizontal velocity for this tick. On the ground it heads for the wished velocity quickly (and stops quickly when
    * nothing is wished). In the air nothing wished keeps the momentum; steering heads for the wished velocity more slowly,
    * and a turn of 90 degrees or less keeps the speed (up to the wished speed) instead of losing it on the way round,
    * which is how Overwatch's air movement has worked since 2018: forward speed turns into sideways speed.
    */
    Vec3 steer(const CharacterControllerSettings& settings, const Vec3& current, const Vec3& wish, bool grounded, float seconds) {
        if (grounded) {
            return moveTowards(current, wish, settings.groundAcceleration * seconds);
        }
        if (wish.lengthSquared() <= negligible * negligible) {
            return current;
        }

        Vec3 steered = moveTowards(current, wish, settings.airAcceleration * seconds);
        if (current.dot(wish) >= 0.0f) {
            const float keep = std::min(current.length(), wish.length());
            const float speed = steered.length();
            if (speed > negligible && speed < keep) {
                steered = steered * (keep / speed);
            }
        }
        return steered;
    }

    /*
    * Gravity with quadratic air drag while falling: a = -g + (g / v_t^2) v^2, which is Newton's law for a falling body with
    * drag proportional to speed squared, and approaches the terminal speed v_t as v(t) = v_t tanh(g t / v_t). Rising has no
    * drag. Semi-implicit Euler: the new velocity is used for this tick's move, which keeps jumps from gaining energy.
    */
    float fall(const CharacterControllerSettings& settings, float verticalSpeed, float seconds) {
        float acceleration = -settings.gravity;
        if (verticalSpeed < 0.0f) {
            const float terminal = settings.maxFallSpeed;
            acceleration += settings.gravity / (terminal * terminal) * verticalSpeed * verticalSpeed;
        }
        return std::max(verticalSpeed + acceleration * seconds, -settings.maxFallSpeed);
    }
}

Capsule controllerCapsule(const CharacterControllerSettings& settings, const Vec3& feet) {
    const Vec3 bottom = feet + Vec3{0.0f, settings.radius, 0.0f};
    const Vec3 top = feet + Vec3{0.0f, std::max(settings.radius, settings.height - settings.radius), 0.0f};
    return Capsule{bottom, top, settings.radius};
}

/*
* One tick, in order: learn the ground if new; jump (before steering, so the take-off tick already uses air rules and
* keeps its speed); steer; fall; move in substeps short enough that no wall can be skipped, pushing out and stepping up
* ledges; follow the ground down a short drop if it was walking; and finally look at what is underfoot.
*/
void moveCharacter(CharacterController& controller, Vec3& feet, const StaticLevel& level, float seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0f) {
        return;
    }
    const CharacterControllerSettings& settings = controller.settings;

    if (!controller.groundKnown) {
        const std::optional<Vec3> ground = groundUnder(settings, feet, level);
        controller.grounded = ground.has_value();
        controller.groundNormal = ground.value_or(Vec3{0.0f, 1.0f, 0.0f});
        controller.groundKnown = true;
    }

    const bool wasGrounded = controller.grounded;
    bool jumped = false;
    if (controller.wantsJump && controller.grounded) {
        controller.velocity.y = settings.jumpSpeed;
        controller.grounded = false;
        jumped = true;
    }
    controller.wantsJump = false;

    const Vec3 horizontal = steer(settings, acrossGround(controller.velocity), acrossGround(controller.wishVelocity),
                                  controller.grounded, seconds);
    const float vertical = controller.grounded ? 0.0f : fall(settings, controller.velocity.y, seconds);
    controller.velocity = Vec3{horizontal.x, vertical, horizontal.z};

    // On the ground, move along the surface at the full speed: tilt the horizontal velocity into the ground's plane and
    // scale it back up, so a slope is walked at the same speed as flat ground.
    Vec3 move = controller.velocity * seconds;
    if (controller.grounded) {
        const Vec3& normal = controller.groundNormal;
        const Vec3 along = horizontal - normal * horizontal.dot(normal);
        const float alongLength = along.length();
        move = alongLength > negligible ? along * (horizontal.length() / alongLength * seconds) : Vec3{};
    }

    const float longestSubstep = settings.radius * settings.substepFraction;
    const int substeps = std::clamp(static_cast<int>(std::ceil(move.length() / longestSubstep)), 1, maxSubsteps);
    const Vec3 part = move / static_cast<float>(substeps);

    // Low ledges need no special step: their edges count as ground (supports), so the capsule rides up over them.
    for (int substep = 0; substep < substeps; ++substep) {
        feet = feet + part;
        pushOut(settings, feet, controller.velocity, level, controller.grounded);
    }
    controller.lastSubsteps = substeps;

    // While walking, settle onto the ground every tick: down a short drop (off a step, over the top of a ramp) rather than
    // starting to fall, and back down after a push-out that left it hovering (pushing straight up out of a rounded edge
    // overshoots a little, and a grounded character has no gravity to bring it back).
    std::optional<Vec3> ground = groundUnder(settings, feet, level);
    if (wasGrounded && !jumped) {
        if (const std::optional<Landing> landing = descend(settings, feet, controller.velocity, level, settings.snapDistance)) {
            feet = landing->feet;
            ground = landing->groundNormal;
        }
    }

    if (ground && controller.velocity.y <= 0.0f) {
        controller.grounded = true;
        controller.groundNormal = *ground;
        controller.velocity.y = 0.0f;
    } else {
        controller.grounded = false;
    }
}

void moveCharacters(World& world, const StaticLevel& level, float seconds) {
    world.each<CharacterController, Transform>([&](Entity, CharacterController& controller, Transform& transform) {
        moveCharacter(controller, transform.position, level, seconds);
    });
}
