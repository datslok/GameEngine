#include "math/ray.h"

#include <cassert>
#include <cmath>
#include <numbers>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }
}

void testRay() {
    const Camera camera{
        Vec3{0.0f, 10.0f, 10.0f},
        Vec3{0.0f, 0.0f, 0.0f},
        Vec3{0.0f, 1.0f, 0.0f},
        std::numbers::pi_v<float> / 2.0f,
        1.0f,
        0.1f,
        100.0f
    };

    // The screen centre points at the camera's target.
    const Ray centre = makeCameraRay(
        camera, 0.5f, 0.5f, 1.0f
    );

    const auto centreHit = intersectGround(centre);

    assert(centreHit.has_value());
    assert(nearlyEqual(centreHit->x, 0.0f));
    assert(nearlyEqual(centreHit->y, 0.0f));
    assert(nearlyEqual(centreHit->z, 0.0f));

    // Screen-right must hit to the right of the centre.
    const auto rightHit = intersectGround(
        makeCameraRay(camera, 0.75f, 0.5f, 1.0f)
    );

    assert(rightHit.has_value());
    assert(nearlyEqual(rightHit->x, std::sqrt(50.0f)));
    assert(nearlyEqual(rightHit->z, 0.0f));

    // A wider viewport spreads the same normalized X farther out.
    const auto wideHit = intersectGround(
        makeCameraRay(camera, 0.75f, 0.5f, 2.0f)
    );

    assert(wideHit.has_value());
    assert(nearlyEqual(wideHit->x, 2.0f * rightHit->x));

    // Clicking below centre hits nearer to the camera.
    const auto lowerHit = intersectGround(
        makeCameraRay(camera, 0.5f, 0.75f, 1.0f)
    );

    assert(lowerHit.has_value());
    assert(nearlyEqual(lowerHit->z, 20.0f / 3.0f));

    // Parallel rays have no ground intersection.
    assert(!intersectGround(Ray{
        Vec3{0.0f, 1.0f, 0.0f},
        Vec3{1.0f, 0.0f, 0.0f}
    }).has_value());

    // Rays pointing away from the ground must also miss.
    assert(!intersectGround(Ray{
        Vec3{0.0f, 1.0f, 0.0f},
        Vec3{0.0f, 1.0f, 0.0f}
    }).has_value());

    // Ground height is configurable.
    const auto raisedHit = intersectGround(
        Ray{
            Vec3{2.0f, 5.0f, 3.0f},
            Vec3{0.0f, -1.0f, 0.0f}
        },
        2.0f
    );

    assert(raisedHit.has_value());
    assert(nearlyEqual(raisedHit->x, 2.0f));
    assert(nearlyEqual(raisedHit->y, 2.0f));
    assert(nearlyEqual(raisedHit->z, 3.0f));
}