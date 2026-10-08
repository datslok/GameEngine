#pragma once

#include "core/pixel.h"
#include "scene/asset_handles.h"

/*
* How the renderer colours a part: a base colour multiplied by a texture, plus a specular highlight.
* No texture handle means plain colour (the renderer uses a white texture).
* Files describe materials with MaterialSource (scene/model.h); the AssetManager turns those into this.
*/
struct Material {
    Pixel colour{255, 255, 255};
    TextureHandle texture;

    // Most surfaces are slightly shiny. Set the strength to 0 for a matte surface.
    float specularStrength = 0.25f;
    float shininess = 32.0f; // Higher means a smaller, sharper highlight.
};
