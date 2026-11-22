#include "render/gpu/material_settings.h"

void visit(MaterialSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Clear coat", settings.clearCoat);
    visitor.field("Parallax", settings.parallax);
    visitor.field("Parallax self-shadows", settings.parallaxSelfShadows);
    visitor.field("Parallax steps", settings.parallaxSteps,
                  {.min = PARALLAX_FEWEST_STEPS, .max = PARALLAX_MOST_STEPS, .sliderMin = PARALLAX_FEWEST_STEPS, .sliderMax = PARALLAX_MOST_STEPS});
    visitor.field("Light through leaves", settings.lightThroughLeaves);
    visitor.field("Leaf normal blend", settings.leafNormalBlend, {.min = 0.0f, .max = 1.0f, .sliderMin = 0.0f, .sliderMax = 1.0f});
}
