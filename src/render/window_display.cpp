#include "render/window_display.h"

DisplayCapabilities readWindowDisplay(SDL_Window* window) {
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);

    DisplayCapabilities capabilities;
    capabilities.hdrEnabled = SDL_GetBooleanProperty(properties, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    capabilities.hdrHeadroom = SDL_GetFloatProperty(properties, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
    capabilities.sdrWhiteLevel = SDL_GetFloatProperty(properties, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
    return capabilities;
}
