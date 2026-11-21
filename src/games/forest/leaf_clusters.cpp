#include "games/forest/leaf_clusters.h"

#include "games/forest/forest_terrain.h"
#include "math/angles.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
    // The stand-in leaf: an ellipse this share of its cell across and up from its middle, its dome's lean at the edge.
    constexpr float standInHalfWidth = 0.3f;
    constexpr float standInHalfHeight = 0.45f;
    constexpr float standInDome = 0.5f;
    constexpr std::uint8_t standInRoughness = 153; // 0.6

    // A twig: how many leaves, where along the stem they start and end, how far they turn out (degrees, most at the
    // stem's foot), how long (a share of the asked length, shorter towards the tip), and their tones.
    constexpr int fewestLeaves = 12;
    constexpr int leafCountRange = 7;
    constexpr float firstLeafAlong = 0.12f;
    constexpr float lastLeafAlong = 0.95f;
    constexpr float widestTurnDegrees = 70.0f;
    constexpr float narrowestTurnDegrees = 35.0f;
    constexpr float turnJitterDegrees = 10.0f;
    constexpr float lastLeafTurn = 0.2f;
    constexpr float shortestLeafShare = 0.7f;
    constexpr float tipLeafShrink = 0.25f;
    constexpr float highestLeafTip = 0.98f;
    constexpr float darkestTone = 0.85f;
    constexpr float toneRange = 0.25f;
    constexpr float mostWarmth = 0.06f;
    constexpr float mostStemBend = 0.12f;

    // The stem: its foot and rise as shares of the cell, half its width at the foot (thinning to a third at the tip),
    // its colour (a share of the leaves' mean, browned), roughness and how finely it is followed.
    constexpr float stemFoot = 0.02f;
    constexpr float stemRise = 0.9f;
    constexpr float stemHalfWidth = 0.008f;
    constexpr float stemDarkening = 0.55f;
    constexpr float stemRoughness = 0.7f;
    constexpr int stemSegments = 32;

    using Texel = std::array<float, 4>;

    std::size_t texelIndex(const ImageData& image, std::uint32_t x, std::uint32_t y) {
        return (static_cast<std::size_t>(y) * image.width + x) * 4;
    }

    ImageData blankImage(std::uint32_t width, std::uint32_t height, TextureEncoding encoding) {
        ImageData image;
        image.width = width;
        image.height = height;
        image.encoding = encoding;
        image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
        return image;
    }

    // An image read bilinearly between texel centres at (u, v) in 0..1, held at the edges; channels 0..255.
    Texel sampleBilinear(const ImageData& image, float u, float v) {
        const float x = std::clamp(u * static_cast<float>(image.width) - 0.5f, 0.0f, static_cast<float>(image.width - 1));
        const float y = std::clamp(v * static_cast<float>(image.height) - 0.5f, 0.0f, static_cast<float>(image.height - 1));
        const std::uint32_t x0 = static_cast<std::uint32_t>(x);
        const std::uint32_t y0 = static_cast<std::uint32_t>(y);
        const std::uint32_t x1 = std::min(x0 + 1, image.width - 1);
        const std::uint32_t y1 = std::min(y0 + 1, image.height - 1);
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        Texel result{};

        for (int channel = 0; channel < 4; ++channel) {
            const float bottom = image.pixels[texelIndex(image, x0, y0) + channel] * (1.0f - fx) + image.pixels[texelIndex(image, x1, y0) + channel] * fx;
            const float top = image.pixels[texelIndex(image, x0, y1) + channel] * (1.0f - fx) + image.pixels[texelIndex(image, x1, y1) + channel] * fx;
            result[static_cast<std::size_t>(channel)] = bottom * (1.0f - fy) + top * fy;
        }

        return result;
    }

    std::uint8_t toByte(float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
    }

    // What composing keeps per texel before it becomes bytes: colour and coverage, the normal, the roughness.
    struct Layer {
        std::vector<float> colour;    // r g b (0..255) and coverage (0..1) per texel
        std::vector<Vec3> normal;
        std::vector<float> roughness; // 0..255
    };

    /*
    * Lays one texel of something over what is there by its coverage (the "over" operator): the colour averaged by
    * coverage, the normal and roughness blended by the new coverage.
    */
    void layOver(Layer& layer, std::size_t texel, const Vec3& rgb, float coverage, const Vec3& normal, float roughness) {
        if (coverage <= 0.0f) {
            return;
        }

        float* colour = &layer.colour[texel * 4];
        const float below = colour[3];
        const float total = coverage + below * (1.0f - coverage);
        const float keep = below * (1.0f - coverage) / total;
        const float add = coverage / total;
        colour[0] = rgb.x * add + colour[0] * keep;
        colour[1] = rgb.y * add + colour[1] * keep;
        colour[2] = rgb.z * add + colour[2] * keep;
        colour[3] = total;
        layer.normal[texel] = layer.normal[texel] * (1.0f - coverage) + normal * coverage;
        layer.roughness[texel] = layer.roughness[texel] * (1.0f - coverage) + roughness * coverage;
    }

    // The mean colour of an atlas's leaves (texels at least half covered), in its bytes.
    Vec3 meanLeafColour(const ImageData& colour) {
        double sum[3] = {0.0, 0.0, 0.0};
        std::size_t count = 0;

        for (std::size_t texel = 0; texel < colour.pixels.size() / 4; ++texel) {
            if (colour.pixels[texel * 4 + 3] >= 128) {
                for (int channel = 0; channel < 3; ++channel) {
                    sum[channel] += colour.pixels[texel * 4 + static_cast<std::size_t>(channel)];
                }

                ++count;
            }
        }

        if (count == 0) {
            return Vec3{70.0f, 105.0f, 50.0f};
        }

        return Vec3{static_cast<float>(sum[0] / count), static_cast<float>(sum[1] / count), static_cast<float>(sum[2] / count)};
    }
}

LeafAtlas standInLeafAtlas(int cellTexels, int columns, int rows, Pixel colour) {
    const std::uint32_t width = static_cast<std::uint32_t>(cellTexels * columns);
    const std::uint32_t height = static_cast<std::uint32_t>(cellTexels * rows);
    LeafAtlas atlas{blankImage(width, height, TextureEncoding::Srgb), blankImage(width, height, TextureEncoding::Linear),
                    blankImage(width, height, TextureEncoding::Linear), columns, rows};

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            // Where the texel lies in its cell, from the leaf's middle, in its half sizes.
            const float cellX = (static_cast<float>(x % static_cast<std::uint32_t>(cellTexels)) + 0.5f) / static_cast<float>(cellTexels);
            const float cellY = (static_cast<float>(y % static_cast<std::uint32_t>(cellTexels)) + 0.5f) / static_cast<float>(cellTexels);
            const float across = (cellX - 0.5f) / standInHalfWidth;
            const float up = (cellY - 0.5f) / standInHalfHeight;
            const bool inside = across * across + up * up <= 1.0f;
            const std::size_t index = texelIndex(atlas.colour, x, y);

            atlas.colour.pixels[index] = colour.r;
            atlas.colour.pixels[index + 1] = colour.g;
            atlas.colour.pixels[index + 2] = colour.b;
            atlas.colour.pixels[index + 3] = inside ? 255 : 0;

            const Vec3 normal = Vec3{across * standInDome, up * standInDome, 1.0f}.normalized();
            atlas.normal.pixels[index] = toByte((normal.x * 0.5f + 0.5f) * 255.0f);
            atlas.normal.pixels[index + 1] = toByte((normal.y * 0.5f + 0.5f) * 255.0f);
            atlas.normal.pixels[index + 2] = toByte((normal.z * 0.5f + 0.5f) * 255.0f);
            atlas.normal.pixels[index + 3] = 255;

            atlas.roughness.pixels[index] = standInRoughness;
            atlas.roughness.pixels[index + 1] = standInRoughness;
            atlas.roughness.pixels[index + 2] = standInRoughness;
            atlas.roughness.pixels[index + 3] = 255;
        }
    }

    return atlas;
}

Vec3 turnTangentNormal(const Vec3& normal, float angleRadians) {
    const float cosine = std::cos(angleRadians);
    const float sine = std::sin(angleRadians);
    return Vec3{normal.x * cosine - normal.y * sine, normal.x * sine + normal.y * cosine, normal.z};
}

Vec2 clusterStemPoint(float t, float bend) {
    return Vec2{0.5f + bend * t * t, stemFoot + stemRise * t};
}

float clusterStemBend(std::uint32_t seed) {
    return mostStemBend * (2.0f * forestUnitRandom(seed, 0) - 1.0f);
}

std::vector<ClusterLeaf> clusterLeaves(int sourceCells, float leafShare, std::uint32_t seed) {
    const float bend = clusterStemBend(seed);
    const int count = std::min(fewestLeaves + static_cast<int>(static_cast<float>(leafCountRange) * forestUnitRandom(seed, 1)),
                               fewestLeaves + leafCountRange - 1);
    std::vector<ClusterLeaf> leaves;
    leaves.reserve(static_cast<std::size_t>(count));
    const float toRadians = fullTurnRadians / 360.0f;

    for (int index = 0; index < count; ++index) {
        const std::uint64_t base = 8 + static_cast<std::uint64_t>(index) * 8;
        const float share = static_cast<float>(index) / static_cast<float>(count - 1);
        const float t = firstLeafAlong + (lastLeafAlong - firstLeafAlong) * share;
        const bool last = index == count - 1;

        ClusterLeaf leaf;
        leaf.base = clusterStemPoint(t, bend);

        if (last) {
            leaf.angle = lastLeafTurn * (2.0f * forestUnitRandom(seed, base) - 1.0f);
        } else {
            const float degrees = std::clamp(widestTurnDegrees - (widestTurnDegrees - narrowestTurnDegrees) * share +
                                                 turnJitterDegrees * (forestUnitRandom(seed, base) - 0.5f),
                                             narrowestTurnDegrees, widestTurnDegrees);
            leaf.angle = (index % 2 == 0 ? 1.0f : -1.0f) * degrees * toRadians;
        }

        leaf.length = leafShare * (shortestLeafShare + (1.0f - shortestLeafShare) * forestUnitRandom(seed, base + 1)) * (1.0f - tipLeafShrink * share);

        // Kept inside the cell: a leaf reaching past the top edge is shortened to end at it.
        const float rise = std::cos(leaf.angle);

        if (leaf.base.y + leaf.length * rise > highestLeafTip) {
            leaf.length = (highestLeafTip - leaf.base.y) / rise;
        }

        leaf.sourceCell = std::min(static_cast<int>(forestUnitRandom(seed, base + 2) * static_cast<float>(sourceCells)), sourceCells - 1);
        leaf.tone = darkestTone + toneRange * forestUnitRandom(seed, base + 3);
        leaf.warmth = mostWarmth * (2.0f * forestUnitRandom(seed, base + 4) - 1.0f);
        leaves.push_back(leaf);
    }

    return leaves;
}

/*
* Each cell is drawn by visiting only the texels near what is drawn: the stem's band and each leaf's bounding box. A
* texel's centre is taken into the leaf's own frame (across and along it, in leaf lengths), and from there into the
* atlas cell the leaf shows: its stem at the middle of the cell's bottom edge, the leaf up the cell.
*/
LeafAtlas composeLeafClusters(const LeafAtlas& source, int cellTexels, float leafShare, std::uint32_t seed) {
    const std::uint32_t size = static_cast<std::uint32_t>(cellTexels) * 2;
    const std::size_t texels = static_cast<std::size_t>(size) * size;
    const Vec3 mean = meanLeafColour(source.colour);
    const int sourceCells = source.columns * source.rows;
    const float cellScale = static_cast<float>(cellTexels);

    // Uncovered texels: the leaves' mean colour with no coverage, a flat normal, the stem's roughness.
    Layer layer;
    layer.colour.resize(texels * 4);
    layer.normal.assign(texels, Vec3{0.0f, 0.0f, 1.0f});
    layer.roughness.assign(texels, stemRoughness * 255.0f);

    for (std::size_t texel = 0; texel < texels; ++texel) {
        layer.colour[texel * 4] = mean.x;
        layer.colour[texel * 4 + 1] = mean.y;
        layer.colour[texel * 4 + 2] = mean.z;
    }

    const Vec3 stemColour = mean * stemDarkening + Vec3{20.0f, 8.0f, 0.0f};

    for (int cell = 0; cell < clusterColumns * clusterRows; ++cell) {
        const std::uint32_t originX = static_cast<std::uint32_t>((cell % clusterColumns) * cellTexels);
        const std::uint32_t originY = static_cast<std::uint32_t>((cell / clusterColumns) * cellTexels);
        const std::uint32_t twigSeed = forestShapeSeed(seed, static_cast<std::uint32_t>(cell) + 1);
        const float bend = clusterStemBend(twigSeed);

        // The texel's centre in the cell's own 0..1 coordinates.
        const auto cellPlace = [&](std::uint32_t x, std::uint32_t y) {
            return Vec2{(static_cast<float>(x - originX) + 0.5f) / cellScale, (static_cast<float>(y - originY) + 0.5f) / cellScale};
        };

        // The stem: each texel's distance to its line of segments, covered within its half width (soft over a texel).
        std::array<Vec2, stemSegments + 1> stem{};

        for (int point = 0; point <= stemSegments; ++point) {
            stem[static_cast<std::size_t>(point)] = clusterStemPoint(static_cast<float>(point) / stemSegments, bend);
        }

        const float stemLeft = std::min(0.5f, 0.5f + bend) - 2.0f * stemHalfWidth;
        const float stemRight = std::max(0.5f, 0.5f + bend) + 2.0f * stemHalfWidth;
        const std::uint32_t firstColumn = originX + static_cast<std::uint32_t>(std::max(0.0f, std::floor(stemLeft * cellScale)));
        const std::uint32_t lastColumn = originX + static_cast<std::uint32_t>(std::min(cellScale - 1.0f, std::ceil(stemRight * cellScale)));

        for (std::uint32_t y = originY; y < originY + static_cast<std::uint32_t>(cellTexels); ++y) {
            for (std::uint32_t x = firstColumn; x <= lastColumn; ++x) {
                const Vec2 place = cellPlace(x, y);
                float nearest = 1e9f;
                float along = 0.0f;

                for (int segment = 0; segment < stemSegments; ++segment) {
                    const Vec2 a = stem[static_cast<std::size_t>(segment)];
                    const Vec2 b = stem[static_cast<std::size_t>(segment) + 1];
                    const Vec2 ab = b - a;
                    const float share = std::clamp(((place.x - a.x) * ab.x + (place.y - a.y) * ab.y) / (ab.x * ab.x + ab.y * ab.y), 0.0f, 1.0f);
                    const Vec2 closest = a + ab * share;
                    const float distance = std::hypot(place.x - closest.x, place.y - closest.y);

                    if (distance < nearest) {
                        nearest = distance;
                        along = (static_cast<float>(segment) + share) / stemSegments;
                    }
                }

                const float halfWidth = stemHalfWidth * (1.0f - (2.0f / 3.0f) * along);
                const float coverage = std::clamp((halfWidth - nearest) * cellScale + 0.5f, 0.0f, 1.0f);
                layOver(layer, static_cast<std::size_t>(y) * size + x, stemColour, coverage, Vec3{0.0f, 0.0f, 1.0f}, stemRoughness * 255.0f);
            }
        }

        for (const ClusterLeaf& leaf : clusterLeaves(sourceCells, leafShare, twigSeed)) {
            const Vec2 along{-std::sin(leaf.angle), std::cos(leaf.angle)};
            const Vec2 across{std::cos(leaf.angle), std::sin(leaf.angle)};
            const int sourceColumn = leaf.sourceCell % source.columns;
            const int sourceRow = leaf.sourceCell / source.columns;
            const float sourceWidth = 1.0f / static_cast<float>(source.columns);
            const float sourceHeight = 1.0f / static_cast<float>(source.rows);

            // The leaf's box in the cell: its four corners' extent.
            float lowX = 1.0f;
            float highX = 0.0f;
            float lowY = 1.0f;
            float highY = 0.0f;

            for (const float side : {-0.5f, 0.5f}) {
                for (const float up : {0.0f, 1.0f}) {
                    const Vec2 corner = leaf.base + (across * side + along * up) * leaf.length;
                    lowX = std::min(lowX, corner.x);
                    highX = std::max(highX, corner.x);
                    lowY = std::min(lowY, corner.y);
                    highY = std::max(highY, corner.y);
                }
            }

            const std::uint32_t x0 = originX + static_cast<std::uint32_t>(std::clamp(std::floor(lowX * cellScale), 0.0f, cellScale - 1.0f));
            const std::uint32_t x1 = originX + static_cast<std::uint32_t>(std::clamp(std::ceil(highX * cellScale), 0.0f, cellScale - 1.0f));
            const std::uint32_t y0 = originY + static_cast<std::uint32_t>(std::clamp(std::floor(lowY * cellScale), 0.0f, cellScale - 1.0f));
            const std::uint32_t y1 = originY + static_cast<std::uint32_t>(std::clamp(std::ceil(highY * cellScale), 0.0f, cellScale - 1.0f));

            for (std::uint32_t y = y0; y <= y1; ++y) {
                for (std::uint32_t x = x0; x <= x1; ++x) {
                    const Vec2 offset = cellPlace(x, y) - leaf.base;
                    const float side = (offset.x * across.x + offset.y * across.y) / leaf.length;
                    const float up = (offset.x * along.x + offset.y * along.y) / leaf.length;

                    if (side < -0.5f || side > 0.5f || up < 0.0f || up > 1.0f) {
                        continue;
                    }

                    const float u = (static_cast<float>(sourceColumn) + side + 0.5f) * sourceWidth;
                    const float v = (static_cast<float>(sourceRow) + up) * sourceHeight;
                    const Texel colour = sampleBilinear(source.colour, u, v);
                    const Texel normalBytes = sampleBilinear(source.normal, u, v);
                    const Texel roughness = sampleBilinear(source.roughness, u, v);
                    const Vec3 tangentNormal{normalBytes[0] / 255.0f * 2.0f - 1.0f, normalBytes[1] / 255.0f * 2.0f - 1.0f,
                                             normalBytes[2] / 255.0f * 2.0f - 1.0f};
                    const Vec3 rgb{colour[0] * leaf.tone * (1.0f + leaf.warmth), colour[1] * leaf.tone, colour[2] * leaf.tone * (1.0f - leaf.warmth)};
                    layOver(layer, static_cast<std::size_t>(y) * size + x, rgb, colour[3] / 255.0f, turnTangentNormal(tangentNormal, leaf.angle),
                            roughness[1]);
                }
            }
        }
    }

    LeafAtlas clusters{blankImage(size, size, TextureEncoding::Srgb), blankImage(size, size, TextureEncoding::Linear),
                       blankImage(size, size, TextureEncoding::Linear), clusterColumns, clusterRows};

    for (std::size_t texel = 0; texel < texels; ++texel) {
        const float* colour = &layer.colour[texel * 4];
        clusters.colour.pixels[texel * 4] = toByte(colour[0]);
        clusters.colour.pixels[texel * 4 + 1] = toByte(colour[1]);
        clusters.colour.pixels[texel * 4 + 2] = toByte(colour[2]);
        clusters.colour.pixels[texel * 4 + 3] = toByte(colour[3] * 255.0f);

        const Vec3 normal = layer.normal[texel].lengthSquared() > 1e-8f ? layer.normal[texel].normalized() : Vec3{0.0f, 0.0f, 1.0f};
        clusters.normal.pixels[texel * 4] = toByte((normal.x * 0.5f + 0.5f) * 255.0f);
        clusters.normal.pixels[texel * 4 + 1] = toByte((normal.y * 0.5f + 0.5f) * 255.0f);
        clusters.normal.pixels[texel * 4 + 2] = toByte((normal.z * 0.5f + 0.5f) * 255.0f);
        clusters.normal.pixels[texel * 4 + 3] = 255;

        const std::uint8_t rough = toByte(layer.roughness[texel]);
        clusters.roughness.pixels[texel * 4] = rough;
        clusters.roughness.pixels[texel * 4 + 1] = rough;
        clusters.roughness.pixels[texel * 4 + 2] = rough;
        clusters.roughness.pixels[texel * 4 + 3] = 255;
    }

    return clusters;
}

namespace {
    // The part of a convex polygon on the side of a line where along . point <= limit (Sutherland-Hodgman, one edge).
    std::vector<Vec2> clipBelow(const std::vector<Vec2>& polygon, const Vec2& along, float limit) {
        std::vector<Vec2> kept;

        for (std::size_t i = 0; i < polygon.size(); ++i) {
            const Vec2& a = polygon[i];
            const Vec2& b = polygon[(i + 1) % polygon.size()];
            const float da = along.x * a.x + along.y * a.y - limit;
            const float db = along.x * b.x + along.y * b.y - limit;

            if (da <= 0.0f) {
                kept.push_back(a);
            }

            if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
                const float t = da / (da - db);
                kept.push_back(Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
            }
        }

        return kept;
    }
}

std::vector<Vec2> clusterOutline(const ImageData& image, std::uint32_t column, std::uint32_t row, std::uint32_t cellTexels,
                                 std::uint8_t alphaCutoff, float marginTexels) {
    // The four directions' lowest and highest reach over the covered texels' corners, in texels from the cell's corner.
    const float diagonal = 0.70710678f;
    const std::array<Vec2, 4> directions{Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}, Vec2{diagonal, diagonal}, Vec2{diagonal, -diagonal}};
    std::array<float, 4> lowest;
    std::array<float, 4> highest;
    lowest.fill(1e30f);
    highest.fill(-1e30f);
    bool any = false;

    for (std::uint32_t y = 0; y < cellTexels; ++y) {
        for (std::uint32_t x = 0; x < cellTexels; ++x) {
            const std::size_t texel = (static_cast<std::size_t>(row * cellTexels + y) * image.width + column * cellTexels + x) * 4 + 3;

            if (image.pixels[texel] < alphaCutoff) {
                continue;
            }

            any = true;

            for (const Vec2 corner : {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}, Vec2{1.0f, 1.0f}}) {
                const Vec2 point{static_cast<float>(x) + corner.x, static_cast<float>(y) + corner.y};

                for (std::size_t d = 0; d < directions.size(); ++d) {
                    const float reach = directions[d].x * point.x + directions[d].y * point.y;
                    lowest[d] = std::min(lowest[d], reach);
                    highest[d] = std::max(highest[d], reach);
                }
            }
        }
    }

    if (!any) {
        return {};
    }

    const float size = static_cast<float>(cellTexels);
    std::vector<Vec2> polygon{Vec2{0.0f, 0.0f}, Vec2{size, 0.0f}, Vec2{size, size}, Vec2{0.0f, size}};

    for (std::size_t d = 0; d < directions.size(); ++d) {
        polygon = clipBelow(polygon, directions[d], highest[d] + marginTexels);
        polygon = clipBelow(polygon, Vec2{-directions[d].x, -directions[d].y}, -(lowest[d] - marginTexels));
    }

    for (Vec2& corner : polygon) {
        corner = Vec2{corner.x / size, corner.y / size};
    }

    return polygon;
}

namespace {
    // The grid the rows' heights are chosen on: this many steps from base to tip.
    constexpr int leafRowSteps = 24;

    // A cell's covered extent on each texel row, as half widths left and right of the midrib (0.5), in cell units;
    // both 0 on a row with nothing covered.
    struct LeafProfile {
        std::vector<float> left;
        std::vector<float> right;
    };

    // The shape's half widths at a height: between its knots (base, rows, tip) linearly, 0 outside base..tip.
    void shapeWidthsAt(const LeafShape& shape, float up, float& left, float& right) {
        left = 0.0f;
        right = 0.0f;

        if (up <= shape.base || up >= shape.tip) {
            return;
        }

        LeafRow below{shape.base, 0.0f, 0.0f};

        for (std::size_t knot = 0; knot <= shape.rows.size(); ++knot) {
            const LeafRow above = knot < shape.rows.size() ? shape.rows[knot] : LeafRow{shape.tip, 0.0f, 0.0f};

            if (up <= above.up) {
                const float span = above.up - below.up;
                const float share = span > 0.0f ? (up - below.up) / span : 0.0f;
                left = below.left + (above.left - below.left) * share;
                right = below.right + (above.right - below.right) * share;
                return;
            }

            below = above;
        }
    }

    // How well a shape matches the covered texels, row by row: the overlap over the union of their widths (1 a match).
    float shapeMatch(const LeafShape& shape, const LeafProfile& profile) {
        const float size = static_cast<float>(profile.left.size());
        float overlap = 0.0f;
        float joined = 0.0f;

        for (std::size_t texelRow = 0; texelRow < profile.left.size(); ++texelRow) {
            float left = 0.0f;
            float right = 0.0f;
            shapeWidthsAt(shape, (static_cast<float>(texelRow) + 0.5f) / size, left, right);
            overlap += std::min(left, profile.left[texelRow]) + std::min(right, profile.right[texelRow]);
            joined += std::max(left, profile.left[texelRow]) + std::max(right, profile.right[texelRow]);
        }

        return joined > 0.0f ? overlap / joined : 0.0f;
    }

    // A row at a height: the profile's widths on its texel row, pulled in by the inset.
    LeafRow profileRow(const LeafProfile& profile, float up, float inset) {
        const std::size_t count = profile.left.size();
        const std::size_t texelRow = std::min(static_cast<std::size_t>(std::max(up, 0.0f) * static_cast<float>(count)), count - 1);
        return LeafRow{up, std::max(profile.left[texelRow] - inset, 0.0f), std::max(profile.right[texelRow] - inset, 0.0f)};
    }

    // Every choice of rowCount heights from the grid's inner steps (ascending), keeping the best matching shape.
    void chooseRows(const LeafProfile& profile, float inset, int rowCount, int firstStep, LeafShape& trial, LeafShape& best, float& bestMatch) {
        if (static_cast<int>(trial.rows.size()) == rowCount) {
            const float match = shapeMatch(trial, profile);

            if (match > bestMatch) {
                bestMatch = match;
                best = trial;
            }

            return;
        }

        const int remaining = rowCount - static_cast<int>(trial.rows.size());

        for (int step = firstStep; step <= leafRowSteps - remaining; ++step) {
            const float up = trial.base + (trial.tip - trial.base) * static_cast<float>(step) / static_cast<float>(leafRowSteps);
            trial.rows.push_back(profileRow(profile, up, inset));
            chooseRows(profile, inset, rowCount, step + 1, trial, best, bestMatch);
            trial.rows.pop_back();
        }
    }
}

LeafShape traceLeafShape(const ImageData& image, std::uint32_t column, std::uint32_t row, std::uint32_t cellTexels,
                         std::uint8_t alphaCutoff, int rowCount, float insetTexels) {
    const float size = static_cast<float>(cellTexels);
    LeafProfile profile;
    profile.left.assign(cellTexels, 0.0f);
    profile.right.assign(cellTexels, 0.0f);
    std::int64_t lowest = -1;
    std::int64_t highest = -1;

    for (std::uint32_t y = 0; y < cellTexels; ++y) {
        std::int64_t first = -1;
        std::int64_t last = -1;

        for (std::uint32_t x = 0; x < cellTexels; ++x) {
            const std::size_t texel = (static_cast<std::size_t>(row * cellTexels + y) * image.width + column * cellTexels + x) * 4 + 3;

            if (image.pixels[texel] >= alphaCutoff) {
                first = first < 0 ? x : first;
                last = x;
            }
        }

        if (first >= 0) {
            profile.left[y] = std::max(0.5f - static_cast<float>(first) / size, 0.0f);
            profile.right[y] = std::max(static_cast<float>(last + 1) / size - 0.5f, 0.0f);
            lowest = lowest < 0 ? y : lowest;
            highest = y;
        }
    }

    const float inset = insetTexels / size;
    LeafShape trial;
    trial.base = lowest < 0 ? 0.0f : static_cast<float>(lowest) / size + inset;
    trial.tip = lowest < 0 ? 0.0f : static_cast<float>(highest + 1) / size - inset;

    if (trial.tip <= trial.base || rowCount < 1) {
        return LeafShape{0.0f, 0.0f, {}};
    }

    LeafShape best = trial;
    float bestMatch = -1.0f;
    chooseRows(profile, inset, std::min(rowCount, leafRowSteps - 1), 1, trial, best, bestMatch);
    return best;
}

float leafShapeArea(const LeafShape& shape) {
    float area = 0.0f;
    LeafRow below{shape.base, 0.0f, 0.0f};

    for (std::size_t knot = 0; knot <= shape.rows.size() && !shape.rows.empty(); ++knot) {
        const LeafRow above = knot < shape.rows.size() ? shape.rows[knot] : LeafRow{shape.tip, 0.0f, 0.0f};
        area += (above.up - below.up) * (below.left + below.right + above.left + above.right) * 0.5f;
        below = above;
    }

    return area;
}

bool insideLeafShape(const LeafShape& shape, const Vec2& point) {
    float left = 0.0f;
    float right = 0.0f;
    shapeWidthsAt(shape, point.y, left, right);
    const float across = point.x - 0.5f;
    return !shape.rows.empty() && point.y > shape.base && point.y < shape.tip && across >= -left && across <= right;
}

/*
* The vertices: the base, each row's left edge, midrib and right edge, the tip. A point x across the midrib and y up the
* cell sits at foot + across x + towardsTip y + facing z (all times the side), where z raises each half by its distance
* from the midrib times tan(fold) and drops the tip by curl t^2 (t from base to tip). across = towardsTip x facing, so
* the triangles below (each checked counterclockwise with x right and y up) face the leaf's facing.
*/
void appendModelledLeaf(IndexedMesh& mesh, const LeafShape& shape, const ModelledLeafPlace& place) {
    if (shape.rows.empty()) {
        return;
    }

    const Vec3 across = place.towardsTip.cross(place.facing);
    const float raise = std::tan(place.foldRadians);
    const float length = std::max(shape.tip - shape.base, 1e-6f);
    const std::uint32_t first = static_cast<std::uint32_t>(mesh.vertices.size());

    const auto addPoint = [&](float x, float y) {
        const float t = std::clamp((y - shape.base) / length, 0.0f, 1.0f);
        const float z = std::abs(x) * raise - place.curl * t * t;
        const Vec3 position = place.foot + (across * x + place.towardsTip * y + place.facing * z) * place.side;
        const Vec2 uv{place.uvMin.x + (place.uvMax.x - place.uvMin.x) * (0.5f + x), place.uvMin.y + (place.uvMax.y - place.uvMin.y) * y};
        mesh.vertices.push_back(MeshVertex{position, Vec3{}, uv, Vec4{across.x, across.y, across.z, 1.0f}});
    };

    addPoint(0.0f, shape.base);

    for (const LeafRow& leafRow : shape.rows) {
        addPoint(-leafRow.left, leafRow.up);
        addPoint(0.0f, leafRow.up);
        addPoint(leafRow.right, leafRow.up);
    }

    addPoint(0.0f, shape.tip);

    const std::uint32_t rows = static_cast<std::uint32_t>(shape.rows.size());
    const std::uint32_t base = first;
    const std::uint32_t tip = first + 1 + 3 * rows;
    const auto left = [first](std::uint32_t r) { return first + 1 + 3 * r; };
    const auto middle = [first](std::uint32_t r) { return first + 2 + 3 * r; };
    const auto right = [first](std::uint32_t r) { return first + 3 + 3 * r; };
    const std::size_t firstIndex = mesh.indices.size();

    mesh.indices.insert(mesh.indices.end(), {base, middle(0), left(0), base, right(0), middle(0)});

    for (std::uint32_t r = 0; r + 1 < rows; ++r) {
        mesh.indices.insert(mesh.indices.end(), {left(r), middle(r), middle(r + 1), left(r), middle(r + 1), left(r + 1)});
        mesh.indices.insert(mesh.indices.end(), {middle(r), right(r), right(r + 1), middle(r), right(r + 1), middle(r + 1)});
    }

    mesh.indices.insert(mesh.indices.end(), {left(rows - 1), middle(rows - 1), tip, middle(rows - 1), right(rows - 1), tip});

    // Each vertex's normal: its triangles' facings summed (so weighted by area), then made unit.
    for (std::size_t index = firstIndex; index + 2 < mesh.indices.size(); index += 3) {
        MeshVertex& a = mesh.vertices[mesh.indices[index]];
        MeshVertex& b = mesh.vertices[mesh.indices[index + 1]];
        MeshVertex& c = mesh.vertices[mesh.indices[index + 2]];
        const Vec3 facing = (b.position - a.position).cross(c.position - a.position);
        a.normal = a.normal + facing;
        b.normal = b.normal + facing;
        c.normal = c.normal + facing;
    }

    for (std::size_t vertex = first; vertex < mesh.vertices.size(); ++vertex) {
        Vec3& normal = mesh.vertices[vertex].normal;
        normal = normal.lengthSquared() > 1e-20f ? normal.normalized() : place.facing;
    }
}
