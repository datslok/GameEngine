#include "math/transform.h"
#include "math/quat.h"
#include "math/vec4.h"

#include <cassert>
#include <cmath>
#include <numbers>

namespace {
    void assertNear(float actual, float expected) {
        assert(std::fabs(actual - expected) < 0.0001f);
    }

    void assertVector(
        const Vec4& actual,
        const Vec4& expected
    ) {
        assertNear(actual.x, expected.x);
        assertNear(actual.y, expected.y);
        assertNear(actual.z, expected.z);
        assertNear(actual.w, expected.w);
    }
}

void testTransform() {
    // Default transform leaves a point unchanged.
    {
        Transform transform;
        const Vec4 point{2.0f, -3.0f, 4.0f, 1.0f};

        assertVector(transform.getMatrix() * point, point);
    }

    // Translation moves points.
    {
        Transform transform;
        transform.position = Vec3{10.0f, -2.0f, 5.0f};

        assertVector(
            transform.getMatrix() * Vec4{1.0f, 2.0f, 3.0f, 1.0f},
            Vec4{11.0f, 0.0f, 8.0f, 1.0f}
        );
    }

    // Translation does not affect directions (w = 0).
    {
        Transform transform;
        transform.position = Vec3{10.0f, -2.0f, 5.0f};

        const Vec4 direction{1.0f, 2.0f, 3.0f, 0.0f};

        assertVector(
            transform.getMatrix() * direction,
            direction
        );
    }

    // Scale first, then rotate Z, then translate.
    {
        Transform transform;
        transform.scale = Vec3{2.0f, 3.0f, 4.0f};
        transform.rotation = Quat::fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, std::numbers::pi_v<float> / 2.0f);
        transform.position = Vec3{10.0f, 20.0f, 30.0f};

        // (1, 2, 3) -> (2, 6, 12) -> (-6, 2, 12)
        // -> (4, 22, 42).
        assertVector(
            transform.getMatrix() * Vec4{1.0f, 2.0f, 3.0f, 1.0f},
            Vec4{4.0f, 22.0f, 42.0f, 1.0f}
        );
    }

    // Rotations built from Euler angles apply X, then Y, then Z.
    {
        Transform transform;
        const float quarterTurn = std::numbers::pi_v<float> / 2.0f;

        transform.rotation = Quat::fromEulerAngles(Vec3{quarterTurn, quarterTurn, quarterTurn});

        // (1, 2, 3) -> (1, -3, 2) -> (2, -3, -1)
        // -> (3, 2, -1).
        assertVector(
            transform.getMatrix() * Vec4{1.0f, 2.0f, 3.0f, 1.0f},
            Vec4{3.0f, 2.0f, -1.0f, 1.0f}
        );
    }

    // The matrix is translation times rotation times scale, whatever the turn, so scale happens along the model's own axes.
    {
        Transform transform;
        transform.position = Vec3{1.0f, -2.0f, 3.0f};
        transform.rotation = Quat::fromAxisAngle(Vec3{1.0f, 2.0f, -2.0f}.normalized(), 0.8f);
        transform.scale = Vec3{2.0f, 3.0f, 4.0f};

        const Mat4 expected = Mat4::translation(1.0f, -2.0f, 3.0f) * transform.rotation.toMatrix() * Mat4::scaling(2.0f, 3.0f, 4.0f);
        const Mat4 actual = transform.getMatrix();
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                assertNear(actual.values[row][column], expected.values[row][column]);
            }
        }
    }
}