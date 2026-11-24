#include "cull_helpers.h"
#include "engine/debug_views.h"
#include "games/forest/forest_ground.h"
#include "games/forest/twigs.h"
#include "math/transform.h"
#include "render/gpu/bone_poses.h"
#include "render/gpu/column_major.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "render/gpu/render_settings.h"
#include "render/gpu/twig_records.h"
#include "render/gpu/wind.h"
#include "render/gpu/wind_bones.h"
#include "scene/mesh.h"
#include "scene/twig_far_levels.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <numbers>
#include <random>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "test_levels.h"
#include "twig_helpers.h"

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
        words[TWIG_TALLY_FAR_BRICKS] = 11;
        words[TWIG_TALLY_FAR_NODES] = 6;
        words[TWIG_TALLY_FAR_MERGED] = 3;
        words[TWIG_TALLY_FAR_CLUSTERS] = 20;
        words[TWIG_TALLY_FAR_TRIANGLES] = 900;
        const TwigTally read = readTwigTally(words);
        assert(read.tested == 7 && read.kept == 5 && read.swaying == 2 && read.levelTwigs[1] == 4 && read.levelTwigs[0] == 0);
        assert(read.levelTriangles[TWIG_MOST_LEVELS - 1] == 9 && read.levelTriangles[0] == 0);
        assert(read.farBricks == 11 && read.farNodes == 6 && read.farMerged == 3 && read.farClusters == 20 && read.farTriangles == 900);
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
    // Finds a bool field by group and name in a settings visit.
    class BoolFinder : public SettingsVisitor {
    public:
        BoolFinder(std::string group, std::string name) : wantedGroup(std::move(group)), wantedName(std::move(name)) {}

        bool* found = nullptr;

    protected:
        void visitFloat(const char*, float&, const FloatLimits&) override {}
        void visitInt(const char*, int&, const IntLimits&) override {}
        void visitChoice(const char*, int&, const char* const*, int, FieldApplies) override {}

        void visitBool(const char* name, bool& value, FieldApplies) override {
            if (group == wantedGroup && name == wantedName) {
                found = &value;
            }
        }

        void visitGroup(const char* name, const std::function<void()>& contents) override {
            const std::string outer = group;
            group = name;
            contents();
            group = outer;
        }

    private:
        std::string wantedGroup;
        std::string wantedName;
        std::string group;
    };

    // Passes "Far leaves" (stage 3e-iii-c): on by default, the render settings' farLeaves, so F5, settings.cfg and the
    // command line can turn the far levels off to compare.
    void testTheFarLeavesSwitch() {
        RenderSettings settings;
        assert(settings.farLeaves);
        BoolFinder finder("Passes", "Far leaves");
        visit(settings, finder);
        assert(finder.found == &settings.farLeaves);
    }

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

        // The far line only once bricks hand over to their far levels.
        for (const std::string& line : lines) {
            assert(line.find("FAR") == std::string::npos);
        }

        stats.twigFarBricks = 900;
        stats.twigFarNodes = 120;
        stats.twigFarMerged = 40;
        stats.twigFarClusters = 2400;
        stats.twigFarTriangles = 115000;
        const std::vector<std::string> far = renderStatsLines(stats);
        assert(std::find(far.begin(), far.end(), "FAR BRICKS 900  NODES 120 (40 MERGED)  CLUSTERS 2400  TRIS 115K") != far.end());

        // Once the mesh shaders test far leaves (stage 3e-v-b): the main pass's hidden of tested, then the late prepass's.
        stats.twigFarLeavesTested = 28000;
        stats.twigFarLeavesHidden = 9000;
        stats.twigFarLeavesLateTested = 600;
        stats.twigFarLeavesLateHidden = 50;
        const std::vector<std::string> tested = renderStatsLines(stats);
        assert(std::find(tested.begin(), tested.end(),
                         "FAR BRICKS 900  NODES 120 (40 MERGED)  CLUSTERS 2400  TRIS 115K  LEAVES HIDDEN 9000/28000 (LATE 50/600)") != tested.end());
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
        std::vector<GpuTwigEntry> entries(room * twigRegions);
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

        // The far region: two far leaf clusters of the part, each one pair, its cluster the far cluster's number with the bit.
        std::vector<GpuTwigFarCluster> farClusters(3);
        farClusters[2] = GpuTwigFarCluster{.sphere = {1.0f, 2.0f, 3.0f, 0.5f}, .firstLeaf = 24, .leafCount = 7, .firstShape = 0};
        entries[twigFarRegion * room + 0] = GpuTwigEntry{0, 0, twigFarClusterBit | 2u, 0};
        entries[twigFarRegion * room + 1] = GpuTwigEntry{0, 0, twigFarClusterBit | 0u, 0};

        MeshClusterTables tables;
        tables.placed.entries = placedEntries;
        tables.placed.partInstances = partInstances;
        tables.placed.parts = parts;
        tables.placed.plants = plants;
        tables.twigs.counts = {6, 2, 0, 1, 2};
        tables.twigs.room = room;
        tables.twigs.entries = entries;
        tables.twigs.twigs = twigs;
        tables.twigs.farClusters = farClusters;

        const CullDrawOrder order = meshClusterDrawOrder(CullDrawOrder{}, 0, CullDrawOrder{}, 1);
        assert(order.order.size() == twigRegions && order.runFirst[1] == 0 && order.runCount[1] == twigRegions);
        assert(order.order[2] == twigGroupBase + 2 && order.order[twigFarRegion] == twigGroupBase + twigFarRegion);
        tables.drawOrder = order.order;
        GpuCameraCull view{};

        for (std::size_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
            view.runFirst[style] = order.runFirst[style];
            view.runCount[style] = order.runCount[style];
        }

        std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS> work{};
        const std::vector<std::uint32_t> ends = meshClusterWorkEnds(tables, view, work);
        assert(work[MESH_CLUSTER_WORK_REAL] == 4 * 1 + 2 * 2 + 0 * 3 + 1 * 4 + 2 * 1); // region 0's six cut to its room of four

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
            {twigInstanceBase + 3 * room, 50},  {twigInstanceBase + 3 * room, 51},  {twigInstanceBase + 3 * room, 52}, {twigInstanceBase + 3 * room, 53},
            {twigInstanceBase + twigFarRegion * room, twigFarClusterBit | 2u}, {twigInstanceBase + twigFarRegion * room + 1, twigFarClusterBit | 0u}};
        assert(found == expected);

        const MeshClusterDrawn drawn = meshClusterDrawnAt(tables, twigInstanceBase + 3 * room);
        const GpuInstance wanted = twigInstance(placedInstance(partInstances[0], plant), twigs[5]);

        for (int value = 0; value < 16; ++value) {
            assert(std::abs(drawn.instance.model[value] - wanted.model[value]) < 1e-6f);
        }

        assert(drawn.entry == placedEntries[0].plant && drawn.farCluster == meshClusterNoFar);
        assert(twigMeshClusterPairBound(room) == room * (1 + 2 + 3 + 4 + 1));

        // A far cluster is drawn as its part (its leaves are in the plant's frame), not as a twig; its cluster is the far
        // record's stand-in (twigFarMeshCluster), whose triangles the list counts.
        const MeshClusterDrawn far = meshClusterDrawnAt(tables, twigInstanceBase + twigFarRegion * room);
        const GpuInstance part = placedInstance(partInstances[0], plant);

        for (int value = 0; value < 16; ++value) {
            assert(far.instance.model[value] == part.model[value]);
        }

        assert(far.twig == meshClusterNoTwig && far.farCluster == 2 && far.entry == placedEntries[0].plant);
        const GpuMeshCluster stood = meshClusterOfPair(tables, twigFarClusterBit | 2u);
        assert(stood.sphere[3] == 0.5f && (stood.counts >> MESH_CLUSTER_TRIANGLE_SHIFT) == 7 * farLeafTriangles);

        // Its pose: its part's tree's move on its sphere when the part is bound to bones and its tree posed, else none.
        assert(!meshClusterPoseAt(tables, far, stood).posed);
        std::vector<GpuPlacedPart> boundParts = parts;
        boundParts[0].unused[1] = 1;
        GpuBoneTree tree;
        tree.firstPose = 0;
        tree.move = 3.0f;
        const std::vector<std::uint32_t> plantTrees{5u, 0u};
        const std::vector<GpuBoneTree> trees{tree};
        const std::vector<GpuBonePose> pool{packBonePose(WindBonePose{}, -1)};
        MeshClusterTables bound = tables;
        bound.placed.parts = boundParts;
        bound.bones = BoneCullTables{plantTrees, trees, pool, {}, 5u};
        const MeshClusterDrawn boundFar = meshClusterDrawnAt(bound, twigInstanceBase + twigFarRegion * room);
        const MeshClusterPose pose = meshClusterPoseAt(bound, boundFar, stood);
        assert(boundFar.partBound && pose.posed && pose.sphere[3] >= 0.5f * plant.scale + tree.move * plant.scale - 1e-4f);
    }

    /*
    * A far cluster's stand-in in the cluster passes (twigFarMeshCluster): its sphere for the frustum and occlusion, its
    * leaves' vertices and triangles counted, always in the cut (its node chose its level), never culled by facing (leaves
    * are seen from both sides), bound to no bone (its part's pose bounds it). So a view keeps it exactly where its placed
    * sphere is in the frustum.
    */
    void testAFarClustersStandIn() {
        const GpuTwigFarCluster far{.sphere = {2.0f, 15.0f, -1.0f, 1.5f}, .firstLeaf = 40, .leafCount = 12, .firstShape = 24};
        const GpuMeshCluster cluster = twigFarMeshCluster(far);
        assert(cluster.sphere[0] == 2.0f && cluster.sphere[3] == 1.5f && cluster.cone[3] >= 1.0f);
        assert((cluster.counts & 0xffu) == 12 * farLeafVertices && (cluster.counts >> MESH_CLUSTER_TRIANGLE_SHIFT) == 12 * farLeafTriangles);
        assert(cluster.ownError == 0.0f && cluster.parentError > static_cast<float>(MESH_CLUSTER_NEVER_COARSER) && !meshClusterBound(cluster));

        const GpuInstance part = placedInstance(Mat4::translation(10.0f, 0.0f, 0.0f));
        const GpuCullRecord record = stillRecord(Vec3{10.0f, 15.0f, 0.0f}, 14.0f);
        std::size_t kept = 0;

        for (float yaw = 0.0f; yaw < 6.28f; yaw += 0.05f) {
            for (float distance : {0.5f, 20.0f, 300.0f}) {
                const GpuCameraCull view = cutView(Vec3{12.0f - distance, 15.0f, -1.0f}, yaw, 1.0f);
                const bool inView = sphereInFrustum(view.frustum, meshClusterPlacedSphere(far.sphere, part).data());
                const std::uint32_t visibility = meshClusterVisibility(cluster, part, 0u, record, view, MeshClusterPose{});
                assert((visibility != MESH_CLUSTER_HIDDEN) == inView);
                kept += inView ? 1 : 0;
            }
        }

        assert(kept > 0);
    }
}

namespace {
    // Real twigs (the forest's, from a stand-in atlas) in a set with far levels, and the tables the twig pass reads of it:
    // the set packed alone, each prototype's levels from its given cluster levels, each run where a pool would hold it.
    struct FarTwigs {
        std::vector<TwigPrototype> prototypes;
        TwigSet set;
        TwigTables tables;
    };

    FarTwigs farTwigs(std::size_t count, float errorPercentile = ForestTwigSettings{}.farErrorPercentile) {
        const LeafAtlas atlas = standInLeafAtlas(64, forestTreeLeafColumns, forestTreeLeafRows, forestLeafStandIn);
        const ForestTwigSettings settings;
        const TracedLeaves leaves = traceAtlasLeaves(atlas, 3, 1.0f);
        FarTwigs made;
        made.prototypes = buildTwigPrototypes(leaves, settings, 1.0f, 0.22f, 3);
        std::vector<TwigFarLeaves> handed;
        std::vector<float> reaches;
        std::vector<MeshHandle> meshes;

        for (const TwigPrototype& twig : made.prototypes) {
            handed.push_back(twigFarLeaves(twig));
            reaches.push_back(meshReach(twigLeafMesh(twig).mesh));
            meshes.push_back(MeshHandle{static_cast<std::uint32_t>(meshes.size())});
        }

        made.set = makeTwigSet(meshes, reaches, scatteredTwigs(count, static_cast<std::uint32_t>(made.prototypes.size())));
        made.set.far = makeTwigFarLevels(made.set, handed, twigFarLeafShapes(leaves, settings), errorPercentile);
        const TwigSet* sets[1] = {&made.set};
        made.tables = packTwigSets(sets, meshes).tables;
        std::uint32_t pool = 0;

        for (const TwigPrototype& twig : made.prototypes) {
            const TwigLeafMesh mesh = twigLeafMesh(twig);
            const MeshClusters clusters = buildGivenClusterLevels(mesh.mesh, mesh.coarser);
            const std::vector<GpuTwigLevel> levels = packTwigLevels(clusters, pool);
            made.tables.prototypes.push_back(GpuTwigPrototype{static_cast<std::uint32_t>(made.tables.levels.size()), static_cast<std::uint32_t>(levels.size())});
            made.tables.levels.insert(made.tables.levels.end(), levels.begin(), levels.end());
            pool += static_cast<std::uint32_t>(clusters.clusters.size());
        }

        return made;
    }

    // Two far leaf records alike in every word.
    bool sameGpuFarLeaf(const GpuFarLeaf& a, const GpuFarLeaf& b) {
        return a.foot[0] == b.foot[0] && a.foot[1] == b.foot[1] && a.foot[2] == b.foot[2] && a.turn[0] == b.turn[0] && a.turn[1] == b.turn[1] &&
               a.sideCellNumber == b.sideCellNumber && a.twig == b.twig;
    }

    /*
    * Sets packed into one table each: every run moved past the sets before (bricks' twigs, far nodes' levels, first
    * bricks and parents, far levels' leaves, far leaves' twigs: a leaf's twig must be the table's, or it takes another
    * twig's binding, lean and tone), a set without far levels naming no node, sets with the same shapes sharing one run
    * and a set with others its own, prototypes numbered by their meshes; a prototype not among the meshes refused.
    */
    void testPackedSetsMoveTheirRuns() {
        const std::vector<TwigFarLeaves> handed = standInFarLeaves();
        const std::vector<float> reaches{1.2f, 1.0f, 0.8f};
        TwigSet first = makeTwigSet({MeshHandle{3}, MeshHandle{4}, MeshHandle{5}}, reaches, scatteredTwigs(700));
        first.far = makeTwigFarLevels(first, handed, standInFarShapes(), farErrorWorstPercentile);
        const TwigSet plain = makeTwigSet({MeshHandle{9}, MeshHandle{4}}, {1.0f, 1.0f}, scatteredTwigs(200, 2));
        TwigSet third = makeTwigSet({MeshHandle{5}, MeshHandle{3}, MeshHandle{4}}, reaches, scatteredTwigs(300));
        third.far = makeTwigFarLevels(third, handed, standInFarShapes(), farErrorWorstPercentile);
        TwigSet curled = third;
        curled.far.shapes.curl = 0.2f;
        const std::vector<MeshHandle> meshes{MeshHandle{3}, MeshHandle{4}, MeshHandle{5}, MeshHandle{9}};
        const std::vector<const TwigSet*> sets{&first, &plain, &third, &curled};
        const PackedTwigSets packed = packTwigSets(sets, meshes);
        const TwigTables& tables = packed.tables;

        std::uint32_t bricksBefore = 0;
        std::uint32_t twigsBefore = 0;
        std::uint32_t nodesBefore = 0;
        std::uint32_t levelsBefore = 0;
        std::uint32_t leavesBefore = 0;
        std::uint32_t clustersBefore = 0;
        const std::uint32_t cells = static_cast<std::uint32_t>(standInFarShapes().cells.size());
        const std::uint32_t firstShapes[4] = {0, 0, 0, cells};
        assert(packed.sets.size() == 4 && tables.farShapes.size() == 2 * cells);

        for (std::size_t number = 0; number < sets.size(); ++number) {
            const TwigSet& set = *sets[number];
            const GpuTwigSet& record = packed.sets[number];
            assert(record.firstBrick == bricksBefore && record.brickCount == set.bricks.size());

            for (std::size_t brick = 0; brick < set.bricks.size(); ++brick) {
                assert(tables.bricks[bricksBefore + brick].firstTwig == twigsBefore + set.bricks[brick].firstTwig);
            }

            for (std::size_t twig = 0; twig < set.twigs.size(); ++twig) {
                const GpuTwig& got = tables.twigs[twigsBefore + twig];
                const MeshHandle mesh = set.prototypes[set.twigs[twig].prototype];
                assert(meshes[got.prototype].index == mesh.index && got.rows[0][3] == set.twigs[twig].foot.x);
                assert(packed.bindings[twigsBefore + twig] == packTwigBinding(set.twigs[twig]));
            }

            if (set.far.nodes.empty()) {
                assert(record.firstNode == TWIG_FAR_NO_NODE);
            } else {
                assert(record.firstNode == nodesBefore && record.firstShape == firstShapes[number]);

                for (std::size_t node = 0; node < set.far.nodes.size(); ++node) {
                    const TwigFarNode& want = set.far.nodes[node];
                    const GpuTwigFarNode& got = tables.farNodes[nodesBefore + node];
                    assert(got.sphere[0] == want.centre.x && got.sphere[3] == want.radius && got.levelCount == want.levelCount);
                    assert(got.firstLevel == levelsBefore + want.firstLevel && got.firstBrick == bricksBefore + want.firstBrick);
                    assert(got.parent == (want.parent == twigFarNoNode ? TWIG_FAR_NO_NODE : nodesBefore + want.parent));
                }

                for (std::size_t level = 0; level < set.far.levels.size(); ++level) {
                    const TwigFarLevel& want = set.far.levels[level];
                    const GpuTwigFarLevel& got = tables.farLevels[levelsBefore + level];
                    assert(got.leafCount == want.leafCount && got.error == want.error);

                    // Its clusters: its leaves in order, farLeavesPerCluster to each but the last, each sphere holding
                    // every vertex of its leaves as the mesh shader makes them, the set's shapes named.
                    std::uint32_t next = leavesBefore + want.firstLeaf;
                    assert(got.clusterCount == (want.leafCount + farLeavesPerCluster - 1) / farLeavesPerCluster);

                    for (std::uint32_t number = got.firstCluster; number < got.firstCluster + got.clusterCount; ++number) {
                        const GpuTwigFarCluster& cluster = tables.farClusters[number];
                        const bool last = number + 1 == got.firstCluster + got.clusterCount;
                        assert(cluster.firstLeaf == next && cluster.firstShape == record.firstShape && cluster.leafCount > 0);
                        assert(cluster.leafCount == (last ? want.leafCount - (got.clusterCount - 1) * farLeavesPerCluster : farLeavesPerCluster));
                        next += cluster.leafCount;
                        const Vec3 centre{cluster.sphere[0], cluster.sphere[1], cluster.sphere[2]};
                        float farthest = 0.0f;

                        for (std::uint32_t leaf = cluster.firstLeaf; leaf < cluster.firstLeaf + cluster.leafCount; ++leaf) {
                            const GpuFarLeaf& made = tables.farLeaves[leaf];
                            const GpuFarLeafShape& shape = tables.farShapes[cluster.firstShape + ((made.sideCellNumber >> 16) & 0xffu)];

                            for (std::uint32_t vertex = 0; vertex < farLeafVertices; ++vertex) {
                                const float distance = (farLeafVertex(made, shape, vertex).position - centre).length();
                                assert(distance <= cluster.sphere[3] * (1.0f + 1e-5f));
                                farthest = std::max(farthest, distance);
                            }
                        }

                        assert(farthest >= cluster.sphere[3] * 0.999f); // tight: some vertex on it
                    }

                    assert(next == leavesBefore + want.firstLeaf + want.leafCount);
                }

                assert(tables.farLevels[levelsBefore].firstCluster == clustersBefore);

                for (std::size_t level = 0; level < set.far.levels.size(); ++level) {
                    clustersBefore += tables.farLevels[levelsBefore + level].clusterCount;
                }

                for (std::size_t leaf = 0; leaf < set.far.leaves.size(); ++leaf) {
                    GpuFarLeaf want = packFarLeaf(set.far.leaves[leaf]);
                    want.twig += twigsBefore;
                    assert(sameGpuFarLeaf(tables.farLeaves[leavesBefore + leaf], want));
                }

                for (std::uint32_t cell = 0; cell < cells; ++cell) {
                    const GpuFarLeafShape want = packFarLeafShape(set.far.shapes.cells[cell], set.far.shapes.foldRadians, set.far.shapes.curl);
                    const GpuFarLeafShape& got = tables.farShapes[record.firstShape + cell];
                    assert(got.shape[1] == want.shape[1] && got.curl == want.curl && got.raise == want.raise && got.uvRect[2] == want.uvRect[2]);
                }
            }

            bricksBefore += static_cast<std::uint32_t>(set.bricks.size());
            twigsBefore += static_cast<std::uint32_t>(set.twigs.size());
            nodesBefore += static_cast<std::uint32_t>(set.far.nodes.size());
            levelsBefore += static_cast<std::uint32_t>(set.far.levels.size());
            leavesBefore += static_cast<std::uint32_t>(set.far.leaves.size());
        }

        assert(tables.bricks.size() == bricksBefore && tables.twigs.size() == twigsBefore && packed.bindings.size() == twigsBefore);
        assert(tables.farNodes.size() == nodesBefore && tables.farLevels.size() == levelsBefore && tables.farLeaves.size() == leavesBefore);
        assert(tables.farClusters.size() == clustersBefore);
        assert(tables.prototypes.empty() && tables.levels.empty());

        const std::vector<MeshHandle> missing{MeshHandle{3}, MeshHandle{4}, MeshHandle{5}};
        assert(throwsInvalid([&] { packTwigSets(sets, missing); }));
    }

    // The level a twig draw's first cluster starts in its prototype's run (TWIG_MOST_LEVELS when none).
    std::uint32_t drawnTwigLevel(const TwigTables& tables, const TwigDraw& draw) {
        const GpuTwigPrototype& prototype = tables.prototypes[tables.twigs[draw.twig].prototype];

        for (std::uint32_t level = 0; level < prototype.levelCount; ++level) {
            if (tables.levels[prototype.firstLevel + level].firstCluster == draw.firstCluster) {
                return level;
            }
        }

        return TWIG_MOST_LEVELS;
    }

    // Whether a far node's finest level fits the view.
    bool farNodeFits(const TwigTables& tables, std::uint32_t node, const GpuInstance& part, const GpuCameraCull& view) {
        return twigFarLevelFits(tables.farNodes[node], tables.farLevels[tables.farNodes[node].firstLevel], part, view);
    }

    /*
    * One choice a brick, from inside the crown to far past it (the cluster cut's rule on the far tree): no brick drawn
    * as twigs and as a far node, nor by two far nodes; each far node drawn fits and its parent does not, at its coarsest
    * level that fits, in the frustum; twigs never coarser than the bricks' level. Where a node fits, every node below
    * it does (so every brick under it finds it), and the twigs of a brick whose node fits holding at least the error
    * percentile of its leaves would draw twigBrickLevel or coarser by themselves (at 100 every twig: the bricks take
    * over from exactly the leaves the twigs showed, no leaf moves; below, its largest twigs' leaves may shrink to their
    * level-2 forms). Near views draw only twigs, far ones only the root, between them both.
    */
    void testABrickMakesOneChoice(float errorPercentile) {
        const FarTwigs made = farTwigs(3000, errorPercentile);
        std::vector<std::size_t> handedLeaves;

        for (const TwigPrototype& twig : made.prototypes) {
            handedLeaves.push_back(twigFarLeaves(twig).leaves.size());
        }

        const TwigTables& tables = made.tables;
        const TwigFarLevels& far = made.set.far;
        const auto root = static_cast<std::uint32_t>(far.nodes.size() - 1);
        const GpuInstance part = placedInstance(Mat4::identity());
        const GpuCullRecord record = stillRecord(Vec3{0.0f, 15.0f, 0.0f}, 14.0f);
        bool onlyTwigs = false;
        bool both = false;
        bool onlyRoot = false;

        // Nearer and nearer to 480 m (within the view's 500 m), then the pixel error grown there until the root is reached.
        std::vector<GpuCameraCull> views;

        for (float distance = 12.0f; distance < 480.0f; distance *= 1.05f) {
            views.push_back(cutView(Vec3{-distance, 15.0f, 0.0f}, 0.0f, 8.0f));
        }

        for (float pixels = 8.0f; pixels < 20000.0f; pixels *= 1.2f) {
            views.push_back(cutView(Vec3{-480.0f, 15.0f, 0.0f}, 0.0f, pixels));
        }

        for (const GpuCameraCull& view : views) {
            std::vector<TwigFarDraw> farDraws;
            TwigTally tally;
            const std::vector<TwigDraw> draws = twigDrawsOnCpu(tables, part, 0u, record, view, &tally, nullptr, false, &farDraws);
            std::vector<std::uint32_t> covered(tables.bricks.size(), 0);

            // The tally: every brick handing over, each far node drawn (merged ones apart), its level's clusters and its
            // leaves' triangles.
            std::uint32_t handing = 0;
            std::uint32_t merged = 0;
            std::uint32_t farClusters = 0;
            std::uint32_t farTriangles = 0;

            for (std::uint32_t brick = 0; brick < tables.bricks.size(); ++brick) {
                handing += twigFarNodeDrawn(tables, brick, part, view) != TWIG_FAR_NO_NODE ? 1 : 0;
            }

            for (const TwigFarDraw& draw : farDraws) {
                merged += draw.node >= tables.bricks.size() ? 1 : 0;
                farClusters += draw.clusterCount;
                farTriangles += tables.farLevels[draw.level].leafCount * farLeafTriangles;
            }

            assert(tally.farBricks == handing && tally.farNodes == farDraws.size() && tally.farMerged == merged);
            assert(tally.farClusters == farClusters && tally.farTriangles == farTriangles && tally.kept == draws.size());
            std::vector<bool> twigged(tables.bricks.size(), false);

            for (const TwigDraw& draw : draws) {
                assert(drawnTwigLevel(tables, draw) <= TWIG_BRICK_LEVEL);
                twigged[draw.twig / twigBrickSize] = true;
            }

            for (const TwigFarDraw& draw : farDraws) {
                const GpuTwigFarNode& node = tables.farNodes[draw.node];
                assert(farNodeFits(tables, draw.node, part, view) && (node.parent == TWIG_FAR_NO_NODE || !farNodeFits(tables, node.parent, part, view)));
                assert(draw.level >= node.firstLevel && draw.level < node.firstLevel + node.levelCount);
                assert(draw.level == node.firstLevel || twigFarLevelFits(node, tables.farLevels[draw.level], part, view));
                assert(draw.level + 1 == node.firstLevel + node.levelCount || !twigFarLevelFits(node, tables.farLevels[draw.level + 1], part, view));
                assert(draw.firstCluster == tables.farLevels[draw.level].firstCluster && draw.clusterCount == tables.farLevels[draw.level].clusterCount);
                assert(sphereInFrustum(view.frustum, meshClusterPlacedSphere(node.sphere, part).data()));

                for (std::uint32_t brick = far.nodes[draw.node].firstBrick; brick < far.nodes[draw.node].firstBrick + far.nodes[draw.node].brickCount; ++brick) {
                    ++covered[brick];
                }
            }

            for (std::size_t brick = 0; brick < tables.bricks.size(); ++brick) {
                assert(covered[brick] <= 1 && !(covered[brick] == 1 && twigged[brick]));
            }

            for (std::uint32_t node = 0; node < far.nodes.size(); ++node) {
                const std::uint32_t parent = tables.farNodes[node].parent;
                assert(parent == TWIG_FAR_NO_NODE || !farNodeFits(tables, parent, part, view) || farNodeFits(tables, node, part, view));
            }

            for (std::uint32_t brick = 0; brick < tables.bricks.size(); ++brick) {
                if (!farNodeFits(tables, brick, part, view)) {
                    continue;
                }

                std::size_t leaves = 0;
                std::size_t handedOver = 0;

                for (std::uint32_t twig = tables.bricks[brick].firstTwig; twig < tables.bricks[brick].firstTwig + tables.bricks[brick].twigCount; ++twig) {
                    const GpuTwigPrototype& prototype = tables.prototypes[tables.twigs[twig].prototype];
                    const GpuInstance placed = twigInstance(part, tables.twigs[twig]);
                    const std::size_t count = handedLeaves[tables.twigs[twig].prototype];
                    leaves += count;
                    handedOver += twigLevel(&tables.levels[prototype.firstLevel], prototype.levelCount, placed, view) >= TWIG_BRICK_LEVEL ? count : 0;
                }

                assert(static_cast<float>(handedOver) >= errorPercentile / 100.0f * static_cast<float>(leaves) - 1e-3f);
            }

            onlyTwigs = onlyTwigs || (!draws.empty() && farDraws.empty());
            both = both || (!draws.empty() && !farDraws.empty());
            onlyRoot = onlyRoot || (draws.empty() && farDraws.size() == 1 && farDraws[0].node == root);        }

        assert(onlyTwigs && both && onlyRoot);
    }

    /*
    * A far node is drawn by its first brick only where it is in the frustum: a view facing away from part of the crown
    * leaves some nodes out, and each brick not drawn by anything has its node, or its twigs, out of view. The twig pass's
    * other bricks are unchanged by the far tree: a set without far nodes draws the same twigs there, its levels cut at
    * twigBrickLevel.
    */
    void testFarNodesOutOfViewAreLeftOut() {
        const FarTwigs made = farTwigs(3000);
        const TwigTables& tables = made.tables;
        TwigTables plain = tables;
        plain.farNodes.clear();
        const GpuInstance part = placedInstance(Mat4::identity());
        const GpuCullRecord record = stillRecord(Vec3{0.0f, 15.0f, 0.0f}, 14.0f);
        bool someLeftOut = false;

        for (float distance = 30.0f; distance < 3000.0f; distance *= 1.2f) {
            // Turned a little, so the view's edge cuts through the crown.
            const GpuCameraCull view = cutView(Vec3{-distance, 15.0f, 0.0f}, 0.35f * 30.0f / distance + 0.01f, 1.0f);
            std::vector<TwigFarDraw> farDraws;
            const std::vector<TwigDraw> draws = twigDrawsOnCpu(tables, part, 0u, record, view, nullptr, nullptr, false, &farDraws);
            const std::vector<TwigDraw> plainDraws = twigDrawsOnCpu(plain, part, 0u, record, view);

            for (std::uint32_t brick = 0; brick < tables.bricks.size(); ++brick) {
                const std::uint32_t node = twigFarNodeDrawn(tables, brick, part, view);
                const bool farDrawn = std::any_of(farDraws.begin(), farDraws.end(), [&](const TwigFarDraw& draw) { return draw.node == node; });
                const auto inBrick = [&](const TwigDraw& draw) { return draw.twig / twigBrickSize == brick; };

                if (node == TWIG_FAR_NO_NODE) {
                    std::vector<TwigDraw> mine;
                    std::vector<TwigDraw> wanted;
                    std::copy_if(draws.begin(), draws.end(), std::back_inserter(mine), inBrick);
                    std::copy_if(plainDraws.begin(), plainDraws.end(), std::back_inserter(wanted), inBrick);

                    for (TwigDraw& want : wanted) {
                        const GpuTwigPrototype& prototype = tables.prototypes[tables.twigs[want.twig].prototype];
                        const GpuTwigLevel& cut = tables.levels[prototype.firstLevel + std::min<std::uint32_t>(TWIG_BRICK_LEVEL, prototype.levelCount - 1)];
                        if (drawnTwigLevel(tables, want) > TWIG_BRICK_LEVEL) {
                            want.firstCluster = cut.firstCluster;
                            want.clusterCount = cut.clusterCount;
                        }
                    }

                    assert(mine == wanted);
                } else {
                    const bool inView = sphereInFrustum(view.frustum, meshClusterPlacedSphere(tables.farNodes[node].sphere, part).data());
                    assert(farDrawn == inView);
                    someLeftOut = someLeftOut || !inView;
                }
            }
        }

        assert(someLeftOut);
    }

    /*
    * A far node of a part bound to bones is kept by its posed sphere (twigSpherePose: grown by its tree's move, as a
    * brick's): a view facing away from the crown keeps no far node of a still tree, and every drawn one when the tree
    * moves farther than the view's distance; a part not bound to bones is never posed.
    */
    void testFarNodesPosedByTheirTree() {
        const FarTwigs made = farTwigs(3000);
        const GpuInstance part = placedInstance(Mat4::identity());
        const GpuCullRecord record = stillRecord(Vec3{0.0f, 15.0f, 0.0f}, 14.0f);
        const GpuCameraCull away = cutView(Vec3{-480.0f, 15.0f, 0.0f}, 3.14159f, 8.0f);
        GpuBoneTree tree;
        tree.firstPose = 0;
        tree.move = 2000.0f;
        const std::vector<std::uint32_t> plantTrees{5u, 0u};
        const std::vector<GpuBoneTree> trees{tree};
        const std::vector<GpuBonePose> pool{packBonePose(WindBonePose{}, -1), packBonePose(WindBonePose{}, -1)};
        const std::vector<std::uint32_t> bindings(made.tables.twigs.size(), swayUnbound);
        const BoneCullTables bones{plantTrees, trees, pool, bindings, 5u};

        std::vector<TwigFarDraw> still;
        twigDrawsOnCpu(made.tables, part, 0u, record, away, nullptr, nullptr, false, &still);
        std::vector<TwigFarDraw> unbound;
        twigDrawsOnCpu(made.tables, part, 0u, record, away, nullptr, &bones, false, &unbound);
        std::vector<TwigFarDraw> posed;
        twigDrawsOnCpu(made.tables, part, 0u, record, away, nullptr, &bones, true, &posed);
        assert(still.empty() && unbound.empty() && !posed.empty());

        // Posed or not, the same nodes are chosen (the choice is by rest errors): facing the crown, still, draws them all.
        const GpuCameraCull facing = cutView(Vec3{-480.0f, 15.0f, 0.0f}, 0.0f, 8.0f);
        std::vector<TwigFarDraw> seen;
        twigDrawsOnCpu(made.tables, part, 0u, record, facing, nullptr, nullptr, false, &seen);
        assert(seen == posed);

        const MeshClusterPose pose = twigSpherePose(made.tables.farNodes[0].sphere, part, true, tree);
        const MeshClusterPose brick = twigBrickPose(made.tables.bricks[0], part, true, tree);
        assert(pose.posed && pose.sphere[3] >= made.tables.farNodes[0].sphere[3] + tree.move && brick.posed);
    }

    /*
    * A far leaf's record keeps its foot exactly, its frame to snorm16 (about 3e-5), its side to a half float, its cell,
    * number and twig exactly; a cell or number past a byte is refused. A shape's record keeps its numbers and the fold's
    * tangent.
    */
    void testAFarLeafsRecord() {
        const Vec3 towardsTip = Vec3{0.3f, 0.9f, -0.2f}.normalized();
        const Vec3 facing = towardsTip.cross(Vec3{1.0f, 0.0f, 0.0f}).normalized();
        const FarLeaf leaf{.foot = Vec3{12.5f, -3.25f, 7.0f}, .towardsTip = towardsTip, .facing = facing, .side = 0.137f, .cell = 23, .number = 127, .twig = 70001};
        const GpuFarLeaf packed = packFarLeaf(leaf);
        assert(packed.foot[0] == 12.5f && packed.foot[1] == -3.25f && packed.foot[2] == 7.0f && packed.twig == 70001);
        assert(((packed.sideCellNumber >> 16) & 0xffu) == 23 && (packed.sideCellNumber >> 24) == 127);

        // A flat leaf's tip vertex lies a side up its midrib from its foot.
        const FarLeafShape flat{.base = 0.0f, .tip = 1.0f, .up = 0.5f, .left = 0.25f, .right = 0.25f, .uvMin = Vec2{0.5f, 0.25f}, .uvMax = Vec2{0.75f, 0.5f}};
        const GpuFarLeafShape shape = packFarLeafShape(flat, 0.0f, 0.0f);
        const FarLeafVertex tip = farLeafVertex(packed, shape, 4);
        assert((tip.position - (leaf.foot + towardsTip * 0.137f)).length() < 1e-4f);
        const Vec3 tipAcross{tip.tangent.x, tip.tangent.y, tip.tangent.z};
        assert(tip.normal.dot(facing) > 0.9999f && tipAcross.dot(towardsTip.cross(facing)) > 0.9999f);
        assert(std::abs(tip.uv.x - (127.0f + 0.625f)) < 1e-4f && std::abs(tip.uv.y - 0.5f) < 1e-6f);

        const GpuFarLeafShape folded = packFarLeafShape(flat, 0.3f, 0.1f);
        assert(folded.shape[0] == 0.0f && folded.shape[1] == 1.0f && folded.shape[2] == 0.5f && folded.shape[3] == 0.25f && folded.right == 0.25f);
        assert(std::abs(folded.raise - std::tan(0.3f)) < 1e-7f && folded.curl == 0.1f && folded.uvRect[2] == 0.75f && folded.uvRect[3] == 0.5f);

        FarLeaf tooMany = leaf;
        tooMany.cell = 256;
        assert(throwsInvalid([&] { packFarLeaf(tooMany); }));
        tooMany = leaf;
        tooMany.number = 256;
        assert(throwsInvalid([&] { packFarLeaf(tooMany); }));
    }

    /*
    * A brick's finest far leaves, packed and expanded as the mesh shader will (farLeafVertex), are their twigs' level-2
    * leaves: each vertex where the twig places its prototype's (within the half float's and snorm's rounding), its normal
    * turned with it, its texture coordinate (and so its number) the same, its tangent across; its triangles wound as
    * the prototype's. Twigs of every prototype, turned at random and scaled.
    */
    void testFarLeavesAreTheTwigsLevel() {
        const FarTwigs made = farTwigs(300);
        const std::vector<TwigPrototype>& prototypes = made.prototypes;
        const TwigSet& set = made.set;
        const TwigFarLevels& far = set.far;
        std::size_t checked = 0;

        for (std::size_t brick = 0; brick < set.bricks.size(); ++brick) {
            const TwigFarLevel& finest = far.levels[far.nodes[brick].firstLevel];

            for (std::uint32_t index = finest.firstLeaf; index < finest.firstLeaf + finest.leafCount; ++index) {
                const FarLeaf& leaf = far.leaves[index];
                const TwigInstance& twig = set.twigs[leaf.twig];
                const TwigLevel& level = prototypes[twig.prototype].levels[static_cast<std::size_t>(twigBrickLevel)];
                const auto kept = static_cast<std::uint32_t>(std::find(level.kept.begin(), level.kept.end(), leaf.number) - level.kept.begin());
                assert(kept < level.kept.size());
                const GpuFarLeaf packed = packFarLeaf(leaf);
                const GpuFarLeafShape shape = packFarLeafShape(far.shapes.cells[leaf.cell], far.shapes.foldRadians, far.shapes.curl);
                const Vec3 across = twig.towardsTip.cross(twig.facing);
                const auto turned = [&](const Vec3& local) {
                    return twig.across * local.x + twig.towardsTip * local.y + twig.facing * local.z;
                };

                for (std::uint32_t vertex = 0; vertex < farLeafVertices; ++vertex) {
                    const MeshVertex& want = level.leaves.vertices[kept * farLeafVertices + vertex];
                    const FarLeafVertex got = farLeafVertex(packed, shape, vertex);
                    assert((got.position - twigPoint(twig, want.position)).length() <= 2e-3f * leaf.side + 1e-6f);
                    assert(got.normal.dot(turned(want.normal).normalized()) > 0.9999f);
                    assert(std::abs(got.uv.x - want.uv.x) < 1e-5f && std::abs(got.uv.y - want.uv.y) < 1e-6f);
                    const Vec3 gotAcross{got.tangent.x, got.tangent.y, got.tangent.z};
                    const Vec3 wantAcross{want.tangent.x, want.tangent.y, want.tangent.z};
                    assert(gotAcross.dot(turned(wantAcross)) > 0.9999f);
                    assert(got.tangent.w == 1.0f && std::abs(across.length() - 1.0f) < 1e-4f);
                }

                for (std::uint32_t triangle = 0; triangle < farLeafTriangles; ++triangle) {
                    const std::array<std::uint32_t, 3> corners = farLeafTriangle(triangle);

                    for (std::uint32_t corner = 0; corner < 3; ++corner) {
                        assert(level.leaves.indices[(kept * farLeafTriangles + triangle) * 3 + corner] == kept * farLeafVertices + corners[corner]);
                    }
                }

                ++checked;
            }
        }

        assert(checked > 1000);
    }
}

void testTwigRecords() {
    testAFarLeafsRecord();
    testFarLeavesAreTheTwigsLevel();
    testPackedSetsMoveTheirRuns();
    testABrickMakesOneChoice(farErrorWorstPercentile);
    testABrickMakesOneChoice(ForestTwigSettings{}.farErrorPercentile);
    testFarNodesOutOfViewAreLeftOut();
    testFarNodesPosedByTheirTree();
    testAFarClustersStandIn();
    testTwigGroupsInTheClusterPasses();
    testATwigsRecord();
    testAPrototypesLevels();
    testATwigsInstance();
    testTheLevelIsTheCuts();
    testTheTwigPassOnTheCpu();
    testAPosedTreesTwigs();
    testTheTwigsLine();
    testTheFarLeavesSwitch();
}
