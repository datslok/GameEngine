#include "cull_helpers.h"
#include "engine/debug_views.h"
#include "games/forest/forest_ground.h"
#include "games/forest/twigs.h"
#include "math/transform.h"
#include "render/gpu/bone_poses.h"
#include "render/gpu/column_major.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "render/gpu/twig_records.h"
#include "render/gpu/wind.h"
#include "render/gpu/wind_bones.h"
#include "scene/mesh.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <numbers>
#include <random>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
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

            // Its triangles: its clusters' together (F4's TWIGS line counts them).
            std::uint32_t triangles = 0;

            for (std::uint32_t cluster = levels[level].firstCluster; cluster < levels[level].firstCluster + levels[level].clusterCount; ++cluster) {
                triangles += clusters.clusters[cluster - 1000].triangleCount;
            }

            assert(triangles > 0 && levels[level].triangleCount == triangles);
        }

        assert(next == 1000 + clusters.clusters.size() && levels.back().error == FLT_MAX);

        // The pool's records give the same levels (the renderer's way to them).
        const PackedMeshClusters packed = packMeshClusters(clusters, 0, 0, 0);
        const std::vector<GpuTwigLevel> fromRecords = packTwigLevels(std::span<const GpuMeshCluster>{packed.clusters}, 1000);
        assert(fromRecords.size() == levels.size());

        for (std::size_t level = 0; level < levels.size(); ++level) {
            assert(fromRecords[level].firstCluster == levels[level].firstCluster && fromRecords[level].clusterCount == levels[level].clusterCount);
            assert(fromRecords[level].error == levels[level].error && fromRecords[level].sphere[3] == levels[level].sphere[3]);
            assert(fromRecords[level].triangleCount == levels[level].triangleCount);
        }

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

namespace {
    Vec3 worldPointOf(const GpuInstance& instance, const Vec3& point) {
        const Mat4 model = readColumnMajor(instance.model);
        return transformPoint(model, point);
    }

    // A twig at a place in a plant, turned and scaled.
    TwigInstance testTwig(const Vec3& foot, float scale) {
        return TwigInstance{.foot = foot,
                            .across = Vec3{0.0f, 0.0f, -1.0f},
                            .towardsTip = Vec3{0.0f, 1.0f, 0.0f},
                            .facing = Vec3{1.0f, 0.0f, 0.0f},
                            .scale = scale,
                            .prototype = 0,
                            .lean = Vec3{}};
    }

    // A real twig prototype's clusters, packed as the pool holds them, and its levels.
    struct TestPrototype {
        IndexedMesh mesh;
        MeshClusters clusters;
        PackedMeshClusters packed;
        std::vector<GpuTwigLevel> levels;
    };

    TestPrototype testPrototype() {
        const LeafAtlas atlas = standInLeafAtlas(64, forestTreeLeafColumns, forestTreeLeafRows, forestLeafStandIn);
        const TwigPrototype twig = buildTwig(traceAtlasLeaves(atlas, 3, 1.0f), ForestTwigSettings{}, 1.0f, 0.22f, 7);
        const TwigLeafMesh leaves = twigLeafMesh(twig);
        TestPrototype made;
        made.mesh = leaves.mesh;
        made.clusters = buildGivenClusterLevels(leaves.mesh, leaves.coarser);
        made.packed = packMeshClusters(made.clusters, 0, 0, 0);
        made.levels = packTwigLevels(made.clusters, 0);
        return made;
    }

    /*
    * A twig's instance takes its prototype's points where the part's matrix takes the twig's points in the plant; its
    * normals turned by the twig after the part's; its scale times the part's.
    */
    void testATwigsInstance() {
        const Mat4 partModel = Transform{.position = Vec3{5.0f, 1.0f, -2.0f}, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.7f),
                                         .scale = Vec3{2.0f, 2.0f, 2.0f}}.getMatrix();
        GpuInstance part = placedInstance(partModel);
        writeColumnMajor(normalMatrix(partModel), part.normalMatrix);
        part.handedness[1] = 2.0f;
        const TwigInstance twig = testTwig(Vec3{0.5f, 3.0f, 0.25f}, 1.5f);
        const GpuInstance placed = twigInstance(part, packTwig(twig));

        for (const Vec3 local : {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.2f, 0.7f, 0.05f}}) {
            assert((worldPointOf(placed, local) - transformPoint(partModel, twigPoint(twig, local))).length() < 1e-4f);
        }

        const Vec3 normal = transformDirection(readColumnMajor(placed.normalMatrix), Vec3{0.0f, 0.0f, 1.0f}).normalized();
        const Vec3 expected = transformDirection(normalMatrix(partModel), twig.facing).normalized();
        assert((normal - expected).length() < 1e-4f);
        assert(std::abs(placed.handedness[1] - 3.0f) < 1e-5f);
    }

    /*
    * The level a twig draws is the cut's on its prototype's clusters: from near to far, exactly the chosen level's
    * clusters are in the cut, the levels never go finer with distance, and they reach the coarsest.
    */
    void testTheLevelIsTheCuts() {
        const TestPrototype prototype = testPrototype();
        const GpuInstance part = placedInstance(Mat4::identity());
        const GpuTwig twig = packTwig(testTwig(Vec3{0.0f, 10.0f, 0.0f}, 1.0f));
        const GpuInstance placed = twigInstance(part, twig);
        std::uint32_t last = 0;

        for (float distance = 0.5f; distance < 3000.0f; distance *= 1.15f) {
            const GpuCameraCull view = cutView(Vec3{-distance, 10.0f, 0.0f}, 0.0f, 1.0f);
            const std::uint32_t level = twigLevel(prototype.levels.data(), static_cast<std::uint32_t>(prototype.levels.size()), placed, view);
            assert(level >= last);
            last = level;
            const GpuTwigLevel& chosen = prototype.levels[level];

            for (std::uint32_t cluster = 0; cluster < prototype.packed.clusters.size(); ++cluster) {
                const bool inChosen = cluster >= chosen.firstCluster && cluster < chosen.firstCluster + chosen.clusterCount;
                assert(meshClusterInCut(prototype.packed.clusters[cluster], placed, view) == inChosen);
            }
        }

        assert(last == prototype.levels.size() - 1);
    }

    /*
    * The twig pass on the CPU: for a still part, every drawn twig's sphere is in the frustum and every twig left out has
    * its own or its brick's out of it (a view facing half the crown leaves some of each); each drawn at its level's
    * clusters. A part swaying where the view's sway reaches keeps every twig.
    */
    void testTheTwigPassOnTheCpu() {
        const TestPrototype prototype = testPrototype();
        std::mt19937 random(9u);
        std::uniform_real_distribution<float> place(-8.0f, 8.0f);
        std::vector<TwigInstance> twigs;

        for (int index = 0; index < 2000; ++index) {
            twigs.push_back(testTwig(Vec3{place(random), 15.0f + place(random) * 0.4f, place(random)}, 1.0f));
        }

        const TwigSet set = makeTwigSet({MeshHandle{0}}, {1.5f}, twigs);
        TwigTables tables;
        tables.levels = prototype.levels;
        tables.prototypes.push_back(GpuTwigPrototype{0, static_cast<std::uint32_t>(prototype.levels.size())});

        for (const TwigBrick& brick : set.bricks) {
            tables.bricks.push_back(packTwigBrick(brick));
        }

        for (const TwigInstance& twig : set.twigs) {
            tables.twigs.push_back(packTwig(twig));
        }

        const GpuInstance part = placedInstance(Mat4::identity());
        const GpuCullRecord record = stillRecord(Vec3{0.0f, 15.0f, 0.0f}, 14.0f);
        const GpuCameraCull view = cutView(Vec3{0.0f, 15.0f, 0.0f}, 0.0f, 1.0f);
        TwigTally tally;
        const std::vector<TwigDraw> draws = twigDrawsOnCpu(tables, part, 0u, record, view, &tally);
        assert(!draws.empty() && draws.size() < set.twigs.size() / 2 + set.twigs.size() / 4);

        // The tally: every twig tested, the drawn kept, none for sway; each level's twigs and their triangles.
        assert(tally.tested == set.twigs.size() && tally.kept == draws.size() && tally.swaying == 0);
        std::uint32_t levelTwigs = 0;

        for (std::size_t level = 0; level < tables.levels.size(); ++level) {
            levelTwigs += tally.levelTwigs[level];
            assert(tally.levelTriangles[level] == tally.levelTwigs[level] * tables.levels[level].triangleCount);
        }

        assert(levelTwigs == tally.kept);

        std::vector<bool> drawn(set.twigs.size(), false);
        std::size_t outBricks = 0;

        for (const TwigDraw& draw : draws) {
            drawn[draw.twig] = true;
            const GpuInstance placed = twigInstance(part, tables.twigs[draw.twig]);
            assert(sphereInFrustum(view.frustum, meshClusterPlacedSphere(tables.levels.back().sphere, placed).data()));
            const GpuTwigLevel& chosen = tables.levels[twigLevel(tables.levels.data(), static_cast<std::uint32_t>(tables.levels.size()), placed, view)];
            assert(draw.firstCluster == chosen.firstCluster && draw.clusterCount == chosen.clusterCount);
        }

        for (const GpuTwigBrick& brick : tables.bricks) {
            const bool brickIn = sphereInFrustum(view.frustum, meshClusterPlacedSphere(brick.sphere, part).data());
            outBricks += brickIn ? 0 : 1;

            for (std::uint32_t twig = brick.firstTwig; twig < brick.firstTwig + brick.twigCount; ++twig) {
                if (!drawn[twig]) {
                    const GpuInstance placed = twigInstance(part, tables.twigs[twig]);
                    assert(!brickIn || !sphereInFrustum(view.frustum, meshClusterPlacedSphere(tables.levels.back().sphere, placed).data()));
                }
            }
        }

        assert(outBricks > 0);

        GpuCullRecord swaying = record;
        swaying.flags |= CULL_SWAYS;
        const GpuCameraCull swayView = [&] {
            GpuCameraCull made = testView(Vec3{0.0f, 15.0f, 0.0f}, 0.0f, true, 1000.0f);
            made.levels = view.levels;
            return made;
        }();
        TwigTally swayTally;
        assert(twigDrawsOnCpu(tables, part, 0u, swaying, swayView, &swayTally).size() == set.twigs.size());
        assert(swayTally.tested == set.twigs.size() && swayTally.kept == swayTally.tested && swayTally.swaying == swayTally.kept);

        // Tallies add up over parts, as the pass's atomics do; the words the pass writes read back as the tally.
        TwigTally both = tally;
        twigDrawsOnCpu(tables, part, 0u, swaying, swayView, &both);
        assert(both.tested == 2 * set.twigs.size() && both.kept == tally.kept + swayTally.kept && both.swaying == swayTally.swaying);
        std::array<std::uint32_t, TWIG_TALLY_WORDS> words{};
        words[TWIG_TALLY_TESTED] = 7;
        words[TWIG_TALLY_KEPT] = 5;
        words[TWIG_TALLY_SWAYING] = 2;
        words[TWIG_TALLY_LEVEL_TWIGS + 1] = 4;
        words[TWIG_TALLY_LEVEL_TRIANGLES + TWIG_MOST_LEVELS - 1] = 9;
        const TwigTally read = readTwigTally(words);
        assert(read.tested == 7 && read.kept == 5 && read.swaying == 2 && read.levelTwigs[1] == 4 && read.levelTwigs[0] == 0);
        assert(read.levelTriangles[TWIG_MOST_LEVELS - 1] == 9 && read.levelTriangles[0] == 0);
    }

    // A point carried by a twig's matrix (its rows) into the plant's frame.
    Vec3 twigRowsPoint(const GpuTwig& twig, const Vec3& local) {
        const float in[4] = {local.x, local.y, local.z, 1.0f};
        float out[3] = {0.0f, 0.0f, 0.0f};

        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                out[row] += twig.rows[row][column] * in[column];
            }
        }

        return Vec3{out[0], out[1], out[2]};
    }

    // A point in the plant's frame placed by an instance into the world.
    Vec3 placedPoint(const GpuInstance& instance, const Vec3& point) {
        const float* m = instance.model;
        return Vec3{m[0] * point.x + m[4] * point.y + m[8] * point.z + m[12], m[1] * point.x + m[5] * point.y + m[9] * point.z + m[13],
                    m[2] * point.x + m[6] * point.y + m[10] * point.z + m[14]};
    }

    /*
    * A posed tree's twigs (ROADMAP phase 21 stage 3c-iv-b): each bound to one of four bones up its trunk, carried whole
    * by its rigid pose. The posed twig's matrix takes each point where the pose carries the twig's. Every twig the pass
    * leaves out has every point of its prototype, carried as drawn and fluttering as far as it can, outside one of the
    * view's planes; each drawn has its posed sphere in view; the posed tree keeps far fewer than the same part swaying
    * unbound (kept whole), and its kept twigs count as moving. A stamp not this frame's leaves the tree standing still:
    * its twigs culled as a still part's.
    */
    void testAPosedTreesTwigs() {
        const TestPrototype prototype = testPrototype();
        std::mt19937 random(13u);
        std::uniform_real_distribution<float> place(-8.0f, 8.0f);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<TwigInstance> twigs;

        for (int index = 0; index < 2000; ++index) {
            twigs.push_back(testTwig(Vec3{place(random), 15.0f + place(random) * 0.4f, place(random)}, 1.0f));
        }

        const TwigSet set = makeTwigSet({MeshHandle{0}}, {1.5f}, twigs);
        TwigTables tables;
        tables.levels = prototype.levels;
        tables.prototypes.push_back(GpuTwigPrototype{0, static_cast<std::uint32_t>(prototype.levels.size())});

        for (const TwigBrick& brick : set.bricks) {
            tables.bricks.push_back(packTwigBrick(brick));
        }

        std::vector<std::uint32_t> bindings;

        for (std::size_t twig = 0; twig < set.twigs.size(); ++twig) {
            tables.twigs.push_back(packTwig(set.twigs[twig]));
            bindings.push_back(packSwayBinding(SwayBinding{.bone = static_cast<int>(twig % 4), .weight = unit(random)}));
        }

        // Four bones up the trunk, each turned up to 0.25 radians about its pivot and carried by the one below.
        std::vector<WindBonePose> poses;
        std::vector<GpuBonePose> pool;

        for (int bone = 0; bone < 4; ++bone) {
            const WindBonePose below = bone == 0 ? WindBonePose{} : poses.back();
            const Vec3 turn = Vec3{unit(random) - 0.5f, unit(random) - 0.5f, unit(random) - 0.5f} * 0.5f;
            poses.push_back(windBonePose(below, turn, Vec3{0.0f, 4.0f * static_cast<float>(bone), 0.0f}));
            pool.push_back(packBonePose(poses.back(), bone - 1));
            pool.push_back(packBonePose(poses.back(), bone - 1));
        }

        // The tree's move: the farthest a twig's sphere goes (a rigid pose keeps its radius).
        const float* coarsest = tables.levels.back().sphere;
        std::vector<WindBonePose> rigid;
        GpuBoneTree tree;
        tree.firstPose = 0;
        tree.lean[0] = 0.2f;
        tree.lean[1] = -0.1f;

        for (std::size_t twig = 0; twig < tables.twigs.size(); ++twig) {
            const BoneVertexPoses both = boneVertexPoses(pool, 0, bindings[twig], false);
            const GpuTwig& record = tables.twigs[twig];
            rigid.push_back(windRigidPose(both.own, both.parent, both.weight, Vec3{record.rows[0][3], record.rows[1][3], record.rows[2][3]}));
            const Vec3 centre = twigRowsPoint(record, Vec3{coarsest[0], coarsest[1], coarsest[2]});
            tree.move = std::max(tree.move, (windBonePlace(rigid.back(), centre) - centre).length());
        }

        const std::vector<std::uint32_t> plantTrees{5u, 0u};
        const std::vector<GpuBoneTree> trees{tree};
        const BoneCullTables bones{plantTrees, trees, pool, bindings, 5u};

        GpuInstance part = placedInstance(Transform{.position = Vec3{2.0f, 0.0f, -1.0f}, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.6f),
                                                    .scale = Vec3{1.3f, 1.3f, 1.3f}}.getMatrix());
        part.sway[3] = 0.3f;
        const float reach = windFlutterReach(part.sway[3], Vec2{tree.lean[0], tree.lean[1]});
        const GpuCullRecord still = stillRecord(Vec3{2.0f, 20.0f, -1.0f}, 20.0f);
        GpuCullRecord swaying = still;
        swaying.flags |= CULL_SWAYS;
        // The posed twig's matrix carries points as its pose does.
        for (std::size_t twig = 0; twig < 20; ++twig) {
            const GpuTwig posed = twigPosed(tables.twigs[twig], rigid[twig]);
            const Vec3 local{0.1f, 0.8f, -0.05f};
            const Vec3 expected = windBonePlace(rigid[twig], twigRowsPoint(tables.twigs[twig], local));
            assert((twigRowsPoint(posed, local) - expected).length() < 1e-4f);
        }

        // Views from inside the crown and from round it, looking past its edge, so planes cut through bricks and twigs.
        std::size_t kept = 0;
        std::size_t tested = 0;
        GpuCameraCull view{};

        for (int round = 0; round < 16; ++round) {
            const float around = 0.4f * static_cast<float>(round);
            const float distance = round == 0 ? 0.0f : 6.0f + 2.0f * static_cast<float>(round % 5);
            const Vec3 eye{2.0f + distance * std::cos(around), 20.0f + place(random) * 0.5f, -1.0f + distance * std::sin(around)};
            const float yaw = around + std::numbers::pi_v<float> + (unit(random) - 0.5f) * 1.6f;
            view = testView(eye, yaw, true, 1000.0f);
            view.levels = cutView(eye, yaw, 1.0f).levels;

            TwigTally tally;
            const std::vector<TwigDraw> draws = twigDrawsOnCpu(tables, part, 0u, swaying, view, &tally, &bones, true);
            assert(twigDrawsOnCpu(tables, part, 0u, swaying, view).size() == set.twigs.size());
            assert(tally.kept == draws.size() && tally.swaying == tally.kept);
            kept += draws.size();
            tested += set.twigs.size();
            std::vector<bool> drawn(set.twigs.size(), false);

            for (const TwigDraw& draw : draws) {
                drawn[draw.twig] = true;
                const MeshClusterPose pose = twigPose(tables.levels.back(), part, tables.twigs[draw.twig], bindings[draw.twig], pool, tree);
                assert(pose.posed && sphereInFrustum(view.frustum, pose.sphere.data()));
            }

            for (std::size_t twig = 0; twig < set.twigs.size(); ++twig) {
                if (drawn[twig]) {
                    continue;
                }

                bool outside = false;

                for (const auto& plane : view.frustum.planes) {
                    bool allOut = true;

                    for (const MeshVertex& vertex : prototype.mesh.vertices) {
                        const Vec3 world = placedPoint(part, windBonePlace(rigid[twig], twigRowsPoint(tables.twigs[twig], vertex.position)));
                        allOut = allOut && plane[0] * world.x + plane[1] * world.y + plane[2] * world.z + plane[3] + reach < 1e-4f;
                    }

                    outside = outside || allOut;
                }

                assert(outside);
            }
        }

        assert(kept > 0 && kept < tested * 3 / 4);

        // Not this frame's stamp: nothing posed, the bound twigs still, culled as a still part's are.
        const BoneCullTables stale{plantTrees, trees, pool, bindings, 6u};
        assert(twigDrawsOnCpu(tables, part, 0u, swaying, view, nullptr, &stale, true) == twigDrawsOnCpu(tables, part, 0u, still, view));
    }

    // F4's TWIGS line: none without twigs tested; kept of tested, those kept for sway, then each level's twigs and
    // thousands of triangles up to the coarsest level drawn.
    void testTheTwigsLine() {
        RenderStats stats;

        for (const std::string& line : renderStatsLines(stats)) {
            assert(line.find("TWIGS") == std::string::npos);
        }

        stats.twigsTested = 1000;
        stats.twigsKept = 300;
        stats.twigsSwaying = 50;
        stats.twigsByLevel = {200, 60, 40};
        stats.twigTrianglesByLevel = {36000, 4800, 1200};
        const std::vector<std::string> lines = renderStatsLines(stats);
        assert(std::find(lines.begin(), lines.end(), "TWIGS 300/1000  SWAY 50  LEVELS 200 60 40  TRIS 36K 4K 1K") != lines.end());
    }
}

namespace {
    /*
    * The cluster passes' twig regions (on the CPU): the drawing order puts them in their style's run; a region's pairs
    * are its entries (at most its room) times its clusters, each item its entry's place and its level's first cluster
    * plus its offset; a twig entry draws its part placed by its plant, then its twig; the bound holds every region full.
    */
    void testTwigGroupsInTheClusterPasses() {
        GpuPlacedPlant plant{};
        plant.foot[0] = 3.0f;
        plant.foot[1] = 1.0f;
        plant.foot[2] = -2.0f;
        plant.scale = 1.5f;
        plant.turnRadians = 0.6f;
        const std::vector<GpuPlacedPlant> plants{plant};
        const std::vector<GpuInstance> partInstances{placedInstance(Mat4::translation(0.0f, 2.0f, 0.0f))};
        const std::vector<GpuPlacedPart> parts{GpuPlacedPart{}};
        const std::vector<GpuPlacedEntry> placedEntries{GpuPlacedEntry{0, 0}};
        std::vector<GpuTwig> twigs;

        for (int index = 0; index < 6; ++index) {
            twigs.push_back(packTwig(testTwig(Vec3{static_cast<float>(index), 5.0f, 0.0f}, 1.0f + 0.1f * static_cast<float>(index))));
        }

        constexpr std::uint32_t room = 4;
        std::vector<GpuTwigEntry> entries(room * twigMostLevelClusters);
        const auto put = [&](std::uint32_t region, std::uint32_t slot, std::uint32_t twig) {
            entries[region * room + slot] = GpuTwigEntry{0, twig, 10 * twig, 0};
        };
        put(0, 0, 0);
        put(0, 1, 1);
        put(0, 2, 2);
        put(0, 3, 3);
        put(1, 0, 3);
        put(1, 1, 4);
        put(3, 0, 5);

        MeshClusterTables tables;
        tables.placed.entries = placedEntries;
        tables.placed.partInstances = partInstances;
        tables.placed.parts = parts;
        tables.placed.plants = plants;
        tables.twigs.counts = {6, 2, 0, 1};
        tables.twigs.room = room;
        tables.twigs.entries = entries;
        tables.twigs.twigs = twigs;

        const CullDrawOrder order = meshClusterDrawOrder(CullDrawOrder{}, 0, CullDrawOrder{}, 1);
        assert(order.order.size() == twigMostLevelClusters && order.runFirst[1] == 0 && order.runCount[1] == twigMostLevelClusters);
        assert(order.order[2] == twigGroupBase + 2);
        tables.drawOrder = order.order;
        GpuCameraCull view{};

        for (std::size_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
            view.runFirst[style] = order.runFirst[style];
            view.runCount[style] = order.runCount[style];
        }

        std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS> work{};
        const std::vector<std::uint32_t> ends = meshClusterWorkEnds(tables, view, work);
        assert(work[MESH_CLUSTER_WORK_REAL] == 4 * 1 + 2 * 2 + 0 * 3 + 1 * 4); // region 0's six cut to its room of four

        std::vector<std::pair<std::uint32_t, std::uint32_t>> found;

        for (std::uint32_t pair = 0; pair < work[MESH_CLUSTER_WORK_END]; ++pair) {
            const MeshClusterPair at = meshClusterPairAt(tables, ends, pair);

            if (at.real) {
                found.emplace_back(at.entry, at.cluster);
            }
        }

        const std::vector<std::pair<std::uint32_t, std::uint32_t>> expected{
            {twigInstanceBase + 0, 0},          {twigInstanceBase + 1, 10},         {twigInstanceBase + 2, 20},         {twigInstanceBase + 3, 30},
            {twigInstanceBase + room, 30},      {twigInstanceBase + room, 31},      {twigInstanceBase + room + 1, 40},  {twigInstanceBase + room + 1, 41},
            {twigInstanceBase + 3 * room, 50},  {twigInstanceBase + 3 * room, 51},  {twigInstanceBase + 3 * room, 52}, {twigInstanceBase + 3 * room, 53}};
        assert(found == expected);

        const MeshClusterDrawn drawn = meshClusterDrawnAt(tables, twigInstanceBase + 3 * room);
        const GpuInstance wanted = twigInstance(placedInstance(partInstances[0], plant), twigs[5]);

        for (int value = 0; value < 16; ++value) {
            assert(std::abs(drawn.instance.model[value] - wanted.model[value]) < 1e-6f);
        }

        assert(drawn.entry == placedEntries[0].plant && twigMeshClusterPairBound(room) == room * (1 + 2 + 3 + 4));
    }
}

void testTwigRecords() {
    testTwigGroupsInTheClusterPasses();
    testATwigsRecord();
    testAPrototypesLevels();
    testATwigsInstance();
    testTheLevelIsTheCuts();
    testTheTwigPassOnTheCpu();
    testAPosedTreesTwigs();
    testTheTwigsLine();
}
