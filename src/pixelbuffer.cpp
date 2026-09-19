#include "pixelbuffer.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>

/*
* Creates a pixel buffer with the specified width and height.
* All pixels are initalised to black.
*/
PixelBuffer::PixelBuffer(std::size_t width, std::size_t height): 
    width(width),
    height(height),
    pixels(width * height, Pixel{0, 0, 0})
{
}

/*
* Prints the pixel buffer's width, height, and pixel values for debugging purposes.
* Pixel values are only printed if the buffer contains 100 pixels or less to avoid excessive output.
*/
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

/*
* Sets the pixel at the specified coordinates.
* Throws an exception if the coordinates are outside the buffer, in order to prevent accessing memory outside the buffer.
*/

void PixelBuffer::setPixel(std::size_t x, std::size_t y, Pixel colour){
    if (x >= width || y >= height) {
        throw std::out_of_range("Pixel coordinates outside buffer");
    }
    /*
    * Converts the 2D coordinates into a 1D index for the pixel vector.
    */
    pixels[y*width + x] = colour;
}

Pixel PixelBuffer::getPixel(std::size_t x, std::size_t y) const{
    if (x >= width || y >= height) {
        throw std::out_of_range("Pixel coordinates outside buffer");
    }
    return pixels[y * width + x];
}

/*
* Fill the buffer with one colour to efficiently reset all pixels.
*/
void PixelBuffer::clear(Pixel colour){
    std::fill(pixels.begin(), pixels.end(), colour);
}

std::size_t PixelBuffer::getWidth() const{
    return width;
}

std::size_t PixelBuffer::getHeight() const{
    return height;
}

/*
* Expose the pixel values so the renderer may access the buffer directly without being able to change the values.
*/
const Pixel* PixelBuffer::data() const {
    return pixels.data();
}
