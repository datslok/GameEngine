#include "render/gpu/atmosphere.h"
#include "render/gpu/ddgi.h"
#include "scene/lighting.h"
#include "scene/sky_object.h"
#include "ecs/world.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {
    constexpr float pi = 3.14159265358979f;

    bool nearlyEqual(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool relativelyEqual(const Vec3& actual, const Vec3& expected, float share) {
        return std::abs(actual.x - expected.x) <= share * std::abs(expected.x) && std::abs(actual.y - expected.y) <= share * std::abs(expected.y) &&
               std::abs(actual.z - expected.z) <= share * std::abs(expected.z);
    }

    const Atmosphere earth;
    const float groundRadius = earth.bottomRadius + 0.002f; // a camera 2 m up

    /*
    * Straight up from the ground, the light kept is exp(-optical depth), and the optical depth has a closed form: each
    * exponential layer's coefficient times its scale height (times 1 - exp(-100 / H), the air ending at 100 km), and the
    * ozone tent's peak coefficient times its area, its half width. Green: 0.0136 x 8 + 0.0084 x 1.2 + 0.0019 x 15 = 0.147,
    * so 86% of the light gets through. The march (40 steps) agrees within half a percent.
    */
    void testTransmittanceStraightUp() {
        const float height = earth.topRadius - earth.bottomRadius;
        const auto layer = [&](float coefficient, float scaleHeight) {
            return coefficient * scaleHeight * (1.0f - std::exp(-height / scaleHeight));
        };
        const float mieExtinction = earth.mieScattering + earth.mieAbsorption;
        const Vec3 depth{layer(earth.rayleighScattering.x, earth.rayleighScaleHeight) + layer(mieExtinction, earth.mieScaleHeight) +
                             earth.ozoneAbsorption.x * earth.ozoneHalfWidth,
                         layer(earth.rayleighScattering.y, earth.rayleighScaleHeight) + layer(mieExtinction, earth.mieScaleHeight) +
                             earth.ozoneAbsorption.y * earth.ozoneHalfWidth,
                         layer(earth.rayleighScattering.z, earth.rayleighScaleHeight) + layer(mieExtinction, earth.mieScaleHeight) +
                             earth.ozoneAbsorption.z * earth.ozoneHalfWidth};
        const Vec3 expected{std::exp(-depth.x), std::exp(-depth.y), std::exp(-depth.z)};

        assert(relativelyEqual(atmosphereTransmittance(earth, earth.bottomRadius, 1.0f), expected, 0.005f));
        assert(nearlyEqual(expected.y, 0.863f, 0.002f));
    }

    // Lower rays cross more air: less light, and less blue than red (the low sun reddens); below the horizon the planet
    // is in the way: none.
    void testTransmittanceTowardsTheHorizon() {
        const Vec3 high = atmosphereTransmittance(earth, groundRadius, 1.0f);
        const Vec3 middle = atmosphereTransmittance(earth, groundRadius, 0.5f);
        const Vec3 low = atmosphereTransmittance(earth, groundRadius, 0.05f);

        assert(high.y > middle.y && middle.y > low.y && low.y > 0.0f);
        assert(low.x / low.z > middle.x / middle.z && middle.x / middle.z > high.x / high.z);

        const Vec3 below = atmosphereTransmittance(earth, groundRadius, -0.2f);
        assert(below.x == 0.0f && below.y == 0.0f && below.z == 0.0f);

        // From space looking down past the planet's edge, and above the air: all gets through.
        const Vec3 above = atmosphereTransmittance(earth, earth.topRadius + 1.0f, 0.3f);
        assert(above.x == 1.0f && above.y == 1.0f && above.z == 1.0f);
    }

    // The medium: each layer at its surface value on the ground, thinning by e over its scale height; the ozone tent.
    void testMedium() {
        const AtmosphereMedium ground = atmosphereMedium(earth, 0.0f);
        assert(nearlyEqual(ground.rayleighScattering.z, earth.rayleighScattering.z, 1e-7f));
        assert(nearlyEqual(ground.mieScattering, earth.mieScattering, 1e-7f));
        assert(nearlyEqual(ground.extinction.y, earth.rayleighScattering.y + earth.mieScattering + earth.mieAbsorption, 1e-7f));
        assert(nearlyEqual(ground.scattering.y, earth.rayleighScattering.y + earth.mieScattering, 1e-7f));

        const AtmosphereMedium scaleHeight = atmosphereMedium(earth, earth.rayleighScaleHeight);
        assert(nearlyEqual(scaleHeight.rayleighScattering.x, earth.rayleighScattering.x / std::exp(1.0f), 1e-6f));

        // At the ozone layer's centre its whole absorption adds to the extinction.
        const AtmosphereMedium ozone = atmosphereMedium(earth, earth.ozoneCentre);
        const float withoutOzone = ozone.rayleighScattering.y + ozone.mieScattering + earth.mieAbsorption * (ozone.mieScattering / earth.mieScattering);
        assert(nearlyEqual(ozone.extinction.y - withoutOzone, earth.ozoneAbsorption.y, 1e-6f));
    }

    // Both phase functions spread a scattered photon over the sphere: their integral is 1. Rayleigh's is symmetric front
    // to back; Henyey-Greenstein's with g = 0.8 mostly forward.
    void testPhaseFunctionsAreNormalised() {
        const int count = 4096;
        float rayleigh = 0.0f;
        float mie = 0.0f;

        for (int index = 0; index < count; ++index) {
            const float cosine = sphericalFibonacci(index, count).z;
            rayleigh += rayleighPhase(cosine);
            mie += henyeyGreensteinPhase(earth.mieAnisotropy, cosine);
        }

        const float solidAngle = 4.0f * pi / static_cast<float>(count);
        assert(nearlyEqual(rayleigh * solidAngle, 1.0f, 1e-3f));
        assert(nearlyEqual(mie * solidAngle, 1.0f, 1e-2f));
        assert(rayleighPhase(1.0f) == rayleighPhase(-1.0f));
        assert(henyeyGreensteinPhase(0.8f, 1.0f) > 100.0f * henyeyGreensteinPhase(0.8f, -1.0f));
        assert(nearlyEqual(henyeyGreensteinPhase(0.0f, 0.3f), 1.0f / (4.0f * pi), 1e-6f));
    }

    // A ray from inside a sphere meets it once going out; from outside, at its near side, or not at all; one that grazes
    // the ground from a metre above meets it where the geometry says, without the rounding of the planet's squares.
    void testRaySphere() {
        assert(nearlyEqual(rayDistanceToSphere(0.0f, 1.0f, 2.0f), 2.0f, 1e-6f));
        assert(nearlyEqual(rayDistanceToSphere(5.0f, -1.0f, 2.0f), 3.0f, 1e-6f));
        assert(rayDistanceToSphere(5.0f, 1.0f, 2.0f) < 0.0f);
        assert(rayDistanceToSphere(5.0f, -0.5f, 2.0f) < 0.0f);

        // From h above a sphere of radius R, looking down at cos -m: about h / m for small h (a flat ground). A float
        // radius of 6,360.001 km has a last digit of half a metre, so h is 1 m only within that, and the distance 10 m
        // within a twentieth.
        const float distance = rayDistanceToSphere(earth.bottomRadius + 0.001f, -0.1f, earth.bottomRadius);
        assert(nearlyEqual(distance, 0.01f, 5e-4f));
    }

    // The tables' coordinates go both ways: a ray (or a direction) to its texel and back.
    void testTableCoordinatesRoundTrip() {
        for (const float radius : {earth.bottomRadius + 0.01f, earth.bottomRadius + 3.0f, earth.bottomRadius + 60.0f}) {
            for (const float cosine : {-0.1f, 0.0f, 0.2f, 0.7f, 1.0f}) {
                if (cosine < -std::sqrt(1.0f - (earth.bottomRadius / radius) * (earth.bottomRadius / radius))) {
                    continue; // meets the planet: not in the transmittance table
                }

                const AtmosphereRay back = transmittanceLutRay(earth, transmittanceLutUv(earth, radius, cosine));
                assert(nearlyEqual(back.radius, radius, 2e-3f));
                assert(nearlyEqual(back.cosZenith, cosine, 2e-3f));

                const AtmosphereRay scattering = multipleScatteringLutRay(earth, multipleScatteringLutUv(earth, radius, cosine));
                assert(nearlyEqual(scattering.radius, radius, 2e-3f));
                assert(nearlyEqual(scattering.cosZenith, cosine, 1e-4f));
            }
        }

        for (const float cosZenith : {-0.6f, -0.05f, 0.0f, 0.1f, 0.5f, 0.99f}) {
            for (const float cosLight : {-1.0f, -0.3f, 0.4f, 1.0f}) {
                const bool meetsGround = cosZenith < -std::sqrt(1.0f - (earth.bottomRadius / groundRadius) * (earth.bottomRadius / groundRadius));
                const SkyViewDirection direction{cosZenith, cosLight, meetsGround};
                const SkyViewDirection back = skyViewLutDirection(earth, groundRadius, skyViewLutUv(earth, groundRadius, direction));
                assert(nearlyEqual(back.cosViewZenith, cosZenith, 2e-3f));
                assert(nearlyEqual(back.cosLightView, cosLight, 2e-3f));
                assert(back.meetsGround == meetsGround);
            }
        }
    }

    // A world direction's place in the sky view table: its height (cosine of the zenith angle) and its bearing from the
    // sun's (cosine of the angle between them seen from above), and whether it meets the ground.
    void testSkyViewLookup() {
        const Vec3 towardsSun = Vec3{0.6f, 0.5f, 0.0f}.normalized();
        const SkyViewDirection up = skyViewLookup(earth, groundRadius, Vec3{0.0f, 1.0f, 0.0f}, towardsSun);
        assert(nearlyEqual(up.cosViewZenith, 1.0f, 1e-6f) && !up.meetsGround);

        const SkyViewDirection away = skyViewLookup(earth, groundRadius, Vec3{-0.8f, 0.6f, 0.0f}, towardsSun);
        assert(nearlyEqual(away.cosLightView, -1.0f, 1e-5f) && nearlyEqual(away.cosViewZenith, 0.6f, 1e-6f));

        const SkyViewDirection side = skyViewLookup(earth, groundRadius, Vec3{0.0f, -0.6f, 0.8f}, towardsSun);
        assert(nearlyEqual(side.cosLightView, 0.0f, 1e-5f) && side.meetsGround);
    }

    /*
    * One step of the march, the paper's energy-conserving form: over a step of length dt with extinction s, the light
    * kept is exp(-s dt), and a constant source S adds S (1 - exp(-s dt)) / s, exactly the integral of S exp(-s t), weighted
    * by what the steps before let through.
    */
    void testMarchStep() {
        AtmosphereMedium medium;
        medium.extinction = Vec3{0.5f, 1.0f, 2.0f};
        medium.scattering = Vec3{0.25f, 0.5f, 1.0f};
        ScatteringMarch march;
        march.throughput = Vec3{0.5f, 0.5f, 0.5f};
        scatteringMarchStep(march, medium, Vec3{2.0f, 2.0f, 2.0f}, 0.3f);

        const float kept = std::exp(-1.0f * 0.3f);
        assert(nearlyEqual(march.throughput.y, 0.5f * kept, 1e-6f));
        assert(nearlyEqual(march.luminance.y, 0.5f * 2.0f * (1.0f - kept) / 1.0f, 1e-6f));
        assert(nearlyEqual(march.multipleScatteringAs1.y, 0.5f * 0.5f * (1.0f - kept) / 1.0f, 1e-6f));
    }

    /*
    * Light scattered twice or more: a positive amount per unit sun illuminance wherever the sun is up, more with the sun
    * high, and some still with the sun just below the horizon (twilight: sunlight on the upper air scatters down).
    */
    void testMultipleScattering() {
        const Vec3 noon = multipleScatteringTexel(earth, groundRadius, 1.0f);
        const Vec3 evening = multipleScatteringTexel(earth, groundRadius, 0.1f);
        const Vec3 dusk = multipleScatteringTexel(earth, groundRadius, -0.1f);

        assert(noon.y > evening.y && evening.y > dusk.y && dusk.y > 0.0f);
        assert(noon.z > noon.x); // blue scatters more
    }

    const AtmosphereLookups exact = exactAtmosphereLookups(earth);

    // The noon sky straight up is blue: more blue than green than red.
    void testTheSkyIsBlue() {
        const Vec3 sun{0.0f, 1.0f, 0.0f};
        const Vec3 zenith = skyRadiance(earth, groundRadius, Vec3{0.0f, 1.0f, 0.0f}, sun, Vec3{1.0f, 1.0f, 1.0f}, exact);
        assert(zenith.z > zenith.y && zenith.y > zenith.x && zenith.x > 0.0f);
    }

    // The sky is mirror symmetric about the sun's vertical plane, and its light is in proportion to the sun's.
    void testSkySymmetryAndScale() {
        const Vec3 sun = Vec3{0.5f, 0.3f, 0.0f}.normalized();
        const Vec3 left = skyRadiance(earth, groundRadius, Vec3{0.3f, 0.4f, 0.6f}.normalized(), sun, Vec3{1.0f, 1.0f, 1.0f}, exact);
        const Vec3 right = skyRadiance(earth, groundRadius, Vec3{0.3f, 0.4f, -0.6f}.normalized(), sun, Vec3{1.0f, 1.0f, 1.0f}, exact);
        assert(relativelyEqual(left, right, 1e-4f));

        const Vec3 doubled = skyRadiance(earth, groundRadius, Vec3{0.3f, 0.4f, 0.6f}.normalized(), sun, Vec3{2.0f, 2.0f, 2.0f}, exact);
        assert(relativelyEqual(doubled, left * 2.0f, 1e-5f));

        // Brighter round the sun (the aerosols' forward glow) than opposite it.
        const Vec3 towards = skyRadiance(earth, groundRadius, Vec3{0.55f, 0.35f, 0.05f}.normalized(), sun, Vec3{1.0f, 1.0f, 1.0f}, exact);
        const Vec3 opposite = skyRadiance(earth, groundRadius, Vec3{-0.55f, 0.35f, 0.05f}.normalized(), sun, Vec3{1.0f, 1.0f, 1.0f}, exact);
        assert(towards.y > 2.0f * opposite.y);
    }

    // At sunset the sky low towards the sun is redder than the noon sky low in the same place.
    void testSunsetIsRed() {
        const Vec3 view = Vec3{1.0f, 0.05f, 0.0f}.normalized();
        const Vec3 sunset = skyRadiance(earth, groundRadius, view, Vec3{1.0f, 0.02f, 0.0f}.normalized(), Vec3{1.0f, 1.0f, 1.0f}, exact);
        const Vec3 noon = skyRadiance(earth, groundRadius, view, Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, exact);
        assert(sunset.x / sunset.z > noon.x / noon.z);
    }

    /*
    * The ground: a matte surface of the albedo lit by the sun (cosine over pi), none from a sun below it; looking down
    * at it from just above, nearly all of that light, with a little haze.
    */
    void testGround() {
        const Vec3 lit = groundRadiance(earth, Vec3{2.0f, 2.0f, 2.0f}, 0.5f);
        assert(nearlyEqual(lit.x, 2.0f * earth.groundAlbedo.x * 0.5f / pi, 1e-6f));
        assert(groundRadiance(earth, Vec3{2.0f, 2.0f, 2.0f}, -0.5f).y == 0.0f);

        const Vec3 sun{0.0f, 1.0f, 0.0f};
        const Vec3 down = skyRadiance(earth, groundRadius, Vec3{0.0f, -1.0f, 0.0f}, sun, Vec3{1.0f, 1.0f, 1.0f}, exact);
        const Vec3 ground = groundRadiance(earth, atmosphereTransmittance(earth, earth.bottomRadius, 1.0f), 1.0f);
        assert(relativelyEqual(down, ground, 0.01f));
    }

    // The flat ambient a sky gives: a uniform sky drawn at B gives B (the engine's units), and only the sky above counts.
    void testSkyAmbient() {
        const Vec3 uniform = skyAmbient([](const Vec3&) { return Vec3{0.4f, 0.5f, 0.6f}; });
        assert(relativelyEqual(uniform, Vec3{0.4f, 0.5f, 0.6f}, 0.01f));

        const Vec3 groundOnly = skyAmbient([](const Vec3& direction) { return direction.y < 0.0f ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.0f, 0.0f, 0.0f}; });
        assert(groundOnly.x == 0.0f);
    }

    // The camera's height in the planet's frame: the ground's altitude plus its height in metres, never below the
    // smallest altitude.
    void testViewRadius() {
        Atmosphere raised = earth;
        raised.groundAltitude = 1.5f;
        assert(nearlyEqual(atmosphereViewRadius(raised, 500.0f), raised.bottomRadius + 2.0f, 1e-3f));
        assert(nearlyEqual(atmosphereViewRadius(earth, -100.0f), earth.bottomRadius + atmosphereMinAltitude, 1e-4f));
    }

    /*
    * Sunlight on the ground: the light's value above the air times what gets through on its way down to the camera; the
    * sun the sky is lit by is the brightest directional light.
    */
    void testSunlightThroughTheAir() {
        DirectionalLight sun;
        sun.direction = Vec3{0.0f, -1.0f, 0.0f};
        sun.colour = Vec3{1.0f, 1.0f, 1.0f};
        sun.intensity = 3.0f;
        const DirectionalLight ground = throughAtmosphere(sun, earth, groundRadius);
        const Vec3 kept = atmosphereTransmittance(earth, groundRadius, 1.0f);
        assert(relativelyEqual(ground.colour * ground.intensity, kept * 3.0f, 1e-5f));

        DirectionalLight moon = sun;
        moon.intensity = 0.01f;
        assert(atmosphereSun(std::vector<DirectionalLight>{moon, sun, moon}) == 1);
        assert(atmosphereSun(std::vector<DirectionalLight>{}) < 0);
    }

    /*
    * What the shaders read: the coefficients, radii, the camera's radius, the sun (its direction, its light above the
    * air as an illuminance, pi times its value, and its disc), present marked 1, and the frame's own ambient light.
    */
    void testPacking() {
        AtmosphereFrame frame;
        frame.viewRadius = groundRadius;
        frame.towardsSun = Vec3{0.0f, 1.0f, 0.0f};
        frame.sunLight = Vec3{2.0f, 2.0f, 2.0f};
        frame.discAngularRadius = 0.01f;
        frame.sunDisc = Vec3{100.0f, 90.0f, 80.0f};
        frame.ambient = Vec3{0.1f, 0.2f, 0.3f};
        const GpuAtmosphere packed = packAtmosphere(earth, frame);

        assert(packed.rayleigh[2] == earth.rayleighScattering.z && packed.rayleigh[3] == earth.rayleighScaleHeight);
        assert(packed.mie[0] == earth.mieScattering && packed.mie[3] == earth.mieAnisotropy);
        assert(packed.radii[0] == earth.bottomRadius && packed.radii[1] == earth.topRadius && packed.radii[3] == groundRadius);
        assert(packed.ground[3] == 1.0f);
        assert(nearlyEqual(packed.sunIlluminance[1], 2.0f * pi, 1e-5f));
        assert(nearlyEqual(packed.sun[3], std::cos(0.01f), 1e-7f));
        assert(packed.sunDisc[0] == 100.0f && packed.ambient[2] == 0.3f);
    }

    // An atmosphere component reaches the frame's lighting (the first, if several).
    void testCollected() {
        World world;
        assert(!collectLighting(world, 1.0f).atmosphere.has_value());

        Atmosphere thin;
        thin.rayleighScaleHeight = 4.0f;
        world.add(world.create(), thin);
        const FrameLighting lighting = collectLighting(world, 1.0f);
        assert(lighting.atmosphere.has_value() && lighting.atmosphere->rayleighScaleHeight == 4.0f);
    }
}

void testAtmosphere() {
    testTransmittanceStraightUp();
    testTransmittanceTowardsTheHorizon();
    testMedium();
    testPhaseFunctionsAreNormalised();
    testRaySphere();
    testTableCoordinatesRoundTrip();
    testSkyViewLookup();
    testMarchStep();
    testMultipleScattering();
    testTheSkyIsBlue();
    testSkySymmetryAndScale();
    testSunsetIsRed();
    testGround();
    testSkyAmbient();
    testViewRadius();
    testSunlightThroughTheAir();
    testPacking();
    testCollected();
}
