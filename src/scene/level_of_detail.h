#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"
#include "scene/frame_description.h"
#include "scene/mesh_level.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

/*
* Level of detail (docs/LOD.md): each view draws a coarser level of a mesh wherever the level's error, seen from the
* view, would cover no more than pixelError pixels. The error's size on screen is how Nanite and meshoptimizer choose;
* Unreal and Unity's screen sizes are the same idea with the error left to the artist.
*/
struct LevelOfDetailSettings {
    bool enabled = true;

    // How many pixels a level's error may cover before the view draws a finer one: more is cheaper and coarser.
    float pixelError = 2.0f;
};

void visit(LevelOfDetailSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a budget not above 0.
void validateLevelOfDetail(const LevelOfDetailSettings& settings);

/*
* What a view needs to choose levels: where it is seen from and how many pixels a metre covers a metre away (its height
* in pixels over 2 tan(fov / 2), the height a metre away spans), so a metre d metres away covers pixelsPerMetre / d.
*/
struct LevelView {
    Vec3 eye;
    float pixelsPerMetre = 0.0f;
    float pixelError = 1.0f;
    bool enabled = false;

    // A view without perspective (the sun's shadow tiles): pixelsPerMetre holds everywhere in it, whatever the distance.
    bool orthographic = false;
};

LevelView levelView(const Vec3& eye, float viewHeightPixels, float verticalFovRadians, const LevelOfDetailSettings& settings);

// How many pixels an error of errorMetres covers distance metres from the view; everything at 0 (or nearer). In an
// orthographic view, errorMetres times its pixels a metre, wherever it is.
float errorInPixels(float errorMetres, float distance, const LevelView& view);

// The level to draw distance metres away: 0 the mesh itself, k levels[k - 1], the coarsest whose error fits the budget.
std::size_t chooseLevel(std::span<const MeshLevel> levels, float distance, const LevelView& view);

// How far a draw's nearest point can be: to its sphere's surface, 0 from inside it.
float levelDistance(const BoundingSphere& bounds, const Vec3& eye);

// The level a draw is drawn at in this view (as chooseLevel counts); unknown bounds keep the mesh itself.
std::size_t drawLevel(const DrawItem& draw, const LevelView& view);

// The mesh a draw is drawn with in this view: its own, or one of its levels; none (an invalid handle) where it draws
// nothing: an impostor not yet far enough, or a tree past its last level.
MeshHandle chosenMesh(const DrawItem& draw, const LevelView& view);

// How many of the chosen draws draw each level in this view (the F4 readout's).
std::array<std::size_t, meshLevelSlots> levelsChosen(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, const LevelView& view);

// Give each draw its mesh's levels and options, from tables indexed like MeshHandle (AssetManager::getMeshLevels and
// getMeshLevelOptions; the levels must outlive the frame), the options' shared box placed by the draw's model matrix.
void attachMeshLevels(std::vector<DrawItem>& draws, const std::vector<std::vector<MeshLevel>>& levelsByMesh,
                      const std::vector<MeshLevelOptions>& optionsByMesh = {});
