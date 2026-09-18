#include "vec4.h"
#include <cassert>

void testVec4(){
    Vec4 zeroVector;

    assert(zeroVector.x == 0.0f);
    assert(zeroVector.y == 0.0f);
    assert(zeroVector.z == 0.0f);
    assert(zeroVector.w == 0.0f);

    Vec4 point{2.0f, -3.0f, 4.0f, 1.0f};

    assert(point.x == 2.0f);
    assert(point.y == -3.0f);
    assert(point.z == 4.0f);
    assert(point.w == 1.0f);

    Vec4 direction{2.0f, -3.0f, 4.0f, 0.0f};

    assert(direction.x == 2.0f);
    assert(direction.y == -3.0f);
    assert(direction.z == 4.0f);
    assert(direction.w == 0.0f);
}