#include "engine/golden.h"

#include "input/input.h"

#include <cctype>
#include <cmath>

namespace {
    // The fixed HDR screen: Windows' SDR brightness slider at 200 nits (scRGB 2.5) and a panel that reaches 800 nits.
    constexpr float goldenSdrWhiteLevel = 2.5f;
    constexpr float goldenHdrHeadroom = 4.0f;
}

bool isGoldenViewName(const std::string& name) {
    if (name.empty()) {
        return false;
    }

    for (const char character : name) {
        const bool allowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';

        if (!allowed) {
            return false;
        }
    }

    return true;
}

DisplayOutput goldenDisplayOutput(bool hdr) {
    DisplayCapabilities screen;
    screen.hdrSwapchainSupported = true;
    screen.hdrEnabled = hdr;
    screen.hdrHeadroom = goldenHdrHeadroom;
    screen.sdrWhiteLevel = goldenSdrWhiteLevel;
    return chooseDisplayOutput(DisplaySettings{}, screen);
}

void advanceToGoldenView(Game& game, World& world, const GoldenView& view, int ticksPerSecond) {
    game.prepareGoldenView(world, view);

    const double tickSeconds = 1.0 / static_cast<double>(ticksPerSecond);
    const long ticks = std::lround(view.simulationSeconds * static_cast<double>(ticksPerSecond));

    for (long tick = 1; tick <= ticks; ++tick) {
        runFixedUpdate(game, world, static_cast<float>(tickSeconds), static_cast<double>(tick) * tickSeconds);
    }

    game.onUpdate(world, Input{}, static_cast<float>(tickSeconds), 1.0f);
}

std::string goldenFolderName(const std::string& gpuName) {
    std::string folder;

    for (const char character : gpuName) {
        if (std::isalnum(static_cast<unsigned char>(character))) {
            folder += character;
        }
        else if (!folder.empty() && folder.back() != '-') {
            folder += '-';
        }
    }

    while (!folder.empty() && folder.back() == '-') {
        folder.pop_back();
    }

    return folder.empty() ? "unknown-gpu" : folder;
}
