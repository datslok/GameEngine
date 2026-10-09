#pragma once

#include "core/pixel.h"
#include "math/vec3.h"
#include "scene/asset_handles.h"

/*
* How the renderer colours a part: a base colour multiplied by a texture, plus a specular highlight, plus any light it emits.
* No texture handle means plain colour (the renderer uses a white texture).
* Files describe materials with MaterialSource (scene/model.h); the AssetManager turns those into this.
*/
struct Material {
    Pixel colour{255, 255, 255}; // sRGB, like image files and colour pickers; decoded to linear light for shading.
    TextureHandle texture;

    // Most surfaces are slightly shiny. Set the strength to 0 for a matte surface.
    float specularStrength = 0.25f;
    float shininess = 32.0f; // Higher means a smaller, sharper highlight.

    // Light the surface gives off itself (0..1 per channel, or more for very bright), added whatever lights the scene has.
    // Black means it does not glow. A glowing object with a black colour shows only its emitted light, so it never changes with lighting.
    Vec3 emissive{0.0f, 0.0f, 0.0f};
};
