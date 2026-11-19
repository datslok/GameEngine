#include "games/forest/structure_seams.h"

#include "core/height_map.h"
#include "core/job_system.h"
#include "core/srgb.h"
#include "games/forest/forest_terrain.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
    // Rows about this tall and panels about this wide (metres), each between its least and most before the set is
    // scaled to fill the tile exactly.
    constexpr float rowLeast = 2.0f;
    constexpr float rowMost = 4.0f;
    constexpr float panelLeast = 2.0f;
    constexpr float panelMost = 6.0f;
    // The share of row boundaries (all but the tile's bottom) and of panel splits drawn as a sawtooth.
    constexpr float sawtoothRows = 0.3f;
    constexpr float sawtoothSplits = 0.25f;
    // The share of panels holding a hatch, the hatch's size range, and its least margin inside the panel (metres).
    constexpr float hatchShare = 0.15f;
    constexpr float hatchWidthLeast = 0.8f;
    constexpr float hatchWidthMost = 1.6f;
    constexpr float hatchHeightLeast = 0.6f;
    constexpr float hatchHeightMost = 1.2f;
    constexpr float hatchMargin = 0.4f;
    // How much a panel's own tone lightens or darkens the albedo (a share).
    constexpr float panelTone = 0.04f;
    constexpr float inverseRootTwo = 0.70710678f;

    // A run of random numbers from one seed, taken in order.
    struct SeamRandom {
        std::uint32_t seed;
        std::uint64_t next = 0;

        float unit() {
            return forestUnitRandom(seed, next++);
        }

        float between(float least, float most) {
            return least + (most - least) * unit();
        }
    };

    /*
    * Lengths between least and most adding up to total: as many as fit at their middle size, each drawn at random, then
    * all scaled to fill it (so a length may land a little outside the range).
    */
    std::vector<float> fillLengths(float total, float least, float most, SeamRandom& random) {
        const int count = std::max(1, static_cast<int>(std::lround(total / (0.5f * (least + most)))));
        std::vector<float> lengths;
        float sum = 0.0f;

        for (int index = 0; index < count; ++index) {
            lengths.push_back(random.between(least, most));
            sum += lengths.back();
        }

        for (float& length : lengths) {
            length *= total / sum;
        }

        return lengths;
    }

    // Where in 0..tile a coordinate falls, the tile repeating.
    float wrapped(float value, float tile) {
        const float inside = std::fmod(value, tile);
        return inside < 0.0f ? inside + tile : inside;
    }

    /*
    * A sawtooth's offset from its line at a place along it: a triangle wave rising and falling at 45 degrees between 0 and
    * half a period, the period about twice sawtoothTooth but dividing the tile, so the teeth meet across its edges.
    */
    float toothOffset(float along, float tile) {
        const float teeth = std::max(1.0f, std::round(tile / (2.0f * sawtoothTooth)));
        const float period = tile / teeth;
        const float half = 0.5f * period;
        return half - std::fabs(wrapped(along, period) - half);
    }

    // Where a line stands at a place along it: its own place, plus a sawtooth's offset.
    float linePlace(const SeamLine& line, float along, float tile) {
        return line.at + (line.sawtooth ? toothOffset(along, tile) : 0.0f);
    }

    // How far from a line a point is, across it (a 45-degree tooth's side is at the gap times cos 45 degrees).
    float lineDistance(const SeamLine& line, float across, float along, float tile) {
        const float gap = std::fabs(across - linePlace(line, along, tile));
        return line.sawtooth ? gap * inverseRootTwo : gap;
    }

    // The row a place in the tile lies in: the last whose bottom line is at or below it (the first's bottom is 0, straight).
    std::size_t rowAt(const SeamLayout& layout, Vec2 place) {
        std::size_t row = 0;

        for (std::size_t index = 1; index < layout.rows.size(); ++index) {
            if (linePlace(layout.rows[index].bottom, place.x, layout.tile) <= place.y) {
                row = index;
            }
        }

        return row;
    }

    // The panel of a row a place lies in: how many of its splits stand at or left of it, the last panel wrapping into the first.
    std::size_t panelAt(const SeamRow& row, Vec2 place, float tile) {
        std::size_t passed = 0;

        for (const SeamLine& split : row.splits) {
            passed += linePlace(split, place.y, tile) <= place.x ? 1 : 0;
        }

        return passed % row.splits.size();
    }

    // How far a point is from a rectangle's outline, inside or out.
    float outlineDistance(const SeamHatch& hatch, Vec2 place) {
        const Vec2 middle = hatch.corner + hatch.size * 0.5f;
        const float outsideX = std::fabs(place.x - middle.x) - 0.5f * hatch.size.x;
        const float outsideY = std::fabs(place.y - middle.y) - 0.5f * hatch.size.y;

        if (outsideX <= 0.0f && outsideY <= 0.0f) {
            return -std::max(outsideX, outsideY);
        }

        return Vec2{std::max(outsideX, 0.0f), std::max(outsideY, 0.0f)}.length();
    }
}

/*
* Rows filled up the tile, each row's panels across it from a random start (so splits do not line up row on row); a few
* boundaries and splits sawtooth; a hatch in some panels wide and tall enough, kept clear of the panel's edges and teeth.
* The panel wrapping round the tile's edge never holds one, so hatches need no wrapping.
*/
SeamLayout seamLayout(float tileMetres, std::uint32_t seed) {
    if (!(tileMetres >= 2.0f * rowMost)) {
        throw std::invalid_argument("A seam tile needs room for at least two rows");
    }

    SeamRandom random{seed};
    SeamLayout layout;
    layout.tile = tileMetres;
    float bottom = 0.0f;

    for (const float height : fillLengths(tileMetres, rowLeast, rowMost, random)) {
        SeamRow row;
        row.bottom = SeamLine{.at = bottom, .sawtooth = bottom > 0.0f && random.unit() < sawtoothRows};
        const std::vector<float> widths = fillLengths(tileMetres, panelLeast, panelMost, random);
        float split = random.between(0.0f, widths.front());

        for (const float width : widths) {
            row.splits.push_back(SeamLine{.at = wrapped(split, tileMetres), .sawtooth = random.unit() < sawtoothSplits});
            split += width;
        }

        std::sort(row.splits.begin(), row.splits.end(), [](const SeamLine& a, const SeamLine& b) {
            return a.at < b.at;
        });

        // Hatches in the panels between neighbouring splits (not the one wrapping round), clear of every tooth.
        const float rowTop = bottom + height;

        for (std::size_t panel = 0; panel + 1 < row.splits.size(); ++panel) {
            const float left = row.splits[panel].at + sawtoothTooth + hatchMargin;
            const float right = row.splits[panel + 1].at - hatchMargin;
            const float low = bottom + sawtoothTooth + hatchMargin;
            const float high = rowTop - hatchMargin;
            const float width = random.between(hatchWidthLeast, hatchWidthMost);
            const float tall = random.between(hatchHeightLeast, hatchHeightMost);
            const bool wanted = random.unit() < hatchShare;
            const float across = random.unit();
            const float up = random.unit();

            if (wanted && right - left >= width && high - low >= tall) {
                layout.hatches.push_back(SeamHatch{.corner = Vec2{left + across * (right - left - width), low + up * (high - low - tall)}, .size = Vec2{width, tall}});
            }
        }

        layout.rows.push_back(std::move(row));
        bottom = rowTop;
    }

    return layout;
}

/*
* Only the nearest lines matter (a groove is centimetres wide, panels metres): the row's own bottom and top lines (the
* top the next row's bottom, or the tile's top), its own splits, each as far as the point stands across it (the point
* lies within the row, so within the split's length), the tile wrapping across; and the hatches' outlines.
*/
float seamDistance(const SeamLayout& layout, Vec2 place) {
    const float tile = layout.tile;
    const Vec2 inside{wrapped(place.x, tile), wrapped(place.y, tile)};
    const std::size_t row = rowAt(layout, inside);
    const SeamRow& seamRow = layout.rows[row];
    const SeamLine top = row + 1 < layout.rows.size() ? layout.rows[row + 1].bottom : SeamLine{.at = tile};
    float nearest = std::min(lineDistance(seamRow.bottom, inside.y, inside.x, tile), lineDistance(top, inside.y, inside.x, tile));

    for (const SeamLine& split : seamRow.splits) {
        for (const float shift : {-tile, 0.0f, tile}) {
            nearest = std::min(nearest, lineDistance(split, inside.x + shift, inside.y, tile));
        }
    }

    for (const SeamHatch& hatch : layout.hatches) {
        nearest = std::min(nearest, outlineDistance(hatch, inside));
    }

    return nearest;
}

float seamHeight(float distance) {
    return smoothStep(seamHalfWidth, seamHalfWidth + seamBevel, distance);
}

/*
* Each texel's height at its middle and its albedo (its panel's tone, darkened into the grooves), a row a job; then the
* normals from the heights by central differences, wrapping (normalMapFromHeight), the grooves grooveDepth deep.
*/
SeamImages seamImages(const SeamLayout& layout, int size, JobSystem& jobs) {
    if (size <= 2) {
        throw std::invalid_argument("A seam map needs more than two texels across");
    }

    const std::size_t texels = static_cast<std::size_t>(size);
    const float texelMetres = layout.tile / static_cast<float>(size);
    const auto placeOf = [&](std::size_t column, std::size_t row) {
        return Vec2{(static_cast<float>(column) + 0.5f) * texelMetres, (static_cast<float>(row) + 0.5f) * texelMetres};
    };
    const std::uint32_t side = static_cast<std::uint32_t>(size);
    SeamImages images;
    images.albedo = ImageData{side, side, std::vector<std::uint8_t>(texels * texels * 4), TextureEncoding::Srgb};
    images.height = ImageData{side, side, std::vector<std::uint8_t>(texels * texels * 4), TextureEncoding::Linear};

    jobs.parallelFor(texels, 1, [&](std::size_t row) {
        for (std::size_t column = 0; column < texels; ++column) {
            const Vec2 place = placeOf(column, row);
            const float height = seamHeight(seamDistance(layout, place));
            const std::size_t seamRow = rowAt(layout, place);
            const std::size_t panel = panelAt(layout.rows[seamRow], place, layout.tile);
            const float tone = 1.0f + panelTone * (2.0f * forestUnitRandom(static_cast<std::uint64_t>(seamRow) * 64 + panel, 0x5345'414du) - 1.0f);
            const std::uint8_t shade = linearToSrgbByte(tone / (1.0f + panelTone) * (grooveShade + (1.0f - grooveShade) * height));
            const std::uint8_t level = static_cast<std::uint8_t>(std::lround(height * 255.0f));
            const std::size_t at = (row * texels + column) * 4;
            std::copy_n(std::array<std::uint8_t, 4>{shade, shade, shade, 255}.begin(), 4, &images.albedo.pixels[at]);
            std::copy_n(std::array<std::uint8_t, 4>{level, level, level, 255}.begin(), 4, &images.height.pixels[at]);
        }
    });

    // The normals from the heights (core/height_map), the depth in texture units: the tile is the texture's width.
    ImageLevel normals = normalMapFromHeight(ImageLevel{side, side, images.height.pixels}, grooveDepth / layout.tile);
    images.normal = ImageData{side, side, std::move(normals.pixels), TextureEncoding::Linear};
    return images;
}
