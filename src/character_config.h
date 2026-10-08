#pragma once

#include <numbers>
#include <string>

struct CharacterConfig {
    // Used by scene setup to load the asset.
    std::string modelPath;
    float modelSize = 2.0f; // Longest dimension, in world units.
    float movementSpeed = 6.0f;
    float turnSpeed = 6.0f * std::numbers::pi_v<float>;

    // Model's forward direction measured from +Z towards +X.
    // +Z-facing models use 0; the +X-facing duck uses pi / 2.
    float modelForwardYaw = 0.0f;
};
