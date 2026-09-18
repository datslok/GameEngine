#include "vec3.h"
#include <cassert>
#include <cmath>

void testVec3() {
    // 1. Constructors
    Vec3 zero;
    assert(zero.x == 0.0f);
    assert(zero.y == 0.0f);
    assert(zero.z == 0.0f);

    Vec3 value{1.0f, -2.0f, 3.0f};
    assert(value.x == 1.0f);
    assert(value.y == -2.0f);
    assert(value.z == 3.0f);

    // 2. Basic Arithmetic Overloads
    Vec3 a{1.0f, 2.0f, 3.0f};
    Vec3 b{4.0f, 5.0f, 6.0f};

    Vec3 sum = a + b;
    assert(sum.x == 5.0f);
    assert(sum.y == 7.0f);
    assert(sum.z == 9.0f);

    Vec3 difference = b - a;
    assert(difference.x == 3.0f);
    assert(difference.y == 3.0f);
    assert(difference.z == 3.0f);

    Vec3 scaledVector = a * 2.0f;
    assert(scaledVector.x == 2.0f);
    assert(scaledVector.y == 4.0f);
    assert(scaledVector.z == 6.0f);

    Vec3 dividedVector = b / 2.0f;
    assert(dividedVector.x == 2.0f);
    assert(dividedVector.y == 2.5f);
    assert(dividedVector.z == 3.0f);

    // 3. Dot Product
    Vec3 dot1{1.0f, 3.0f, -5.0f};
    Vec3 dot2{4.0f, -2.0f, -1.0f};
    // (1*4) + (3*-2) + (-5*-1) = 4 - 6 + 5 = 3
    assert(dot1.dot(dot2) == 3.0f);

    // 4. Magnitude and Length
    Vec3 lengthTest{3.0f, 4.0f, 0.0f};
    assert(lengthTest.lengthSquared() == 25.0f);
    assert(std::abs(lengthTest.length() - 5.0f) < 0.00001f);

    // 5. Normalization
    Vec3 unitVector = lengthTest.normalized();
    assert(std::abs(unitVector.x - 0.6f) < 0.00001f);
    assert(std::abs(unitVector.y - 0.8f) < 0.00001f);
    assert(unitVector.z == 0.0f);

    // Safe zero-length vector edge case
    Vec3 zeroVector;
    Vec3 normalizedZero = zeroVector.normalized();
    assert(normalizedZero.x == 0.0f);
    assert(normalizedZero.y == 0.0f);
    assert(normalizedZero.z == 0.0f);

    // 6. Cross Product
    // Standard Basis Test: i x j = k
    Vec3 i{1.0f, 0.0f, 0.0f};
    Vec3 j{0.0f, 1.0f, 0.0f};
    Vec3 k = i.cross(j);
    assert(k.x == 0.0f);
    assert(k.y == 0.0f);
    assert(k.z == 1.0f);

    // Arbitrary vectors
    Vec3 v1{2.0f, 3.0f, 4.0f};
    Vec3 v2{5.0f, 6.0f, 7.0f};
    Vec3 crossResult = v1.cross(v2);
    // Expected components: (-3, 6, -3)
    assert(std::abs(crossResult.x - (-3.0f)) < 0.00001f);
    assert(std::abs(crossResult.y - 6.0f) < 0.00001f);
    assert(std::abs(crossResult.z - (-3.0f)) < 0.00001f);
}
