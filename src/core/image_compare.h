#pragma once

#include "core/raw_image.h"

#include <cstddef>
#include <cstdint>

/*
* How close a captured image must be to its golden reference. Differences are counted in steps: bytes for 8-bit
* images, representable values for half floats (core/half_float.h). The same GPU, driver and shaders should give
* identical images, so the defaults are strict: one step of rounding, in at most 0.1% of the pixels.
*/
struct ImageTolerance {
    std::uint32_t maxStepDifference = 1;
    double maxDifferingShare = 0.001;
};

struct ImageComparison {
    bool sameShape = false;              // same width, height and format (nothing else is compared otherwise)
    std::uint32_t largestDifference = 0; // the most steps any channel of any pixel is off
    std::size_t differingPixels = 0;     // pixels with any difference at all
    std::size_t pixelsOverTolerance = 0; // pixels off by more than maxStepDifference
    bool passes = false;

    // Where they differ, the size of the images: black where they agree, yellow where within tolerance, red beyond it.
    RawImage heatmap;
};

/*
* Passes when the images have the same shape, no pixel is off by more than the tolerance, and at most the allowed share
* of pixels differs at all (a whole image one step brighter is a real change, not rounding).
*/
ImageComparison compareImages(const RawImage& reference, const RawImage& candidate, const ImageTolerance& tolerance);
