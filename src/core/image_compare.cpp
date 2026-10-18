#include "core/image_compare.h"

#include "core/half_float.h"

#include <algorithm>
#include <cstring>

namespace {
    // The largest step difference over a pixel's four channels.
    std::uint32_t pixelDifference(const std::uint8_t* reference, const std::uint8_t* candidate, PixelFormat format) {
        std::uint32_t largest = 0;

        for (std::size_t channel = 0; channel < 4; ++channel) {
            std::uint32_t difference = 0;

            if (format == PixelFormat::RgbaHalf) {
                std::uint16_t first = 0;
                std::uint16_t second = 0;
                std::memcpy(&first, reference + channel * 2, 2);
                std::memcpy(&second, candidate + channel * 2, 2);
                difference = halfStepDistance(first, second);
            }
            else {
                const int signedDifference = static_cast<int>(reference[channel]) - static_cast<int>(candidate[channel]);
                difference = static_cast<std::uint32_t>(signedDifference < 0 ? -signedDifference : signedDifference);
            }

            largest = std::max(largest, difference);
        }

        return largest;
    }

    void paint(RawImage& heatmap, std::size_t pixel, std::uint8_t red, std::uint8_t green) {
        std::uint8_t* bytes = heatmap.bytes.data() + pixel * 4;
        bytes[0] = red;
        bytes[1] = green;
    }
}

ImageComparison compareImages(const RawImage& reference, const RawImage& candidate, const ImageTolerance& tolerance) {
    ImageComparison result;
    result.sameShape = reference.width == candidate.width &&
                       reference.height == candidate.height &&
                       reference.format == candidate.format &&
                       reference.bytes.size() == candidate.bytes.size() &&
                       reference.bytes.size() == static_cast<std::size_t>(reference.width) * reference.height * bytesPerPixel(reference.format);

    if (!result.sameShape) {
        return result;
    }

    const std::size_t pixels = static_cast<std::size_t>(reference.width) * reference.height;
    const std::size_t stride = bytesPerPixel(reference.format);

    // Opaque black to start with; differing pixels are painted over.
    result.heatmap.width = reference.width;
    result.heatmap.height = reference.height;
    result.heatmap.format = PixelFormat::Rgba8;
    result.heatmap.bytes.assign(pixels * 4, 0);

    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        result.heatmap.bytes[pixel * 4 + 3] = 255;
        const std::uint32_t difference = pixelDifference(reference.bytes.data() + pixel * stride, candidate.bytes.data() + pixel * stride, reference.format);

        if (difference == 0) {
            continue;
        }

        result.largestDifference = std::max(result.largestDifference, difference);
        ++result.differingPixels;

        if (difference > tolerance.maxStepDifference) {
            ++result.pixelsOverTolerance;
            paint(result.heatmap, pixel, 255, 0);
        }
        else {
            paint(result.heatmap, pixel, 255, 255);
        }
    }

    const double differingShare = pixels > 0 ? static_cast<double>(result.differingPixels) / static_cast<double>(pixels) : 0.0;
    result.passes = result.pixelsOverTolerance == 0 && differingShare <= tolerance.maxDifferingShare;
    return result;
}
