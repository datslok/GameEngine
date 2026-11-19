#include "ecs/world.h"
#include "math/transform.h"
#include "scene/beacon.h"
#include "scene/light.h"
#include "scene/lighting.h"

#include <cassert>
#include <cmath>

namespace {
    bool near(float actual, float expected, float tolerance = 1e-5f) {
        return std::abs(actual - expected) <= tolerance;
    }

    /*
    * The flash (FAA L-864 by default: 30 a minute, 0.5 s on, an LED's quick ramps): on for its on-time from each period's
    * start, ramping up and down over the ramp, dark the rest; shifted by its phase; the same every period, before the
    * start too; steady with no period.
    */
    void testTheFlashFollowsTheClock() {
        const BeaconFlash flash;
        assert(near(flash.periodSeconds, 2.0f) && near(flash.onSeconds, 0.5f) && flash.rampSeconds > 0.0f && flash.rampSeconds <= 0.1f);

        assert(near(beaconLevel(flash, 0.25), 1.0f));
        assert(near(beaconLevel(flash, 1.0), 0.0f) && near(beaconLevel(flash, 1.9), 0.0f));
        assert(near(beaconLevel(flash, 0.5 * flash.rampSeconds), 0.5f, 1e-4f));
        assert(near(beaconLevel(flash, flash.onSeconds - 0.5 * flash.rampSeconds), 0.5f, 1e-4f));
        assert(near(beaconLevel(flash, 0.0), 0.0f) && near(beaconLevel(flash, flash.onSeconds), 0.0f));
        assert(near(beaconLevel(flash, 6.25), 1.0f) && near(beaconLevel(flash, -1.75), 1.0f));

        // A long clock keeps its fraction of a period exactly (seconds are doubles).
        assert(near(beaconLevel(flash, 86400.0 * 30.0 + 0.25), 1.0f));

        BeaconFlash late = flash;
        late.phaseSeconds = 1.0f;
        assert(near(beaconLevel(late, 1.25), 1.0f) && near(beaconLevel(late, 0.25), 0.0f));

        BeaconFlash steady = flash;
        steady.periodSeconds = 0.0f;
        assert(near(beaconLevel(steady, 1.0), 1.0f) && near(beaconLevel(steady, 123.4), 1.0f));

        // An on-time longer than the period is always on; none is always dark.
        BeaconFlash always = flash;
        always.onSeconds = 5.0f;
        always.rampSeconds = 0.0f;
        assert(near(beaconLevel(always, 1.3), 1.0f));
        BeaconFlash never = flash;
        never.onSeconds = 0.0f;
        assert(near(beaconLevel(never, 0.25), 0.0f));
    }

    /*
    * flashBeacons sets every beacon's level from the clock, and its spill (the light it throws on what is round it) as a
    * point light on its entity, unshadowed and fast (it pulses), at the spill times the level; a beacon without spill
    * gets no light.
    */
    void testFlashingSetsTheLevelsAndTheSpill() {
        World world;
        const Entity lit = world.create();
        world.add(lit, Transform{.position = Vec3{1.0f, 2.0f, 3.0f}});
        world.add(lit, Beacon{.colour = Vec3{1.0f, 0.1f, 0.0f}, .intensity = 100.0f, .spill = 30.0f, .spillRange = 20.0f});
        const Entity bare = world.create();
        world.add(bare, Transform{});
        world.add(bare, Beacon{.intensity = 50.0f});

        flashBeacons(world, 0.25);
        assert(near(world.get<Beacon>(lit).level, 1.0f) && near(world.get<Beacon>(bare).level, 1.0f));
        assert(world.has<PointLight>(lit) && !world.has<PointLight>(bare));
        const PointLight& spill = world.get<PointLight>(lit);
        assert(near(spill.intensity, 30.0f) && near(spill.range, 20.0f) && !spill.castsShadows && spill.fast);
        assert(near(spill.colour.x, 1.0f) && near(spill.colour.y, 0.1f));

        flashBeacons(world, 1.0);
        assert(near(world.get<Beacon>(lit).level, 0.0f) && near(world.get<PointLight>(lit).intensity, 0.0f));

        // Its spill turned off takes the light away.
        world.get<Beacon>(lit).spill = 0.0f;
        flashBeacons(world, 0.25);
        assert(!world.has<PointLight>(lit));
    }

    // The frame gathers the lit beacons where they are, their light the colour times intensity times level; dark ones not.
    void testTheFrameGathersTheLitBeacons() {
        World world;
        const Entity on = world.create();
        world.add(on, Transform{.position = Vec3{5.0f, 6.0f, 7.0f}});
        world.add(on, Beacon{.colour = Vec3{1.0f, 0.5f, 0.0f}, .intensity = 10.0f, .lensRadius = 0.2f, .level = 0.5f});
        const Entity off = world.create();
        world.add(off, Transform{});
        world.add(off, Beacon{.level = 0.0f});

        const FrameLighting lighting = collectLighting(world, 1.0f);
        assert(lighting.beacons.size() == 1);
        const PlacedBeacon& placed = lighting.beacons.front();
        assert(near(placed.position.x, 5.0f) && near(placed.position.z, 7.0f) && near(placed.lensRadius, 0.2f));
        assert(near(placed.light.x, 5.0f) && near(placed.light.y, 2.5f) && near(placed.light.z, 0.0f));
    }
}

void testBeacon() {
    testTheFlashFollowsTheClock();
    testFlashingSetsTheLevelsAndTheSpill();
    testTheFrameGathersTheLitBeacons();
}
