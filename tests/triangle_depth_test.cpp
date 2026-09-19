#include "rasterizer.h"
#include "depthbuffer.h"
#include "pixelbuffer.h"

#include <cassert>
#include <cmath>
#include <cstddef>

void testTriangleDepth(){
    PixelBuffer farFirst{8, 8};
    PixelBuffer nearFirst{8, 8};

    DepthBuffer farFirstDepth{8, 8};
    DepthBuffer nearFirstDepth{8, 8};

    const Pixel background{0, 0, 0};
    const Pixel nearColour{255, 0, 0};
    const Pixel farColour{0, 0, 255};

    farFirst.clear(background);
    nearFirst.clear(background);

    // Draw the far triangle, then the near triangle.
    fillTriangleDepth(
        farFirst, farFirstDepth,
        1, 1, 0.8f,
        6, 1, 0.8f,
        1, 6, 0.8f,
        farColour
    );

    fillTriangleDepth(
        farFirst, farFirstDepth,
        2, 2, 0.2f,
        6, 2, 0.2f,
        2, 6, 0.2f,
        nearColour
    );

    // Draw the same triangles in the opposite order.
    fillTriangleDepth(
        nearFirst, nearFirstDepth,
        2, 2, 0.2f,
        6, 2, 0.2f,
        2, 6, 0.2f,
        nearColour
    );

    fillTriangleDepth(
        nearFirst, nearFirstDepth,
        1, 1, 0.8f,
        6, 1, 0.8f,
        1, 6, 0.8f,
        farColour
    );

    // Both drawing orders must produce identical buffers.
    for (std::size_t y = 0; y < 8; ++y){
        for (std::size_t x = 0; x < 8; ++x){
            const Pixel first = farFirst.getPixel(x, y);
            const Pixel second = nearFirst.getPixel(x, y);

            assert(first.r == second.r);
            assert(first.g == second.g);
            assert(first.b == second.b);

            assert(
                std::fabs(
                    farFirstDepth.getDepth(x, y) -
                    nearFirstDepth.getDepth(x, y)
                ) < 0.0001f
            );
        }
    }

    // In the overlap, the nearer red triangle must win.
    const Pixel overlap = farFirst.getPixel(3, 3);

    assert(overlap.r == nearColour.r);
    assert(overlap.g == nearColour.g);
    assert(overlap.b == nearColour.b);
    assert(
        std::fabs(farFirstDepth.getDepth(3, 3) - 0.2f)
        < 0.0001f
    );

    // A pixel covered only by the far triangle stays blue.
    const Pixel farOnly = farFirst.getPixel(2, 1);

    assert(farOnly.r == farColour.r);
    assert(farOnly.g == farColour.g);
    assert(farOnly.b == farColour.b);
    assert(
        std::fabs(farFirstDepth.getDepth(2, 1) - 0.8f)
        < 0.0001f
    );

    // A pixel outside both triangles stays black.
    const Pixel outside = farFirst.getPixel(7, 7);

    assert(outside.r == background.r);
    assert(outside.g == background.g);
    assert(outside.b == background.b);
    assert(farFirstDepth.getDepth(7, 7) == 1.0f);
}