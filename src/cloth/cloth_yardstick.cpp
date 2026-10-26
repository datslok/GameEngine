#include "cloth/cloth_yardstick.h"

#include "scene/mesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
    // Point (row, column) of a sheet of squares a side, rows counted up from the hem (Mesh::grid).
    std::size_t sheetPoint(int squares, int row, int column) {
        return static_cast<std::size_t>(row) * static_cast<std::size_t>(squares + 1) + static_cast<std::size_t>(column);
    }

    // How far an edge is stretched: its length over its rest length, less 1 (negative when squashed).
    float edgeStrain(const std::vector<Vec3>& positions, const StretchConstraint& edge) {
        return (positions[edge.second] - positions[edge.first]).length() / edge.restLength - 1.0f;
    }
}

// The grid is centred on the origin, so it moves down half its height to hang from y = 0.
Cloth yardstickSheet(int squares, const ClothSettings& settings) {
    if (squares < 1) {
        throw std::invalid_argument("A yardstick sheet needs at least one square a side");
    }

    const Mesh grid = Mesh::grid(squares, squares, yardstickSheetSize, yardstickSheetSize);
    Cloth cloth = makeCloth(grid, Mat4::translation(0.0f, -0.5f * yardstickSheetSize, 0.0f), settings);
    addClothPin(cloth, static_cast<std::uint32_t>(sheetPoint(squares, squares, 0)));
    addClothPin(cloth, static_cast<std::uint32_t>(sheetPoint(squares, squares, squares)));
    return cloth;
}

SheetMeasure measureSheet(const Cloth& cloth, int squares) {
    const std::vector<Vec3>& positions = cloth.particles.positions;
    SheetMeasure measure;

    for (int column = 0; column <= squares; ++column) {
        float length = 0.0f;
        for (int row = 0; row < squares; ++row) {
            length += (positions[sheetPoint(squares, row + 1, column)] - positions[sheetPoint(squares, row, column)]).length();
        }
        measure.pathStretch = std::max(measure.pathStretch, length / yardstickSheetSize - 1.0f);
    }

    for (const StretchConstraint& edge : cloth.stretch) {
        measure.worstEdgeStrain = std::max(measure.worstEdgeStrain, edgeStrain(positions, edge));
    }

    const float pinHeight = 0.5f * (positions[sheetPoint(squares, squares, 0)].y + positions[sheetPoint(squares, squares, squares)].y);
    const float hemHeight = 0.5f * (positions[sheetPoint(squares, 0, squares / 2)].y + positions[sheetPoint(squares, 0, (squares + 1) / 2)].y);
    measure.sag = pinHeight - hemHeight;
    return measure;
}

/*
* The percentile is the strain at place ceil(share x edges) in the edges sorted by strain, counted from 1: the least
* strain that share of them are at or below.
*/
GarmentMeasure measureGarment(const Cloth& cloth, const std::vector<std::uint32_t>& hem, float floorHeight, const ClothFields& fields) {
    const std::vector<Vec3>& positions = cloth.particles.positions;
    GarmentMeasure measure;

    std::vector<float> strains;
    strains.reserve(cloth.stretch.size());
    double stretchSum = 0.0;
    for (const StretchConstraint& edge : cloth.stretch) {
        const float strain = edgeStrain(positions, edge);
        strains.push_back(strain);
        stretchSum += std::max(strain, 0.0f);
        measure.worstEdgeStrain = std::max(measure.worstEdgeStrain, strain);
    }
    if (!strains.empty()) {
        measure.meanStretch = static_cast<float>(stretchSum / static_cast<double>(strains.size()));
        const double place = std::ceil(static_cast<double>(garmentStrainPercentile) * static_cast<double>(strains.size()));
        const auto at = strains.begin() + std::max<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(place), 1) - 1;
        std::nth_element(strains.begin(), at, strains.end());
        measure.percentileStrain = *at;
    }

    for (const ClothStitch& stitch : cloth.stitches) {
        measure.worstSeamGap = std::max(measure.worstSeamGap, (positions[stitch.second] - positions[stitch.first]).length());
    }

    double heightSum = 0.0;
    for (const std::uint32_t point : hem) {
        heightSum += positions[point].y - floorHeight;
    }
    measure.hemHeight = hem.empty() ? 0.0f : static_cast<float>(heightSum / static_cast<double>(hem.size()));

    measure.closestToBody = std::numeric_limits<float>::infinity();
    for (const std::shared_ptr<const SignedDistanceField>& field : fields) {
        for (const Vec3& position : positions) {
            measure.closestToBody = std::min(measure.closestToBody, surfacePoint(*field, position).distance);
        }
    }
    return measure;
}

// The positions a second before the end are kept as the ticks pass them, so the run needs no history of its own.
float settleYardstick(Cloth& cloth, float seconds, float tickSeconds, JobSystem& jobs, const ClothFields& fields, const ClothTickPreparation& prepare) {
    const int ticks = static_cast<int>(std::lround(seconds / tickSeconds));
    const int lastSecondTicks = std::min(ticks, static_cast<int>(std::lround(1.0f / tickSeconds)));

    std::vector<Vec3> before = cloth.particles.positions;
    for (int tick = 0; tick < ticks; ++tick) {
        if (tick == ticks - lastSecondTicks) {
            before = cloth.particles.positions;
        }
        if (prepare) {
            prepare(cloth, tick);
        }
        stepCloth(cloth, tickSeconds, {}, jobs, fields);
    }

    float farthest = 0.0f;
    for (std::size_t index = 0; index < before.size(); ++index) {
        farthest = std::max(farthest, (cloth.particles.positions[index] - before[index]).length());
    }
    return farthest;
}
