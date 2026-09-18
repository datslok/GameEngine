#include "pixelbuffer.h"
#include <cassert>

void testPixelBuffer() {
    PixelBuffer buffer{4, 3};

    // Check dimensions.
    assert(buffer.getWidth() == 4);
    assert(buffer.getHeight() == 3);

    // Set a pixel and read it back.
    buffer.setPixel(2, 1, Pixel{255, 128, 64});
    Pixel colour = buffer.getPixel(2, 1);

    assert(colour.r == 255);
    assert(colour.g == 128);
    assert(colour.b == 64);

    // Clear to blue and check every pixel.
    buffer.clear(Pixel{0, 0, 255});

    for (std::size_t y = 0; y < buffer.getHeight(); ++y) {
        for (std::size_t x = 0; x < buffer.getWidth(); ++x) {
            Pixel pixel = buffer.getPixel(x, y);

            assert(pixel.r == 0);
            assert(pixel.g == 0);
            assert(pixel.b == 255);
        }
    }
}
