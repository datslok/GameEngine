#include "rasterizer.h"

#include <cassert>
#include <cstddef>

namespace {
    void assertPixelEquals(Pixel actual, Pixel expected) {
        assert(actual.r == expected.r);
        assert(actual.g == expected.g);
        assert(actual.b == expected.b);
    }

    void assertBufferIsFilledWith(
        const PixelBuffer& buffer,
        Pixel expected
    ) {
        for (std::size_t y = 0; y < buffer.getHeight(); ++y) {
            for (std::size_t x = 0; x < buffer.getWidth(); ++x) {
                assertPixelEquals(buffer.getPixel(x, y), expected);
            }
        }
    }
}

void testFilledTriangle() {
    const Pixel background{0, 0, 0};
    const Pixel triangleColour{255, 0, 0};

    // A pixel inside should be coloured.
    {
        PixelBuffer buffer{8, 8};
        buffer.clear(background);

        fillTriangle(
            buffer,
            1, 1,
            6, 1,
            1, 6,
            triangleColour
        );

        assertPixelEquals(
            buffer.getPixel(2, 2),
            triangleColour
        );
    }

    // A pixel outside should remain unchanged.
    {
        PixelBuffer buffer{8, 8};
        buffer.clear(background);

        fillTriangle(
            buffer,
            1, 1,
            6, 1,
            1, 6,
            triangleColour
        );

        assertPixelEquals(
            buffer.getPixel(6, 6),
            background
        );
    }

    // Reversing the vertex order should still fill the triangle.
    {
        PixelBuffer buffer{8, 8};
        buffer.clear(background);

        fillTriangle(
            buffer,
            1, 1,
            1, 6,
            6, 1,
            triangleColour
        );

        assertPixelEquals(
            buffer.getPixel(2, 2),
            triangleColour
        );
    }

    // A triangle completely outside the buffer should not throw
    // or modify any pixels.
    {
        PixelBuffer buffer{8, 8};
        buffer.clear(background);

        fillTriangle(
            buffer,
            -4, -4,
            -2, -4,
            -4, -2,
            triangleColour
        );

        assertBufferIsFilledWith(buffer, background);
    }

    // Collinear vertices form a degenerate triangle and should
    // leave the buffer unchanged.
    {
        PixelBuffer buffer{8, 8};
        buffer.clear(background);

        fillTriangle(
            buffer,
            1, 1,
            3, 3,
            5, 5,
            triangleColour
        );

        assertBufferIsFilledWith(buffer, background);
    }
}