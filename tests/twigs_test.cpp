#include "games/forest/forest_ground.h"
#include "games/forest/island_hangings.h"
#include "games/forest/leaf_clusters.h"
#include "games/forest/twigs.h"
#include "scene/leaf_pruning.h"

#include "forest_helpers.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
    constexpr int testCellTexels = 64;
    constexpr std::uint8_t testCutoff = 128;

    LeafAtlas testAtlas() {
        return standInLeafAtlas(testCellTexels, forestTreeLeafColumns, forestTreeLeafRows, forestLeafStandIn);
    }

    // A texel's alpha in a cell (row from the bottom).
    std::uint8_t& alphaAt(LeafAtlas& atlas, int cell, int x, int y) {
        const int column = cell % atlas.columns;
        const int row = cell / atlas.columns;
        const std::size_t texel = (static_cast<std::size_t>(row * testCellTexels + y) * atlas.colour.width + static_cast<std::size_t>(column * testCellTexels + x)) * 4 + 3;
        return atlas.colour.pixels[texel];
    }

    // How a shape covers a cell's leaf, texel centre by texel centre: the covered texels inside it, and the texels
    // inside it that are not covered, each as a share of the covered texels; and the overlap over the union.
    struct Coverage {
        float inside = 0.0f;
        float outside = 0.0f;
        float match = 0.0f;
        int leftHalf = 0;
        int rightHalf = 0;
    };

    Coverage coverage(LeafAtlas& atlas, int cell, const LeafShape& shape) {
        int covered = 0;
        int coveredInside = 0;
        int uncoveredInside = 0;
        Coverage result;

        for (int y = 0; y < testCellTexels; ++y) {
            for (int x = 0; x < testCellTexels; ++x) {
                const Vec2 point{(static_cast<float>(x) + 0.5f) / testCellTexels, (static_cast<float>(y) + 0.5f) / testCellTexels};
                const bool isCovered = alphaAt(atlas, cell, x, y) >= testCutoff;
                const bool isInside = insideLeafShape(shape, point);
                covered += isCovered ? 1 : 0;
                coveredInside += isCovered && isInside ? 1 : 0;
                uncoveredInside += !isCovered && isInside ? 1 : 0;

                if (isInside) {
                    (point.x < 0.5f ? result.leftHalf : result.rightHalf) += 1;
                }
            }
        }

        result.inside = static_cast<float>(coveredInside) / static_cast<float>(covered);
        result.outside = static_cast<float>(uncoveredInside) / static_cast<float>(covered);
        result.match = static_cast<float>(coveredInside) / static_cast<float>(covered + uncoveredInside);
        return result;
    }

    LeafShape traced(LeafAtlas& atlas, int cell, int rows, float inset = 0.0f) {
        return traceLeafShape(atlas.colour, static_cast<std::uint32_t>(cell % atlas.columns), static_cast<std::uint32_t>(cell / atlas.columns),
                              testCellTexels, testCutoff, rows, inset);
    }

    /*
    * A leaf's outline follows its alpha. Its corners lie on the leaf's edge, so on the stand-in's ellipse it is at best
    * a regular octagon in a stretched circle with three rows (2 sqrt 2 / pi, 90% of the area): it holds at least 85%
    * with almost nothing outside, and more rows fit better than one. The inset pulls it inside the alpha: nothing
    * outside, a little less inside. A bite out of one side leaves that side narrower; an empty cell, no shape.
    */
    void testTracingFollowsTheLeaf() {
        LeafAtlas atlas = testAtlas();
        const Coverage one = coverage(atlas, 0, traced(atlas, 0, 1));
        const Coverage three = coverage(atlas, 0, traced(atlas, 0, 3));
        assert(three.inside >= 0.85f && three.outside < 0.03f);
        assert(three.match > one.match + 0.1f);

        const Coverage inset = coverage(atlas, 0, traced(atlas, 0, 3, 1.0f));
        assert(inset.outside == 0.0f && inset.inside < three.inside && inset.inside > 0.75f);

        // The stand-in leaf is an ellipse: its two halves alike.
        assert(std::abs(three.leftHalf - three.rightHalf) <= three.leftHalf / 10);

        // A bite out of the right side of cell 1.
        for (int y = 0; y < testCellTexels; ++y) {
            for (int x = 0; x < testCellTexels; ++x) {
                const float across = (static_cast<float>(x) + 0.5f) / testCellTexels - 0.75f;
                const float up = (static_cast<float>(y) + 0.5f) / testCellTexels - 0.5f;

                if (across * across + up * up < 0.15f * 0.15f) {
                    alphaAt(atlas, 1, x, y) = 0;
                }
            }
        }

        const LeafShape bitten = traced(atlas, 1, 3);
        const Coverage bittenCoverage = coverage(atlas, 1, bitten);
        assert(bittenCoverage.inside >= 0.8f && bittenCoverage.outside < 0.05f);
        assert(bittenCoverage.rightHalf * 10 < bittenCoverage.leftHalf * 9);

        // Cell 2 emptied.
        for (int y = 0; y < testCellTexels; ++y) {
            for (int x = 0; x < testCellTexels; ++x) {
                alphaAt(atlas, 2, x, y) = 0;
            }
        }

        assert(traced(atlas, 2, 3).rows.empty());
    }

    float meshArea(const IndexedMesh& mesh) {
        float area = 0.0f;

        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const Vec3& a = mesh.vertices[mesh.indices[i]].position;
            const Vec3& b = mesh.vertices[mesh.indices[i + 1]].position;
            const Vec3& c = mesh.vertices[mesh.indices[i + 2]].position;
            area += 0.5f * (b - a).cross(c - a).length();
        }

        return area;
    }

    /*
    * A modelled leaf: 4 triangles a row, facing its facing, textured inside its cell; laid flat its area is the traced
    * shape's times its side squared, and folded it stays facing the same way.
    */
    void testAModelledLeaf() {
        LeafAtlas atlas = testAtlas();

        for (int rows = 1; rows <= 4; ++rows) {
            const LeafShape shape = traced(atlas, 4, rows);
            const AtlasCell cell = atlasCell(4, atlas.columns, atlas.rows);
            ModelledLeafPlace place{.foot = Vec3{1.0f, 2.0f, 3.0f}, .towardsTip = Vec3{0.0f, 1.0f, 0.0f}, .facing = Vec3{0.0f, 0.0f, 1.0f},
                                    .side = 0.5f, .uvMin = cell.uvMin, .uvMax = cell.uvMax};
            IndexedMesh flat;
            appendModelledLeaf(flat, shape, place);
            assert(static_cast<int>(flat.indices.size()) == 3 * 4 * rows && static_cast<int>(flat.vertices.size()) == 3 * rows + 2);
            assert(std::abs(meshArea(flat) - leafShapeArea(shape) * 0.25f) < 1e-4f * meshArea(flat) + 1e-7f);
            assert(trianglesFaceTheirNormals(flat));

            for (const MeshVertex& vertex : flat.vertices) {
                assert(std::abs(vertex.position.z - 3.0f) < 1e-6f);
                assert(vertex.uv.x >= cell.uvMin.x && vertex.uv.x <= cell.uvMax.x && vertex.uv.y >= cell.uvMin.y && vertex.uv.y <= cell.uvMax.y);
            }

            place.foldRadians = 0.25f;
            place.curl = 0.08f;
            IndexedMesh folded;
            appendModelledLeaf(folded, shape, place);
            assert(trianglesFaceTheirNormals(folded));

            for (const MeshVertex& vertex : folded.vertices) {
                assert(vertex.normal.dot(place.facing) > 0.5f);
            }
        }
    }

    /*
    * A twig's levels: the finest keeps every leaf (4 triangles a row each) and the stem; each coarser keeps a subset of
    * the finer's, about half, drawn with fewer rows, down to one leaf; the first halving never loses two neighbours; the
    * leaf area stays the finest's; the error starts at 0 and grows; all of it faces its normals.
    */
    void testTwigLevels() {
        const TracedLeaves leaves = traceAtlasLeaves(testAtlas(), 3, 1.0f);
        ForestTwigSettings settings;
        const TwigPrototype twig = buildTwig(leaves, settings, 1.0f, 0.22f, 7);
        const std::uint32_t count = static_cast<std::uint32_t>(twig.levels.front().kept.size());
        assert(count >= 12 && count <= 18);
        assert(static_cast<int>(twig.levels.size()) == prunedLevels(count) && twig.levels.back().kept.size() == 1);
        assert(!twig.levels.front().stem.indices.empty() && trianglesFaceTheirNormals(twig.levels.front().stem));
        assert(twig.levels.front().error == 0.0f && twig.levels.front().growth == 1.0f);

        for (std::size_t level = 0; level < twig.levels.size(); ++level) {
            const TwigLevel& made = twig.levels[level];
            const std::size_t rows = static_cast<std::size_t>(twigLevelRows(settings.leafRows, static_cast<int>(level)));
            assert(made.kept.size() == prunedKept(count, static_cast<int>(level)));
            assert(made.leaves.indices.size() == made.kept.size() * 4 * rows * 3);
            assert(trianglesFaceTheirNormals(made.leaves));

            if (level > 0) {
                const TwigLevel& finer = twig.levels[level - 1];
                assert(made.stem.indices.empty());
                assert(made.error > finer.error && made.growth > finer.growth);

                for (const std::uint32_t leaf : made.kept) {
                    assert(std::find(finer.kept.begin(), finer.kept.end(), leaf) != finer.kept.end());
                }
            }
        }

        const std::vector<std::uint32_t>& half = twig.levels[1].kept;

        for (std::uint32_t leaf = 0; leaf + 1 < count; ++leaf) {
            assert(std::find(half.begin(), half.end(), leaf) != half.end() || std::find(half.begin(), half.end(), leaf + 1) != half.end());
        }

        // Laid flat (tilts only turn leaves, keeping their area), every level holds the finest's leaf area.
        settings.foldDegrees = 0.0f;
        settings.curl = 0.0f;
        const TwigPrototype flat = buildTwig(leaves, settings, 1.0f, 0.22f, 7);
        const float fullArea = meshArea(flat.levels.front().leaves);

        for (const TwigLevel& made : flat.levels) {
            assert(std::abs(meshArea(made.leaves) - fullArea) < 1e-3f * fullArea);
        }
    }

    // The same seed grows the same twig; the prototypes differ from each other; the settings name seven fields, read at start-up.
    void testTwigsAreRepeatableAndVaried() {
        const TracedLeaves leaves = traceAtlasLeaves(testAtlas(), 3, 1.0f);
        const ForestTwigSettings settings;
        const TwigPrototype first = buildTwig(leaves, settings, 1.0f, 0.22f, 11);
        const TwigPrototype again = buildTwig(leaves, settings, 1.0f, 0.22f, 11);
        assert(first.levels.size() == again.levels.size());

        for (std::size_t level = 0; level < first.levels.size(); ++level) {
            assert(first.levels[level].leaves.vertices.size() == again.levels[level].leaves.vertices.size());

            for (std::size_t vertex = 0; vertex < first.levels[level].leaves.vertices.size(); ++vertex) {
                assert((first.levels[level].leaves.vertices[vertex].position - again.levels[level].leaves.vertices[vertex].position).lengthSquared() == 0.0f);
            }
        }

        const std::vector<TwigPrototype> prototypes = buildTwigPrototypes(leaves, settings, 1.0f, 0.22f, 3);
        assert(static_cast<int>(prototypes.size()) == settings.prototypes);

        for (std::size_t index = 1; index < prototypes.size(); ++index) {
            const std::vector<MeshVertex>& a = prototypes[index - 1].levels.front().leaves.vertices;
            const std::vector<MeshVertex>& b = prototypes[index].levels.front().leaves.vertices;
            assert(a.size() != b.size() || (a.front().position - b.front().position).lengthSquared() > 0.0f);
        }

        ForestTwigSettings counted;
        FieldCounter counter;
        visit(counted, counter);
        assert(counter.fields == 7 && counter.live == 0);
    }
}

void testTwigs() {
    testTracingFollowsTheLeaf();
    testAModelledLeaf();
    testTwigLevels();
    testTwigsAreRepeatableAndVaried();
}
