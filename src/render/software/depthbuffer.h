#pragma once

#include <cstddef>
#include <vector>

/*
* Stores the per-pixel depth values so the renderer can determine which geometry is closest to the camera
* and prevent hidden surfaces from being drawn.
*/
class DepthBuffer {
    public:
        DepthBuffer(std::size_t width, std::size_t height);

        void clear(float depth = 1.0f);

        bool testAndSet(
            std::size_t x,
            std::size_t y,
            float depth
        );

        float getDepth(
            std::size_t x,
            std::size_t y
        ) const;

        std::size_t getWidth() const;
        std::size_t getHeight() const;

    private:
        std::size_t width;
        std::size_t height;
        std::vector<float> depths;
};