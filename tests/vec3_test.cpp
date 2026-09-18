#include "vec3.h"
#include <cassert>

void testVec3(){
    Vec3 zero;

    assert(zero.x == 0.0f);
    assert(zero.y == 0.0f);
    assert(zero.z == 0.0f);

    Vec3 value{1.0f, -2.0f, 3.0f};

    assert(value.x == 1.0f);
    assert(value.y == -2.0f);
    assert(value.z == 3.0f);
}