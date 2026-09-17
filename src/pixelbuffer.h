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
};