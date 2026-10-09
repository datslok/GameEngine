#include "math/mat4.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected) {
        return nearlyEqual(actual.x, expected.x) && nearlyEqual(actual.y, expected.y) && nearlyEqual(actual.z, expected.z);
    }

    // Normals are directions, so w = 0: translation must not move them.
    Vec3 transformNormal(const Mat4& matrix, const Vec3& normal) {
        const Vec4 result = matrix * Vec4{normal.x, normal.y, normal.z, 0.0f};
        return Vec3{result.x, result.y, result.z}.normalized();
    }

    Vec3 transformDirection(const Mat4& matrix, const Vec3& direction) {
        const Vec4 result = matrix * Vec4{direction.x, direction.y, direction.z, 0.0f};
        return Vec3{result.x, result.y, result.z};
    }

    void assertUpperMatricesEqual(const Mat4& actual, const Mat4& expected) {
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                assert(nearlyEqual(actual.values[row][column], expected.values[row][column]));
            }
        }
    }

    void testIdentityAndTranslationLeaveNormalsAlone() {
        assertUpperMatricesEqual(normalMatrix(Mat4::identity()), Mat4::identity());
        assertUpperMatricesEqual(normalMatrix(Mat4::translation(5.0f, 6.0f, 7.0f)), Mat4::identity());
    }

    // For a pure rotation the inverse transpose is the rotation itself.
    void testRotationIsUnchanged() {
        const Mat4 rotation = Mat4::rotationY(0.7f) * Mat4::rotationX(-0.3f);
        assertUpperMatricesEqual(normalMatrix(rotation), rotation);
    }

    // Stretching a 45 degree slope sideways makes it shallower; its normal must stay perpendicular to it.
    void testNonUniformScaleKeepsNormalsPerpendicular() {
        const Mat4 stretch = Mat4::scaling(2.0f, 1.0f, 1.0f);
        const Vec3 tangent{1.0f, 1.0f, 0.0f};
        const Vec3 normal = Vec3{1.0f, -1.0f, 0.0f}.normalized();

        const Vec3 transformedTangent = transformDirection(stretch, tangent);
        const Vec3 transformedNormal = transformNormal(normalMatrix(stretch), normal);

        assert(nearlyEqual(transformedTangent.dot(transformedNormal), 0.0f));
        assert(nearlyEqual(transformedNormal, Vec3{1.0f, -2.0f, 0.0f}.normalized()));
    }

    // A mirrored object must still have outward normals, or it would be lit from behind.
    void testMirrorKeepsNormalsOutward() {
        const Mat4 mirror = Mat4::scaling(-1.0f, 1.0f, 1.0f);

        // The +x face of a cube becomes the -x face, so its outward normal must point along -x.
        assert(nearlyEqual(transformNormal(normalMatrix(mirror), Vec3{1.0f, 0.0f, 0.0f}), Vec3{-1.0f, 0.0f, 0.0f}));
    }

    // An object squashed flat has no well-defined normals: zero, like the shader's old behaviour.
    void testZeroScaleGivesZero() {
        assertUpperMatricesEqual(normalMatrix(Mat4::scaling(0.0f, 1.0f, 1.0f)), Mat4{});
    }
}

void testNormalMatrix() {
    testIdentityAndTranslationLeaveNormalsAlone();
    testRotationIsUnchanged();
    testNonUniformScaleKeepsNormalsPerpendicular();
    testMirrorKeepsNormalsOutward();
    testZeroScaleGivesZero();
}
