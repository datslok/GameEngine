#pragma once

#include "ecs/world.h"
#include "engine/game.h"
#include "engine/golden_view.h"
#include "render/gpu/display_output.h"

#include <cstdint>
#include <string>

/*
* Golden images: fixed views rendered once and kept as references (tests/golden/<GPU name>/), so a renderer change, the
* Vulkan port above all, can be checked against them pixel by pixel. The same GPU, driver and shaders give the same
* image, so the references are only valid on the GPU that made them.
*/

// Every golden image is this size, whatever the window, so references and captures always line up.
inline constexpr std::uint32_t goldenImageWidth = 1280;
inline constexpr std::uint32_t goldenImageHeight = 720;

/*
* Frames drawn before a view with light probes is captured, each one tick (1/120 s) long, so the probes have gathered
* their light: 8 s is four of the irradiance's default time constants (2 s; 2% of the start left) and eight of the
* distances'. Light bounced several times needs a time constant per bounce, so the dimmest corners are the last to
* settle; the capture is the same every run either way, which is what a reference needs.
*/
inline constexpr int goldenProbeWarmUpFrames = 960;

// Lower-case letters, digits and dashes, not empty: safe as a file name everywhere.
bool isGoldenViewName(const std::string& name);

/*
* How a golden view reaches its image: SDR, or HDR as on a fixed screen (SDR white at 200 nits, a peak four times
* that), so HDR captures do not depend on the monitor or Windows' settings.
*/
DisplayOutput goldenDisplayOutput(bool hdr);

/*
* Bring a game fresh from onInit to a golden view: let it set the view up, run the ticks up to the view's time through
* runFixedUpdate (as the engine loop does), then one frame update with no input. The engine and the tests share it.
*/
void advanceToGoldenView(Game& game, World& world, const GoldenView& view, int ticksPerSecond);

// A GPU's name as a folder name: letters and digits kept, everything else a dash, runs of dashes joined.
std::string goldenFolderName(const std::string& gpuName);
