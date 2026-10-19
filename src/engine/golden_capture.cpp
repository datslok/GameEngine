#include "engine/golden_capture.h"

#include "assets/image_loader.h"
#include "core/image_compare.h"
#include "engine/application.h"
#include "engine/golden.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // A captured image as it is stored: 8-bit images as they are, float images packed two pixels per pixel.
    RawImage storable(const RawImage& image) {
        return image.format == PixelFormat::RgbaHalf ? packHalfImage(image) : image;
    }

    RawImage loadStored(const std::filesystem::path& path, PixelFormat format) {
        const RawImage loaded = loadRawImagePng(path.string());
        return format == PixelFormat::RgbaHalf ? unpackHalfImage(loaded) : loaded;
    }

    struct NamedImage {
        std::string name;
        const RawImage* image;
    };

    // One image against its reference; prints a line and returns whether it passed.
    bool checkImage(const NamedImage& captured, const std::filesystem::path& referenceFolder, const std::filesystem::path& outputFolder) {
        const std::filesystem::path referencePath = referenceFolder / (captured.name + ".png");
        saveRawImagePng(storable(*captured.image), (outputFolder / (captured.name + ".png")).string());

        if (!std::filesystem::exists(referencePath)) {
            std::cout << "  " << captured.name << ": MISSING reference " << referencePath.generic_string() << '\n';
            return false;
        }

        const RawImage reference = loadStored(referencePath, captured.image->format);
        const ImageComparison result = compareImages(reference, *captured.image, ImageTolerance{});

        if (!result.sameShape) {
            std::cout << "  " << captured.name << ": FAIL, the reference has a different size or format\n";
            return false;
        }

        std::cout << "  " << captured.name << ": " << (result.passes ? "pass" : "FAIL") << " (" << result.differingPixels
                  << " pixels differ, " << result.pixelsOverTolerance << " beyond tolerance, largest " << result.largestDifference
                  << (captured.image->format == PixelFormat::RgbaHalf ? " half-float steps)" : " steps of 255)");

        if (result.differingPixels > 0) {
            const std::filesystem::path heatmapPath = outputFolder / (captured.name + "-diff.png");
            saveRawImagePng(result.heatmap, heatmapPath.string());
            std::cout << ", heatmap " << heatmapPath.generic_string();
        }

        std::cout << '\n';
        return result.passes;
    }
}

/*
* A fresh game and engine per view, so no view sees what an earlier one switched on, and each starts with a cold shadow
* cache and no light history, exactly like the game's first frame.
*/
int runGoldenImages(const std::function<std::unique_ptr<Game>()>& makeGame, const std::string& gameFolder, GoldenMode mode) {
    if (!isGoldenViewName(gameFolder)) {
        throw std::invalid_argument("Golden game folders must be lower-case letters, digits and dashes: " + gameFolder);
    }

    const std::vector<GoldenView> views = makeGame()->goldenViews();
    int failures = 0;

    for (const GoldenView& view : views) {
        if (!isGoldenViewName(view.name)) {
            throw std::invalid_argument("Golden view names must be lower-case letters, digits and dashes: " + view.name);
        }

        std::unique_ptr<Game> game = makeGame();
        Application application{"Golden images", 640, 360, *game, EngineSettings{}, SavedSettings::Ignore};
        const CapturedFrame captured = application.captureGoldenView(view);

        const std::string gpuFolder = goldenFolderName(application.getRendererName());
        const std::filesystem::path referenceFolder = std::filesystem::path{goldenReferenceRoot} / gpuFolder / gameFolder;
        const std::filesystem::path outputFolder = std::filesystem::path{goldenCheckRoot} / gpuFolder / gameFolder;
        const NamedImage images[] = {{view.name + "-scene", &captured.scene}, {view.name + "-final", &captured.final}};

        if (mode == GoldenMode::Save) {
            std::filesystem::create_directories(referenceFolder);

            for (const NamedImage& image : images) {
                const std::filesystem::path path = referenceFolder / (image.name + ".png");
                saveRawImagePng(storable(*image.image), path.string());
                std::cout << "Saved " << path.generic_string() << '\n';
            }

            continue;
        }

        std::filesystem::create_directories(outputFolder);
        std::cout << gameFolder << '/' << view.name << (view.hdr ? " (HDR)" : "") << ":\n";

        for (const NamedImage& image : images) {
            failures += checkImage(image, referenceFolder, outputFolder) ? 0 : 1;
        }
    }

    if (mode == GoldenMode::Check) {
        std::cout << gameFolder << ": " << (failures == 0 ? "all golden images match.\n" : std::to_string(failures) + " golden images differ.\n");
    }

    return failures;
}
