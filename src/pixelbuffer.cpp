#include "pixelbuffer.h"

PixelBuffer::PixelBuffer(std::size_t width, std::size_t height): width(width) ,height(height) ,pixels(width * height, Pixel{0, 0, 0})
{
}