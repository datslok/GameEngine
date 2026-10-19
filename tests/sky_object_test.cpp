#include "scene/sky_object.h"

#include <cassert>
#include <cmath>

namespace {
    bool near(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) <= tolerance;
    }

    // The object sits the given distance from the camera, straight along the way towards it (which need not be unit
    // length), sized so its edge is the angular radius off that line as the camera sees it.
    void testPoseIsAlongTheDirectionAtTheAngularSize() {
        const Vec3 camera{3.0f, 2.0f, -5.0f};
        const Vec3 towards{0.0f, 3.0f, -4.0f}; // length 5
        const float angularRadius = 0.01f;
        const Transform pose = skyObjectPose(camera, towards, 900.0f, angularRadius);

        const Vec3 offset = pose.position - camera;
        assert(near(offset.length(), 900.0f, 1e-2f));
        assert(near(offset.normalized().dot(towards.normalized()), 1.0f, 1e-6f));

        // A unit sphere scaled by r, seen from d away, spans asin(r / d) either side of its centre.
        assert(near(pose.scale.x, pose.scale.y, 0.0f) && near(pose.scale.y, pose.scale.z, 0.0f));
        assert(near(std::asin(pose.scale.x / 900.0f), angularRadius, 1e-6f));
    }

    // No parallax: wherever the camera goes, the object is in the same direction and looks the same size.
    void testNoParallax() {
        const Vec3 towards{0.4f, 0.5f, -0.7f};
        const Transform here = skyObjectPose(Vec3{0.0f, 2.0f, 0.0f}, towards, 900.0f, 0.005f);
        const Transform there = skyObjectPose(Vec3{250.0f, 40.0f, -180.0f}, towards, 900.0f, 0.005f);

        const Vec3 fromHere = here.position - Vec3{0.0f, 2.0f, 0.0f};
        const Vec3 fromThere = there.position - Vec3{250.0f, 40.0f, -180.0f};
        assert(near((fromHere - fromThere).length(), 0.0f, 1e-3f));
        assert(here.scale.x == there.scale.x);
    }

    // A disc's solid angle, the patch of sky it covers: about pi theta^2 when small, a hemisphere (2 pi) at 90 degrees
    // and the whole sky (4 pi) at 180.
    void testSolidAngle() {
        assert(near(discSolidAngle(0.00465f) / (3.14159265f * 0.00465f * 0.00465f), 1.0f, 1e-4f));
        assert(near(discSolidAngle(1.57079633f), 2.0f * 3.14159265f, 1e-5f));
        assert(near(discSolidAngle(3.14159265f), 4.0f * 3.14159265f, 1e-5f));
        assert(discSolidAngle(0.0f) == 0.0f);
    }

    /*
    * A disc that gives a light's illuminance must glow with radiance L = E / solid angle. The engine's light values
    * already include Lambert's 1/pi (a white surface facing a light of value v shows v), so E = pi v, and L = pi v / omega.
    * A small one checks the relation: L times the solid angle gives back pi v.
    */
    void testDiscRadianceGivesTheLightsIlluminance() {
        const Vec3 light{0.002f, 0.001f, 0.0005f};
        const float angularRadius = 0.0465f;
        const Vec3 radiance = discRadiance(light, angularRadius);
        const float solidAngle = discSolidAngle(angularRadius);

        assert(near(radiance.x * solidAngle, 3.14159265f * light.x, 1e-6f));
        assert(near(radiance.y * solidAngle, 3.14159265f * light.y, 1e-6f));
        assert(near(radiance.z * solidAngle, 3.14159265f * light.z, 1e-6f));

        // Twice as wide, four times the sky: a quarter as bright, the same light in total.
        const Vec3 wider = discRadiance(light, 2.0f * angularRadius);
        assert(near(wider.x / radiance.x, 0.25f, 1e-3f));

        assert(discRadiance(Vec3{0.0f, 0.0f, 0.0f}, angularRadius).x == 0.0f);
    }

    /*
    * The real sun is about 46,000 times brighter than a white surface it lights, which overflows the half floats the
    * scene is drawn into (65,504 at most). Above maxSkyRadiance the whole colour is scaled down together, so its hue stays.
    */
    void testDiscRadianceIsCappedKeepingItsColour() {
        const Vec3 sunlight{3.0f, 2.7f, 2.4f};
        const Vec3 radiance = discRadiance(sunlight, 0.00465f);

        assert(near(radiance.x, maxSkyRadiance, 1e-2f));
        assert(near(radiance.y / radiance.x, 0.9f, 1e-5f));
        assert(near(radiance.z / radiance.x, 0.8f, 1e-5f));
        assert(maxSkyRadiance < 65504.0f / 2.0f);
    }
}

void testSkyObject() {
    testPoseIsAlongTheDirectionAtTheAngularSize();
    testNoParallax();
    testSolidAngle();
    testDiscRadianceGivesTheLightsIlluminance();
    testDiscRadianceIsCappedKeepingItsColour();
}
