#include "clipper.h"

#include <cassert>
#include <cmath>

namespace{
    void assertNear(float actual, float expected) {
        assert(std::fabs(actual - expected) < 0.0001f);
    }
}

void testClipper(){
    // A line completely inside remains unchanged.
    {
        Vec4 start{-0.5f, 0.0f, 0.0f, 1.0f};
        Vec4 end{0.5f, 0.0f, 0.0f, 1.0f};

        assert(clipLine(start, end));
        assertNear(start.x, -0.5f);
        assertNear(end.x, 0.5f);
    }

    // A line crossing the left plane is shortened.
    {
        Vec4 start{-2.0f, 0.0f, 0.0f, 1.0f};
        Vec4 end{0.0f, 0.0f, 0.0f, 1.0f};

        assert(clipLine(start, end));
        assertNear(start.x, -1.0f);
        assertNear(end.x, 0.0f);
    }

    // A line crossing the near plane is shortened.
    {
        Vec4 start{0.0f, 0.0f, -2.0f, 1.0f};
        Vec4 end{0.0f, 0.0f, 0.0f, 1.0f};

        assert(clipLine(start, end));
        assertNear(start.z, -1.0f);
    }

    // A line completely outside remains invisible.
    {
        Vec4 start{-3.0f, 0.0f, 0.0f, 1.0f};
        Vec4 end{-2.0f, 0.0f, 0.0f, 1.0f};

        assert(!clipLine(start, end));
    }

    {
        Vec4 first{-0.5f, -0.5f, 0.0f, 1.0f};
        Vec4 second{0.5f, -0.5f, 0.0f, 1.0f};
        Vec4 third{0.0f, 0.5f, 0.0f, 1.0f};

        const std::vector<Vec4> result =
            clipTriangle(first, second, third);

        assert(result.size() == 3);
    }

    {
        Vec4 first{-2.0f, -0.5f, 0.0f, 1.0f};
        Vec4 second{0.5f, -0.5f, 0.0f, 1.0f};
        Vec4 third{0.0f, 0.5f, 0.0f, 1.0f};

        const std::vector<Vec4> result =
            clipTriangle(first, second, third);

        assert(!result.empty());
        assert(result.size() == 4);
    }

    {
        Vec4 first{-3.0f, 0.0f, 0.0f, 1.0f};
        Vec4 second{-2.0f, 0.0f, 0.0f, 1.0f};
        Vec4 third{-2.0f, 0.5f, 0.0f, 1.0f};

        const std::vector<Vec4> result =
            clipTriangle(first, second, third);

        assert(result.empty());
    }
}