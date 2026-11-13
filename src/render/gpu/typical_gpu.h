#pragma once

#include "render/gpu/render_settings.h"

#include <optional>

/*
* The typical GPU setup: the engine drawn as a mid-range PC would draw it, so features are judged as most players will
* see them (ROADMAP phase 16's milestone). The High quality preset (Ultra's shadows and probes take more memory than an
* 8 GB card running a real game can spare), ray tracing off, and the upscaler at Quality (a render scale of 0.67): DLSS
* where it runs (the user's choice, Nov 13 2026), else FSR, which runs on any GPU, else the engine's own TAA. Everything
* else (taste, the camera's finish, what the preset leaves alone) is kept.
*/

// The upscaler a typical GPU runs: DLSS where it can, else FSR, else TAA.
Upscaler typicalUpscaler(UpscalerSupport support);

// Set the typical GPU setup's values; all other settings are kept.
void applyTypicalGpu(RenderSettings& settings, UpscalerSupport support);

/*
* The switch over it (F5's checkbox, the typical-gpu command, --typical-gpu): on remembers the settings it found and
* applies the setup; off gives them back exactly, whatever was changed meanwhile. On while on applies the setup again
* (another upscaler may have become available) and keeps the settings it first found; off while off does nothing.
*/
class TypicalGpuView {
public:
    bool isOn() const;
    void turnOn(RenderSettings& settings, UpscalerSupport support);
    void turnOff(RenderSettings& settings);

private:
    std::optional<RenderSettings> saved;
};
