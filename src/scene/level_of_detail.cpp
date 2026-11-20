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
    visitor.field("Impostor self-shadow", settings.impostorSelfShadow);
    visitor.field("Impostor shadow bias", settings.impostorShadowBias, {.min = 0.0f, .max = 16.0f, .sliderMax = 4.0f, .unit = "texels"});
    visitor.field("Impostor shadow spread", settings.impostorShadowSpread, {.min = 0.0f, .max = 1.0f, .unit = "of a radius"});
    visitor.field("Fade band", settings.fadeBand, {.min = 0.0f, .max = fadeBandMost, .sliderMax = 0.5f});
}

void validateLevelOfDetail(const LevelOfDetailSettings& settings) {
    if (!(settings.pixelError > 0.0f)) {
        throw std::invalid_argument("Level of detail needs a pixel error above 0");
    }

    if (!(settings.fadeBand >= 0.0f && settings.fadeBand <= fadeBandMost)) {
        throw std::invalid_argument("Level of detail needs a fade band from 0 to 0.9");
    }

    if (!(settings.impostorShadowBias >= 0.0f)) {
        throw std::invalid_argument("Level of detail needs an impostor shadow bias of 0 or more");
    }

    if (!(settings.impostorShadowSpread >= 0.0f && settings.impostorShadowSpread <= 1.0f)) {
        throw std::invalid_argument("Level of detail needs an impostor shadow spread from 0 to 1");
    }
}

LevelView levelView(const Vec3& eye, float viewHeightPixels, float verticalFovRadians, const LevelOfDetailSettings& settings) {
    return LevelView{.eye = eye,
                     .pixelsPerMetre = viewHeightPixels / (2.0f * std::tan(0.5f * verticalFovRadians)),
                     .pixelError = settings.pixelError,
                     .enabled = settings.enabled,
                     .impostors = settings.impostors};
}

LevelView cameraLevelView(const Vec3& eye, float viewHeightPixels, float verticalFovRadians, const LevelOfDetailSettings& settings) {
    LevelView view = levelView(eye, viewHeightPixels, verticalFovRadians, settings);
    view.fadeBand = settings.fadeBand;
    return view;
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

float thinningErrorPerMetre(const LevelView& view) {
    if (!view.enabled || view.orthographic || !(view.pixelsPerMetre > 0.0f)) {
        return 0.0f;
    }

    return view.pixelError / view.pixelsPerMetre;
}

namespace {
    // The levels go coarsest last, each straying further, so the first that does not fit the budget ends the search; so
    // does one without a mesh while impostors are off, keeping the last level that draws something.
    std::size_t levelWithin(std::span<const MeshLevel> levels, float distance, const LevelView& view, float budget) {
        if (!view.enabled) {
            return 0;
        }

        std::size_t chosen = 0;

        for (std::size_t level = 0; level < levels.size(); ++level) {
            if (errorInPixels(levels[level].errorMetres, distance, view) > budget || (!view.impostors && !levels[level].mesh.isValid())) {
                break;
            }

            chosen = level + 1;
        }

        return chosen;
    }

    // The sphere a draw's level is chosen by: its options' shared box when it has one, else its own bounds.
    const BoundingSphere& choosingSphere(const DrawItem& draw) {
        return draw.levelBounds.radius >= 0.0f ? draw.levelBounds : draw.bounds;
    }

    /*
    * The level a draw takes under a budget of pixels, as chooseLevel counts; -1 for an impostor (hiddenBelowError above
    * 0) not drawn there: it is drawn only where its error fits, which is where its tree's last level, drawing nothing, is
    * chosen (the same error from the same sphere). With levels off it is never drawn, and the tree is drawn whole; with
    * impostors off neither, and the tree keeps its last level with a mesh.
    */
    int drawLevelWithin(const DrawItem& draw, const LevelView& view, float budget) {
        const BoundingSphere& sphere = choosingSphere(draw);

        if (draw.hiddenBelowError > 0.0f) {
            if (!view.enabled || !view.impostors || sphere.radius < 0.0f || errorInPixels(draw.hiddenBelowError, levelDistance(sphere, view.eye), view) > budget) {
                return -1;
            }
        }

        if (draw.levels.empty() || sphere.radius < 0.0f) {
            return 0;
        }

        return static_cast<int>(levelWithin(draw.levels, levelDistance(sphere, view.eye), view, budget));
    }

    // The mesh of a level as drawLevelWithin counts them; none for -1 or a level without one.
    MeshHandle levelMesh(const DrawItem& draw, int level) {
        if (level < 0) {
            return MeshHandle{};
        }

        return level == 0 ? draw.mesh : draw.levels[static_cast<std::size_t>(level) - 1].mesh;
    }
}

std::size_t chooseLevel(std::span<const MeshLevel> levels, float distance, const LevelView& view) {
    return levelWithin(levels, distance, view, view.pixelError);
}

float levelDistance(const BoundingSphere& bounds, const Vec3& eye) {
    return std::max((bounds.centre - eye).length() - bounds.radius, 0.0f);
}

std::size_t drawLevel(const DrawItem& draw, const LevelView& view) {
    const BoundingSphere& sphere = choosingSphere(draw);

    if (draw.levels.empty() || sphere.radius < 0.0f) {
        return 0;
    }

    return chooseLevel(draw.levels, levelDistance(sphere, view.eye), view);
}

MeshHandle chosenMesh(const DrawItem& draw, const LevelView& view) {
    return levelMesh(draw, drawLevelWithin(draw, view, view.pixelError));
}

/*
* The band before a switch is where a coarser level would fit a budget of pixelError / (1 - band): the draw seen from
* (1 - band) times as far. Its share is how far through it the draw is, by the coarser level's error e: 0 where e
* covers that wider budget, 1 where it covers pixelError, the switch. A finer level with two switches in the band steps
* to its next at the first; the share keeps running to the last.
*/
namespace {
    // levelFade by level numbers, as drawLevelWithin counts them.
    struct FadeLevels {
        bool fading = false;
        int finer = 0;
        int coarser = 0;
        float share = 0.0f;
    };

    FadeLevels fadeLevels(const DrawItem& draw, const LevelView& view) {
        if (!draw.fadesLevels || !(view.fadeBand > 0.0f) || view.orthographic || !view.enabled) {
            return FadeLevels{};
        }

        const float keep = 1.0f - view.fadeBand;
        const int finer = drawLevelWithin(draw, view, view.pixelError);
        const int coarser = drawLevelWithin(draw, view, view.pixelError / keep);

        if (finer == coarser) {
            return FadeLevels{};
        }

        const float error = finer < 0 ? draw.hiddenBelowError : draw.levels[static_cast<std::size_t>(coarser) - 1].errorMetres;
        const float pixels = errorInPixels(error, levelDistance(choosingSphere(draw), view.eye), view);
        const float share = std::clamp((view.pixelError / pixels - keep) / view.fadeBand, 0.0f, 1.0f);
        return FadeLevels{true, finer, coarser, share};
    }
}

LevelFade levelFade(const DrawItem& draw, const LevelView& view) {
    const FadeLevels fade = fadeLevels(draw, view);

    if (!fade.fading) {
        return LevelFade{};
    }

    return LevelFade{.fading = true, .finer = levelMesh(draw, fade.finer), .coarser = levelMesh(draw, fade.coarser), .share = fade.share};
}

// Draws drawing nothing in this view (an impostor near, a tree far) are not counted; a fading draw counts each level it
// draws (an impostor fading in, its card at 0).
std::array<std::size_t, meshLevelSlots> levelsChosen(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, const LevelView& view) {
    std::array<std::size_t, meshLevelSlots> counts{};

    for (const std::size_t index : chosen) {
        const DrawItem& draw = draws[index];
        const FadeLevels fade = fadeLevels(draw, view);
        const int drawn[2] = {fade.fading ? fade.finer : drawLevelWithin(draw, view, view.pixelError), fade.fading ? fade.coarser : -1};

        for (const int level : drawn) {
            if (levelMesh(draw, level).isValid()) {
                ++counts[std::min(static_cast<std::size_t>(level), meshLevelSlots - 1)];
            }
        }
    }

    return counts;
}

void attachMeshLevels(std::span<DrawItem> draws, const std::vector<std::vector<MeshLevel>>& levelsByMesh,
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
            draw.fadesLevels = options.fadesLevels;

            if (options.chooseBounds) {
                draw.levelBounds = boundingSphere(*options.chooseBounds, draw.model);
            }
        }
    }
}
