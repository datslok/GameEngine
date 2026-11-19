#include "scene/beacon.h"

#include "scene/light.h"

#include <algorithm>
#include <cmath>

/*
* The time into the period (taken in doubles, so a clock of days keeps its fraction exactly), then the product of a rise
* from the period's start and a fall to the on-time's end, each over the ramp, clamped: a trapezium, or a step without
* ramps.
*/
float beaconLevel(const BeaconFlash& flash, double seconds) {
    if (!(flash.periodSeconds > 0.0f)) {
        return 1.0f;
    }

    if (flash.onSeconds >= flash.periodSeconds) {
        return 1.0f;
    }

    const double period = flash.periodSeconds;
    double into = std::fmod(seconds - static_cast<double>(flash.phaseSeconds), period);

    if (into < 0.0) {
        into += period;
    }

    const float time = static_cast<float>(into);

    if (!(flash.rampSeconds > 0.0f)) {
        return time < flash.onSeconds ? 1.0f : 0.0f;
    }

    const float rise = std::clamp(time / flash.rampSeconds, 0.0f, 1.0f);
    const float fall = std::clamp((flash.onSeconds - time) / flash.rampSeconds, 0.0f, 1.0f);
    return rise * fall;
}

/*
* The spill is set on the light, not added each frame, so a beacon's light keeps its seat in the renderer's lights; fast,
* since it changes every flash (the fog lights it fresh and the probes keep its bounce apart).
*/
void flashBeacons(World& world, double seconds) {
    world.each<Beacon>([&](Entity entity, Beacon& beacon) {
        beacon.level = beaconLevel(beacon.flash, seconds);

        if (!(beacon.spill > 0.0f)) {
            if (world.has<PointLight>(entity)) {
                world.remove<PointLight>(entity);
            }

            return;
        }

        if (!world.has<PointLight>(entity)) {
            world.add(entity, PointLight{.castsShadows = false, .fast = true});
        }

        PointLight& light = world.get<PointLight>(entity);
        light.colour = beacon.colour;
        light.intensity = beacon.spill * beacon.level;
        light.range = std::max(beacon.spillRange, 0.01f);
        light.castsShadows = false;
        light.fast = true;
    });
}
