#pragma once

#include "core/pixel.h"
#include "math/vec3.h"
#include "scene/asset_handles.h"

#include <array>
#include <limits>
#include <vector>

/*
* How a surface's alpha is used (glTF's alphaMode): ignored (Opaque), a cut-out (Mask) whose texels under the cutoff are
* holes, for leaves, fences and grates (render/gpu/alpha_mask.h), or coverage (Blend): that share of the pixel is the
* surface and the rest what lies behind, for decals and fading things, drawn after the opaque scene (render/gpu/transparency.h).
*/
enum class AlphaMode {
    Opaque,
    Mask,
    Blend
};

// What paints a layered material's layers (Material::groundPaint): a weights texture over the mesh's texture
// coordinates, or strata, a strip of weights by world height.
enum class GroundPaint {
    TextureCoordinates,
    Strata
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

    // Thin glass (KHR_materials_transmission): the share of the light from behind that passes through the non-metal part
    // (0..1), tinted by the colour, less what its surface reflects; its texture's red channel (linear) multiplies it.
    // A transmitting material is drawn after the opaque scene, blended over it (render/gpu/transparency.h).
    float transmission = 0.0f;
    TextureHandle transmissionTexture;

    // Thick glass (KHR_materials_volume): how far light travels through it, in the model's own units (scaled with the
    // object), its texture's green channel (linear) multiplying it; 0 is thin glass, which bends nothing. Light crossing
    // it keeps attenuationColour after attenuationDistance (Beer's law; infinite absorbs nothing).
    float thickness = 0.0f;
    TextureHandle thicknessTexture;
    Vec3 attenuationColour{1.0f, 1.0f, 1.0f};
    float attenuationDistance = std::numeric_limits<float>::infinity();

    // The index of refraction (KHR_materials_ior): how strongly light bends entering it, 1.5 for glass, 1.33 for water.
    float ior = 1.5f;

    // A clear coat (KHR_materials_clearcoat): a thin layer of clear varnish over the surface, as on car paint, lacquered
    // wood or a varnished floor (render/gpu/brdf.h, addClearCoat). clearCoat is how much of the surface it covers (0..1,
    // 0 none), its texture's red channel (linear) multiplying it; clearCoatRoughness its own roughness (0..1, usually
    // near 0, a smooth gloss), its texture's green channel multiplying it. The coat lies smooth on the geometry, whatever
    // normalTexture does to the layer under it, unless it has a normal map of its own (clearCoatNormalTexture, scaled by
    // clearCoatNormalScale). Ignored on transparent materials, whose glass is a varnish-like surface already.
    float clearCoat = 0.0f;
    TextureHandle clearCoatTexture;
    float clearCoatRoughness = 0.0f;
    TextureHandle clearCoatRoughnessTexture;
    TextureHandle clearCoatNormalTexture;
    float clearCoatNormalScale = 1.0f;

    // A height map for parallax occlusion mapping (render/gpu/parallax.h): a linear texture whose red channel is how high
    // each texel stands (1 the top, at the polygon, 0 the deepest), making flat polygons seem to stand out (bricks, cobbles).
    // heightScale is how deep the deepest lies, in texture units (a share of the texture's width: 0.03 for a mortar groove
    // 3% of it deep). No handle means a flat surface. Ignored on cut-outs and transparent materials.
    TextureHandle heightTexture;
    float heightScale = 0.03f;

    // A blended ground (render/gpu/ground_layers.h): up to four layer materials (each an ordinary material: grass, moss,
    // mud, rock), how much of each covers each place given by groundWeights (a linear RGBA texture over the mesh's texture
    // coordinates, one channel a layer), each layer's textures repeating groundRepeats times across those coordinates, and
    // where layers meet the one whose height map stands higher showing, within groundBlendDepth (0..1 of the height maps'
    // range; deeper is a softer cross-fade). A layer's normal map is read at the ground's normalScale; a layer
    // without one lies flat, and one without a height map (or with heightScale 0) stands halfway. No layers: an
    // ordinary material. Layers may not be layered themselves.
    std::vector<Material> groundLayers;
    std::array<float, 4> groundRepeats{1.0f, 1.0f, 1.0f, 1.0f};
    TextureHandle groundWeights;
    float groundBlendDepth = 0.2f;
    // Projected layers (render/gpu/ground_layers.h, projectionWeights): each layer's textures read along the object's own
    // three axes in metres instead of the mesh's texture coordinates (triplanar mapping), for shapes made by code and
    // stretched to size; groundRepeats then count repeats a metre.
    bool groundProjected = false;
    // What paints the layers: groundWeights over the mesh's texture coordinates, or strata: groundWeights as a strip of
    // weights by world height, its first column at strataBottom and its last at strataTop (render/gpu/ground_layers.h,
    // strataPlace), so every object with this material shows the same bands at the same heights.
    GroundPaint groundPaint = GroundPaint::TextureCoordinates;
    float strataBottom = 0.0f;
    float strataTop = 1.0f;
    // With strata, a layer (0..3; -1 none) taking over where the surface faces up: none of it while the normal's upward
    // part is under groundTopStart, all of it from groundTopFull (strataPaint).
    int groundTopLayer = -1;
    float groundTopStart = 0.6f;
    float groundTopFull = 0.8f;
    // With strata, wavy beds (warpedStrataHeight): strataWarp, a linear texture whose red channel (0.5 none) moves the
    // height the strata are read at by up to strataWarpMetres either way, repeating every strataWarpTile metres over the
    // world's x and z. No texture or 0 metres: level beds.
    TextureHandle strataWarp;
    float strataWarpMetres = 0.0f;
    float strataWarpTile = 1000.0f;
    // With projected layers, water streaks down the sides (streakPlace, streakShade): groundStreaks, a linear texture
    // whose red channel is clean rock (1) to the darkest stain (0), repeating every streakWidth metres across and every
    // streakLength metres down, darkening the albedo by up to streakStrength. No texture or 0 strength: none.
    TextureHandle groundStreaks;
    float streakStrength = 0.0f;
    float streakWidth = 8.0f;
    float streakLength = 200.0f;
    // Each layer read by hex tiling (render/gpu/ground_layers.h, hexTiling), so a texture repeated over a big object
    // shows no repeat; about one to two times the reads.
    bool groundHexTiling = false;
    // With projected layers, slow tone variation (variationShade): groundVariation, a linear texture whose red channel
    // (0.5 none) lightens or darkens the albedo by up to variationStrength, repeating every variationTile metres along
    // each projection.
    TextureHandle groundVariation;
    float variationStrength = 0.0f;
    float variationTile = 160.0f;
};

inline bool isCutOut(const Material& material) {
    return material.alphaMode == AlphaMode::Mask;
}

// Seen through: blended or transmitting, so drawn after the opaque scene, sorted far to near.
inline bool isTransparent(const Material& material) {
    return material.alphaMode == AlphaMode::Blend || material.transmission > 0.0f;
}
