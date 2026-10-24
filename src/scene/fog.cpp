#include "scene/fog.h"

void visit(Fog& fog, SettingsVisitor& visitor) {
    visitor.field("Density", fog.density, {.min = 0.0f, .max = 10.0f, .sliderMax = 0.1f, .unit = "/m"});
    visitor.field("Base height", fog.baseHeight, {.min = -10000.0f, .max = 10000.0f, .sliderMin = -20.0f, .sliderMax = 50.0f, .unit = "m"});
    visitor.field("Scale height", fog.scaleHeight, {.min = 0.1f, .max = 10000.0f, .sliderMax = 100.0f, .unit = "m"});
    visitor.field("Albedo red", fog.albedo.x, {.min = 0.0f, .max = 1.0f});
    visitor.field("Albedo green", fog.albedo.y, {.min = 0.0f, .max = 1.0f});
    visitor.field("Albedo blue", fog.albedo.z, {.min = 0.0f, .max = 1.0f});
    visitor.field("Forward scattering", fog.anisotropy, {.min = -0.95f, .max = 0.95f});
    visitor.field("Farthest", fog.maxDistance, {.min = 1.0f, .max = 100000.0f, .sliderMax = 5000.0f, .unit = "m"});
}
