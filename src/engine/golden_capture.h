#pragma once

#include "engine/game.h"

#include <functional>
#include <memory>
#include <string>

/*
* Running the golden image check (engine/golden.h) for a game: every view it lists is captured in a fresh game and a
* fresh engine (default settings, settings.cfg ignored), then either saved as the reference or compared with it.
* References live in tests/golden/<GPU>/<game>/ and are committed; a check writes its own captures, and a heatmap for
* each image that differs, to build/golden/<GPU>/<game>/. Float images are stored as packed PNGs (core/raw_image.h).
*/
enum class GoldenMode {
    Save, // make these captures the references
    Check // compare captures with the references
};

inline constexpr const char* goldenReferenceRoot = "tests/golden";
inline constexpr const char* goldenCheckRoot = "build/golden";

// Returns the number of images that failed (missing references count as failures), 0 when all passed or were saved.
// gameFolder names the game's folder (a golden view name: lower-case letters, digits and dashes).
int runGoldenImages(const std::function<std::unique_ptr<Game>()>& makeGame, const std::string& gameFolder, GoldenMode mode);
