#include "games/forest/twigs.h"

#include "games/forest/forest_ground.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/island_hangings.h"
#include "math/angles.h"
#include "scene/foliage_shading.h"
#include "scene/leaf_pruning.h"

#include <algorithm>
#include <cmath>

namespace {
    // The stem: rings along it, and corners round each (the first repeated at the seam, so the picture wraps).
    constexpr int stemRings = 5;
    constexpr int stemSides = 3;
    constexpr float stemTipShare = 1.0f / 3.0f;

    // The random numbers' slots in a twig's seed: its pruning offset, then two a leaf from leafSlots on.
    constexpr std::uint64_t pruningSlot = 999;
    constexpr std::uint64_t leafSlots = 1000;

    // A cell point (clusterLeaves' 0..1 units) in the twig's frame: the cell's bottom middle at the origin.
    Vec3 twigPoint(const Vec2& cell, float twigSize) {
        return Vec3{(cell.x - 0.5f) * twigSize, cell.y * twigSize, 0.0f};
    }

    /*
    * A thin tube up the twig's stem (clusterStemPoint): rings square to the stem, each corner's normal straight out from
    * it, the picture's u round it and v up it. Each quad is wound counterclockwise seen from outside.
    */
    void appendStem(IndexedMesh& mesh, float bend, float twigSize, float radius) {
        const Vec3 facing{0.0f, 0.0f, 1.0f};
        const std::uint32_t first = static_cast<std::uint32_t>(mesh.vertices.size());

        for (int ring = 0; ring < stemRings; ++ring) {
            const float t = static_cast<float>(ring) / static_cast<float>(stemRings - 1);
            const float step = 0.01f;
            const Vec3 centre = twigPoint(clusterStemPoint(t, bend), twigSize);
            const Vec3 ahead = twigPoint(clusterStemPoint(std::min(t + step, 1.0f), bend), twigSize) - twigPoint(clusterStemPoint(std::max(t - step, 0.0f), bend), twigSize);
            const Vec3 along = ahead.normalized();
            const Vec3 side = along.cross(facing).normalized();
            const float ringRadius = radius * (1.0f - (1.0f - stemTipShare) * t);

            for (int corner = 0; corner <= stemSides; ++corner) {
                const float angle = fullTurnRadians * static_cast<float>(corner) / static_cast<float>(stemSides);
                const Vec3 out = side * std::cos(angle) + facing * std::sin(angle);
                const Vec3 around = facing * std::cos(angle) - side * std::sin(angle);
                mesh.vertices.push_back(MeshVertex{centre + out * ringRadius, out, Vec2{static_cast<float>(corner) / stemSides, t},
                                                   Vec4{around.x, around.y, around.z, 1.0f}});
            }
        }

        const std::uint32_t perRing = stemSides + 1;

        for (std::uint32_t ring = 0; ring + 1 < static_cast<std::uint32_t>(stemRings); ++ring) {
            for (std::uint32_t corner = 0; corner < static_cast<std::uint32_t>(stemSides); ++corner) {
                const std::uint32_t a = first + ring * perRing + corner;
                const std::uint32_t b = a + 1;
                const std::uint32_t c = a + perRing;
                const std::uint32_t d = c + 1;
                mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
            }
        }
    }

    // A leaf's way to its tip and its facing: turned angle in the twig's plane, then pitched out of it by pitch and
    // rolled about its own length by roll (radians).
    void leafFrame(float angle, float pitch, float roll, Vec3& towardsTip, Vec3& facing) {
        const Vec3 along{-std::sin(angle), std::cos(angle), 0.0f};
        const Vec3 across{std::cos(angle), std::sin(angle), 0.0f};
        const Vec3 flat{0.0f, 0.0f, 1.0f};
        towardsTip = (along * std::cos(pitch) + flat * std::sin(pitch)).normalized();
        const Vec3 pitched = flat * std::cos(pitch) - along * std::sin(pitch);
        facing = (pitched * std::cos(roll) + across * std::sin(roll)).normalized();
    }
}

void visit(ForestTwigSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Prototypes", settings.prototypes, {.min = 1, .max = 64, .sliderMax = 16, .applies = FieldApplies::AtStartup});
    visitor.field("Leaf rows", settings.leafRows, {.min = 1, .max = 4, .applies = FieldApplies::AtStartup});
    visitor.field("Fold", settings.foldDegrees, {.min = 0.0f, .max = 45.0f, .unit = "degrees", .applies = FieldApplies::AtStartup});
    visitor.field("Curl", settings.curl, {.min = 0.0f, .max = 0.5f, .applies = FieldApplies::AtStartup});
    visitor.field("Tilt", settings.tiltDegrees, {.min = 0.0f, .max = 60.0f, .unit = "degrees", .applies = FieldApplies::AtStartup});
    visitor.field("Inset", settings.insetTexels, {.min = 0.0f, .max = 8.0f, .unit = "texels", .applies = FieldApplies::AtStartup});
    visitor.field("Stem radius", settings.stemRadius, {.min = 0.0f, .max = 0.05f, .sliderMax = 0.02f, .unit = "m", .applies = FieldApplies::AtStartup});
}

TracedLeaves traceAtlasLeaves(const LeafAtlas& atlas, int mostRows, float insetTexels) {
    TracedLeaves traced;
    traced.columns = atlas.columns;
    traced.rows = atlas.rows;
    const std::uint32_t cellTexels = atlas.colour.width / static_cast<std::uint32_t>(std::max(atlas.columns, 1));
    const std::uint8_t cutoff = static_cast<std::uint8_t>(std::lround(forestLeafCutoff * 255.0f));

    for (int rows = 1; rows <= std::max(mostRows, 1); ++rows) {
        std::vector<LeafShape>& shapes = traced.byRows.emplace_back();

        for (int cell = 0; cell < atlas.columns * atlas.rows; ++cell) {
            shapes.push_back(traceLeafShape(atlas.colour, static_cast<std::uint32_t>(cell % atlas.columns), static_cast<std::uint32_t>(cell / atlas.columns),
                                            cellTexels, cutoff, rows, insetTexels));
        }
    }

    return traced;
}

int twigLevelRows(int leafRows, int level) {
    const int rows = std::max(leafRows, 1);
    return level == 0 ? rows : level == 1 ? std::min(rows, 2) : 1;
}

/*
* Level 0's leaf area is the twig's: each leaf's traced area times its length squared. A coarser level's growth is the
* square root of that area over its kept leaves' area at its own rows, so it keeps the area exactly however the leaves
* differ in size and however the coarser rows trim them.
*/
TwigPrototype buildTwig(const TracedLeaves& leaves, const ForestTwigSettings& settings, float twigSize, float leafShare, std::uint32_t seed) {
    const int cells = leaves.columns * leaves.rows;
    const std::vector<ClusterLeaf> laid = clusterLeaves(cells, leafShare, seed);
    const std::uint32_t count = static_cast<std::uint32_t>(laid.size());
    const std::vector<std::uint32_t> order = prunedOrder(count, static_cast<std::uint32_t>(forestUnitRandom(seed, pruningSlot) * static_cast<float>(count)));
    const float toRadians = fullTurnRadians / 360.0f;
    const int mostRows = static_cast<int>(leaves.byRows.size());

    const auto shapeOf = [&](std::uint32_t leaf, int level) -> const LeafShape& {
        const int rows = std::min(twigLevelRows(settings.leafRows, level), mostRows);
        return leaves.byRows[static_cast<std::size_t>(rows - 1)][static_cast<std::size_t>(laid[leaf].sourceCell)];
    };

    const auto areaOf = [&](std::uint32_t leaf, int level) {
        return leafShapeArea(shapeOf(leaf, level)) * laid[leaf].length * laid[leaf].length;
    };

    float fullArea = 0.0f;

    for (std::uint32_t leaf = 0; leaf < count; ++leaf) {
        fullArea += areaOf(leaf, 0);
    }

    TwigPrototype twig;

    for (int level = 0; level < prunedLevels(count); ++level) {
        TwigLevel& made = twig.levels.emplace_back();
        made.kept.assign(order.begin(), order.begin() + prunedKept(count, level));
        std::sort(made.kept.begin(), made.kept.end());
        float keptArea = 0.0f;

        for (const std::uint32_t leaf : made.kept) {
            keptArea += areaOf(leaf, level);
        }

        made.growth = keptArea > 0.0f ? std::sqrt(fullArea / keptArea) : 1.0f;
        made.error = level == 0 ? 0.0f : twig.levels[static_cast<std::size_t>(level) - 1].error;

        for (const std::uint32_t leaf : made.kept) {
            const ClusterLeaf& placed = laid[leaf];
            const AtlasCell cell = atlasCell(placed.sourceCell, leaves.columns, leaves.rows);
            const float pitch = settings.tiltDegrees * toRadians * (2.0f * forestUnitRandom(seed, leafSlots + leaf * 2) - 1.0f);
            const float roll = settings.tiltDegrees * toRadians * (2.0f * forestUnitRandom(seed, leafSlots + leaf * 2 + 1) - 1.0f);
            ModelledLeafPlace place;
            leafFrame(placed.angle, pitch, roll, place.towardsTip, place.facing);
            place.foot = twigPoint(placed.base, twigSize);
            place.side = placed.length * twigSize * made.growth;
            place.foldRadians = settings.foldDegrees * toRadians;
            place.curl = settings.curl;
            // The leaf's number in the texture coordinate's whole part (scene/foliage_shading.h): the shaders tone it by it.
            place.uvMin = Vec2{numberedLeafU(cell.uvMin.x, leaf), cell.uvMin.y};
            place.uvMax = Vec2{numberedLeafU(cell.uvMax.x, leaf), cell.uvMax.y};
            appendModelledLeaf(made.leaves, shapeOf(leaf, level), place);
            made.error = std::max(made.error, placed.length * twigSize * (made.growth - 1.0f));
        }

        if (level == 0 && settings.stemRadius > 0.0f) {
            appendStem(made.stem, clusterStemBend(seed), twigSize, settings.stemRadius);
        }
    }

    return twig;
}

std::uint32_t twigPrototypeOf(float pick, std::size_t count) {
    if (count == 0) {
        return 0;
    }

    const auto chosen = static_cast<std::size_t>(std::clamp(pick, 0.0f, 1.0f) * static_cast<float>(count));
    return static_cast<std::uint32_t>(std::min(chosen, count - 1));
}

TwigSet treeTwigSet(const std::vector<PlacedTwig>& twigs, std::vector<MeshHandle> prototypes, const std::vector<float>& reaches) {
    std::vector<TwigInstance> instances;
    instances.reserve(twigs.size());

    for (const PlacedTwig& twig : twigs) {
        instances.push_back(TwigInstance{.foot = twig.foot,
                                         .across = twig.across,
                                         .towardsTip = twig.towardsTip,
                                         .facing = twig.facing,
                                         .scale = twig.scale,
                                         .prototype = twigPrototypeOf(twig.pick, prototypes.size()),
                                         .lean = twig.lean});
    }

    return makeTwigSet(std::move(prototypes), reaches, std::move(instances));
}

// Each level's vertices after the finer ones', its indices moved past them.
TwigLeafMesh twigLeafMesh(const TwigPrototype& twig) {
    TwigLeafMesh made;

    for (std::size_t level = 0; level < twig.levels.size(); ++level) {
        const IndexedMesh& leaves = twig.levels[level].leaves;
        const auto first = static_cast<std::uint32_t>(made.mesh.vertices.size());
        made.mesh.vertices.insert(made.mesh.vertices.end(), leaves.vertices.begin(), leaves.vertices.end());
        std::vector<std::uint32_t>& indices = level == 0 ? made.mesh.indices : made.coarser.emplace_back().indices;

        for (const std::uint32_t corner : leaves.indices) {
            indices.push_back(first + corner);
        }

        if (level > 0) {
            made.coarser.back().error = twig.levels[level].error;
        }
    }

    return made;
}

std::vector<TwigPrototype> buildTwigPrototypes(const TracedLeaves& leaves, const ForestTwigSettings& settings, float twigSize, float leafShare,
                                               std::uint32_t seed) {
    std::vector<TwigPrototype> twigs;

    for (int index = 0; index < std::max(settings.prototypes, 1); ++index) {
        twigs.push_back(buildTwig(leaves, settings, twigSize, leafShare, forestShapeSeed(seed, static_cast<std::uint32_t>(index) + 1)));
    }

    return twigs;
}
