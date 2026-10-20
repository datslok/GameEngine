#include "scene/atmosphere.h"

void visit(Atmosphere& atmosphere, SettingsVisitor& visitor) {
    visitor.field("Rayleigh scale height", atmosphere.rayleighScaleHeight, {.min = 0.1f, .max = 100.0f, .sliderMax = 20.0f, .unit = "km"});
    visitor.field("Aerosol scattering", atmosphere.mieScattering, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.05f, .unit = "/km"});
    visitor.field("Aerosol absorption", atmosphere.mieAbsorption, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.05f, .unit = "/km"});
    visitor.field("Aerosol scale height", atmosphere.mieScaleHeight, {.min = 0.1f, .max = 20.0f, .sliderMax = 5.0f, .unit = "km"});
    visitor.field("Aerosol forward scattering", atmosphere.mieAnisotropy, {.min = -0.99f, .max = 0.99f});
    visitor.field("Ozone centre", atmosphere.ozoneCentre, {.min = 0.0f, .max = 100.0f, .unit = "km"});
    visitor.field("Ozone half width", atmosphere.ozoneHalfWidth, {.min = 0.1f, .max = 50.0f, .unit = "km"});
    visitor.field("Ground altitude", atmosphere.groundAltitude, {.min = 0.0f, .max = 90.0f, .sliderMax = 10.0f, .unit = "km"});
}
