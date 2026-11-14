#pragma once

#include "assets/asset_manager.h"
#include "core/pixel.h"
#include "scene/material.h"

#include <array>
#include <string>

// Where tools/fetch_forest_assets.py puts the forest's downloaded assets, one folder per asset (git-ignored).
inline constexpr const char* forestAssetFolder = "assets/forest";

/*
* One of the ground's layers (games/forest/forest_terrain.h's channels): the downloaded texture set's folder, its
* texture's size on the ground (it repeats across the land), and the plain colour and roughness it stands in with when
* the folder is missing (a fresh clone before the fetch, and the tests).
*/
struct ForestGroundLayer {
    const char* folder;
    float tileMetres;
    Pixel standIn;
    float standInRoughness;
};

/*
* The pillars' and islands' rock, in the strata strip's channel order (games/forest/valley_strata.h): red-brown strata
* (Poly Haven's cliff_side, a 1.8 m scan, shown 4 m), grey layers (ambientCG's Rock061), big orange blocks (Poly Haven's
* marble_cliff_03, 5.7 m from the air, shown 12 m, so a band of it reads from far off), and last the moss taking over
* where the rock faces up (Poly Haven's aerial_rocks_02, 50 m of mossy rock from the air). Projected in metres.
*/
inline constexpr std::array<ForestGroundLayer, 4> forestRockLayers{{
    {"cliff_side", 4.0f, Pixel{150, 96, 70}, 0.9f},
    {"Rock061", 3.0f, Pixel{138, 130, 120}, 0.9f},
    {"marble_cliff_03", 12.0f, Pixel{192, 142, 92}, 0.85f},
    {"aerial_rocks_02", 50.0f, Pixel{92, 104, 68}, 0.9f}
}};

// The rock layer that takes over facing up.
inline constexpr int forestMossLayer = 3;

// Grass, leaf litter, mud and mossy rock, in the weights texture's channel order.
inline constexpr std::array<ForestGroundLayer, 4> forestGroundLayers{{
    {"Grass004", 2.0f, Pixel{70, 100, 45}, 0.9f},
    {"leaves_forest_ground", 2.5f, Pixel{95, 72, 48}, 0.85f},
    {"brown_mud_leaves_01", 2.5f, Pixel{78, 60, 44}, 0.7f},
    {"mossy_rock", 3.0f, Pixel{100, 104, 90}, 0.8f}
}};

/*
* A texture set's files by role, as the fetch tool names them: colour, normal (OpenGL's convention, green up the picture)
* and height, and either Poly Haven's packed occlusion, roughness and metal ("arm", red, green and blue, as glTF packs
* them) or ambientCG's separate roughness and occlusion. Empty for a role with no file.
*/
struct ForestTextureFiles {
    std::string colour;
    std::string normal;
    std::string height;
    std::string packedSurface;
    std::string roughness;
    std::string occlusion;
};

// The files present in a folder, each role's name tried as .png and then .jpg.
ForestTextureFiles findForestTextureFiles(const std::string& folder);

// A ground layer's material from the texture set in assetFolder/layer.folder, or its plain stand-in when there is none.
Material forestLayerMaterial(AssetManager& assets, const std::string& assetFolder, const ForestGroundLayer& layer);

// The finest paint's texels: 2048 over the walked-on ring's 1.28 km.
inline constexpr float forestFinestPaintTexel = 0.625f;

/*
* The ground's blend depth (Material::groundBlendDepth) for paint whose texels are texelMetres across: the setting's own
* on the finest, deeper by the square root of how many times coarser they are, at most 1 (a plain cross-fade). A shallow
* blend turns the weights' bilinear slopes into hard edges, which on texels tens of metres wide show their corners as
* squares; a deeper one lets them fade.
*/
float forestBlendDepth(float blendDepth, float texelMetres);
