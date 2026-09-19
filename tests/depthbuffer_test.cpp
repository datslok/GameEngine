#include "depthbuffer.h"

#include <cassert>
#include <cmath>

void testDepthBuffer(){
    DepthBuffer depth{2, 2};

    assert(std::fabs(depth.getDepth(0, 0) - 1.0f) < 0.0001f);

    assert(depth.testAndSet(0, 0, 0.7f));
    assert(!depth.testAndSet(0, 0, 0.8f));
    assert(depth.testAndSet(0, 0, 0.3f));

    assert(
        std::fabs(depth.getDepth(0, 0) - 0.3f)
        < 0.0001f
    );

    depth.clear();

    assert(std::fabs(depth.getDepth(0, 0) - 1.0f) < 0.0001f);
}