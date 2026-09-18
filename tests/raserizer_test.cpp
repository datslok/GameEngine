#include "rasterizer.h"
#include <cassert>
#include <cstddef>
#include <initializer_list>

namespace {
    struct Point {
        int x;
        int y;
    };

    void checkLine(
        int x0, int y0,
        int x1, int y1,
        std::initializer_list<Point> expectedPixels
    ) {
        PixelBuffer buffer{8, 8};
        const Pixel background{0, 0, 0};
        const Pixel lineColour{255, 128, 64};

        buffer.clear(background);
        drawLine(buffer, x0, y0, x1, y1, lineColour);

        // Check every pixel, including those that should stay black.
        for (std::size_t y = 0; y < buffer.getHeight(); ++y) {
            for (std::size_t x = 0; x < buffer.getWidth(); ++x) {
                bool shouldBeColoured = false;

                for (const Point& point : expectedPixels) {
                    if (point.x == static_cast<int>(x) &&
                        point.y == static_cast<int>(y)) {
                        shouldBeColoured = true;
                        break;
                    }
                }

                Pixel expected = background;

                if (shouldBeColoured) {
                    expected = lineColour;
                }

                const Pixel actual = buffer.getPixel(x, y);

                assert(actual.r == expected.r);
                assert(actual.g == expected.g);
                assert(actual.b == expected.b);
            }
        }
    }
}

void testRasterizer() {
    // Horizontal line, including both endpoints.
    checkLine(1, 2, 5, 2, {
        {1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}
    });

    // Reversed endpoints.
    checkLine(5, 2, 1, 2, {
        {1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}
    });

    // Vertical line.
    checkLine(3, 1, 3, 5, {
        {3, 1}, {3, 2}, {3, 3}, {3, 4}, {3, 5}
    });

    // Shallow slope: fractional y positions must be rounded.
    checkLine(1, 1, 5, 3, {
        {1, 1}, {2, 2}, {3, 2}, {4, 3}, {5, 3}
    });

    // Steep slope: fractional x positions must be rounded.
    checkLine(1, 1, 3, 5, {
        {1, 1}, {2, 2}, {2, 3}, {3, 4}, {3, 5}
    });

    // Negative slope.
    checkLine(1, 5, 5, 1, {
        {1, 5}, {2, 4}, {3, 3}, {4, 2}, {5, 1}
    });

    // Identical endpoints should draw one pixel.
    checkLine(4, 4, 4, 4, {
        {4, 4}
    });

    // Line crosses both the left and right boundaries.
    checkLine(-2, 3, 10, 3, {
        {0, 3}, {1, 3}, {2, 3}, {3, 3},
        {4, 3}, {5, 3}, {6, 3}, {7, 3}
    });

    // Line crosses both the top and bottom boundaries.
    checkLine(2, -2, 2, 10, {
        {2, 0}, {2, 1}, {2, 2}, {2, 3},
        {2, 4}, {2, 5}, {2, 6}, {2, 7}
    });

    // Entirely outside: no pixels should change.
    checkLine(-3, -3, -1, -1, {});

    // Single point outside the buffer.
    checkLine(8, 8, 8, 8, {});
}