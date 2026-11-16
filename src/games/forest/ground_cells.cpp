#include "games/forest/ground_cells.h"
#include "games/forest/forest_terrain.h"
#include "math/angles.h"
#include "scene/level_of_detail.h"

#include <algorithm>
#include <cmath>

int groundDensityStep(int steps, float leastPaint, float paint) {
    if (paint < leastPaint || paint <= 0.0f) {
        return 0;
    }

    const int most = std::max(steps, 1);
    return std::clamp(static_cast<int>(std::lround(paint * static_cast<float>(most))), 1, most);
}

std::vector<GroundCell> forestGroundCells(const GroundCellOptions& options, JobSystem& jobs) {
    const float cell = std::max(options.cellMetres, 0.5f);
    const std::vector<Vec2>& path = forestPathPoints();
    Vec2 lowest = path.front();
    Vec2 highest = path.front();

    for (const Vec2& point : path) {
        lowest = Vec2{std::min(lowest.x, point.x), std::min(lowest.y, point.y)};
        highest = Vec2{std::max(highest.x, point.x), std::max(highest.y, point.y)};
    }

    const int firstColumn = static_cast<int>(std::floor((lowest.x - options.reachMetres) / cell));
    const int lastColumn = static_cast<int>(std::ceil((highest.x + options.reachMetres) / cell));
    const int firstRow = static_cast<int>(std::floor((lowest.y - options.reachMetres) / cell));
    const int lastRow = static_cast<int>(std::ceil((highest.y + options.reachMetres) / cell));
    const std::size_t rows = static_cast<std::size_t>(std::max(lastRow - firstRow, 0));
    const int variants = std::max(options.variants, 1);
    std::vector<std::vector<GroundCell>> byRow(rows);

    jobs.parallelFor(rows, 1, [&](std::size_t rowIndex) {
        const int row = firstRow + static_cast<int>(rowIndex);

        for (int column = firstColumn; column < lastColumn; ++column) {
            const Vec2 centre{(static_cast<float>(column) + 0.5f) * cell, (static_cast<float>(row) + 0.5f) * cell};

            if (forestPathDistance(centre.x, centre.y) > options.reachMetres) {
                continue;
            }

            constexpr int samples = 4;
            float paint = 0.0f;

            for (int i = 0; i < samples; ++i) {
                for (int j = 0; j < samples; ++j) {
                    const float x = centre.x + ((static_cast<float>(i) + 0.5f) / samples - 0.5f) * cell;
                    const float z = centre.y + ((static_cast<float>(j) + 0.5f) / samples - 0.5f) * cell;
                    paint += forestGroundPaint(x, z)[options.layer];
                }
            }

            const int step = groundDensityStep(options.densitySteps, options.leastPaint, paint / static_cast<float>(samples * samples));

            if (step == 0) {
                continue;
            }

            const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(row)) << 32) | static_cast<std::uint32_t>(column);
            GroundCell ground;
            ground.centre = centre;
            ground.step = step;
            ground.variant = std::min(variants - 1, static_cast<int>(forestUnitRandom(options.seed, 2 * key) * static_cast<float>(variants)));
            ground.quarterTurns = std::min(3, static_cast<int>(forestUnitRandom(options.seed, 2 * key + 1) * 4.0f));
            byRow[rowIndex].push_back(ground);
        }
    });

    std::vector<GroundCell> cells;

    for (const std::vector<GroundCell>& row : byRow) {
        cells.insert(cells.end(), row.begin(), row.end());
    }

    return cells;
}

float groundCoverLevelError(float distance) {
    constexpr float referenceHeightPixels = 1080.0f;
    constexpr float referenceFovDegrees = 70.0f;
    const LevelView view = levelView(Vec3{0.0f, 0.0f, 0.0f}, referenceHeightPixels, radiansFromDegrees(referenceFovDegrees), LevelOfDetailSettings{});
    return distance * view.pixelError / view.pixelsPerMetre;
}
