#pragma once

#include <string>

/*
* One fixed view for the golden image check (engine/golden.h): a scene the renderer must keep drawing the same. The
* game sets it up (Game::prepareGoldenView: camera pose, what is switched on); the engine simulates it to its time and
* captures it.
*/
struct GoldenView {
    std::string name;               // also the file name: lower-case letters, digits and dashes
    double simulationSeconds = 0.0; // simulated from the start before the capture, so moving things are placed
    bool hdr = false;               // tone mapped for an HDR screen (fixed display numbers) instead of SDR
};
