#pragma once

#include "core/pixel.h"
#include "scene/asset_handles.h"

/*
* How the renderer colours a part: a base colour multiplied by a texture.
* No texture handle means plain colour (the renderer uses a white texture).
* Files describe materials with MaterialSource (scene/model.h); the AssetManager turns those into this.
*/
struct Material {
    Pixel colour{255, 255, 255};
    TextureHandle texture;
};
