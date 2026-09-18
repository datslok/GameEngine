#include "depthbuffer.h"

#include <algorithm>
#include <stdexcept>

DepthBuffer::DepthBuffer(
    std::size_t width,
    std::size_t height
):
    width(width),
    height(height),
    depths(width * height, 1.0f){
}

void DepthBuffer::clear(float depth){
    std::fill(depths.begin(), depths.end(), depth);
}

bool DepthBuffer::testAndSet(
    std::size_t x,
    std::size_t y,
    float depth
){
    if (x >= width || y >= height){
        throw std::out_of_range(
            "Depth coordinates outside buffer"
        );
    }

    const std::size_t index = y * width + x;

    if (depth < depths[index]){
        depths[index] = depth;
        return true;
    }

    return false;
}

float DepthBuffer::getDepth(
    std::size_t x,
    std::size_t y
) const {
    if (x >= width || y >= height){
        throw std::out_of_range(
            "Depth coordinates outside buffer"
        );
    }

    return depths[y * width + x];
}

std::size_t DepthBuffer::getWidth() const{
    return width;
}

std::size_t DepthBuffer::getHeight() const{
    return height;
}