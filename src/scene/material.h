#pragma once

#include "core/pixel.h"
#include "math/vec3.h"
#include "scene/asset_handles.h"

/*
* How the renderer colours a part: a base colour multiplied by a texture, how metallic and rough it is, and any light it emits.
* No texture handle means plain colour (the renderer uses a white texture).
* Files describe materials with MaterialSource (scene/model.h); the AssetManager turns those into this.
*/
struct Material {
    Pixel colour{255, 255, 255}; // sRGB, like image files and colour pickers; decoded to linear light for shading.
    TextureHandle texture;

    // Physically based shading (render/gpu/brdf.h): metallic is 0 for non-metals (plastic, stone, wood, skin), whose
    // colour comes from light scattered inside them, and 1 for metals, whose colour is in their reflection. Roughness
    // runs from a polished mirror (0) to fully matte (1): how widely the surface spreads a reflected highlight.
    float metallic = 0.0f;
    float roughness = 0.5f;

    // Optional per-texel metallic and roughness, packed as glTF does: roughness in green, metallic in blue, each
    // multiplying the numbers above. A linear (not sRGB) texture; no handle means the numbers alone.
    TextureHandle metallicRoughnessTexture;

    // Optional normal map: a linear texture of small tilts of the surface normal, in the frame of the mesh's tangents
    // (render/gpu/normal_mapping.h), so flat geometry catches the light like bricks or weave. normalScale multiplies the
    // tilts (glTF's normalTexture.scale): 0 flattens them, more than 1 exaggerates them. No handle means no tilts.
    TextureHandle normalTexture;
    float normalScale = 1.0f;

    // Light the surface gives off itself (0..1 per channel, or more for very bright), added whatever lights the scene has.
    // Black means it does not glow. A glowing object with a black colour shows only its emitted light, so it never changes with lighting.
    Vec3 emissive{0.0f, 0.0f, 0.0f};
};
