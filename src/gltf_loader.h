#pragma once

#include "model.h"

#include <string>

// Load a static model from a .gltf or .glb file.
// Throws std::runtime_error if loading fails or a required
// feature is unsupported.
Model loadGltf(const std::string& filename);