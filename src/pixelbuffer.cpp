#include "pixelbuffer.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>

PixelBuffer::PixelBuffer(std::size_t width, std::size_t height): 
    width(width),
    height(height),
    pixels(width * height, Pixel{0, 0, 0})
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

void PixelBuffer::setPixel(std::size_t x, std::size_t y, Pixel colour){
    if (x >= width || y >= height) {
        throw std::out_of_range("Pixel coordinates outside buffer");
    }
    pixels[y*width + x] = colour;
}

Pixel PixelBuffer::getPixel(std::size_t x, std::size_t y) const{
    if (x >= width || y >= height) {
        throw std::out_of_range("Pixel coordinates outside buffer");
    }
    return pixels[y * width + x];
}

void PixelBuffer::clear(Pixel colour){
    std::fill(pixels.begin(), pixels.end(), colour);
}

std::size_t PixelBuffer::getWidth() const{
    return width;
}

std::size_t PixelBuffer::getHeight() const{
    return height;
}
