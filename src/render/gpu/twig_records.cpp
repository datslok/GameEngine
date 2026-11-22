#include "render/gpu/twig_records.h"

#include <stdexcept>

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

        ++levels[expected].clusterCount;
    }

    for (std::size_t level = 0; level < levels.size(); ++level) {
        const MeshClusterGroup& group = clusters.groups[level];
        levels[level].sphere[0] = group.centre.x;
        levels[level].sphere[1] = group.centre.y;
        levels[level].sphere[2] = group.centre.z;
        levels[level].sphere[3] = group.radius;
        levels[level].error = group.error;

        if (levels[level].clusterCount == 0) {
            throw std::invalid_argument("A twig prototype's level has no clusters");
        }
    }

    return levels;
}
