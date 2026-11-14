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
* texture's size on the ground (it repeats across the land), the plain colour and roughness it stands in with when
* the folder is missing (a fresh clone before the fetch, and the tests), and a tint multiplying its colour texture
* (sRGB, white for none), so one scan can serve as two tones of the same rock.
*/
struct ForestGroundLayer {
    const char* folder;
    float tileMetres;
    Pixel standIn;
    float standInRoughness;
    Pixel tint{255, 255, 255};
};

/*
* The pillars' and islands' rock, in the strata strip's channel order (games/forest/valley_strata.h): one sandstone
* (Poly Haven's cliff_side, a 1.8 m scan of red-brown strata, shown 4 m) in two tones, its own and a greyer, darker one,
* which the soft beds mix by their tone; the hard beds' big blocks (Poly Haven's marble_cliff_03, 5.7 m from the air,
* shown 12 m); and last the moss taking over where the rock faces up (Poly Haven's aerial_rocks_02, 50 m of mossy rock
* from the air). Projected in metres.
*/
inline constexpr std::array<ForestGroundLayer, 4> forestRockLayers{{
    {"cliff_side", 4.0f, Pixel{150, 96, 70}, 0.9f},
    {"cliff_side", 4.0f, Pixel{122, 100, 84}, 0.9f, Pixel{205, 190, 178}},
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
* them) or ambientCG's separate roughness and occlusion, and a cut-out's opacity (ambientCG's leaves). Empty for a role
* with no file.
*/
struct ForestTextureFiles {
    std::string colour;
    std::string normal;
    std::string height;
    std::string packedSurface;
    std::string roughness;
    std::string occlusion;
    std::string opacity;
};

// The files present in a folder, each role's name tried as .png and then .jpg.
ForestTextureFiles findForestTextureFiles(const std::string& folder);

// A ground layer's material from the texture set in assetFolder/layer.folder, or its plain stand-in when there is none.
Material forestLayerMaterial(AssetManager& assets, const std::string& assetFolder, const ForestGroundLayer& layer);

// The islands' roots and vines (games/forest/island_hangings.h): Poly Haven's brown bark, its tile set by the tubes'
// own texture coordinates.
inline constexpr ForestGroundLayer forestBarkLayer{"bark_brown_02", 2.0f, Pixel{88, 66, 48}, 0.9f};

// The vines' ivy leaves: ambientCG's LeafSet017, six leaves in an atlas.
inline constexpr const char* forestIvyFolder = "LeafSet017";

// The trees' bark (Poly Haven's oak bark, a tile 1.5 m round the trunk) and leaves (ambientCG's LeafSet024, nine
// beech-like leaves in a 3 x 3 atlas).
inline constexpr ForestGroundLayer forestTreeBarkLayer{"jolcham_oak_bark_01", 1.5f, Pixel{92, 78, 64}, 0.9f};
inline constexpr const char* forestTreeLeafFolder = "LeafSet024";

// The scan's leaves are a light spring green; a rainforest's are darker and deeper. This multiplies their colour (sRGB,
// as colour pickers give it): about a quarter of the light, a little greener.
inline constexpr Pixel forestTreeLeafTint{140, 150, 125};

// LeafSet024's grid of leaves.
inline constexpr int forestTreeLeafColumns = 3;
inline constexpr int forestTreeLeafRows = 3;

// The leaves' plain colour without the scans.
inline constexpr Pixel forestLeafStandIn{70, 105, 50};

// A leaf's alpha under this is a hole.
inline constexpr float forestLeafCutoff = 0.5f;

/*
* A leaf atlas's material (the ivy's by default): a double-sided cut-out, its colour with the opacity map in its alpha
* (made here, its mips keeping the leaves' coverage) multiplied by tint, its normal and roughness maps; without the
* files, a plain green cut-out with no holes.
*/
Material forestLeafMaterial(AssetManager& assets, const std::string& assetFolder, const char* leafFolder = forestIvyFolder,
                            Pixel tint = Pixel{255, 255, 255});

/*
* The trees' leaf clusters' material (games/forest/leaf_clusters.h): twigs of LeafSet024's leaves (or the stand-in
* leaf without the scan) composed at start-up into a 2 x 2 atlas of cellTexels a cell, its colour (tinted), normal and
* roughness maps; a double-sided cut-out, its mips keeping the leaves' coverage. leafShare is a leaf's length on a
* cluster as a share of the cell.
*/
Material forestLeafClusterMaterial(AssetManager& assets, const std::string& assetFolder, Pixel tint, int cellTexels, float leafShare);

// Hanging moss's colour comes from ambientCG's Moss001 scan (its mean), or this without it.
inline constexpr const char* forestMossFolder = "Moss001";
inline constexpr Pixel forestMossStandIn{96, 104, 62};

// The moss strands' texture's size: tall, as the clumps are.
inline constexpr int forestMossTextureWidth = 512;
inline constexpr int forestMossTextureHeight = 1024;

/*
* Hanging moss's material (games/forest/hanging_moss.h): a double-sided cut-out of the strands' texture, drawn here in
* the scan's mean colour (forestMossStandIn without it), its mips keeping the strands' coverage; rough, not metal.
*/
Material forestMossMaterial(AssetManager& assets, const std::string& assetFolder);

// The finest paint's texels: 2048 over the walked-on ring's 1.28 km.
inline constexpr float forestFinestPaintTexel = 0.625f;

/*
* The ground's blend depth (Material::groundBlendDepth) for paint whose texels are texelMetres across: the setting's own
* on the finest, deeper by the square root of how many times coarser they are, at most 1 (a plain cross-fade). A shallow
* blend turns the weights' bilinear slopes into hard edges, which on texels tens of metres wide show their corners as
* squares; a deeper one lets them fade.
*/
float forestBlendDepth(float blendDepth, float texelMetres);
