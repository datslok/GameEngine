#include "render/gpu/twig_records.h"

#include "render/gpu/bone_records.h"
#include "render/gpu/column_major.h"
#include "render/gpu/mesh_cluster_culling.h"

#include <cmath>
#include <stdexcept>

std::uint32_t packTwigBinding(const TwigInstance& twig) {
    return twig.binding.bone < 0 ? swayUnbound : packSwayBinding(twig.binding);
}

GpuTwig packTwig(const TwigInstance& twig) {
    GpuTwig packed;
    const Vec3 columns[3] = {twig.across * twig.scale, twig.towardsTip * twig.scale, twig.facing * twig.scale};

    for (int column = 0; column < 3; ++column) {
        packed.rows[0][column] = columns[column].x;
        packed.rows[1][column] = columns[column].y;
        packed.rows[2][column] = columns[column].z;
    }

    packed.rows[0][3] = twig.foot.x;
    packed.rows[1][3] = twig.foot.y;
    packed.rows[2][3] = twig.foot.z;
    packed.lean[0] = twig.lean.x;
    packed.lean[1] = twig.lean.y;
    packed.lean[2] = twig.lean.z;
    packed.prototype = twig.prototype;
    return packed;
}

GpuTwigBrick packTwigBrick(const TwigBrick& brick) {
    GpuTwigBrick packed;
    packed.sphere[0] = brick.centre.x;
    packed.sphere[1] = brick.centre.y;
    packed.sphere[2] = brick.centre.z;
    packed.sphere[3] = brick.radius;
    packed.firstTwig = brick.firstTwig;
    packed.twigCount = brick.twigCount;
    return packed;
}

// Each group's clusters must run together, group by group: the chain's level k from its first cluster to the next's.
std::vector<GpuTwigLevel> packTwigLevels(const MeshClusters& clusters, std::uint32_t firstPoolCluster) {
    std::vector<GpuTwigLevel> levels(clusters.groups.size());
    std::uint32_t expected = 0;

    for (std::uint32_t number = 0; number < clusters.clusters.size(); ++number) {
        const MeshCluster& cluster = clusters.clusters[number];

        if (cluster.group != expected && cluster.group != expected + 1) {
            throw std::invalid_argument("A twig prototype's clusters are not a chain of levels");
        }

        if (cluster.group == expected + 1 || number == 0) {
            expected = cluster.group;
            levels[expected].firstCluster = firstPoolCluster + number;
        }

        if (clusters.groups[cluster.group].level != cluster.group) {
            throw std::invalid_argument("A twig prototype's clusters are not a chain of levels");
        }

        levels[expected].triangleCount += cluster.triangleCount;
        ++levels[expected].clusterCount;
    }

    for (std::size_t level = 0; level < levels.size(); ++level) {
        const MeshClusterGroup& group = clusters.groups[level];
        levels[level].sphere[0] = group.centre.x;
        levels[level].sphere[1] = group.centre.y;
        levels[level].sphere[2] = group.centre.z;
        levels[level].sphere[3] = group.radius;
        levels[level].error = group.error;

        if (levels[level].clusterCount == 0 || levels[level].clusterCount > twigMostLevelClusters) {
            throw std::invalid_argument("A twig prototype's level has no clusters or more than TWIG_MOST_LEVEL_CLUSTERS");
        }
    }

    if (levels.size() > twigMostLevels) {
        throw std::invalid_argument("A twig prototype has more than TWIG_MOST_LEVELS levels");
    }

    return levels;
}

// Level by level the records' runs (their groups' levels from 0 up, each run together), each its group's sphere and error.
std::vector<GpuTwigLevel> packTwigLevels(std::span<const GpuMeshCluster> records, std::uint32_t firstPoolCluster) {
    std::vector<GpuTwigLevel> levels;

    for (std::uint32_t number = 0; number < records.size(); ++number) {
        const GpuMeshCluster& cluster = records[number];

        if (cluster.level == levels.size()) {
            GpuTwigLevel level;
            std::copy(cluster.parentSphere, cluster.parentSphere + 4, level.sphere);
            level.error = cluster.parentError;
            level.firstCluster = firstPoolCluster + number;
            levels.push_back(level);
        } else if (levels.empty() || cluster.level + 1 != levels.size()) {
            throw std::invalid_argument("A twig prototype's clusters are not a chain of levels");
        }

        ++levels.back().clusterCount;
        levels.back().triangleCount += cluster.counts >> MESH_CLUSTER_TRIANGLE_SHIFT;
    }

    for (const GpuTwigLevel& level : levels) {
        if (level.clusterCount > twigMostLevelClusters) {
            throw std::invalid_argument("A twig prototype's level has more than TWIG_MOST_LEVEL_CLUSTERS clusters");
        }
    }

    if (levels.size() > twigMostLevels) {
        throw std::invalid_argument("A twig prototype has more than TWIG_MOST_LEVELS levels");
    }

    return levels;
}

namespace {
    // A twig's matrix: its rows as GpuTwig holds them, the last (0 0 0 1).
    Mat4 twigMatrix(const GpuTwig& twig) {
        Mat4 matrix = Mat4::identity();

        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                matrix.values[row][column] = twig.rows[row][column];
            }
        }

        return matrix;
    }

    // Whether a sphere (plant or prototype frame) placed by an instance is kept: always where the part is not rigid.
    bool twigSphereKept(const float sphere[4], const GpuInstance& placedBy, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record,
                        const GpuCameraCull& view) {
        if (!meshClusterRigid(part, entry, record, view)) {
            return true;
        }

        const std::array<float, 4> placed = meshClusterPlacedSphere(sphere, placedBy);
        return sphereInFrustum(view.frustum, placed.data());
    }
}

// The twig's scale is its across column's length (its frame is unit, its scale uniform).
GpuInstance twigInstance(const GpuInstance& part, const GpuTwig& twig) {
    const Mat4 matrix = twigMatrix(twig);
    Mat4 turn = matrix;
    turn.values[0][3] = 0.0f;
    turn.values[1][3] = 0.0f;
    turn.values[2][3] = 0.0f;
    GpuInstance instance = part;
    writeColumnMajor(readColumnMajor(part.model) * matrix, instance.model);
    writeColumnMajor(readColumnMajor(part.normalMatrix) * turn, instance.normalMatrix);
    const float scale = std::sqrt(twig.rows[0][0] * twig.rows[0][0] + twig.rows[1][0] * twig.rows[1][0] + twig.rows[2][0] * twig.rows[2][0]);
    instance.handedness[1] = part.handedness[1] * scale;
    return instance;
}

bool twigLevelDrawn(const GpuTwigLevel& before, bool finest, const GpuTwigLevel& level, const GpuInstance& twig, const GpuCameraCull& view) {
    const float budget = view.levels.pixelError;
    const bool ownFits = finest || meshClusterErrorPixels(before.sphere, before.error, twig, view) <= budget;
    const bool coarserFits = level.error <= static_cast<float>(MESH_CLUSTER_NEVER_COARSER) && meshClusterErrorPixels(level.sphere, level.error, twig, view) <= budget;
    return ownFits && !coarserFits;
}

std::uint32_t twigLevel(const GpuTwigLevel* levels, std::uint32_t levelCount, const GpuInstance& twig, const GpuCameraCull& view) {
    for (std::uint32_t level = 0; level < levelCount; ++level) {
        if (twigLevelDrawn(levels[level == 0 ? 0 : level - 1], level == 0, levels[level], twig, view)) {
            return level;
        }
    }

    return levelCount - 1;
}

bool twigBrickKept(const GpuTwigBrick& brick, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view) {
    return twigSphereKept(brick.sphere, part, part, entry, record, view);
}

bool twigKept(const GpuTwigLevel& coarsest, const GpuInstance& twig, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view) {
    return twigSphereKept(coarsest.sphere, twig, twig, entry, record, view);
}

TwigTally readTwigTally(std::span<const std::uint32_t, TWIG_TALLY_WORDS> words) {
    TwigTally tally;
    tally.tested = words[TWIG_TALLY_TESTED];
    tally.kept = words[TWIG_TALLY_KEPT];
    tally.swaying = words[TWIG_TALLY_SWAYING];

    for (std::uint32_t level = 0; level < twigMostLevels; ++level) {
        tally.levelTwigs[level] = words[TWIG_TALLY_LEVEL_TWIGS + level];
        tally.levelTriangles[level] = words[TWIG_TALLY_LEVEL_TRIANGLES + level];
    }

    return tally;
}

// The tally as twig_cull.comp counts: every twig of every brick tested, whether its brick is kept or not.
std::vector<TwigDraw> twigDrawsOnCpu(const TwigTables& tables, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record,
                                     const GpuCameraCull& view, TwigTally* tally) {
    std::vector<TwigDraw> draws;
    const bool swaying = !meshClusterRigid(part, entry, record, view);

    for (const GpuTwigBrick& brick : tables.bricks) {
        if (tally != nullptr) {
            tally->tested += brick.twigCount;
        }

        if (!twigBrickKept(brick, part, entry, record, view)) {
            continue;
        }

        for (std::uint32_t number = brick.firstTwig; number < brick.firstTwig + brick.twigCount; ++number) {
            const GpuTwig& twig = tables.twigs[number];
            const GpuTwigPrototype& prototype = tables.prototypes[twig.prototype];
            const GpuTwigLevel* levels = &tables.levels[prototype.firstLevel];
            const GpuInstance placed = twigInstance(part, twig);

            if (!twigKept(levels[prototype.levelCount - 1], placed, entry, record, view)) {
                continue;
            }

            const std::uint32_t level = twigLevel(levels, prototype.levelCount, placed, view);
            const GpuTwigLevel& chosen = levels[level];
            draws.push_back(TwigDraw{number, chosen.firstCluster, chosen.clusterCount});

            if (tally != nullptr) {
                ++tally->kept;
                tally->swaying += swaying ? 1 : 0;
                ++tally->levelTwigs[level];
                tally->levelTriangles[level] += chosen.triangleCount;
            }
        }
    }

    return draws;
}
