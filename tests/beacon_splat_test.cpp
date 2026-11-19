#include "render/gpu/beacon_splat.h"
#include "render/gpu/stars.h"

#include <cassert>
#include <cmath>
#include <numbers>
#include <vector>

namespace {
    bool near(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) <= tolerance;
    }

    // Every pixel's weight round a beacon falling at offset (within a pixel) from a pixel's centre, summed.
    float summedWeights(const Vec2& offset, float lensPixels) {
        const int reach = static_cast<int>(std::ceil(beaconExtentPixels(lensPixels))) + 1;
        float sum = 0.0f;

        for (int y = -reach; y <= reach; ++y) {
            for (int x = -reach; x <= reach; ++x) {
                sum += beaconSplatWeight(Vec2{static_cast<float>(x) - offset.x, static_cast<float>(y) - offset.y}, lensPixels);
            }
        }

        return sum;
    }

    /*
    * The lens's radius in pixels is its angular radius over a pixel's angular size; a beacon far smaller than a pixel
    * splats as a star does (the same tent), a near one as its lens's disc.
    */
    void testTheLensInPixels() {
        assert(near(beaconLensPixels(0.15f, 100.0f, 1e-6f), 0.15f / 100.0f / 1e-3f, 1e-4f));
        assert(std::isfinite(beaconLensPixels(0.15f, 0.0f, 1e-6f)) && std::isfinite(beaconSplatRadiance(1.0f, 0.0f, 1e-6f)));

        for (const Vec2 offset : {Vec2{0.0f, 0.0f}, Vec2{0.3f, -0.7f}, Vec2{0.5f, 0.5f}}) {
            assert(near(beaconSplatWeight(offset, 0.01f), starSplatWeight(offset), 1e-6f));
        }

        assert(beaconExtentPixels(0.01f) >= 1.0f && beaconExtentPixels(5.0f) >= 5.5f);
        assert(beaconSplatWeight(Vec2{1.01f, 0.0f}, 0.2f) == 0.0f && beaconSplatWeight(Vec2{6.6f, 0.0f}, 5.0f) == 0.0f);
    }

    /*
    * The weights share all of the beacon's light wherever it falls inside a pixel: exactly for a point (the tent), to a few
    * per cent for a disc (its edge is a pixel-wide ramp, not an exact area), and in between while one turns into the other.
    */
    void testTheWeightsKeepTheLight() {
        const std::vector<Vec2> offsets{{0.0f, 0.0f}, {0.25f, 0.1f}, {0.5f, 0.5f}, {-0.4f, 0.33f}};

        for (const Vec2& offset : offsets) {
            assert(near(summedWeights(offset, 0.0f), 1.0f, 1e-5f));
            assert(near(summedWeights(offset, 0.9f), 1.0f, 1e-5f));

            for (const float lensPixels : {1.3f, 1.7f, 2.5f, 4.0f, 9.0f, 20.0f}) {
                assert(near(summedWeights(offset, lensPixels), 1.0f, 0.04f));
            }
        }
    }

    /*
    * Energy kept with distance: the light reaching the eye, E = I / d^2, is what the pixels add up to (radiance times
    * weight times a pixel's solid angle), from metres to kilometres; and up close the lens shows its own luminance,
    * I / (pi r^2), whatever the distance.
    */
    void testTheLightAtAnyDistance() {
        const float intensity = 120.0f;
        const float lens = 0.15f;
        const float pixel = 1e-6f; // steradians: about a milliradian square

        for (const float distance : {20.0f, 150.0f, 1000.0f, 8000.0f}) {
            const float radiance = beaconSplatRadiance(intensity, distance, pixel);
            const float lensPixels = beaconLensPixels(lens, distance, pixel);
            const float reaching = radiance * summedWeights(Vec2{0.2f, 0.4f}, lensPixels) * pixel;
            assert(near(reaching, intensity / (distance * distance), 0.04f * intensity / (distance * distance)));
        }

        const float distance = 15.0f;
        const float lensPixels = beaconLensPixels(lens, distance, pixel);
        assert(lensPixels > 5.0f);
        const float centre = beaconSplatRadiance(intensity, distance, pixel) * beaconSplatWeight(Vec2{0.0f, 0.0f}, lensPixels);
        const float luminance = intensity / (std::numbers::pi_v<float> * lens * lens);
        assert(near(centre, luminance, 0.01f * luminance));
    }

    // The records: each beacon measured from the camera, its lens in w, its light in rgb.
    void testTheRecords() {
        const std::vector<PlacedBeacon> beacons{{Vec3{10.0f, 20.0f, 30.0f}, Vec3{5.0f, 0.5f, 0.1f}, 0.2f}};
        const std::vector<GpuBeacon> packed = packBeacons(beacons, Vec3{1.0f, 2.0f, 3.0f});
        assert(packed.size() == 1);
        assert(packed[0].positionAndLens[0] == 9.0f && packed[0].positionAndLens[1] == 18.0f && packed[0].positionAndLens[2] == 27.0f);
        assert(packed[0].positionAndLens[3] == 0.2f && packed[0].light[0] == 5.0f && packed[0].light[2] == 0.1f);
    }
}

void testBeaconSplat() {
    testTheLensInPixels();
    testTheWeightsKeepTheLight();
    testTheLightAtAnyDistance();
    testTheRecords();
}
