#pragma once

#include "math/aabb.h"
#include "scene/asset_handles.h"

#include <cstddef>
#include <optional>

/*
* A coarser stand-in for a mesh (level of detail, docs/LOD.md): drawn instead of it wherever errorMetres, how far it
* strays from the mesh, covers few enough pixels (scene/level_of_detail.h). A mesh's levels go coarsest last, each
* straying further, and are named on the AssetManager (setMeshLevels). A last level without a mesh draws nothing.
*/
struct MeshLevel {
    MeshHandle mesh;
    float errorMetres = 0.0f;
};

/*
* How a mesh's level is chosen beyond its levels (AssetManager::setMeshLevelOptions): an impostor is drawn only where
* hiddenBelowError fits (the error its tree's parts end on, a last level without a mesh), and a tree's parts and its
* impostor choose by one box in their own frame (chooseBounds), so they switch at the same place; else by each draw's
* own bounds.
*/
struct MeshLevelOptions {
    float hiddenBelowError = 0.0f;
    std::optional<Aabb> chooseBounds;
};

// The mesh itself and at most three coarser levels: Horizon Zero Dawn's trees and SpeedTree's typical chains have 3-4
// meshes (docs/LOD.md).
inline constexpr std::size_t meshLevelSlots = 4;
