#pragma once

#include "core/pixel.h"
#include "math/vec3.h"
#include "scene/asset_handles.h"

/*
* How a surface's alpha is used (glTF's alphaMode): ignored (Opaque), or a cut-out (Mask) whose texels under the cutoff
* are holes, for leaves, fences and grates (render/gpu/alpha_mask.h). Blending (glass) is not drawn yet.
*/
enum class AlphaMode {
    Opaque,
    Mask
};

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

    // Optional baked occlusion (glTF's occlusionTexture): a linear texture whose red channel is how much of the light
    // from all around reaches each texel (0 buried, 1 open), worked out offline from the model's full detail. It dims only
    // that light, like the screen's ambient occlusion (render/gpu/ambient_occlusion.h), whichever of the two is more
    // closed. occlusionStrength (0..1) blends from no effect (0) to the texture's full effect (1).
    TextureHandle occlusionTexture;
    float occlusionStrength = 1.0f;

    // Light the surface gives off itself (0..1 per channel, or more for very bright), added whatever lights the scene has.
    // Black means it does not glow. A glowing object with a black colour shows only its emitted light, so it never changes with lighting.
    Vec3 emissive{0.0f, 0.0f, 0.0f};

    // Optional pattern for the glow (glTF's emissiveTexture): an sRGB colour texture multiplying emissive, for screens,
    // dials and painted lights. No handle means the colour above everywhere.
    TextureHandle emissiveTexture;

    // A cut-out's alpha is its colour texture's alpha times opacity (glTF's base colour factor alpha, 0..1); texels
    // under alphaCutoff are holes. Ignored for opaque materials.
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    float opacity = 1.0f;

    // Both sides drawn (glTF's doubleSided), the back shaded as the front's mirror image: a leaf or a fence seen from
    // behind. One-sided surfaces skip their backs, as a closed mesh's are never seen.
    bool doubleSided = false;
};

inline bool isCutOut(const Material& material) {
    return material.alphaMode == AlphaMode::Mask;
}
