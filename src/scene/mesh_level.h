#pragma once

#include "scene/asset_handles.h"

#include <cstddef>

/*
* A coarser stand-in for a mesh (level of detail, docs/LOD.md): drawn instead of it wherever errorMetres, how far it
* strays from the mesh, covers few enough pixels (scene/level_of_detail.h). A mesh's levels go coarsest last, each
* straying further, and are named on the AssetManager (setMeshLevels).
*/
struct MeshLevel {
    MeshHandle mesh;
    float errorMetres = 0.0f;
};

// The mesh itself and at most three coarser levels: Horizon Zero Dawn's trees and SpeedTree's typical chains have 3-4
// meshes (docs/LOD.md).
inline constexpr std::size_t meshLevelSlots = 4;
