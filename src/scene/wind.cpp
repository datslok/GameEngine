#include "scene/wind.h"

void visit(Wind& wind, SettingsVisitor& visitor) {
    visitor.field("Towards", wind.towardsDegrees, {.min = -360.0f, .max = 360.0f, .sliderMin = 0.0f, .sliderMax = 360.0f, .unit = "deg"});
    visitor.field("Speed", wind.speed, {.min = 0.0f, .max = 60.0f, .sliderMax = 20.0f, .unit = "m/s"});
    visitor.field("Gust strength", wind.gustStrength, {.min = 0.0f, .max = 1.0f});
    visitor.field("Gust size", wind.gustSize, {.min = 1.0f, .max = 1000.0f, .sliderMax = 200.0f, .unit = "m"});
    visitor.field("Turbulence", wind.turbulence, {.min = 0.0f, .max = 1.0f});
    visitor.field("Turbulence size", wind.turbulenceSize, {.min = 0.5f, .max = 200.0f, .sliderMax = 50.0f, .unit = "m"});
}
