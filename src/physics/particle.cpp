#include "physics/particle.h"

#include "math/transform.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace {
    // After hitting a surface the ball is left this far clear of it. Ending exactly in contact would leave it a rounding
    // error inside half the time, and the next sweep would start in contact.
    constexpr float clearance = 1e-3f;

    // Surfaces within this of the ball count as touching it: it rests or slides on them. More than the clearance, so a
    // ball left clear of a floor still knows it lies on it.
    constexpr float touchingDistance = 2.0f * clearance;

    // Overlaps shallower than this are touching, not overlapping, and are left alone.
    constexpr float overlapDepth = 1e-5f;

    // At most this many pushes out of overlaps per tick (a corner needs one per surface).
    constexpr int pushOutPasses = 4;

    // Below this a length counts as zero.
    constexpr float negligible = 1e-9f;

    Sphere ball(const ParticleSettings& settings, const Vec3& centre) {
        return Sphere{centre, settings.radius};
    }

    /*
    * A safety net: out of anything the ball overlaps (it was placed inside something, or rounding left it a hair inside),
    * deepest first, asking again after each push. The velocity into each surface is taken away, as in a collision that
    * does not bounce.
    */
    void pushOut(const ParticleSettings& settings, Vec3& centre, Vec3& velocity, const StaticLevel& level) {
        for (int pass = 0; pass < pushOutPasses; ++pass) {
            const std::vector<Contact> found = contacts(level, Capsule{centre, centre, settings.radius});
            const Contact* deepest = nullptr;
            for (const Contact& candidate : found) {
                if (candidate.depth > overlapDepth && (!deepest || candidate.depth > deepest->depth)) {
                    deepest = &candidate;
                }
            }
            if (!deepest) {
                break;
            }
            centre = centre + deepest->normal * (deepest->depth + clearance);
            const float into = velocity.dot(deepest->normal);
            if (into < 0.0f) {
                velocity = velocity - deepest->normal * into;
            }
        }
    }

    /*
    * The surfaces the ball touches push back on it for the whole tick, the way a floor holds up a ball lying on it. Each
    * one the velocity would carry the ball into (nearest first) acts like a bounce, which below the resting speed simply
    * cancels the speed into it, and its friction slows the sliding. True if any did.
    */
    bool pressAgainstTouching(const ParticleSettings& settings, const Vec3& centre, Vec3& velocity, const StaticLevel& level) {
        std::vector<Contact> touching = contacts(level, Capsule{centre, centre, settings.radius + touchingDistance});
        std::sort(touching.begin(), touching.end(), [](const Contact& first, const Contact& second) {
            return first.depth > second.depth;
        });

        bool pressed = false;
        for (const Contact& surface : touching) {
            if (velocity.dot(surface.normal) < 0.0f) {
                velocity = bounceVelocity(velocity, surface.normal, settings);
                pressed = true;
            }
        }
        return pressed;
    }
}

FlightStep fly(const Vec3& velocity, const Vec3& acceleration, float seconds) {
    const Vec3 end = velocity + acceleration * seconds;
    return FlightStep{(velocity + end) * (0.5f * seconds), end};
}

/*
* Split the velocity into the part into the surface (along the normal) and the sliding part (across it). The surface's
* impulse sends the normal part back e times as fast, a change of (1 + e) times the speed in; Coulomb friction can then
* change the sliding speed by at most friction times that, and only towards zero. On a ball resting on a floor, gravity
* adds g dt into it each tick and nothing comes back (below the resting speed), so friction takes friction * g dt off the
* sliding speed each tick: the steady deceleration friction * g of kinetic friction.
*/
Vec3 bounceVelocity(const Vec3& velocity, const Vec3& normal, const ParticleSettings& settings) {
    const float into = velocity.dot(normal);
    if (into >= 0.0f) {
        return velocity;
    }
    const float speedIn = -into;
    const float restitution = speedIn < settings.restingSpeed ? 0.0f : settings.restitution;

    const Vec3 sliding = velocity - normal * into;
    const float slidingSpeed = sliding.length();
    const float slowing = settings.friction * (1.0f + restitution) * speedIn;
    const Vec3 slid = slidingSpeed > slowing ? sliding * (1.0f - slowing / slidingSpeed) : Vec3{};

    return slid + normal * (restitution * speedIn);
}

/*
* One tick, in order:
* 1. Out of any overlap.
* 2. If it touches something it would move into this tick, that surface pushes for the whole tick: gravity goes into the
*    velocity at once, the surface's push takes it back out (with friction), and the ball moves at the result. Otherwise
*    it is in free flight under gravity, which fly() follows exactly.
* 3. Sweep the flight through the level. At the first surface met, stop there (a little clear of it), work out the
*    velocity it arrived with, bounce, and fly on for the rest of the tick, up to maxBounces surfaces. Where the sweep
*    meets a surface, the share of the path covered stands in for the share of the time (exact for a straight path; the
*    parabola strays from its chord by at most g dt^2 / 8, under a tenth of a millimetre at 120 ticks a second).
*/
void moveParticle(Particle& particle, Vec3& centre, const StaticLevel& level, float seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0f) {
        return;
    }
    const ParticleSettings& settings = particle.settings;
    const Vec3 gravity{0.0f, -settings.gravity, 0.0f};

    pushOut(settings, centre, particle.velocity, level);

    Vec3 velocity = particle.velocity + gravity * seconds;
    const bool supported = pressAgainstTouching(settings, centre, velocity, level);
    if (!supported) {
        velocity = particle.velocity;
    }
    const Vec3 acceleration = supported ? Vec3{} : gravity;

    float timeLeft = seconds;
    int bounces = 0;
    while (timeLeft > 0.0f) {
        const FlightStep step = fly(velocity, acceleration, timeLeft);
        const std::optional<LevelHit> hit = sweepSphere(level, ball(settings, centre), step.displacement);
        if (!hit) {
            centre = centre + step.displacement;
            velocity = step.velocity;
            break;
        }

        const float path = step.displacement.length();
        const float spent = path > negligible ? timeLeft * (hit->hit.distance / path) : 0.0f;
        const Vec3 arriving = velocity + acceleration * spent;
        centre = hit->hit.distance > 0.0f ? hit->hit.point + hit->hit.normal * clearance : hit->hit.point;
        velocity = bounceVelocity(arriving, hit->hit.normal, settings);
        timeLeft -= spent;

        if (++bounces >= settings.maxBounces) {
            break;
        }
    }
    particle.velocity = velocity;
}

void moveParticles(World& world, const StaticLevel& level, float seconds) {
    world.each<Particle, Transform>([&](Entity, Particle& particle, Transform& transform) {
        moveParticle(particle, transform.position, level, seconds);
    });
}

void visit(ParticleSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Radius", settings.radius, {.min = 0.001f, .sliderMin = 0.01f, .sliderMax = 1.0f, .unit = "m"});
    visitor.field("Gravity", settings.gravity, {.sliderMin = 0.0f, .sliderMax = 30.0f, .unit = "m/s^2"});
    visitor.field("Restitution", settings.restitution, {.min = 0.0f, .max = 1.0f});
    visitor.field("Friction", settings.friction, {.min = 0.0f, .sliderMax = 2.0f});
    visitor.field("Resting speed", settings.restingSpeed, {.min = 0.0f, .sliderMax = 5.0f, .unit = "m/s"});
    visitor.field("Max bounces", settings.maxBounces, {.min = 1, .sliderMax = 16});
}
