#include "scene/level_of_detail.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

void visit(LevelOfDetailSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Enabled", settings.enabled);
    visitor.field("Pixel error", settings.pixelError, {.min = 0.01f, .max = 1000.0f, .sliderMin = 0.25f, .sliderMax = 16.0f, .unit = "px"});
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
                     .enabled = settings.enabled};
}

float errorInPixels(float errorMetres, float distance, const LevelView& view) {
    if (distance <= 0.0f) {
        return std::numeric_limits<float>::infinity();
    }

    return errorMetres * view.pixelsPerMetre / distance;
}

// The levels go coarsest last, each straying further, so the first that does not fit ends the search.
std::size_t chooseLevel(std::span<const MeshLevel> levels, float distance, const LevelView& view) {
    if (!view.enabled) {
        return 0;
    }

    std::size_t chosen = 0;

    for (std::size_t level = 0; level < levels.size(); ++level) {
        if (errorInPixels(levels[level].errorMetres, distance, view) > view.pixelError) {
            break;
        }

        chosen = level + 1;
    }

    return chosen;
}

float levelDistance(const BoundingSphere& bounds, const Vec3& eye) {
    return std::max((bounds.centre - eye).length() - bounds.radius, 0.0f);
}

std::size_t drawLevel(const DrawItem& draw, const LevelView& view) {
    if (draw.levels.empty() || draw.bounds.radius < 0.0f) {
        return 0;
    }

    return chooseLevel(draw.levels, levelDistance(draw.bounds, view.eye), view);
}

MeshHandle chosenMesh(const DrawItem& draw, const LevelView& view) {
    const std::size_t level = drawLevel(draw, view);
    return level == 0 ? draw.mesh : draw.levels[level - 1].mesh;
}

std::array<std::size_t, meshLevelSlots> levelsChosen(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, const LevelView& view) {
    std::array<std::size_t, meshLevelSlots> counts{};

    for (const std::size_t index : chosen) {
        ++counts[std::min(drawLevel(draws[index], view), meshLevelSlots - 1)];
    }

    return counts;
}

void attachMeshLevels(std::vector<DrawItem>& draws, const std::vector<std::vector<MeshLevel>>& levelsByMesh) {
    for (DrawItem& draw : draws) {
        if (draw.mesh.isValid() && draw.mesh.index < levelsByMesh.size()) {
            draw.levels = levelsByMesh[draw.mesh.index];
        }
    }
}
