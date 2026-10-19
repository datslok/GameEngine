#include "render/gpu/light_falloff.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 1e-5f) {
        return std::abs(actual - expected) <= tolerance;
    }

    /*
    * Far inside the range the window is nearly 1 and the falloff is the inverse square law softened by the source:
    * 1 / (d^2 + r^2). A larger source is gentler up close and matches a point source far away.
    */
    void testInverseSquareInsideTheRange() {
        const float range = 1000.0f;
        assert(nearlyEqual(distanceFalloff(2.0f, range, 1.0f), 1.0f / 5.0f));
        assert(nearlyEqual(distanceFalloff(10.0f, range, 0.0f), 1.0f / 100.0f));

        // Twice as far, about a quarter of the light once the source size no longer matters.
        const float ratio = distanceFalloff(40.0f, range, 0.5f) / distanceFalloff(20.0f, range, 0.5f);
        assert(nearlyEqual(ratio, 0.25f, 0.002f));
    }

    // At the light itself a source of radius r gives 1 / r^2 instead of an infinity.
    void testFiniteAtTheLight() {
        assert(nearlyEqual(distanceFalloff(0.0f, 10.0f, 1.0f), 1.0f));
        assert(nearlyEqual(distanceFalloff(0.0f, 10.0f, 0.5f), 4.0f));
    }

    // The window (1 - (d / range)^4)^2 reaches exactly 0 at the range and stays there, and the light only ever dims with distance.
    void testWindowEndsAtTheRange() {
        assert(distanceFalloff(10.0f, 10.0f, 1.0f) == 0.0f);
        assert(distanceFalloff(15.0f, 10.0f, 1.0f) == 0.0f);

        float previous = distanceFalloff(0.0f, 10.0f, 1.0f);

        for (float distance = 0.25f; distance <= 10.0f; distance += 0.25f) {
            const float falloff = distanceFalloff(distance, 10.0f, 1.0f);
            assert(falloff <= previous);
            previous = falloff;
        }
    }

    /*
    * The spotlight's cone is GLSL's smoothstep between the outer and inner cosines: 0 outside the outer cone, 1 inside
    * the inner one, the Hermite curve 3t^2 - 2t^3 between, so it starts and ends with zero slope (no visible ring).
    */
    void testSpotCone() {
        const float cosOuter = 0.8f;
        const float cosInner = 0.9f;

        assert(spotCone(0.5f, cosOuter, cosInner) == 0.0f);
        assert(spotCone(cosOuter, cosOuter, cosInner) == 0.0f);
        assert(spotCone(cosInner, cosOuter, cosInner) == 1.0f);
        assert(spotCone(1.0f, cosOuter, cosInner) == 1.0f);
        assert(nearlyEqual(spotCone(0.85f, cosOuter, cosInner), 0.5f));

        // A quarter of the way: t = 0.25 gives 3/16 - 2/64 = 0.15625.
        assert(nearlyEqual(spotCone(0.825f, cosOuter, cosInner), 0.15625f));
    }
}

void testLightFalloff() {
    testInverseSquareInsideTheRange();
    testFiniteAtTheLight();
    testWindowEndsAtTheRange();
    testSpotCone();
}
