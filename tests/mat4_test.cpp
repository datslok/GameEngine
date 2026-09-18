#include "mat4.h"
#include <cassert>
#include <cmath>
#include <numbers>

void testMat4(){
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
}