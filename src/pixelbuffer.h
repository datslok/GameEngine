#pragma once
#include <cstddef>
#include <vector>
#include "pixel.h"

class PixelBuffer {
    private:
        std::size_t width;
        std::size_t height;
        std::vector<Pixel> pixels;

    public:
        PixelBuffer(std::size_t width, std::size_t height);
        void print() const;
        void setPixel(std::size_t x, std::size_t y, Pixel colour);
        Pixel getPixel(std::size_t x, std::size_t y) const;
        void clear(Pixel colour);
        std::size_t getWidth() const;
        std::size_t getHeight() const;
};