#include "pixelbuffer.h"
#include <iostream>

PixelBuffer::PixelBuffer(std::size_t width, std::size_t height): width(width) ,height(height) ,pixels(width * height, Pixel{0, 0, 0})
{
}

void PixelBuffer::print() const {
    std::cout << "Width: " << width
              << ", height: " << height
              << ", pixels: " << pixels.size() << '\n';

    if(pixels.size() <= 100){
        for (const Pixel& pixel : pixels) {
            std::cout << '('
                    << static_cast<int>(pixel.r) << ", "
                    << static_cast<int>(pixel.g) << ", "
                    << static_cast<int>(pixel.b) << ")\n";
        }
    }
}