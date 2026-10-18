#include "math/mat4.h"

#include <cassert>
#include <cmath>
#include <optional>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    void assertVector(const Vec3& actual, const Vec3& expected) {
        assert(nearlyEqual(actual.x, expected.x));
        assert(nearlyEqual(actual.y, expected.y));
        assert(nearlyEqual(actual.z, expected.z));
    }

    void assertIdentity(const Mat4& matrix) {
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                assert(nearlyEqual(matrix.values[row][column], row == column ? 1.0f : 0.0f));
            }
        }
    }
}

void testAffineInverse() {
    // Points pick up the translation, directions do not.
    const Mat4 shift = Mat4::translation(1.0f, 2.0f, 3.0f);
    assertVector(transformPoint(shift, Vec3{1.0f, 1.0f, 1.0f}), Vec3{2.0f, 3.0f, 4.0f});
    assertVector(transformDirection(shift, Vec3{1.0f, 1.0f, 1.0f}), Vec3{1.0f, 1.0f, 1.0f});

    // A typical model matrix: moved, turned about two axes and stretched unevenly. Its inverse undoes it from either side.
    const Mat4 model = Mat4::translation(4.0f, -2.0f, 7.0f)
        * Mat4::rotationY(0.7f) * Mat4::rotationX(-0.4f)
        * Mat4::scaling(2.0f, 0.5f, 3.0f);
    const std::optional<Mat4> inverse = affineInverse(model);
    assert(inverse.has_value());
    assertIdentity(model * *inverse);
    assertIdentity(*inverse * model);

    // So a point taken out and back returns where it started.
    const Vec3 point{0.3f, -1.2f, 5.0f};
    assertVector(transformPoint(*inverse, transformPoint(model, point)), point);

    // A matrix that squashes space flat has no inverse.
    assert(!affineInverse(Mat4::scaling(1.0f, 0.0f, 1.0f)).has_value());
}
