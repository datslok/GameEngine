#include "games/forest/forest_ground.h"
#include "games/forest/twigs.h"
#include "render/gpu/twig_records.h"
#include "scene/mesh.h"

#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
    // A twig's record takes its prototype's points where the twig puts them; its lean and prototype are kept.
    void testATwigsRecord() {
        const TwigInstance twig{.foot = Vec3{1.0f, 2.0f, 3.0f},
                                .across = Vec3{0.0f, 0.0f, -1.0f},
                                .towardsTip = Vec3{0.0f, 1.0f, 0.0f},
                                .facing = Vec3{1.0f, 0.0f, 0.0f},
                                .scale = 1.5f,
                                .prototype = 5,
                                .lean = Vec3{0.25f, -0.5f, 0.125f}};
        const GpuTwig packed = packTwig(twig);

        for (const Vec3 local : {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.3f, 0.9f, -0.2f}, Vec3{-1.0f, 2.0f, 0.5f}}) {
            const float in[4] = {local.x, local.y, local.z, 1.0f};
            float out[3] = {0.0f, 0.0f, 0.0f};

            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 4; ++column) {
                    out[row] += packed.rows[row][column] * in[column];
                }
            }

            const Vec3 expected = twigPoint(twig, local);
            assert(std::abs(out[0] - expected.x) < 1e-6f && std::abs(out[1] - expected.y) < 1e-6f && std::abs(out[2] - expected.z) < 1e-6f);
        }

        assert(packed.prototype == 5 && packed.lean[0] == 0.25f && packed.lean[1] == -0.5f && packed.lean[2] == 0.125f);

        const GpuTwigBrick brick = packTwigBrick(TwigBrick{Vec3{4.0f, 5.0f, 6.0f}, 2.5f, 128, 64});
        assert(brick.sphere[0] == 4.0f && brick.sphere[1] == 5.0f && brick.sphere[2] == 6.0f && brick.sphere[3] == 2.5f);
        assert(brick.firstTwig == 128 && brick.twigCount == 64);
    }

    /*
    * A twig prototype's levels (a real twig's, through its given cluster levels): one a level, each its group's sphere
    * and error, its clusters' run moved to where the pool holds them; clusters that are not a chain are refused.
    */
    void testAPrototypesLevels() {
        const LeafAtlas atlas = standInLeafAtlas(64, forestTreeLeafColumns, forestTreeLeafRows, forestLeafStandIn);
        const TwigPrototype twig = buildTwig(traceAtlasLeaves(atlas, 3, 1.0f), ForestTwigSettings{}, 1.0f, 0.22f, 7);
        const TwigLeafMesh leaves = twigLeafMesh(twig);
        const MeshClusters clusters = buildGivenClusterLevels(leaves.mesh, leaves.coarser);
        const std::vector<GpuTwigLevel> levels = packTwigLevels(clusters, 1000);
        assert(levels.size() == twig.levels.size());

        std::uint32_t next = 1000;

        for (std::size_t level = 0; level < levels.size(); ++level) {
            assert(levels[level].firstCluster == next && levels[level].clusterCount > 0);

            for (std::uint32_t cluster = levels[level].firstCluster; cluster < levels[level].firstCluster + levels[level].clusterCount; ++cluster) {
                assert(clusters.clusters[cluster - 1000].group == level);
            }

            next += levels[level].clusterCount;
            assert(levels[level].error == clusters.groups[level].error && levels[level].sphere[3] == clusters.groups[level].radius);
            assert(levels[level].sphere[0] == clusters.groups[level].centre.x);
        }

        assert(next == 1000 + clusters.clusters.size() && levels.back().error == FLT_MAX);

        // A sphere's simplified levels mix groups within a level: not a chain.
        const MeshClusters simplified = buildMeshClusters(buildIndexedMesh(Mesh::sphere(96, 48)), ClusterLevels::All);
        bool refused = false;

        try {
            packTwigLevels(simplified, 0);
        } catch (const std::invalid_argument&) {
            refused = true;
        }

        assert(refused);
    }
}

void testTwigRecords() {
    testATwigsRecord();
    testAPrototypesLevels();
}
