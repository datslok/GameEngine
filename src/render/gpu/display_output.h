#pragma once

#include "render/gpu/tone_mapping.h"

/*
* HDR output: on an HDR screen the final image can be brighter than white, so a floodlight's hotspot really is brighter
* on the panel than a lit wall, not just as white. The renderer then draws into a 16-bit float screen image (scRGB:
* linear light where 1 is the reference white of 80 nits), and Windows maps it to the panel.
*/

// scRGB's 1: the brightness, in nits (candela per square metre), that a value of 1 is shown at.
inline constexpr float scrgbWhiteNits = 80.0f;

// The player's choices, as HDR games offer them. HDR is used only when the screen can show it.
struct DisplaySettings {
    bool hdrOutput = true;

    // How bright white (a lit white wall, a sheet of paper) is shown, in nits. 0 uses Windows' SDR brightness slider.
    float paperWhiteNits = 0.0f;

    // The brightest the game shows anything, in nits, never past what the monitor reports. 0 uses the monitor's peak.
    float peakNits = 0.0f;
};

// What the window's screen reports (read from SDL each frame; it changes when Windows HDR is switched or the window
// moves to another monitor).
struct DisplayCapabilities {
    bool hdrSwapchainSupported = false; // the GPU can present a float (scRGB) image to this window
    bool hdrEnabled = false;            // Windows HDR is on for the window's monitor
    float hdrHeadroom = 1.0f;           // how many times brighter than SDR white the screen can go
    float sdrWhiteLevel = 1.0f;         // where SDR white sits in scRGB (Windows' SDR brightness slider: 2.5 is 200 nits)
};

// How this frame reaches the screen.
struct DisplayOutput {
    bool hdr = false;
    float peakBrightness = sdrPeakBrightness; // the tone mapper's peak, in units of SDR white
    float whiteLevel = 1.0f;                  // what SDR white is written as (scRGB scale); 1 on an SDR screen
};

/*
* HDR only when it is wanted and the screen can show it, with a usable headroom above white; otherwise the ordinary
* SDR output. Values the system reports that are not usable numbers are ignored. The player's paper white and peak
* replace the system's when set; the peak is kept within the monitor's and never below white.
*/
DisplayOutput chooseDisplayOutput(const DisplaySettings& settings, const DisplayCapabilities& capabilities);
