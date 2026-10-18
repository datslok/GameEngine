#include "assets/image_loader.h"

#include <cassert>
#include <cstdint>
#include <filesystem>

/*
* Golden images must survive a save and a load bit for bit, including a packed float image whose "alpha" bytes are
* really parts of numbers (0 alpha must not lose its colour bytes).
*/
void testRawImagePng() {
    RawImage image;
    image.width = 5;
    image.height = 3;
    image.format = PixelFormat::Rgba8;

    for (std::size_t index = 0; index < static_cast<std::size_t>(image.width) * image.height * 4; ++index) {
        image.bytes.push_back(static_cast<std::uint8_t>(index * 37 + 11));
    }

    image.bytes[3] = 0;

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "game_engine_raw_image_png_test.png";
    saveRawImagePng(image, path.string());
    const RawImage loaded = loadRawImagePng(path.string());
    std::filesystem::remove(path);

    assert(loaded.width == image.width);
    assert(loaded.height == image.height);
    assert(loaded.format == PixelFormat::Rgba8);
    assert(loaded.bytes == image.bytes);
}
