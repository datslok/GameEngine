#include "games/forest/leaf_clusters.h"
#include "math/angles.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <set>
#include <vector>

namespace {
    // A texel of an image by column and row (row 0 at v 0).
    const std::uint8_t* texel(const ImageData& image, std::uint32_t x, std::uint32_t y) {
        return &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
    }

    Vec3 decodedNormal(const std::uint8_t* bytes) {
        return Vec3{bytes[0] / 255.0f * 2.0f - 1.0f, bytes[1] / 255.0f * 2.0f - 1.0f, bytes[2] / 255.0f * 2.0f - 1.0f};
    }

    // A tangent-space normal turned with its leaf: x towards y for a quarter turn, its length and lean out kept.
    void testTurningNormals() {
        const Vec3 normal{0.6f, 0.0f, 0.8f};
        const Vec3 quarter = turnTangentNormal(normal, 0.25f * fullTurnRadians);
        assert(near(quarter.x, 0.0f, 1e-6f) && near(quarter.y, 0.6f, 1e-6f) && quarter.z == 0.8f);
        const Vec3 same = turnTangentNormal(normal, 0.0f);
        assert(same.x == normal.x && same.y == normal.y && same.z == normal.z);
        assert(near(turnTangentNormal(Vec3{0.3f, -0.4f, 0.866f}, 1.1f).length(), Vec3{0.3f, -0.4f, 0.866f}.length(), 1e-5f));
    }

    // The stem rises from near the bottom edge's middle to near the top, bending a little.
    void testTheStem() {
        const Vec2 foot = clusterStemPoint(0.0f, 0.08f);
        const Vec2 tip = clusterStemPoint(1.0f, 0.08f);
        assert(near(foot.x, 0.5f, 1e-6f) && foot.y < 0.05f && tip.y > 0.85f && tip.y < 0.97f && near(tip.x, 0.58f, 1e-5f));

        for (float t = 0.0f; t < 1.0f; t += 0.05f) {
            assert(clusterStemPoint(t + 0.05f, 0.08f).y > clusterStemPoint(t, 0.08f).y);
        }
    }

    /*
    * A twig's leaves: 12-18 along the stem, alternately left and right, turned 35-70 degrees out (the last near straight
    * up), no longer than asked, every leaf's tip inside the cell, each showing one of the atlas's cells, toned a little;
    * the same seed the same leaves.
    */
    void testATwigsLeaves() {
        for (std::uint32_t seed = 1; seed <= 20; ++seed) {
            const std::vector<ClusterLeaf> leaves = clusterLeaves(9, 0.27f, seed);
            assert(leaves.size() >= 12 && leaves.size() <= 18);

            for (std::size_t i = 0; i < leaves.size(); ++i) {
                const ClusterLeaf& leaf = leaves[i];
                assert(leaf.base.y >= 0.08f && leaf.base.y <= 0.96f && leaf.base.x > 0.3f && leaf.base.x < 0.7f);
                assert(leaf.length > 0.0f && leaf.length <= 0.27f + 1e-6f);
                assert(leaf.sourceCell >= 0 && leaf.sourceCell < 9);
                assert(leaf.tone >= 0.85f && leaf.tone <= 1.1f && std::fabs(leaf.warmth) <= 0.06f);

                const Vec2 tip = leaf.base + Vec2{-std::sin(leaf.angle), std::cos(leaf.angle)} * leaf.length;
                assert(tip.x >= 0.0f && tip.x <= 1.0f && tip.y >= 0.0f && tip.y <= 0.98f + 1e-5f);

                if (i + 1 < leaves.size()) {
                    const float degrees = std::fabs(leaf.angle) * 360.0f / fullTurnRadians;
                    assert(degrees >= 35.0f - 1e-3f && degrees <= 70.0f + 1e-3f);

                    if (i > 0 && i + 2 < leaves.size()) {
                        assert((leaf.angle > 0.0f) != (leaves[i - 1].angle > 0.0f));
                    }
                } else {
                    assert(std::fabs(leaf.angle) <= 0.35f);
                }
            }
        }

        const std::vector<ClusterLeaf> again = clusterLeaves(9, 0.27f, 4u);
        assert(again[3].angle == clusterLeaves(9, 0.27f, 4u)[3].angle && again[3].angle != clusterLeaves(9, 0.27f, 5u)[3].angle);
    }

    // The stand-in atlas: a leaf in every cell (solid at its middle, clear at its corners), domed normals.
    void testTheStandIn() {
        const LeafAtlas atlas = standInLeafAtlas(32, 3, 3, Pixel{70, 105, 50});
        assert(atlas.colour.width == 96 && atlas.colour.height == 96 && atlas.columns == 3 && atlas.rows == 3);
        assert(atlas.normal.width == 96 && atlas.roughness.height == 96);
        assert(atlas.colour.encoding == TextureEncoding::Srgb && atlas.normal.encoding == TextureEncoding::Linear);

        for (std::uint32_t cell = 0; cell < 9; ++cell) {
            const std::uint32_t x0 = (cell % 3) * 32;
            const std::uint32_t y0 = (cell / 3) * 32;
            assert(texel(atlas.colour, x0 + 16, y0 + 16)[3] == 255);
            assert(texel(atlas.colour, x0, y0)[3] == 0 && texel(atlas.colour, x0 + 31, y0 + 31)[3] == 0);
            assert(decodedNormal(texel(atlas.normal, x0 + 22, y0 + 16)).x > 0.05f);
            assert(decodedNormal(texel(atlas.normal, x0 + 10, y0 + 16)).x < -0.05f);
        }
    }

    /*
    * The clusters' atlas: four cells of twigs, each covering a good share of its cell and clear at its bottom corners;
    * the texels nothing covers carry the leaves' mean colour; the covered texels' normals unit and facing out; the same
    * seed the same picture, another a different one.
    */
    void testComposedClusters() {
        const LeafAtlas source = standInLeafAtlas(32, 3, 3, Pixel{70, 105, 50});
        const LeafAtlas clusters = composeLeafClusters(source, 64, 0.27f, 7u);
        assert(clusters.colour.width == 128 && clusters.colour.height == 128 && clusters.columns == clusterColumns && clusters.rows == clusterRows);
        assert(clusters.normal.width == 128 && clusters.roughness.width == 128);
        assert(clusters.colour.encoding == TextureEncoding::Srgb && clusters.normal.encoding == TextureEncoding::Linear &&
               clusters.roughness.encoding == TextureEncoding::Linear);

        for (std::uint32_t cell = 0; cell < 4; ++cell) {
            const std::uint32_t x0 = (cell % 2) * 64;
            const std::uint32_t y0 = (cell / 2) * 64;
            int covered = 0;

            for (std::uint32_t y = y0; y < y0 + 64; ++y) {
                for (std::uint32_t x = x0; x < x0 + 64; ++x) {
                    const std::uint8_t* colour = texel(clusters.colour, x, y);

                    if (colour[3] >= 128) {
                        ++covered;
                        const Vec3 normal = decodedNormal(texel(clusters.normal, x, y));
                        assert(near(normal.length(), 1.0f, 0.05f) && normal.z > 0.0f);
                    }
                }
            }

            assert(covered > 64 * 64 / 6 && covered < 64 * 64 * 5 / 6);
            assert(texel(clusters.colour, x0, y0)[3] == 0 && texel(clusters.colour, x0 + 63, y0)[3] == 0);
        }

        const std::uint8_t* clear = texel(clusters.colour, 0, 0);
        assert(clear[1] > clear[0] && clear[1] > clear[2]);

        const LeafAtlas again = composeLeafClusters(source, 64, 0.27f, 7u);
        const LeafAtlas other = composeLeafClusters(source, 64, 0.27f, 8u);
        assert(again.colour.pixels == clusters.colour.pixels && other.colour.pixels != clusters.colour.pixels);
    }
}

void testLeafClusters() {
    testTurningNormals();
    testTheStem();
    testATwigsLeaves();
    testTheStandIn();
    testComposedClusters();
}
