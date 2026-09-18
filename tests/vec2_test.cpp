#include "vec2.h"
#include <cassert>

void testVec2() {
    Vec2 zero;

    assert(zero.x == 0.0f);
    assert(zero.y == 0.0f);

    Vec2 value{3.0f, -2.0f};

    assert(value.x == 3.0f);
    assert(value.y == -2.0f);

    Vec2 a{3.0f, 4.0f};
    Vec2 b{2.0f, 1.0f};

    assert(a.dot(b) == 10.0f);

    Vec2 c{3.0f, 4.0f};
    Vec2 d{1.0f, 2.0f};

    Vec2 sum = c + d;
    assert(sum.x == 4.0f);
    assert(sum.y == 6.0f);

    Vec2 difference = c - d;
    assert(difference.x == 2.0f);
    assert(difference.y == 2.0f);

    Vec2 vectorForScalarTest{3.0f, 4.0f};

    Vec2 scaledVector = vectorForScalarTest * 2.0f;

    assert(scaledVector.x == 6.0f);
    assert(scaledVector.y == 8.0f);

    Vec2 dividedVector = vectorForScalarTest / 2.0f;

    assert(dividedVector.x == 1.5f);
    assert(dividedVector.y == 2.0f);

    Vec2 lengthTestVector{3.0f, 4.0f};

    assert(lengthTestVector.lengthSquared() == 25.0f);
    assert(std::abs(lengthTestVector.length() - 5.0f) < 0.00001f);

    Vec2 unitVector = lengthTestVector.normalized();

    assert(std::abs(unitVector.x - 0.6f) < 0.00001f);
    assert(std::abs(unitVector.y - 0.8f) < 0.00001f);

    Vec2 zeroVector;
    Vec2 normalizedZero = zeroVector.normalized();

    assert(normalizedZero.x == 0.0f);
    assert(normalizedZero.y == 0.0f);
}