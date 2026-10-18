#include "math/transform.h"
#include "physics/particle.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace {
    constexpr float tick = 1.0f / 120.0f;

    bool nearVector(const Vec3& actual, const Vec3& expected, float tolerance) {
        return (actual - expected).length() <= tolerance;
    }

    // Runs a particle for the given time at the engine's tick, calling watch(centre) after each tick.
    template <typename Watch>
    void run(Particle& particle, Vec3& centre, const StaticLevel& level, float seconds, Watch watch) {
        const int ticks = static_cast<int>(std::lround(seconds / tick));
        for (int i = 0; i < ticks; ++i) {
            moveParticle(particle, centre, level, tick);
            watch(centre);
        }
    }

    void run(Particle& particle, Vec3& centre, const StaticLevel& level, float seconds) {
        run(particle, centre, level, seconds, [](const Vec3&) {});
    }

    // Flight under a constant acceleration, step by step, lands exactly on x0 + v0 t + a t^2 / 2 (and v0 + a t).
    void testFlightIsExact() {
        const Vec3 start{1.0f, 2.0f, 3.0f};
        const Vec3 launch{3.0f, 8.0f, -2.0f};
        const Vec3 gravity{0.0f, -9.81f, 0.0f};

        Vec3 position = start;
        Vec3 velocity = launch;
        for (int i = 0; i < 240; ++i) {
            const FlightStep step = fly(velocity, gravity, tick);
            position = position + step.displacement;
            velocity = step.velocity;
        }
        const float t = 240.0f * tick;
        assert(nearVector(position, start + launch * t + gravity * (0.5f * t * t), 1e-4f));
        assert(nearVector(velocity, launch + gravity * t, 1e-4f));

        // With no obstacles, a particle follows the same arc.
        Particle particle;
        particle.settings.gravity = 9.81f;
        particle.velocity = launch;
        Vec3 centre = start;
        run(particle, centre, StaticLevel{}, 2.0f);
        assert(nearVector(centre, position, 1e-4f));
        assert(nearVector(particle.velocity, velocity, 1e-4f));
    }

    // Restitution sends the normal part back, scaled; Coulomb friction slows the sliding part by at most
    // friction * (1 + e) * the normal speed, and never turns it round.
    void testBounceVelocity() {
        ParticleSettings settings;
        settings.restitution = 0.5f;
        settings.friction = 0.4f;
        settings.restingSpeed = 1.0f;
        const Vec3 up{0.0f, 1.0f, 0.0f};

        assert(nearVector(bounceVelocity(Vec3{0.0f, -5.0f, 0.0f}, up, settings), Vec3{0.0f, 2.5f, 0.0f}, 1e-5f));
        // Leaving the surface already: untouched.
        assert(nearVector(bounceVelocity(Vec3{2.0f, 1.0f, 0.0f}, up, settings), Vec3{2.0f, 1.0f, 0.0f}, 1e-6f));

        ParticleSettings slippery = settings;
        slippery.friction = 0.0f;
        assert(nearVector(bounceVelocity(Vec3{3.0f, -4.0f, 0.0f}, up, slippery), Vec3{3.0f, 2.0f, 0.0f}, 1e-5f));

        // Friction 0.2 takes 0.2 * 1.5 * 4 = 1.2 off the sliding speed of 3.
        ParticleSettings rough = settings;
        rough.friction = 0.2f;
        assert(nearVector(bounceVelocity(Vec3{3.0f, -4.0f, 0.0f}, up, rough), Vec3{1.8f, 2.0f, 0.0f}, 1e-5f));

        // Much more friction than sliding: it stops sliding, but does not slide backwards.
        ParticleSettings sticky = settings;
        sticky.friction = 1.0f;
        assert(nearVector(bounceVelocity(Vec3{0.5f, -4.0f, 0.0f}, up, sticky), Vec3{0.0f, 2.0f, 0.0f}, 1e-5f));

        // Arriving slower than the resting speed: no bounce at all, and friction works from the normal speed alone.
        assert(nearVector(bounceVelocity(Vec3{2.0f, -0.5f, 0.0f}, up, settings), Vec3{1.8f, 0.0f, 0.0f}, 1e-5f));

        // A tilted surface: the result leaves it, and the sliding part keeps its direction.
        const Vec3 tilted = Vec3{1.0f, 2.0f, 0.5f}.normalized();
        const Vec3 incoming{-3.0f, -6.0f, 4.0f};
        const Vec3 outgoing = bounceVelocity(incoming, tilted, settings);
        assert(outgoing.dot(tilted) > 0.0f);
        const Vec3 slideIn = incoming - tilted * incoming.dot(tilted);
        const Vec3 slideOut = outgoing - tilted * outgoing.dot(tilted);
        assert(slideOut.length() < slideIn.length());
        assert(slideOut.normalized().dot(slideIn.normalized()) > 0.9999f);
    }

    // However fast, a particle never passes through a thin wall: it bounces back off it.
    void testNoTunnelling() {
        StaticLevel level;
        level.add(block(Vec3{0.0f, -2.0f, -2.0f}, Vec3{0.02f, 2.0f, 2.0f}));
        Particle particle;
        particle.settings.radius = 0.05f;
        particle.settings.gravity = 0.0f;
        particle.velocity = Vec3{200.0f, 0.0f, 0.0f};
        Vec3 centre{-1.0f, 0.0f, 0.0f};

        moveParticle(particle, centre, level, tick);
        assert(centre.x < -particle.settings.radius + 1e-4f);
        assert(particle.velocity.x < 0.0f);
    }

    // Dropped onto a frictionless floor, each bounce peaks at e^2 of the height before (speed scales by e, height by
    // speed squared).
    void testBounceHeights() {
        const StaticLevel level = flatLevel();
        Particle particle;
        particle.settings.restitution = 0.5f;
        particle.settings.friction = 0.0f;
        const float radius = particle.settings.radius;
        Vec3 centre{0.0f, 2.0f + radius, 0.0f};

        // Fall, bounce, and record the top of the next flight.
        bool bounced = false;
        float peak = 0.0f;
        run(particle, centre, level, 1.2f, [&](const Vec3& now) {
            if (particle.velocity.y > 0.0f) {
                bounced = true;
            }
            if (bounced) {
                peak = std::max(peak, now.y - radius);
            }
        });
        assert(bounced);
        assert(near(peak, 0.5f, 0.02f));
    }

    // Thrown along a floor with friction it bounces, slides and stops, and then stays exactly where it stopped.
    void testComesToRest() {
        const StaticLevel level = flatLevel();
        Particle particle;
        particle.velocity = Vec3{2.0f, 3.0f, 1.0f};
        Vec3 centre{0.0f, 1.0f, 0.0f};

        run(particle, centre, level, 5.0f);
        assert(near(centre.y, particle.settings.radius, 2e-3f));
        assert(centre.y >= particle.settings.radius);
        const Vec3 stopped = centre;
        run(particle, centre, level, 2.0f);
        assert(nearVector(centre, stopped, 1e-6f));
        assert(particle.velocity.length() < 1e-6f);
    }

    // Sliding on level ground at speed v with friction mu stops after v^2 / (2 mu g), as kinetic friction should.
    void testSlidingFriction() {
        const StaticLevel level = flatLevel();
        Particle particle;
        particle.settings.friction = 0.5f;
        particle.velocity = Vec3{4.0f, 0.0f, 0.0f};
        Vec3 centre{0.0f, particle.settings.radius, 0.0f};

        run(particle, centre, level, 3.0f);
        const float expected = 16.0f / (2.0f * 0.5f * particle.settings.gravity);
        assert(near(centre.x, expected, expected * 0.03f));
        assert(particle.velocity.length() < 1e-6f);
    }

    // On a ramp, a particle stays put if friction can hold it (mu > tan of the angle), and slides down if not.
    void testRamps() {
        const auto placeOnRamp = [](float degrees, float along, float radius) {
            const float slope = degrees * std::numbers::pi_v<float> / 180.0f;
            const Vec3 surface{0.0f, along * std::sin(slope), along * std::cos(slope)};
            const Vec3 normal{0.0f, std::cos(slope), -std::sin(slope)};
            return surface + normal * radius;
        };

        StaticLevel gentle = flatLevel();
        addRamp(gentle, 10.0f, 8.0f);
        Particle holding;
        Vec3 heldCentre = placeOnRamp(10.0f, 4.0f, holding.settings.radius);
        const Vec3 start = heldCentre;
        run(holding, heldCentre, gentle, 2.0f);
        assert(nearVector(heldCentre, start, 1e-3f));

        StaticLevel steep = flatLevel();
        addRamp(steep, 40.0f, 8.0f);
        Particle sliding;
        Vec3 slidCentre = placeOnRamp(40.0f, 4.0f, sliding.settings.radius);
        const Vec3 top = slidCentre;
        run(sliding, slidCentre, steep, 1.0f);
        // Down the slope at g (sin - mu cos), about 3.3 m/s^2 here: about 1.6 m in a second.
        assert((top - slidCentre).length() > 1.0f);
        assert(slidCentre.y < top.y && slidCentre.z < top.z);
    }

    // Started overlapping the floor, it is pushed out first.
    void testPushedOut() {
        const StaticLevel level = flatLevel();
        Particle particle;
        Vec3 centre{0.0f, particle.settings.radius - 0.05f, 0.0f};
        moveParticle(particle, centre, level, tick);
        assert(centre.y >= particle.settings.radius - 1e-4f);
    }

    // Random throws round a closed room with a step and a pillar: no particle ever ends a tick overlapping the level,
    // and none gets out.
    void testNeverOverlaps() {
        StaticLevel room;
        room.add(block(Vec3{-6.0f, -1.0f, -6.0f}, Vec3{6.0f, 0.0f, 6.0f}));
        room.add(block(Vec3{-6.0f, 4.0f, -6.0f}, Vec3{6.0f, 5.0f, 6.0f}));
        room.add(block(Vec3{-7.0f, -1.0f, -6.0f}, Vec3{-6.0f, 5.0f, 6.0f}));
        room.add(block(Vec3{6.0f, -1.0f, -6.0f}, Vec3{7.0f, 5.0f, 6.0f}));
        room.add(block(Vec3{-6.0f, -1.0f, -7.0f}, Vec3{6.0f, 5.0f, -6.0f}));
        room.add(block(Vec3{-6.0f, -1.0f, 6.0f}, Vec3{6.0f, 5.0f, 7.0f}));
        room.add(block(Vec3{1.0f, 0.0f, -6.0f}, Vec3{6.0f, 0.3f, 6.0f}));
        room.add(block(Vec3{-3.0f, 0.0f, 1.0f}, Vec3{-2.0f, 4.0f, 2.0f}));

        std::uint64_t state = 5;
        const auto next = [&](float low, float high) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return low + (high - low) * static_cast<float>(state >> 40) / static_cast<float>(1ull << 24);
        };

        for (int throwNumber = 0; throwNumber < 20; ++throwNumber) {
            Particle particle;
            particle.settings.radius = next(0.05f, 0.3f);
            particle.settings.restitution = next(0.0f, 0.9f);
            particle.velocity = Vec3{next(-30.0f, 30.0f), next(-30.0f, 30.0f), next(-30.0f, 30.0f)};
            Vec3 centre{0.0f, 2.0f, -3.0f};

            run(particle, centre, room, 2.0f, [&](const Vec3& now) {
                for (const Contact& found : contacts(room, Capsule{now, now, particle.settings.radius})) {
                    assert(found.depth <= 1e-4f);
                }
                assert(std::abs(now.x) < 6.0f && std::abs(now.z) < 6.0f && now.y > 0.0f && now.y < 4.0f);
            });
        }
    }

    // No time, no movement.
    void testNoTime() {
        const StaticLevel level = flatLevel();
        Particle particle;
        particle.velocity = Vec3{1.0f, 2.0f, 3.0f};
        Vec3 centre{0.0f, 1.0f, 0.0f};
        moveParticle(particle, centre, level, 0.0f);
        moveParticle(particle, centre, level, -1.0f);
        assert(nearVector(centre, Vec3{0.0f, 1.0f, 0.0f}, 0.0f));
        assert(nearVector(particle.velocity, Vec3{1.0f, 2.0f, 3.0f}, 0.0f));
    }

    // moveParticles moves every entity with a Particle and a Transform, whose position is the particle's centre.
    void testMoveParticles() {
        const StaticLevel level = flatLevel();
        World world;
        const Entity ball = world.create();
        world.add(ball, Transform{Vec3{0.0f, 3.0f, 0.0f}});
        Particle particle;
        particle.velocity = Vec3{1.0f, 0.0f, 0.0f};
        world.add(ball, particle);

        moveParticles(world, level, tick);
        const Vec3 moved = world.get<Transform>(ball).position;
        assert(moved.x > 0.0f && moved.y < 3.0f);
    }
}

void testParticle() {
    testFlightIsExact();
    testBounceVelocity();
    testNoTunnelling();
    testBounceHeights();
    testComesToRest();
    testSlidingFriction();
    testRamps();
    testPushedOut();
    testNeverOverlaps();
    testNoTime();
    testMoveParticles();
}
