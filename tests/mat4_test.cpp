#include "math/mat4.h"
#include <cassert>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {
    bool isIdentity(const Mat4& matrix, float tolerance) {
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (std::abs(matrix.values[row][column] - (row == column ? 1.0f : 0.0f)) > tolerance) {
                    return false;
                }
            }
        }

        return true;
    }

    /*
    * The general inverse: a matrix times its inverse is the identity, for a model matrix and for a perspective
    * projection (whose last row is not 0 0 0 1, which affineInverse cannot take); a matrix that squashes space flat has none.
    */
    void testInverse() {
        const Mat4 model = Mat4::translation(3.0f, -2.0f, 7.0f) * Mat4::rotationY(0.7f) * Mat4::scaling(2.0f, 0.5f, 1.5f);
        const std::optional<Mat4> modelInverse = inverse(model);
        assert(modelInverse && isIdentity(model * *modelInverse, 1e-5f) && isIdentity(*modelInverse * model, 1e-5f));

        const Mat4 projection = Mat4::perspective(1.2f, 16.0f / 9.0f, 0.1f, 500.0f) * Mat4::rotationX(-0.2f);
        const std::optional<Mat4> projectionInverse = inverse(projection);
        assert(projectionInverse && isIdentity(projection * *projectionInverse, 1e-4f));

        assert(!inverse(Mat4::scaling(1.0f, 0.0f, 1.0f)));
        assert(Mat4::identity() == Mat4::identity() && !(Mat4::identity() == Mat4::translation(0.0f, 1.0f, 0.0f)));
    }
}

void testMat4(){
    testInverse();

    Mat4 zeroMatrix;
    Mat4 identityMatrix = Mat4::identity();

    for (int row = 0; row < 4; ++row){
        for (int column = 0; column < 4; ++column) {
            assert(zeroMatrix.values[row][column] == 0.0f);

            float expected = 0.0f;

            if (row == column) {
                expected = 1.0f;
            }

            assert(identityMatrix.values[row][column] == expected);
        }
    }
    {
        // Identity leaves all components unchanged.
        Vec4 input{2.0f, -3.0f, 4.0f, 1.0f};
        Vec4 result = Mat4::identity() * input;

        assert(result.x == 2.0f);
        assert(result.y == -3.0f);
        assert(result.z == 4.0f);
        assert(result.w == 1.0f);
    }

    {
        // An asymmetric matrix catches row/column mix-ups.
        Mat4 matrix;
        float entry = 1.0f;

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                matrix.values[row][column] = entry;
                entry += 1.0f;
            }
        }

        Vec4 input{1.0f, 2.0f, 3.0f, 4.0f};
        Vec4 result = matrix * input;

        assert(result.x == 30.0f);
        assert(result.y == 70.0f);
        assert(result.z == 110.0f);
        assert(result.w == 150.0f);
    }

    {
        Mat4 translation = Mat4::translation(10.0f, -2.0f, 5.0f);
        Vec4 point{1.0f, 2.0f, 3.0f, 1.0f};

        Vec4 result = translation * point;

        assert(result.x == 11.0f);
        assert(result.y == 0.0f);
        assert(result.z == 8.0f);
        assert(result.w == 1.0f);
    }

    {
        Mat4 translation = Mat4::translation(10.0f, -2.0f, 5.0f);
        Vec4 direction{1.0f, 2.0f, 3.0f, 0.0f};

        Vec4 result = translation * direction;

        assert(result.x == 1.0f);
        assert(result.y == 2.0f);
        assert(result.z == 3.0f);
        assert(result.w == 0.0f);
    }

    {
        Mat4 scale = Mat4::scaling(2.0f, 3.0f, 4.0f);
        Vec4 point{1.0f, -2.0f, 3.0f, 1.0f};

        Vec4 result = scale * point;

        assert(result.x == 2.0f);
        assert(result.y == -6.0f);
        assert(result.z == 12.0f);
        assert(result.w == 1.0f);
    }

    {
        Mat4 scale = Mat4::scaling(-2.0f, 0.0f, 3.0f);
        Vec4 direction{1.0f, 2.0f, 3.0f, 0.0f};

        Vec4 result = scale * direction;

        assert(result.x == -2.0f);
        assert(result.y == 0.0f);
        assert(result.z == 9.0f);
        assert(result.w == 0.0f);
    }

    {
        const float quarterTurn = std::numbers::pi_v<float> / 2.0f;
        Mat4 rotation = Mat4::rotationX(quarterTurn);

        Vec4 point{2.0f, 3.0f, 4.0f, 1.0f};
        Vec4 result = rotation * point;

        // A positive 90-degree rotation maps (y, z) to (-z, y).
        assert(result.x == 2.0f);
        assert(std::abs(result.y - (-4.0f)) < 0.00001f);
        assert(std::abs(result.z - 3.0f) < 0.00001f);
        assert(result.w == 1.0f);
    }

    {
        const float quarterTurn = std::numbers::pi_v<float> / 2.0f;
        Mat4 rotation = Mat4::rotationY(quarterTurn);

        Vec4 point{2.0f, 3.0f, 4.0f, 1.0f};
        Vec4 result = rotation * point;

        // A positive 90-degree rotation maps (x, z) to (z, -x).
        assert(std::abs(result.x - 4.0f) < 0.00001f);
        assert(result.y == 3.0f);
        assert(std::abs(result.z - (-2.0f)) < 0.00001f);
        assert(result.w == 1.0f);
    }

    {
        const float quarterTurn = std::numbers::pi_v<float> / 2.0f;
        Mat4 rotation = Mat4::rotationZ(quarterTurn);

        Vec4 point{2.0f, 3.0f, 4.0f, 1.0f};
        Vec4 result = rotation * point;

        // A positive 90-degree rotation maps (x, y) to (-y, x).
        assert(std::abs(result.x - (-3.0f)) < 0.00001f);
        assert(std::abs(result.y - 2.0f) < 0.00001f);
        assert(result.z == 4.0f);
        assert(result.w == 1.0f);
    }

    {
        Mat4 translation = Mat4::translation(10.0f, -2.0f, 5.0f);
        Mat4 scale = Mat4::scaling(2.0f, 3.0f, 4.0f);
        Vec4 point{1.0f, 2.0f, 3.0f, 1.0f};

        Mat4 combined = translation * scale;
        Vec4 result = combined * point;

        // Scale: (2, 6, 12), then translate: (12, 4, 17).
        assert(result.x == 12.0f);
        assert(result.y == 4.0f);
        assert(result.z == 17.0f);
        assert(result.w == 1.0f);
    }

    {
        Mat4 translation = Mat4::translation(10.0f, -2.0f, 5.0f);
        Mat4 scale = Mat4::scaling(2.0f, 3.0f, 4.0f);
        Vec4 point{1.0f, 2.0f, 3.0f, 1.0f};

        Mat4 combined = scale * translation;
        Vec4 result = combined * point;

        // Translate: (11, 0, 8), then scale: (22, 0, 32).
        assert(result.x == 22.0f);
        assert(result.y == 0.0f);
        assert(result.z == 32.0f);
        assert(result.w == 1.0f);
    }

    {
        const float fov = std::numbers::pi_v<float> / 2.0f;
        Mat4 projection = Mat4::perspective(fov, 1.0f, 1.0f, 10.0f);

        Vec4 nearby = projection * Vec4{1.0f, 0.0f, -2.0f, 1.0f};
        Vec4 distant = projection * Vec4{1.0f, 0.0f, -4.0f, 1.0f};

        assert(std::abs(nearby.x / nearby.w - 0.5f) < 0.00001f);
        assert(std::abs(distant.x / distant.w - 0.25f) < 0.00001f);

        Vec4 nearPoint = projection * Vec4{0.0f, 0.0f, -1.0f, 1.0f};
        Vec4 farPoint = projection * Vec4{0.0f, 0.0f, -10.0f, 1.0f};

        assert(std::abs(nearPoint.z / nearPoint.w + 1.0f) < 0.00001f);
        assert(std::abs(farPoint.z / farPoint.w - 1.0f) < 0.00001f);
    }

    {
        // Our original camera should produce the identity matrix.
        Mat4 view = Mat4::lookAt(
            Vec3{0.0f, 0.0f, 0.0f},
            Vec3{0.0f, 0.0f, -1.0f},
            Vec3{0.0f, 1.0f, 0.0f}
        );

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                float expected = 0.0f;

                if (row == column) {
                    expected = 1.0f;
                }

                assert(std::abs(view.values[row][column] - expected)
                    < 0.00001f);
            }
        }
    }

    {
        // A moved camera looking along positive X.
        Mat4 view = Mat4::lookAt(
            Vec3{2.0f, 3.0f, 4.0f},
            Vec3{3.0f, 3.0f, 4.0f},
            Vec3{0.0f, 1.0f, 0.0f}
        );

        // The camera's own position becomes the origin.
        Vec4 cameraPosition = view * Vec4{2.0f, 3.0f, 4.0f, 1.0f};

        assert(std::abs(cameraPosition.x) < 0.00001f);
        assert(std::abs(cameraPosition.y) < 0.00001f);
        assert(std::abs(cameraPosition.z) < 0.00001f);
        assert(cameraPosition.w == 1.0f);

        // The target becomes a point one unit straight ahead.
        Vec4 targetPosition = view * Vec4{3.0f, 3.0f, 4.0f, 1.0f};

        assert(std::abs(targetPosition.x) < 0.00001f);
        assert(std::abs(targetPosition.y) < 0.00001f);
        assert(std::abs(targetPosition.z + 1.0f) < 0.00001f);

        // Positive world Z is camera-right for this orientation.
        Vec4 rightDirection = view * Vec4{0.0f, 0.0f, 1.0f, 0.0f};

        assert(std::abs(rightDirection.x - 1.0f) < 0.00001f);
        assert(std::abs(rightDirection.y) < 0.00001f);
        assert(std::abs(rightDirection.z) < 0.00001f);
        assert(rightDirection.w == 0.0f);
    }

    {
        bool rejected = false;

        try {
            Mat4::lookAt(
                Vec3{0.0f, 0.0f, 0.0f},
                Vec3{0.0f, 0.0f, 0.0f},
                Vec3{0.0f, 1.0f, 0.0f}
            );
        }
        catch (const std::invalid_argument&) {
            rejected = true;
        }

        assert(rejected);
    }

    {
        bool rejected = false;

        try {
            Mat4::lookAt(
                Vec3{0.0f, 0.0f, 0.0f},
                Vec3{0.0f, 0.0f, -1.0f},
                Vec3{0.0f, 0.0f, 1.0f}
            );
        }
        catch (const std::invalid_argument&) {
            rejected = true;
        }

        assert(rejected);
    }
}