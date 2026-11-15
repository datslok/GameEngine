#include "scene/level_of_detail.h"
#include "scene/culling.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

void visit(LevelOfDetailSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Enabled", settings.enabled);
    visitor.field("Pixel error", settings.pixelError, {.min = 0.01f, .max = 1000.0f, .sliderMin = 0.25f, .sliderMax = 16.0f, .unit = "px"});
    visitor.field("Impostors", settings.impostors);
}

void validateLevelOfDetail(const LevelOfDetailSettings& settings) {
    if (!(settings.pixelError > 0.0f)) {
        throw std::invalid_argument("Level of detail needs a pixel error above 0");
    }
}

LevelView levelView(const Vec3& eye, float viewHeightPixels, float verticalFovRadians, const LevelOfDetailSettings& settings) {
    return LevelView{.eye = eye,
                     .pixelsPerMetre = viewHeightPixels / (2.0f * std::tan(0.5f * verticalFovRadians)),
                     .pixelError = settings.pixelError,
                     .enabled = settings.enabled,
                     .impostors = settings.impostors};
}

float errorInPixels(float errorMetres, float distance, const LevelView& view) {
    if (view.orthographic) {
        return errorMetres * view.pixelsPerMetre;
    }

    if (distance <= 0.0f) {
        return std::numeric_limits<float>::infinity();
    }

    return errorMetres * view.pixelsPerMetre / distance;
}

// The levels go coarsest last, each straying further, so the first that does not fit ends the search; so does one
// without a mesh while impostors are off, keeping the last level that draws something.
std::size_t chooseLevel(std::span<const MeshLevel> levels, float distance, const LevelView& view) {
    if (!view.enabled) {
        return 0;
    }

    std::size_t chosen = 0;

    for (std::size_t level = 0; level < levels.size(); ++level) {
        if (errorInPixels(levels[level].errorMetres, distance, view) > view.pixelError || (!view.impostors && !levels[level].mesh.isValid())) {
            break;
        }

        chosen = level + 1;
    }

    return chosen;
}

float levelDistance(const BoundingSphere& bounds, const Vec3& eye) {
    return std::max((bounds.centre - eye).length() - bounds.radius, 0.0f);
}

namespace {
    // The sphere a draw's level is chosen by: its options' shared box when it has one, else its own bounds.
    const BoundingSphere& choosingSphere(const DrawItem& draw) {
        return draw.levelBounds.radius >= 0.0f ? draw.levelBounds : draw.bounds;
    }
}

std::size_t drawLevel(const DrawItem& draw, const LevelView& view) {
    const BoundingSphere& sphere = choosingSphere(draw);

    if (draw.levels.empty() || sphere.radius < 0.0f) {
        return 0;
    }

    return chooseLevel(draw.levels, levelDistance(sphere, view.eye), view);
}

/*
* An impostor (hiddenBelowError above 0) is drawn only where its error fits, which is where its tree's last level,
* drawing nothing, is chosen: the same error from the same sphere. With levels off it is never drawn, and the tree is
* drawn whole; with impostors off neither, and the tree keeps its last level with a mesh.
*/
MeshHandle chosenMesh(const DrawItem& draw, const LevelView& view) {
    if (draw.hiddenBelowError > 0.0f) {
        const BoundingSphere& sphere = choosingSphere(draw);

        if (!view.enabled || !view.impostors || sphere.radius < 0.0f || errorInPixels(draw.hiddenBelowError, levelDistance(sphere, view.eye), view) > view.pixelError) {
            return MeshHandle{};
        }
    }

    const std::size_t level = drawLevel(draw, view);
    return level == 0 ? draw.mesh : draw.levels[level - 1].mesh;
}

// Draws drawing nothing in this view (an impostor near, a tree far) are not counted.
std::array<std::size_t, meshLevelSlots> levelsChosen(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, const LevelView& view) {
    std::array<std::size_t, meshLevelSlots> counts{};

    for (const std::size_t index : chosen) {
        if (chosenMesh(draws[index], view).isValid()) {
            ++counts[std::min(drawLevel(draws[index], view), meshLevelSlots - 1)];
        }
    }

    return counts;
}

void attachMeshLevels(std::vector<DrawItem>& draws, const std::vector<std::vector<MeshLevel>>& levelsByMesh,
                      const std::vector<MeshLevelOptions>& optionsByMesh) {
    for (DrawItem& draw : draws) {
        if (!draw.mesh.isValid()) {
            continue;
        }

        if (draw.mesh.index < levelsByMesh.size()) {
            draw.levels = levelsByMesh[draw.mesh.index];
        }

        if (draw.mesh.index < optionsByMesh.size()) {
            const MeshLevelOptions& options = optionsByMesh[draw.mesh.index];
            draw.hiddenBelowError = options.hiddenBelowError;

            if (options.chooseBounds) {
                draw.levelBounds = boundingSphere(*options.chooseBounds, draw.model);
            }
        }
    }
}
