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
