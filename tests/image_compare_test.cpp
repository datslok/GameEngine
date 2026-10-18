#include "core/image_compare.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
    RawImage greyImage(std::uint32_t width, std::uint32_t height, std::uint8_t level) {
        RawImage image;
        image.width = width;
        image.height = height;
        image.format = PixelFormat::Rgba8;
        image.bytes.assign(static_cast<std::size_t>(width) * height * 4, level);
        return image;
    }

    RawImage halfImage(std::uint32_t width, std::uint32_t height, std::uint16_t value) {
        RawImage image;
        image.width = width;
        image.height = height;
        image.format = PixelFormat::RgbaHalf;
        image.bytes.resize(static_cast<std::size_t>(width) * height * 8);

        for (std::size_t offset = 0; offset < image.bytes.size(); offset += 2) {
            std::memcpy(image.bytes.data() + offset, &value, 2);
        }

        return image;
    }

    void setHalf(RawImage& image, std::size_t pixel, std::size_t channel, std::uint16_t value) {
        std::memcpy(image.bytes.data() + pixel * 8 + channel * 2, &value, 2);
    }

    // The heatmap's pixel: black where the images agree, yellow where they differ within tolerance, red beyond it.
    bool heatmapIs(const ImageComparison& result, std::size_t pixel, std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
        const std::uint8_t* bytes = result.heatmap.bytes.data() + pixel * 4;
        return bytes[0] == red && bytes[1] == green && bytes[2] == blue && bytes[3] == 255;
    }

    void testIdenticalImagesPass() {
        const ImageComparison result = compareImages(greyImage(4, 3, 100), greyImage(4, 3, 100), ImageTolerance{});

        assert(result.passes);
        assert(result.largestDifference == 0);
        assert(result.differingPixels == 0);
        assert(result.pixelsOverTolerance == 0);
        assert(result.heatmap.width == 4 && result.heatmap.height == 3);
        assert(heatmapIs(result, 0, 0, 0, 0));
    }

    // One step in one channel of one pixel is rounding, within the default tolerance, but counts as differing.
    void testOneStepIsTolerated() {
        RawImage candidate = greyImage(100, 100, 100);
        candidate.bytes[4 * 57 + 2] = 101;

        const ImageComparison result = compareImages(greyImage(100, 100, 100), candidate, ImageTolerance{});

        assert(result.passes);
        assert(result.largestDifference == 1);
        assert(result.differingPixels == 1);
        assert(result.pixelsOverTolerance == 0);
        assert(heatmapIs(result, 57, 255, 255, 0));
    }

    // Two steps is a real difference: the check fails and the heatmap marks the pixel red.
    void testLargerDifferenceFails() {
        RawImage candidate = greyImage(10, 10, 100);
        candidate.bytes[4 * 3] = 98;

        const ImageComparison result = compareImages(greyImage(10, 10, 100), candidate, ImageTolerance{});

        assert(!result.passes);
        assert(result.largestDifference == 2);
        assert(result.pixelsOverTolerance == 1);
        assert(heatmapIs(result, 3, 255, 0, 0));
        assert(heatmapIs(result, 4, 0, 0, 0));
    }

    // Small differences are allowed only in a small share of the image: a whole image off by one is a real change.
    void testManySmallDifferencesFail() {
        const ImageComparison result = compareImages(greyImage(10, 10, 100), greyImage(10, 10, 101), ImageTolerance{});

        assert(!result.passes);
        assert(result.differingPixels == 100);
        assert(result.pixelsOverTolerance == 0);
    }

    void testDifferentSizesFail() {
        const ImageComparison result = compareImages(greyImage(10, 10, 100), greyImage(10, 9, 100), ImageTolerance{});

        assert(!result.passes);
        assert(!result.sameShape);
    }

    // Float images count steps between neighbouring half floats, so the same tolerance suits dark and bright pixels.
    void testHalfImagesCountSteps() {
        RawImage candidate = halfImage(20, 20, 0x3C00);
        setHalf(candidate, 5, 1, 0x3C01);
        setHalf(candidate, 6, 0, 0x3C03);

        const ImageComparison result = compareImages(halfImage(20, 20, 0x3C00), candidate, ImageTolerance{});

        assert(!result.passes);
        assert(result.largestDifference == 3);
        assert(result.differingPixels == 2);
        assert(result.pixelsOverTolerance == 1);
        assert(heatmapIs(result, 5, 255, 255, 0));
        assert(heatmapIs(result, 6, 255, 0, 0));
    }

    void testDifferentFormatsFail() {
        assert(!compareImages(greyImage(2, 2, 0), halfImage(2, 2, 0), ImageTolerance{}).sameShape);
    }

    /*
    * Float images are stored losslessly in ordinary 8-bit PNGs: each pixel's eight bytes become two RGBA pixels side by
    * side, so the PNG is twice as wide and unpacks to exactly the same bits.
    */
    void testHalfImagesPackIntoEightBitImages() {
        RawImage image = halfImage(3, 2, 0x3C00);
        setHalf(image, 4, 3, 0xABCD);

        const RawImage packed = packHalfImage(image);
        assert(packed.format == PixelFormat::Rgba8);
        assert(packed.width == 6 && packed.height == 2);
        assert(packed.bytes == image.bytes);

        const RawImage unpacked = unpackHalfImage(packed);
        assert(unpacked.format == PixelFormat::RgbaHalf);
        assert(unpacked.width == 3 && unpacked.height == 2);
        assert(unpacked.bytes == image.bytes);
    }
}

void testImageCompare() {
    testIdenticalImagesPass();
    testOneStepIsTolerated();
    testLargerDifferenceFails();
    testManySmallDifferencesFail();
    testDifferentSizesFail();
    testHalfImagesCountSteps();
    testDifferentFormatsFail();
    testHalfImagesPackIntoEightBitImages();
}
