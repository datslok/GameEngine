#include "depthbuffer.h"

#include <algorithm>
#include <stdexcept>

/*
* Initialise the depth buffer with a default depth value of 1.0f for all pixels.
*/
DepthBuffer::DepthBuffer(
    std::size_t width,
    std::size_t height
):
    width(width),
    height(height),
    depths(width * height, 1.0f){
}

/*
* Reset all depth values so the buffer can be reused for a new frame.
*/
void DepthBuffer::clear(float depth){
    std::fill(depths.begin(), depths.end(), depth);
}

/*
* Update the pixel's depth only when the new value is closer to the camera, ensuring nearer geometry is rendered in front of the farther geometry.
*/
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

/*
* Return the stored depth value so the renderer can determine which geometry is closest to the camera at this pixel, and therefore should be visible in the final image.
*/
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

/*
* Return the buffer width so its dimensions can be used by rendering operations.
*/
std::size_t DepthBuffer::getWidth() const{
    return width;
}

/*
* Return the buffer height so its dimensions can be used by rendering operations. 
*/
std::size_t DepthBuffer::getHeight() const{
    return height;
}