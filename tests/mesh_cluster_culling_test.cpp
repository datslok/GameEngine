#include "core/command_registry.h"
#include "engine/debug_views.h"
#include "engine/engine_commands.h"
#include "math/frustum.h"
#include "math/mat4.h"
#include "math/transform.h"
#include "cull_helpers.h"
#include "mesh_helpers.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/instance_shared.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "render/gpu/placed_drawing.h"
#include "render/gpu/planting_records.h"
#include "render/gpu/screen_reflections.h"
#include "render/gpu/taa.h"
#include "render/gpu/wind.h"
#include "render/gpu/wind_bones.h"
#include "render/render_stats.h"
#include "scene/twig_far_levels_shared.h"
#include "scene/indexed_mesh.h"
#include "scene/level_of_detail.h"
#include "scene/mesh.h"
#include "scene/mesh_clusters.h"
#include "scene/sway_bones.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace {
    Vec3 worldPoint(const GpuInstance& instance, const Vec3& point) {
        const float* m = instance.model;
        return Vec3{m[0] * point.x + m[4] * point.y + m[8] * point.z + m[12], m[1] * point.x + m[5] * point.y + m[9] * point.z + m[13],
                    m[2] * point.x + m[6] * point.y + m[10] * point.z + m[14]};
    }

    // A cluster's triangle's corners in mesh vertex numbers (the packed vertices less the mesh's first vertex).
    std::array<std::uint32_t, 3> packedTriangle(const PackedMeshClusters& packed, const GpuMeshCluster& cluster, std::uint32_t firstClusterVertex,
                                                std::uint32_t firstTriangleWord, std::uint32_t triangle) {
        std::array<std::uint32_t, 3> corners{};

        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            const std::uint32_t byte = triangle * 3 + corner;
            const std::uint32_t word = packed.triangleWords[cluster.firstTriangleWord - firstTriangleWord + byte / 4];
            const std::uint32_t local = (word >> (8u * (byte % 4))) & 0xFFu;
            corners[corner] = packed.vertices[cluster.firstVertex - firstClusterVertex + local] - cluster.meshFirstVertex;
        }

        return corners;
    }

    /*
    * Each cluster's bones (ROADMAP phase 21 stage 3c-iv-a): its vertices' own bones, ascending, up to the most, the rest
    * empty; the many mark when it has more; every slot empty without bindings; bindings not one a vertex refused. A ball
    * bound by latitude bands has clusters of one bone, of a few, of more than four and of too many, at every level.
    */
    void testPackingKeepsEachClustersBones() {
        const IndexedMesh ball = buildIndexedMesh(Mesh::sphere(64, 32));
        std::vector<SwayBinding> bindings;

        for (const MeshVertex& vertex : ball.vertices) {
            // Broad bands below, thin ones near the top, so clusters hold one bone, a few, or too many.
            const float y = vertex.position.y;
            const int bone = y > 0.4f ? 20 + static_cast<int>(y * 60.0f) : static_cast<int>((y + 1.0f) * 3.0f);
            bindings.push_back(SwayBinding{.bone = bone, .weight = 0.5f});
        }

        for (const ClusterLevels levels : {ClusterLevels::One, ClusterLevels::All}) {
            const MeshClusters built = buildMeshClusters(ball, levels);
            const PackedMeshClusters packed = packMeshClusters(built, 0, 0, 0, bindings);
            std::array<std::size_t, meshClusterMostBones + 2> seen{};

            for (std::size_t k = 0; k < built.clusters.size(); ++k) {
                const MeshCluster& cluster = built.clusters[k];
                std::vector<std::uint32_t> bones;

                for (std::uint32_t vertex = 0; vertex < cluster.vertexCount; ++vertex) {
                    bones.push_back(static_cast<std::uint32_t>(bindings[built.vertices[cluster.firstVertex + vertex]].bone));
                }

                std::sort(bones.begin(), bones.end());
                bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
                const GpuMeshCluster& record = packed.clusters[k];

                if (bones.size() > meshClusterMostBones) {
                    assert(meshClusterBone(record, 0) == meshClusterManyBones);
                    ++seen.back();
                    continue;
                }

                for (std::uint32_t slot = 0; slot < meshClusterMostBones; ++slot) {
                    assert(meshClusterBone(record, slot) == (slot < bones.size() ? bones[slot] : meshClusterNoBone));
                }

                ++seen[bones.size()];
            }

            assert(seen[0] == 0 && seen[1] > 0 && seen[2] + seen[3] + seen[4] > 0 && seen[5] + seen[6] + seen[7] + seen[8] > 0 && seen.back() > 0);
        }

        const PackedMeshClusters unbound = packMeshClusters(buildMeshClusters(ball, ClusterLevels::One), 0, 0, 0);

        for (const GpuMeshCluster& record : unbound.clusters) {
            for (std::uint32_t slot = 0; slot < meshClusterMostBones; ++slot) {
                assert(meshClusterBone(record, slot) == meshClusterNoBone);
            }
        }

        bindings.pop_back();
        assert(throwsInvalid([&] { packMeshClusters(buildMeshClusters(ball, ClusterLevels::One), 0, 0, 0, bindings); }));
    }

    // Packed for the pools: every cluster's vertices and triangles as built, numbered from where they go.
    void testPackingKeepsEveryTriangle() {
        const IndexedMesh ball = buildIndexedMesh(Mesh::sphere(64, 32));
        const MeshClusters built = buildMeshClusters(ball, ClusterLevels::One);
        const PackedMeshClusters packed = packMeshClusters(built, 1000, 50, 70);
        assert(packed.clusters.size() == built.clusters.size());
        assert(packed.vertices.size() == built.vertices.size());
        std::uint32_t nextWord = 70;

        for (std::size_t index = 0; index < built.clusters.size(); ++index) {
            const MeshCluster& cluster = built.clusters[index];
            const GpuMeshCluster& record = packed.clusters[index];
            assert((record.counts & 0xFFu) == cluster.vertexCount);
            assert((record.counts >> MESH_CLUSTER_TRIANGLE_SHIFT) == cluster.triangleCount);
            assert(record.firstVertex == 50 + cluster.firstVertex && record.meshFirstVertex == 1000);
            assert(record.firstTriangleWord == nextWord);
            nextWord += meshClusterTriangleWords(cluster.triangleCount);
            assert(record.sphere[3] == cluster.radius && record.cone[3] == cluster.coneCutoff);

            for (std::uint32_t triangle = 0; triangle < cluster.triangleCount; ++triangle) {
                const std::array<std::uint32_t, 3> corners = packedTriangle(packed, record, 50, 70, triangle);

                for (std::uint32_t corner = 0; corner < 3; ++corner) {
                    const std::uint8_t local = built.triangles[(cluster.firstTriangle + triangle) * 3 + corner];
                    assert(corners[corner] == built.vertices[cluster.firstVertex + local]);
                }
            }
        }

        assert(packed.triangleWords.size() == nextWord - 70);
    }

    /*
    * Random views of rigid instances (turned, scaled unevenly, some mirrored, some double-sided, some shadow views): a
    * cluster culled has no triangle the view could see, each having all its corners outside one plane or (single-sided,
    * not mirrored, not a shadow view) its back to the eye. And the test is of use: a good share culled.
    */
    void testACulledClusterHidesNothing() {
        std::mt19937 random(7);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const IndexedMesh meshes[2] = {buildIndexedMesh(Mesh::sphere(96, 48)), rippledSheet()};
        std::size_t culled = 0;
        std::size_t tested = 0;

        for (int round = 0; round < 60; ++round) {
            const IndexedMesh& mesh = meshes[round % 2];
            const PackedMeshClusters packed = packMeshClusters(buildMeshClusters(mesh, ClusterLevels::One), 0, 0, 0);
            const Vec3 eye{(unit(random) - 0.5f) * 100.0f, 2.0f + unit(random) * 10.0f, (unit(random) - 0.5f) * 100.0f};
            const float yaw = unit(random) * 2.0f * std::numbers::pi_v<float>;
            GpuCameraCull view = testView(eye, yaw, false, 0.0f);
            const bool mirrored = round % 7 == 3;
            const bool doubleSided = round % 5 == 2;

            // A shadow view (the sun's virtual shadow maps' casters, ROADMAP phase 21 4c-i) never culls by facing: the
            // light sees the sides the eye does not.
            const bool shadow = round % 4 == 1;
            view.flags |= shadow ? CAMERA_CULL_SHADOW : 0u;

            for (int copy = 0; copy < 8; ++copy) {
                const Vec3 place = eye + Vec3{(unit(random) - 0.5f) * 30.0f, (unit(random) - 0.5f) * 8.0f, (unit(random) - 0.5f) * 30.0f};
                const Mat4 model = Mat4::translation(place.x, place.y, place.z) * Mat4::rotationY(unit(random) * 6.0f) * Mat4::rotationX(unit(random) * 6.0f) *
                                   Mat4::scaling((mirrored ? -1.0f : 1.0f) * (0.5f + 3.0f * unit(random)), 0.5f + 3.0f * unit(random), 0.5f + 3.0f * unit(random));
                const GpuInstance instance = placedInstance(model);
                GpuCullRecord record = stillRecord(place, 10.0f);
                record.flags |= doubleSided ? CULL_DOUBLE_SIDED : 0u;
                const bool faces = !doubleSided && !shadow && meshClusterDeterminant(instance) > 0.0f;

                for (const GpuMeshCluster& cluster : packed.clusters) {
                    ++tested;

                    if (meshClusterKept(cluster, instance, 0u, record, view)) {
                        continue;
                    }

                    ++culled;

                    for (std::uint32_t triangle = 0; triangle < (cluster.counts >> MESH_CLUSTER_TRIANGLE_SHIFT); ++triangle) {
                        const std::array<std::uint32_t, 3> corners = packedTriangle(packed, cluster, 0, 0, triangle);
                        const Vec3 a = worldPoint(instance, mesh.vertices[corners[0]].position);
                        const Vec3 b = worldPoint(instance, mesh.vertices[corners[1]].position);
                        const Vec3 c = worldPoint(instance, mesh.vertices[corners[2]].position);
                        bool outside = false;

                        for (const auto& plane : view.frustum.planes) {
                            const auto distance = [&](const Vec3& p) { return plane[0] * p.x + plane[1] * p.y + plane[2] * p.z + plane[3]; };
                            outside = outside || (distance(a) < 0.0f && distance(b) < 0.0f && distance(c) < 0.0f);
                        }

                        const Vec3 front = (b - a).cross(c - a);
                        const Vec3 toEye = eye - a;
                        const bool away = faces && front.dot(toEye) <= 1e-4f * std::sqrt(front.dot(front) * toEye.dot(toEye));
                        assert(outside || away);
                    }
                }
            }
        }

        assert(culled > tested / 3);
    }

    // A ball bound to a chain of bones up its height (each vertex to its band's bone, weighted by how far up the band it
    // is; broad bands below, thin ones near the top, so clusters hold one bone, a few or too many), and a pool of the
    // chain's poses, each bone turned up to 0.3 radians about its pivot and carried by the one below.
    struct PosedBall {
        IndexedMesh mesh;
        std::vector<SwayBinding> bindings;
        std::vector<WindBonePose> poses;
        std::vector<GpuBonePose> pool;
        GpuBoneTree tree;
    };

    // A ball vertex where its bones carry it: its bone's pose and the one below, blended by its weight (as the GPU packs it).
    Vec3 ballVertexPosed(const PosedBall& ball, std::size_t vertex) {
        const BoneVertexPoses poses = boneVertexPoses(ball.pool, 0, packSwayBinding(ball.bindings[vertex]), false);
        return windBlendedPlace(poses.own, poses.parent, poses.weight, ball.mesh.vertices[vertex].position);
    }

    PosedBall posedBall(std::mt19937& random) {
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        PosedBall ball;
        ball.mesh = buildIndexedMesh(Mesh::sphere(64, 32));
        constexpr int bones = 81;

        for (const MeshVertex& vertex : ball.mesh.vertices) {
            const float y = vertex.position.y;
            const float band = std::min(y > 0.4f ? 20.0f + y * 60.0f : (y + 1.0f) * 3.0f, bones - 1e-3f);
            ball.bindings.push_back(SwayBinding{.bone = static_cast<int>(band), .weight = band - std::floor(band)});
        }

        for (int bone = 0; bone < bones; ++bone) {
            const WindBonePose below = bone == 0 ? WindBonePose{} : ball.poses.back();
            const Vec3 pivot{0.0f, -1.0f + 2.0f * static_cast<float>(bone) / bones, 0.0f};
            ball.poses.push_back(windBonePose(below, Vec3{unit(random), unit(random), unit(random)} * 0.17f, pivot));
            ball.pool.push_back(packBonePose(ball.poses.back(), bone - 1));
            ball.pool.push_back(packBonePose(ball.poses.back(), bone - 1));
        }

        ball.tree.firstPose = 0;
        ball.tree.lean[0] = 0.25f;
        ball.tree.lean[1] = 0.1f;

        // The tree's move: the farthest any vertex goes.
        for (std::size_t vertex = 0; vertex < ball.mesh.vertices.size(); ++vertex) {
            ball.tree.move = std::max(ball.tree.move, (ballVertexPosed(ball, vertex) - ball.mesh.vertices[vertex].position).length());
        }

        return ball;
    }

    /*
    * Clusters of a tree posed by its bones (ROADMAP phase 21 stage 3c-iv-b), swaying where the view's sway reaches:
    * random views of the posed ball placed (turned, scaled evenly, as plants are), its own clusters (of one bone, a few
    * and too many); a cluster culled by its posed sphere (meshClusterBonesSphere, meshClusterPosed) has every
    * corner of every triangle, blended by its two bones and fluttering as far as it can, outside one plane. The same
    * draw unbound keeps everything (it sways per vertex); bound and not posed it culls as a still draw. A good share culled.
    */
    void testAPosedClusterHidesNothing() {
        std::mt19937 random(23);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const PosedBall ball = posedBall(random);
        const PackedMeshClusters packed = packMeshClusters(buildMeshClusters(ball.mesh, ClusterLevels::One), 0, 0, 0, ball.bindings);
        std::size_t culled = 0;
        std::size_t tested = 0;
        std::size_t many = 0;

        for (int round = 0; round < 40; ++round) {
            const Vec3 eye{(unit(random) - 0.5f) * 100.0f, 2.0f + unit(random) * 10.0f, (unit(random) - 0.5f) * 100.0f};
            const float yaw = unit(random) * 2.0f * std::numbers::pi_v<float>;
            const GpuCameraCull view = testView(eye, yaw, true, 1000.0f);

            for (int copy = 0; copy < 6; ++copy) {
                const Vec3 place = eye + Vec3{(unit(random) - 0.5f) * 30.0f, (unit(random) - 0.5f) * 8.0f, (unit(random) - 0.5f) * 30.0f};
                const float scale = 0.5f + 3.0f * unit(random);
                GpuInstance instance = placedInstance(
                    Transform{.position = place, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 6.0f * unit(random)), .scale = Vec3{scale, scale, scale}}
                        .getMatrix());
                instance.sway[3] = 0.4f * unit(random);
                const float reach = windFlutterReach(instance.sway[3], Vec2{ball.tree.lean[0], ball.tree.lean[1]});
                const GpuCullRecord still = stillRecord(place, 4.0f * scale);
                GpuCullRecord swaying = still;
                swaying.flags |= CULL_SWAYS;

                for (const GpuMeshCluster& cluster : packed.clusters) {
                    if (!meshClusterInCut(cluster, instance, view)) {
                        continue;
                    }

                    ++tested;
                    many += meshClusterBone(cluster, 0) == meshClusterManyBones ? 1u : 0u;
                    assert(meshClusterBound(cluster) && meshClusterKept(cluster, instance, 0u, swaying, view));
                    assert(meshClusterKept(cluster, instance, 0u, swaying, view, MeshClusterPose{.bound = true}) ==
                           meshClusterKept(cluster, instance, 0u, still, view));
                    const Vec4 posed = meshClusterBonesSphere(cluster, ball.pool, ball.tree);
                    const float sphere[4] = {posed.x, posed.y, posed.z, posed.w};
                    const MeshClusterPose pose = meshClusterPosed(sphere, instance, instance.sway[3], Vec2{ball.tree.lean[0], ball.tree.lean[1]});

                    if (meshClusterKept(cluster, instance, 0u, swaying, view, pose)) {
                        continue;
                    }

                    ++culled;
                    bool outside = false;

                    for (const auto& plane : view.frustum.planes) {
                        bool allOut = true;

                        for (std::uint32_t triangle = 0; triangle < (cluster.counts >> MESH_CLUSTER_TRIANGLE_SHIFT); ++triangle) {
                            for (const std::uint32_t corner : packedTriangle(packed, cluster, 0, 0, triangle)) {
                                const Vec3 world = worldPoint(instance, ballVertexPosed(ball, corner));
                                allOut = allOut && plane[0] * world.x + plane[1] * world.y + plane[2] * world.z + plane[3] + reach < 1e-4f;
                            }
                        }

                        outside = outside || allOut;
                    }

                    assert(outside);
                }
            }
        }

        assert(many > 0 && many < tested / 2 && culled > tested / 4);
    }

    /*
    * How a pair's cluster stands (meshClusterPoseAt): a placed plant's bound cluster posed by its tree when claimed with
    * the frame's stamp and given room, else standing still (bound); a frame's draw's bound cluster stands still; an
    * unbound cluster is neither. A twig by its own binding: posed as twigPose has it, unbound without one.
    */
    void testThePoseOfAPair() {
        std::mt19937 random(5);
        const PosedBall ball = posedBall(random);
        const PackedMeshClusters packed = packMeshClusters(buildMeshClusters(ball.mesh, ClusterLevels::One), 0, 0, 0, ball.bindings);
        const PackedMeshClusters unbound = packMeshClusters(buildMeshClusters(ball.mesh, ClusterLevels::One), 0, 0, 0);
        const GpuMeshCluster& cluster = packed.clusters[3];
        GpuBoneTree noRoom = ball.tree;
        noRoom.firstPose = boneNoPose;
        const std::vector<GpuBoneTree> trees{noRoom, ball.tree};
        const std::vector<std::uint32_t> plantTrees{8u, 1u, 9u, 0u, 0u, 0u, 9u, 1u};
        const std::vector<std::uint32_t> twigBindings{packSwayBinding(SwayBinding{.bone = 7, .weight = 0.3f}), swayUnbound};
        MeshClusterTables tables{};
        tables.bones = BoneCullTables{plantTrees, trees, ball.pool, twigBindings, 9u};
        const std::vector<GpuTwig> twigs{packTwig(TwigInstance{.foot = Vec3{0.2f, 0.3f, 0.0f}, .across = Vec3{1.0f, 0.0f, 0.0f}, .towardsTip = Vec3{0.0f, 1.0f, 0.0f},
                                                               .facing = Vec3{0.0f, 0.0f, 1.0f}, .scale = 0.5f, .prototype = 0, .lean = Vec3{}}),
                                         packTwig(TwigInstance{.foot = Vec3{0.0f, -0.3f, 0.1f}, .across = Vec3{1.0f, 0.0f, 0.0f}, .towardsTip = Vec3{0.0f, 1.0f, 0.0f},
                                                               .facing = Vec3{0.0f, 0.0f, 1.0f}, .scale = 0.5f, .prototype = 0, .lean = Vec3{}})};
        tables.twigs.twigs = twigs;

        MeshClusterDrawn drawn;
        drawn.instance = placedInstance(Mat4::translation(4.0f, 0.0f, 1.0f) * Mat4::scaling(2.0f, 2.0f, 2.0f));
        drawn.instance.sway[3] = 0.3f;
        drawn.part = drawn.instance;
        drawn.plant = 3;
        const Vec4 posed = meshClusterBonesSphere(cluster, ball.pool, ball.tree);
        const float sphere[4] = {posed.x, posed.y, posed.z, posed.w};
        const MeshClusterPose expected = meshClusterPosed(sphere, drawn.instance, 0.3f, Vec2{ball.tree.lean[0], ball.tree.lean[1]});
        const MeshClusterPose found = meshClusterPoseAt(tables, drawn, cluster);
        assert(found.bound && found.posed && found.sphere == expected.sphere);

        const auto standsStill = [&](const MeshClusterDrawn& at) {
            const MeshClusterPose pose = meshClusterPoseAt(tables, at, cluster);
            return pose.bound && !pose.posed;
        };
        MeshClusterDrawn other = drawn;
        other.plant = 1; // claimed, given no room
        assert(standsStill(other));
        other.plant = 0; // claimed by another frame
        assert(standsStill(other));
        other.plant = 40; // past the table
        assert(standsStill(other));
        other.plant = boneNoTree; // a frame's draw
        assert(standsStill(other));
        assert(!meshClusterPoseAt(tables, drawn, unbound.clusters[3]).bound);
        tables.bones.stamp = 10;
        assert(standsStill(drawn));
        tables.bones.stamp = 9;

        MeshClusterDrawn twig = drawn;
        twig.twig = 0;
        const MeshClusterPose twigFound = meshClusterPoseAt(tables, twig, unbound.clusters[3]);
        GpuTwigLevel unboundOwn;
        std::copy(std::begin(unbound.clusters[3].sphere), std::end(unbound.clusters[3].sphere), std::begin(unboundOwn.sphere));
        const MeshClusterPose twigExpected = twigPose(unboundOwn, drawn.part, twigs[0], twigBindings[0], ball.pool, ball.tree);
        assert(twigFound.posed && twigFound.sphere == twigExpected.sphere);
        twig.twig = 1;
        assert(!meshClusterPoseAt(tables, twig, unbound.clusters[3]).bound);
    }

    // A draw not rigid keeps every cluster, even one far out of view: terrain, an impostor, ground-following, a liana,
    // thinned cards, swaying within the sway's reach; past the reach a swaying draw is rigid again.
    void testADrawNotRigidKeepsEverything() {
        const PackedMeshClusters packed = packMeshClusters(buildMeshClusters(buildIndexedMesh(Mesh::sphere(16, 8)), ClusterLevels::One), 0, 0, 0);
        const GpuMeshCluster& cluster = packed.clusters.front();
        const GpuCameraCull view = testView(Vec3{0.0f, 2.0f, 0.0f}, 0.0f, true, 100.0f);
        const Vec3 behind{-40.0f, 2.0f, 0.0f};
        const GpuInstance plain = placedInstance(Mat4::translation(behind.x, behind.y, behind.z));
        const GpuCullRecord record = stillRecord(behind, 1.0f);
        assert(!meshClusterKept(cluster, plain, 0u, record, view));

        GpuInstance terrain = plain;
        terrain.material[2] = 1;
        GpuInstance impostor = plain;
        impostor.material[3] = INSTANCE_KIND_IMPOSTOR;
        GpuInstance following = plain;
        following.material[3] = INSTANCE_KIND_FOLLOWS_GROUND;
        GpuInstance liana = plain;
        liana.hanging[3] = 1;

        for (const GpuInstance& instance : {terrain, impostor, following, liana}) {
            assert(meshClusterKept(cluster, instance, 0u, record, view));
        }

        assert(meshClusterKept(cluster, plain, DRAW_LIST_THINNED_BIT, record, view));

        GpuCullRecord swaying = record;
        swaying.flags |= CULL_SWAYS;
        assert(meshClusterKept(cluster, plain, 0u, swaying, view));

        const GpuCameraCull still = testView(Vec3{0.0f, 2.0f, 0.0f}, 0.0f, false, 100.0f);
        assert(!meshClusterKept(cluster, plain, 0u, swaying, still));

        const GpuCameraCull shortReach = testView(Vec3{0.0f, 2.0f, 0.0f}, 0.0f, true, 10.0f);
        assert(!meshClusterKept(cluster, plain, 0u, swaying, shortReach));
    }

    // The clusters the cut draws, by number.
    std::vector<std::uint32_t> cutClusters(const PackedMeshClusters& packed, const GpuInstance& instance, const GpuCameraCull& view) {
        std::vector<std::uint32_t> drawn;

        for (std::uint32_t number = 0; number < packed.clusters.size(); ++number) {
            if (meshClusterInCut(packed.clusters[number], instance, view)) {
                drawn.push_back(number);
            }
        }

        return drawn;
    }

    // Whether a packed sphere is a built one.
    bool sameSphere(const float packed[4], const Vec3& centre, float radius) {
        return packed[0] == centre.x && packed[1] == centre.y && packed[2] == centre.z && packed[3] == radius;
    }

    /*
    * Packed for the cut: each record's own error and sphere are those of the group it was made from (0 and its own
    * sphere for the mesh's own triangles), its parent's those of the group it was simplified with.
    */
    void testPackingKeepsTheCut() {
        const MeshClusters built = buildMeshClusters(buildIndexedMesh(Mesh::sphere(64, 32)), ClusterLevels::All);
        const PackedMeshClusters packed = packMeshClusters(built, 0, 0, 0);
        assert(built.groups.size() > 1 && built.clusters.size() > built.originalCount);

        for (std::size_t index = 0; index < built.clusters.size(); ++index) {
            const MeshCluster& cluster = built.clusters[index];
            const GpuMeshCluster& record = packed.clusters[index];
            const MeshClusterGroup& parent = built.groups[cluster.group];
            assert(record.parentError == parent.error && sameSphere(record.parentSphere, parent.centre, parent.radius));

            if (cluster.refinedGroup == meshClusterNoGroup) {
                assert(record.ownError == 0.0f && sameSphere(record.ownSphere, cluster.centre, cluster.radius));
            } else {
                const MeshClusterGroup& own = built.groups[cluster.refinedGroup];
                assert(record.ownError == own.error && sameSphere(record.ownSphere, own.centre, own.radius));
            }
        }
    }

    /*
    * The cut of a sphere with levels, placed turned, scaled unevenly or mirrored, seen from inside it to far away under
    * several pixel errors: always closed and within its error of the sphere's volume (so no part drawn at two levels);
    * with almost no error allowed the mesh's own clusters exactly, far away only the coarsest; never more triangles as
    * the eye backs away from outside. A mesh with one level draws everything however far; a draw not rigid keeps
    * exactly the cut.
    */
    void testTheCut() {
        const IndexedMesh ball = buildIndexedMesh(Mesh::sphere(96, 48));
        const MeshClusters built = buildMeshClusters(ball, ClusterLevels::All);
        const PackedMeshClusters packed = packMeshClusters(built, 0, 0, 0);
        const std::vector<std::uint32_t> numbers = positionNumbers(ball);
        std::vector<std::uint32_t> originals(built.originalCount);
        std::vector<std::uint32_t> coarsest;

        for (std::uint32_t number = 0; number < packed.clusters.size(); ++number) {
            if (number < built.originalCount) {
                originals[number] = number;
            }

            if (packed.clusters[number].parentError > static_cast<float>(MESH_CLUSTER_NEVER_COARSER)) {
                coarsest.push_back(number);
            }
        }

        const float volume = enclosedVolume(ball, built, originals);
        const float area = 4.0f * std::numbers::pi_v<float>;
        std::mt19937 random(5);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<std::uint32_t> triangleCounts;

        for (int round = 0; round < 12; ++round) {
            Vec3 direction{unit(random) - 0.5f, unit(random) - 0.5f, unit(random) - 0.5f};
            direction = direction * (1.0f / direction.length());
            const Vec3 place{(unit(random) - 0.5f) * 200.0f, unit(random) * 20.0f, (unit(random) - 0.5f) * 200.0f};
            const float scales[3] = {(round % 5 == 3 ? -1.0f : 1.0f) * (0.5f + 3.0f * unit(random)), 0.5f + 3.0f * unit(random), 0.5f + 3.0f * unit(random)};
            const GpuInstance instance = placedInstance(Mat4::translation(place.x, place.y, place.z) * Mat4::rotationY(unit(random) * 6.0f) *
                                                        Mat4::rotationX(unit(random) * 6.0f) * Mat4::scaling(scales[0], scales[1], scales[2]));
            const float largest = meshClusterLargestScale(instance);
            const float pixelError = round % 3 == 0 ? 0.25f : round % 3 == 1 ? 1.0f : 4.0f;
            const float yaw = unit(random) * 6.0f;

            assert(cutClusters(packed, instance, cutView(place + direction * 0.2f, yaw, 1e-6f)) == originals);
            std::uint32_t previous = 0xFFFFFFFFu;

            for (float distance = 0.2f; distance < 2e5f; distance *= 1.6f) {
                const std::vector<std::uint32_t> drawn = cutClusters(packed, instance, cutView(place + direction * distance, yaw, pixelError));
                assert(!drawn.empty());
                assert(openEdges(ball, built, numbers, drawn).empty());
                std::uint32_t triangles = 0;
                float largestError = 0.0f;

                for (std::uint32_t number : drawn) {
                    triangles += packed.clusters[number].counts >> MESH_CLUSTER_TRIANGLE_SHIFT;
                    largestError = std::max(largestError, packed.clusters[number].ownError);
                }

                assert(std::abs(enclosedVolume(ball, built, drawn) - volume) <= area * largestError + 1e-4f * volume);

                if (distance > 1.2f * largest) {
                    assert(triangles <= previous);
                    previous = triangles;
                }

                if (std::find(triangleCounts.begin(), triangleCounts.end(), triangles) == triangleCounts.end()) {
                    triangleCounts.push_back(triangles);
                }
            }

            assert(cutClusters(packed, instance, cutView(place + direction * 1e6f, yaw, pixelError)) == coarsest);
        }

        // The cut meets many levels on the way out.
        assert(triangleCounts.size() >= 4);

        // One level: everything, near or far.
        const PackedMeshClusters one = packMeshClusters(buildMeshClusters(ball, ClusterLevels::One), 0, 0, 0);
        const GpuInstance still = placedInstance(Mat4::translation(0.0f, 0.0f, -10.0f));

        for (float distance : {0.0f, 3.0f, 1e6f}) {
            assert(cutClusters(one, still, cutView(Vec3{0.0f, 0.0f, distance}, 0.0f, 1.0f)).size() == one.clusters.size());
        }

        // A draw not rigid (a liana's, behind the eye) keeps exactly its cut.
        GpuInstance liana = placedInstance(Mat4::translation(-60.0f, 2.0f, 0.0f));
        liana.hanging[3] = 1;
        const GpuCameraCull view = cutView(Vec3{0.0f, 2.0f, 0.0f}, 0.0f, 1.0f);
        const GpuCullRecord record = stillRecord(Vec3{-60.0f, 2.0f, 0.0f}, 1.0f);
        std::size_t kept = 0;

        for (const GpuMeshCluster& cluster : packed.clusters) {
            assert(meshClusterKept(cluster, liana, 0u, record, view) == meshClusterInCut(cluster, liana, view));
            kept += meshClusterKept(cluster, liana, 0u, record, view) ? 1u : 0u;
        }

        assert(kept > 0 && kept < built.originalCount);
    }

    // A small scene for the passes: three meshes' clusters in one pool, groups of them in four styles' runs (one empty),
    // instances in rows before the eye, each group's commands' instances a run of the draw list.
    struct PassScene {
        std::vector<GpuMeshCluster> clusters;
        std::vector<GpuCullGroup> groups;
        std::vector<GpuDrawCommand> commands;
        std::vector<std::uint32_t> drawOrder;
        std::vector<std::uint32_t> drawList;
        std::vector<GpuInstance> instances;
        std::vector<GpuCullRecord> records;
        GpuCameraCull view;

        MeshClusterTables tables() const {
            return MeshClusterTables{commands, drawOrder, groups, drawList, instances, records, clusters};
        }
    };

    PassScene passScene(std::mt19937& random) {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        PassScene scene;
        const IndexedMesh meshes[3] = {buildIndexedMesh(Mesh::sphere(48, 24)), rippledSheet(), buildIndexedMesh(Mesh::sphere(12, 6))};
        std::vector<std::pair<std::uint32_t, std::uint32_t>> meshClusters;

        for (const IndexedMesh& mesh : meshes) {
            const PackedMeshClusters packed = packMeshClusters(buildMeshClusters(mesh, ClusterLevels::One), 0, 0, 0);
            meshClusters.emplace_back(static_cast<std::uint32_t>(scene.clusters.size()), static_cast<std::uint32_t>(packed.clusters.size()));
            scene.clusters.insert(scene.clusters.end(), packed.clusters.begin(), packed.clusters.end());
        }

        const Vec3 eye{0.0f, 2.0f, 0.0f};
        scene.view = testView(eye, 0.0f, true, 20.0f);
        const std::uint32_t groupsInStyle[MESH_CLUSTER_STYLES] = {3, 0, 2, 1};

        for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
            scene.view.runFirst[style] = static_cast<std::uint32_t>(scene.drawOrder.size());
            scene.view.runCount[style] = groupsInStyle[style];

            for (std::uint32_t member = 0; member < groupsInStyle[style]; ++member) {
                const std::uint32_t group = static_cast<std::uint32_t>(scene.groups.size());
                const auto [firstCluster, clusterCount] = meshClusters[group % 3];
                GpuCullGroup culled;
                culled.firstCluster = firstCluster;
                culled.clusterCount = clusterCount;
                scene.groups.push_back(culled);

                // Some groups draw nothing this frame.
                const std::uint32_t instances = group == 4 ? 0u : 1u + static_cast<std::uint32_t>(unit(random) * 6.0f);
                scene.commands.push_back(GpuDrawCommand{0, instances, 0, 0, static_cast<std::uint32_t>(scene.drawList.size())});

                for (std::uint32_t copy = 0; copy < instances; ++copy) {
                    const Vec3 place{5.0f + unit(random) * 40.0f, unit(random) * 4.0f, (unit(random) - 0.5f) * 40.0f};
                    const std::uint32_t draw = static_cast<std::uint32_t>(scene.instances.size());
                    scene.instances.push_back(placedInstance(Mat4::translation(place.x, place.y, place.z) * Mat4::rotationY(unit(random) * 6.0f)));
                    GpuCullRecord record = stillRecord(place, 2.0f);
                    record.flags |= (style & 1u) != 0u ? CULL_DOUBLE_SIDED : 0u;
                    record.flags |= copy % 3 == 1 ? CULL_SWAYS : 0u;
                    scene.records.push_back(record);
                    scene.drawList.push_back(draw | (copy % 4 == 2 ? DRAW_LIST_THINNED_BIT : 0u));
                }
            }

            for (std::uint32_t member = 0; member < groupsInStyle[style]; ++member) {
                scene.drawOrder.push_back(scene.view.runFirst[style] + member);
            }
        }

        return scene;
    }

    /*
    * The passes' steps on the CPU equal a plain walk of the drawing order (style by style, group, instance, cluster),
    * each style's run where its survivors are, every style from a block's first pair; within a smaller room, the walk's
    * first pairs and the runs cut to it.
    */
    void testThePassesEqualAPlainWalk() {
        std::mt19937 random(11);

        for (int round = 0; round < 6; ++round) {
            const PassScene scene = passScene(random);
            const MeshClusterTables tables = scene.tables();
            std::vector<GpuMeshClusterItem> walked;
            std::array<std::uint32_t, MESH_CLUSTER_STYLES> perStyle{};
            std::uint32_t pairs = 0;

            for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
                for (std::uint32_t place = scene.view.runFirst[style]; place < scene.view.runFirst[style] + scene.view.runCount[style]; ++place) {
                    const std::uint32_t group = scene.drawOrder[place];
                    const GpuDrawCommand& command = scene.commands[group];

                    for (std::uint32_t instance = 0; instance < command.instanceCount; ++instance) {
                        for (std::uint32_t cluster = 0; cluster < scene.groups[group].clusterCount; ++cluster) {
                            ++pairs;
                            const std::uint32_t entry = scene.drawList[command.firstInstance + instance];
                            const std::uint32_t draw = entry & DRAW_LIST_INSTANCE_MASK;
                            const std::uint32_t number = scene.groups[group].firstCluster + cluster;

                            if (meshClusterKept(scene.clusters[number], scene.instances[draw], entry, scene.records[draw], scene.view)) {
                                walked.push_back(GpuMeshClusterItem{command.firstInstance + instance, number});
                                ++perStyle[style];
                            }
                        }
                    }
                }
            }

            std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS> work{};
            meshClusterWorkEnds(tables, scene.view, work);

            for (std::uint32_t style = 0; style <= MESH_CLUSTER_STYLES; ++style) {
                assert(work[MESH_CLUSTER_WORK_STYLE_FIRST + style] % meshClusterGroupSize == 0);
            }

            const MeshClusterList list = cullMeshClustersOnCpu(tables, scene.view, 1u << 20);
            assert(list.items == walked);
            assert(list.tested == pairs && list.kept == walked.size());
            assert(walked.size() < pairs);
            std::uint32_t triangles = 0;

            for (const GpuMeshClusterItem& item : walked) {
                triangles += scene.clusters[item.cluster].counts >> MESH_CLUSTER_TRIANGLE_SHIFT;
            }

            assert(list.triangles == triangles);

            // The bound the buffers are sized by holds the pairs (each group's candidates: here its drawn instances).
            CameraCullFrame frame;
            frame.groups = scene.groups;
            std::vector<GpuCullSegment> segments;

            for (std::size_t group = 0; group < scene.groups.size(); ++group) {
                frame.groups[group].firstSegment = static_cast<std::uint32_t>(segments.size());
                frame.groups[group].segments = 1;
                segments.push_back(GpuCullSegment{0, scene.commands[group].instanceCount, cullNoChunk, 0});
            }

            frame.movingSegments = segments;
            std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS> bounded{};
            meshClusterWorkEnds(tables, scene.view, bounded);
            assert(meshClusterPairBound(frame) >= bounded[MESH_CLUSTER_WORK_END]);
            std::uint32_t next = 0;

            for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
                assert(list.runFirst[style] == next && list.runCount[style] == perStyle[style]);
                next += perStyle[style];
            }

            const std::uint32_t room = static_cast<std::uint32_t>(walked.size() / 2);
            const MeshClusterList cut = cullMeshClustersOnCpu(tables, scene.view, room);
            assert(cut.items.size() == room && std::equal(cut.items.begin(), cut.items.end(), walked.begin()));
            assert(cut.kept == walked.size());

            for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
                assert(cut.runFirst[style] + cut.runCount[style] <= room);
            }
        }
    }

    // The scene's draws as a pooled drawing order (its runs the view's, every place pooled).
    CullDrawOrder passOrder(const PassScene& scene) {
        CullDrawOrder order;
        order.order = scene.drawOrder;

        for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
            order.runFirst[style] = scene.view.runFirst[style];
            order.runCount[style] = scene.view.runCount[style];
        }

        order.pooled = static_cast<std::uint32_t>(scene.drawOrder.size());
        return order;
    }

    // The view drawing that order's runs.
    GpuCameraCull viewOfOrder(GpuCameraCull view, const CullDrawOrder& order) {
        for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
            view.runFirst[style] = order.runFirst[style];
            view.runCount[style] = order.runCount[style];
        }

        return view;
    }

    /*
    * The pass scene's draws as placed plants (render/gpu/placed_drawing.h): each draw a plant's part, a placed group a
    * draw group with the same clusters and commands from placedInstanceBase. entities: the same scene with each
    * instance and record placed from its plant and part as the vertex shaders and passes do, so both must cull alike.
    */
    struct PlacedPassScene {
        PassScene entities;
        std::vector<GpuDrawCommand> commands;
        std::vector<GpuPlacedGroup> groups;
        std::vector<GpuPlacedEntry> entries;
        std::vector<GpuInstance> partInstances;
        std::vector<GpuPlacedPart> parts;
        std::vector<GpuPlacedPlant> plants;
        CullDrawOrder order;

        MeshClusterPlacedTables tables() const {
            return MeshClusterPlacedTables{commands, groups, entries, partInstances, parts, plants};
        }
    };

    PlacedPassScene placedPassScene(std::mt19937& random) {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        PlacedPassScene placed;
        placed.entities = passScene(random);
        PassScene& scene = placed.entities;
        placed.order = passOrder(scene);

        for (std::size_t group = 0; group < scene.groups.size(); ++group) {
            GpuPlacedGroup placedGroup;
            placedGroup.firstCluster = scene.groups[group].firstCluster;
            placedGroup.clusterCount = scene.groups[group].clusterCount;
            placed.groups.push_back(placedGroup);
            GpuDrawCommand command = scene.commands[group];
            command.firstInstance += placedInstanceBase;
            placed.commands.push_back(command);
        }

        for (std::size_t draw = 0; draw < scene.instances.size(); ++draw) {
            const GpuPlacedPlant plant{{5.0f + unit(random) * 40.0f, unit(random) * 2.0f, (unit(random) - 0.5f) * 40.0f}, 0.6f + unit(random) * 0.8f,
                                       unit(random) * 6.0f, 0.5f, 0, 0.0f};
            const Vec3 offset{(unit(random) - 0.5f) * 2.0f, unit(random) * 2.0f, (unit(random) - 0.5f) * 2.0f};
            GpuInstance part = placedInstance(Mat4::translation(offset.x, offset.y, offset.z) * Mat4::rotationY(unit(random) * 6.0f));
            GpuPlacedPart placedPart;
            placedPart.record = stillRecord(offset, 2.0f);
            placedPart.record.flags = scene.records[draw].flags;
            placed.partInstances.push_back(part);
            placed.parts.push_back(placedPart);
            placed.plants.push_back(plant);
            placed.entries.push_back(
                GpuPlacedEntry{static_cast<std::uint32_t>(draw) | (scene.drawList[draw] & ~DRAW_LIST_INSTANCE_MASK), static_cast<std::uint32_t>(draw)});
            scene.instances[draw] = ::placedInstance(part, plant);
            scene.records[draw] = placedCullRecord(placedPart.record, plant);
        }

        return placed;
    }

    // A list's items of one style.
    std::vector<GpuMeshClusterItem> styleItems(const MeshClusterList& list, std::uint32_t style) {
        const auto first = list.items.begin() + list.runFirst[style];
        return std::vector<GpuMeshClusterItem>(first, first + list.runCount[style]);
    }

    /*
    * The drawing order the passes walk: each style's run of the draws' pooled groups, then its run of the placed groups
    * (numbered after the draws' groups); the draws' unpooled groups left out.
    */
    void testTheDrawingOrderTakesThePlacedGroups() {
        CullDrawOrder draws;
        draws.order = {4, 1, 0, 3, 2};
        draws.runFirst = {0, 2, 2, 3};
        draws.runCount = {2, 0, 1, 0};
        draws.pooled = 3;
        CullDrawOrder placed;
        placed.order = {2, 0, 3, 1};
        placed.runFirst = {0, 1, 2, 2};
        placed.runCount = {1, 1, 0, 2};
        placed.pooled = 4;
        const CullDrawOrder both = meshClusterDrawOrder(draws, 5, placed);
        assert((both.order == std::vector<std::uint32_t>{4, 1, 7, 5, 0, 8, 6}));
        assert((both.runFirst == std::array<std::uint32_t, cullStyles>{0, 3, 4, 5}));
        assert((both.runCount == std::array<std::uint32_t, cullStyles>{3, 1, 1, 2}));
        assert(both.pooled == 7);

        // Without placed plants, the draws' pooled runs alone.
        const CullDrawOrder alone = meshClusterDrawOrder(draws, 5, CullDrawOrder{});
        assert((alone.order == std::vector<std::uint32_t>{4, 1, 0}));
        assert((alone.runCount == std::array<std::uint32_t, cullStyles>{2, 0, 1, 0}) && alone.pooled == 3);
    }

    /*
    * A placed plant's part culls as a frame's draw placed where its plant puts it: alone, the same pairs from the placed
    * entries (their places from placedInstanceBase); with the draws, each style's draws' pairs then its placed ones.
    * Each pair's drawn instance, entry and record, and its edges, are the entity's.
    */
    void testPlacedPlantsCullAsTheirEntities() {
        std::mt19937 random(23);

        for (int round = 0; round < 4; ++round) {
            const PlacedPassScene placed = placedPassScene(random);
            const PassScene& scene = placed.entities;
            const MeshClusterList asDraws = cullMeshClustersOnCpu(scene.tables(), scene.view, 1u << 20);
            assert(asDraws.kept > 0 && asDraws.kept < asDraws.tested);

            const CullDrawOrder placedOnly = meshClusterDrawOrder(CullDrawOrder{}, 0, placed.order);
            MeshClusterTables alone{{}, placedOnly.order, {}, {}, {}, {}, scene.clusters, placed.tables()};
            const GpuCameraCull aloneView = viewOfOrder(scene.view, placedOnly);
            const MeshClusterList asPlaced = cullMeshClustersOnCpu(alone, aloneView, 1u << 20);
            assert(asPlaced.items.size() == asDraws.items.size());
            assert(asPlaced.tested == asDraws.tested && asPlaced.kept == asDraws.kept && asPlaced.triangles == asDraws.triangles);
            assert(asPlaced.runFirst == asDraws.runFirst && asPlaced.runCount == asDraws.runCount);

            for (std::size_t item = 0; item < asDraws.items.size(); ++item) {
                const GpuMeshClusterItem& draw = asDraws.items[item];
                const GpuMeshClusterItem& plant = asPlaced.items[item];
                assert(plant.entry == draw.entry + placedInstanceBase && plant.cluster == draw.cluster);

                const MeshClusterDrawn drawnAsDraw = meshClusterDrawnAt(scene.tables(), draw.entry);
                const MeshClusterDrawn drawnAsPlant = meshClusterDrawnAt(alone, plant.entry);
                assert(std::equal(std::begin(drawnAsDraw.instance.model), std::end(drawnAsDraw.instance.model), std::begin(drawnAsPlant.instance.model)));
                assert(drawnAsDraw.entry == drawnAsPlant.entry && drawnAsDraw.entry == scene.drawList[draw.entry]);
                assert(std::equal(std::begin(drawnAsDraw.record.bounds), std::end(drawnAsDraw.record.bounds), std::begin(drawnAsPlant.record.bounds)));
                assert(nearAMeshClusterEdge(scene.tables(), draw, scene.view) == nearAMeshClusterEdge(alone, plant, aloneView));
            }

            const CullDrawOrder both = meshClusterDrawOrder(passOrder(scene), static_cast<std::uint32_t>(scene.groups.size()), placed.order);
            MeshClusterTables together = scene.tables();
            together.drawOrder = both.order;
            together.placed = placed.tables();
            const GpuCameraCull togetherView = viewOfOrder(scene.view, both);
            const MeshClusterList asBoth = cullMeshClustersOnCpu(together, togetherView, 1u << 20);
            assert(asBoth.tested == 2 * asDraws.tested && asBoth.kept == 2 * asDraws.kept && asBoth.triangles == 2 * asDraws.triangles);

            for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
                std::vector<GpuMeshClusterItem> expected = styleItems(asDraws, style);
                const std::vector<GpuMeshClusterItem> plants = styleItems(asPlaced, style);
                expected.insert(expected.end(), plants.begin(), plants.end());
                assert(styleItems(asBoth, style) == expected);
            }

            // The bound on pairs holds the placed ones too: the draws' own and the placed groups' at most.
            CameraCullFrame frame;
            frame.groups = scene.groups;

            for (std::size_t group = 0; group < scene.groups.size(); ++group) {
                frame.groups[group].firstSegment = static_cast<std::uint32_t>(frame.movingSegments.size());
                frame.groups[group].segments = 1;
                frame.movingSegments.push_back(GpuCullSegment{0, scene.commands[group].instanceCount, cullNoChunk, 0});
            }

            std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS> work{};
            meshClusterWorkEnds(together, togetherView, work);
            assert(2 * meshClusterPairBound(frame) >= work[MESH_CLUSTER_WORK_END]);
        }
    }

    /*
    * The placed plants' bound on pairs for a view: each of its items' blocks' plants (placedGroupSize of them) times the
    * most any kind of the block's layer can test, every level slot of each of its parts (a plant draws at most two slots
    * of a part, while fading, so this holds); other views' items left out.
    */
    void testThePlacedPairBound() {
        PlacedTable table;
        table.groups.resize(6);
        const std::uint32_t clusters[6] = {10, 4, 7, 20, 5, 0};

        for (std::size_t group = 0; group < 6; ++group) {
            table.groups[group].clusterCount = clusters[group];
        }

        table.parts.resize(3);
        table.parts[0].levels.levels = 1;
        table.parts[0].firstGroup = 0;
        table.parts[1].levels.levels = 0;
        table.parts[1].firstGroup = 2;
        table.parts[2].levels.levels = 2;
        table.parts[2].firstGroup = 3;
        table.kinds.resize(2 * static_cast<std::size_t>(plantingMostKinds));
        table.kinds[0].parts = 2;
        table.kinds[0].part[0] = 0;
        table.kinds[0].part[1] = 1;
        table.kinds[1].parts = 1;
        table.kinds[1].part[0] = 2;
        table.kinds[plantingMostKinds].parts = 1;
        table.kinds[plantingMostKinds].part[0] = 1;

        const std::vector<GpuPlacedBlock> blocks{GpuPlacedBlock{0, 0, 0, 0}, GpuPlacedBlock{64, 1, 0, static_cast<std::uint32_t>(plantingMostKinds)}};
        const std::vector<GpuPlacedItem> items{GpuPlacedItem{0, 0}, GpuPlacedItem{1, 0}, GpuPlacedItem{0, 1}, GpuPlacedItem{0, 0}};
        GpuPlacedView view;
        view.firstItem = 0;
        view.items = 2;
        assert(placedMeshClusterPairBound(table, blocks, items, view) == placedGroupSize * (25 + 7));
        view.firstItem = 2;
        view.items = 2;
        assert(placedMeshClusterPairBound(table, blocks, items, view) == placedGroupSize * (25 + 25));
        view.items = 0;
        assert(placedMeshClusterPairBound(table, blocks, items, view) == 0);
    }

    // The way testViewProjection looks, at unit length.
    Vec3 testForward(float yaw) {
        return Vec3{std::cos(yaw), -0.1f, std::sin(yaw)}.normalized();
    }

    // The depth (reversed-Z) of a point this far straight ahead: every point at that distance along the view has it.
    float depthAhead(const Mat4& relativeViewProjection, float yaw, float distance) {
        const Vec3 ahead = testForward(yaw) * distance;
        const Vec4 clip = relativeViewProjection * Vec4{ahead.x, ahead.y, ahead.z, 1.0f};
        return clip.z / clip.w;
    }

    // A depth image of sky (the far depth) and a few walls at random distances, each a rectangle of the screen, the
    // nearest wall winning where they overlap; leftOut: a wall not drawn (its numbers still drawn from random).
    std::vector<float> wallDepths(std::mt19937& random, RenderSize size, const Mat4& relativeViewProjection, float yaw, int leftOut = -1) {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<float> depths(static_cast<std::size_t>(size.width) * size.height, gpuFarDepth);

        for (int wall = 0; wall < 6; ++wall) {
            const int x0 = static_cast<int>(unit(random) * static_cast<float>(size.width));
            const int y0 = static_cast<int>(unit(random) * static_cast<float>(size.height));
            const int x1 = std::min(static_cast<int>(size.width), x0 + 1 + static_cast<int>(unit(random) * static_cast<float>(size.width) * 0.6f));
            const int y1 = std::min(static_cast<int>(size.height), y0 + 1 + static_cast<int>(unit(random) * static_cast<float>(size.height) * 0.8f));
            const float depth = depthAhead(relativeViewProjection, yaw, 3.0f + unit(random) * 40.0f);

            if (wall == leftOut) {
                continue;
            }

            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    float& at = depths[static_cast<std::size_t>(y) * size.width + static_cast<std::size_t>(x)];
                    at = std::max(at, depth);
                }
            }
        }

        return depths;
    }

    // A random sphere (centre measured from the camera, radius) ahead of a camera at the origin looking along yaw.
    std::array<float, 4> sphereAhead(std::mt19937& random, float yaw) {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const float turn = yaw + (unit(random) - 0.5f) * 1.4f;
        const Vec3 direction = Vec3{std::cos(turn), -0.1f + (unit(random) - 0.5f) * 0.8f, std::sin(turn)}.normalized();
        const Vec3 centre = direction * (1.0f + unit(random) * 80.0f);
        return {centre.x, centre.y, centre.z, 0.05f + unit(random) * 4.0f};
    }

    /*
    * The screen box holds the sphere: every point on it lands within the box's pixels, no nearer than its nearest depth;
    * a sphere the camera's plane cuts has none.
    */
    void testScreenBoxHoldsTheSphere() {
        std::mt19937 random(5);
        std::normal_distribution<float> gauss(0.0f, 1.0f);
        const Vec2 size{191.0f, 107.0f};
        int bounded = 0;

        for (int round = 0; round < 400; ++round) {
            const float yaw = static_cast<float>(round) * 0.37f;
            const Mat4 viewProjection = testRelativeViewProjection(Vec3{}, yaw);
            const std::array<float, 4> sphere = sphereAhead(random, yaw);
            const MeshClusterScreenBox box = meshClusterScreenBox(sphere, viewProjection, size);

            if (!box.onScreen) {
                continue;
            }

            ++bounded;
            assert(box.box[0] <= box.box[2] && box.box[1] <= box.box[3]);

            for (int sample = 0; sample < 300; ++sample) {
                const Vec3 offset = Vec3{gauss(random), gauss(random), gauss(random)}.normalized() * sphere[3];
                const Vec4 clip = viewProjection * Vec4{sphere[0] + offset.x, sphere[1] + offset.y, sphere[2] + offset.z, 1.0f};
                assert(clip.w > 0.0f);
                const Vec2 pixel = ndcToPixel(Vec2{clip.x / clip.w, clip.y / clip.w}, size);
                const float slack = 1e-3f * std::max(1.0f, std::fabs(pixel.x) + std::fabs(pixel.y));
                assert(pixel.x >= box.box[0] - slack && pixel.x <= box.box[2] + slack);
                assert(pixel.y >= box.box[1] - slack && pixel.y <= box.box[3] + slack);
                assert(clip.z / clip.w <= box.nearestDepth + 1e-6f);
            }
        }

        assert(bounded > 300);
        const Mat4 viewProjection = testRelativeViewProjection(Vec3{}, 0.0f);
        assert(!meshClusterScreenBox({0.0f, 0.0f, 0.0f, 1.0f}, viewProjection, size).onScreen);
        assert(!meshClusterScreenBox({-5.0f, 0.0f, 0.0f, 1.0f}, viewProjection, size).onScreen);
    }

    /*
    * The pyramid hides nothing with a pixel nearer than it: a hidden sphere's box covers only pixels farther than its
    * nearest point, read at a level where it spans at most two cells each way; screens of odd sizes and one pixel thin.
    */
    void testOcclusionHidesNothingSeen() {
        std::mt19937 random(9);
        int hidden = 0;
        int tested = 0;

        for (const RenderSize size : {RenderSize{191, 107}, RenderSize{64, 64}, RenderSize{1, 5}, RenderSize{33, 1}}) {
            const float yaw = 0.3f;
            const Mat4 viewProjection = testRelativeViewProjection(Vec3{}, yaw);
            const std::vector<float> depths = wallDepths(random, size, viewProjection, yaw);
            const DepthPyramid pyramid = buildDepthPyramid(depths, size, DepthReduction::Farthest);
            const Vec2 screen{static_cast<float>(size.width), static_cast<float>(size.height)};

            for (int round = 0; round < 600; ++round) {
                const std::array<float, 4> sphere = sphereAhead(random, yaw);
                ++tested;

                if (!meshClusterHidden(pyramid, sphere, viewProjection)) {
                    continue;
                }

                ++hidden;
                const MeshClusterScreenBox box = meshClusterScreenBox(sphere, viewProjection, screen);
                assert(box.onScreen);
                const std::array<int, 4> pixels = meshClusterBoxPixels(box.box, static_cast<int>(size.width), static_cast<int>(size.height));
                assert(pixels[0] <= pixels[2] && pixels[1] <= pixels[3]);

                for (int y = pixels[1]; y <= pixels[3]; ++y) {
                    for (int x = pixels[0]; x <= pixels[2]; ++x) {
                        assert(depths[static_cast<std::size_t>(y) * size.width + static_cast<std::size_t>(x)] > box.nearestDepth);
                    }
                }

                const std::uint32_t level = meshClusterPyramidLevel(pixels, static_cast<std::uint32_t>(pyramid.sizes.size()));
                const int width = static_cast<int>(pyramid.sizes[level].width);
                const int height = static_cast<int>(pyramid.sizes[level].height);
                assert(pyramidCell(pixels[2], level, width) - pyramidCell(pixels[0], level, width) <= 1);
                assert(pyramidCell(pixels[3], level, height) - pyramidCell(pixels[1], level, height) <= 1);
            }
        }

        assert(hidden > 100 && hidden < tested / 2);

        // Off screen, nothing is covered; a box's extent picks the level that spans it in two cells.
        assert(meshClusterBoxPixels({-30.0f, 4.0f, -2.0f, 9.0f}, 64, 64)[0] > meshClusterBoxPixels({-30.0f, 4.0f, -2.0f, 9.0f}, 64, 64)[2]);
        assert(meshClusterBoxPixels({10.0f, 70.0f, 20.0f, 90.0f}, 64, 64)[1] > meshClusterBoxPixels({10.0f, 70.0f, 20.0f, 90.0f}, 64, 64)[3]);
        assert(meshClusterPyramidLevel({5, 5, 5, 5}, 8) == 0 && meshClusterPyramidLevel({5, 5, 6, 5}, 8) == 0);
        assert(meshClusterPyramidLevel({5, 5, 7, 5}, 8) == 1 && meshClusterPyramidLevel({0, 0, 63, 2}, 8) == 6);
        assert(meshClusterPyramidLevel({0, 0, 63, 2}, 4) == 3);
    }

    /*
    * A far leaf is never hidden by a depth it shows in (stage 3e-v-b): leaves of five vertices 0.1-1.5 m across (so
    * their boxes span one pyramid cell or several), ahead
    * of walls or behind them, each drawn into the walls' depth (points of its hull, reversed-Z: the nearer wins) before
    * its pyramid is built, as the main pass's whole-frame pyramid holds the leaf's own depth. A leaf hidden by that
    * pyramid won no pixel; many behind walls are hidden. Its box holds its vertices; one it cannot project is never on
    * screen; the margin keeps a leaf a rounding nearer than the pyramid seen.
    */
    void testFarLeafHiddenOnlyWhenUnseen() {
        std::mt19937 random(31);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const RenderSize size{191, 107};
        const Vec2 screen{static_cast<float>(size.width), static_cast<float>(size.height)};
        const float yaw = 0.3f;
        const Mat4 viewProjection = testRelativeViewProjection(Vec3{}, yaw);
        int hidden = 0;
        int seen = 0;

        for (int round = 0; round < 500; ++round) {
            std::vector<float> depths = wallDepths(random, size, viewProjection, yaw);
            const std::array<float, 4> centre = sphereAhead(random, yaw);
            std::array<Vec4, FAR_LEAF_VERTICES> clips{};
            const float across = 0.1f + unit(random) * 1.4f;

            for (Vec4& clip : clips) {
                const Vec3 offset{(unit(random) - 0.5f) * across, (unit(random) - 0.5f) * across, (unit(random) - 0.5f) * across};
                clip = viewProjection * Vec4{centre[0] + offset.x, centre[1] + offset.y, centre[2] + offset.z, 1.0f};
            }

            const MeshClusterScreenBox box = farLeafScreenBox(clips, screen);
            assert(box.onScreen);

            for (const Vec4& clip : clips) {
                const Vec2 pixel = ndcToPixel(Vec2{clip.x / clip.w, clip.y / clip.w}, screen);
                assert(pixel.x >= box.box[0] && pixel.x <= box.box[2] && pixel.y >= box.box[1] && pixel.y <= box.box[3]);
                assert(clip.z / clip.w < box.nearestDepth);
            }

            // The leaf drawn: points of its hull (random weights of its vertices), each at its pixel if nearer.
            bool won = false;

            for (int sample = 0; sample < 200; ++sample) {
                Vec4 point{};
                float total = 0.0f;

                for (const Vec4& clip : clips) {
                    const float weight = unit(random);
                    point = point + clip * weight;
                    total += weight;
                }

                point = point * (1.0f / total);
                const Vec2 pixel = ndcToPixel(Vec2{point.x / point.w, point.y / point.w}, screen);
                const int x = static_cast<int>(std::floor(pixel.x));
                const int y = static_cast<int>(std::floor(pixel.y));

                if (x < 0 || y < 0 || x >= static_cast<int>(size.width) || y >= static_cast<int>(size.height)) {
                    continue;
                }

                float& at = depths[static_cast<std::size_t>(y) * size.width + static_cast<std::size_t>(x)];

                if (point.z / point.w > at) {
                    at = point.z / point.w;
                    won = true;
                }
            }

            const DepthPyramid pyramid = buildDepthPyramid(depths, size, DepthReduction::Farthest);

            if (meshClusterBoxHidden(pyramid, box)) {
                assert(!won);
                ++hidden;
            } else if (won) {
                ++seen;
            }
        }

        assert(hidden > 50 && seen > 50);

        // A vertex on the camera's plane: no box.
        std::array<Vec4, FAR_LEAF_VERTICES> cut{};
        cut.fill(Vec4{0.0f, 0.0f, 0.5f, 1.0f});
        cut[3].w = 0.0f;
        assert(!farLeafScreenBox(cut, screen).onScreen);

        // A flat pyramid at depth: a leaf a rounding farther is still seen (the margin), a percent farther hidden.
        const float depth = 0.01f;
        const DepthPyramid flat = buildDepthPyramid(std::vector<float>(static_cast<std::size_t>(size.width) * size.height, depth), size, DepthReduction::Farthest);
        std::array<Vec4, FAR_LEAF_VERTICES> near{};
        std::array<Vec4, FAR_LEAF_VERTICES> far{};

        for (std::size_t vertex = 0; vertex < near.size(); ++vertex) {
            const float x = -0.1f + 0.05f * static_cast<float>(vertex);
            near[vertex] = Vec4{x, 0.1f, depth * (1.0f - 1e-6f), 1.0f};
            far[vertex] = Vec4{x, 0.1f, depth * 0.99f, 1.0f};
        }

        assert(!meshClusterBoxHidden(flat, farLeafScreenBox(near, screen)));
        assert(meshClusterBoxHidden(flat, farLeafScreenBox(far, screen)));
    }

    // A list's pairs as a sorted copy, for set tests.
    std::vector<GpuMeshClusterItem> sortedItems(std::vector<GpuMeshClusterItem> items) {
        std::sort(items.begin(), items.end(), [](const GpuMeshClusterItem& a, const GpuMeshClusterItem& b) {
            return a.entry != b.entry ? a.entry < b.entry : a.cluster < b.cluster;
        });
        return items;
    }

    /*
    * The two passes miss nothing: the early and late lists never share a pair and only hold what the frustum and facing
    * keep; whatever they leave out this frame's pyramid hides (built from the early list's depth, farther than the whole
    * frame's); the late list's candidates are the early list's leftovers. With no history the early list is everything
    * and the late one empty; with occlusion off, the list is as before.
    */
    void testTwoPassesMissNothing() {
        std::mt19937 random(23);
        const RenderSize size{160, 90};
        const Vec3 eye{0.0f, 2.0f, 0.0f};
        std::uint32_t hiddenInAll = 0;
        std::uint32_t lateInAll = 0;

        for (int round = 0; round < 6; ++round) {
            const PassScene scene = passScene(random);
            const MeshClusterTables tables = scene.tables();
            const Mat4 current = testRelativeViewProjection(eye, 0.0f);
            const Mat4 previous = testRelativeViewProjection(eye, 0.04f);
            // The early list's depth: the frame's walls but one (an occluder the early list hid), so never nearer.
            // Last frame's depth: the same walls, seen from the camera turned a little.
            std::mt19937 sameWalls = random;
            std::mt19937 lastWalls = random;
            const std::vector<float> whole = wallDepths(sameWalls, size, current, 0.0f);
            const DepthPyramid lastFrame = buildDepthPyramid(wallDepths(lastWalls, size, previous, 0.04f), size, DepthReduction::Farthest);
            const std::vector<float> partial = wallDepths(random, size, current, 0.0f, round % 6);

            for (std::size_t pixel = 0; pixel < whole.size(); ++pixel) {
                assert(partial[pixel] <= whole[pixel]);
            }

            const DepthPyramid thisFrame = buildDepthPyramid(partial, size, DepthReduction::Farthest);
            const std::uint32_t room = 1u << 20;
            const MeshClusterList plain = cullMeshClustersOnCpu(tables, scene.view, room);

            MeshClusterOcclusionView occlusion{previous, current, eye, true, true, &lastFrame, &thisFrame, MESH_CLUSTER_EARLY};
            const MeshClusterList early = cullMeshClustersOnCpu(tables, scene.view, room, &occlusion);
            occlusion.list = MESH_CLUSTER_LATE;
            const MeshClusterList late = cullMeshClustersOnCpu(tables, scene.view, room, &occlusion);
            assert(early.tested == plain.tested && late.tested == plain.tested);
            assert(late.candidates == plain.kept - early.kept && early.candidates == 0);

            const std::vector<GpuMeshClusterItem> all = sortedItems(plain.items);
            const std::vector<GpuMeshClusterItem> earlyItems = sortedItems(early.items);
            const std::vector<GpuMeshClusterItem> lateItems = sortedItems(late.items);
            std::vector<GpuMeshClusterItem> both;
            std::set_intersection(earlyItems.begin(), earlyItems.end(), lateItems.begin(), lateItems.end(), std::back_inserter(both),
                                  [](const GpuMeshClusterItem& a, const GpuMeshClusterItem& b) { return a.entry != b.entry ? a.entry < b.entry : a.cluster < b.cluster; });
            assert(both.empty());

            for (const MeshClusterList* list : {&early, &late}) {
                std::uint32_t next = 0;

                for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
                    assert(list->runFirst[style] == next);
                    next += list->runCount[style];
                }

                assert(next == list->kept);

                for (const GpuMeshClusterItem& item : list->items) {
                    assert(std::binary_search(all.begin(), all.end(), item, [](const GpuMeshClusterItem& a, const GpuMeshClusterItem& b) {
                        return a.entry != b.entry ? a.entry < b.entry : a.cluster < b.cluster;
                    }));
                }
            }

            for (const GpuMeshClusterItem& item : all) {
                const auto before = [](const GpuMeshClusterItem& a, const GpuMeshClusterItem& b) { return a.entry != b.entry ? a.entry < b.entry : a.cluster < b.cluster; };

                if (std::binary_search(earlyItems.begin(), earlyItems.end(), item, before) || std::binary_search(lateItems.begin(), lateItems.end(), item, before)) {
                    continue;
                }

                ++hiddenInAll;
                const GpuInstance& instance = scene.instances[scene.drawList[item.entry] & DRAW_LIST_INSTANCE_MASK];
                const std::array<float, 4> world = meshClusterWorldSphere(scene.clusters[item.cluster], instance);
                assert(meshClusterHidden(thisFrame, {world[0] - eye.x, world[1] - eye.y, world[2] - eye.z, world[3]}, current));
            }

            lateInAll += late.kept;

            // No history: the early list is everything, the late one empty.
            occlusion.history = false;
            occlusion.list = MESH_CLUSTER_EARLY;
            assert(cullMeshClustersOnCpu(tables, scene.view, room, &occlusion).items == plain.items);
            occlusion.list = MESH_CLUSTER_LATE;
            assert(cullMeshClustersOnCpu(tables, scene.view, room, &occlusion).kept == 0);

            // Off: as before.
            occlusion.on = false;
            occlusion.history = true;
            occlusion.list = MESH_CLUSTER_EARLY;
            assert(cullMeshClustersOnCpu(tables, scene.view, room, &occlusion).items == plain.items);
        }

        assert(hiddenInAll > 0 && lateInAll > 0);
    }

    // The occlusion record: both matrices column-major, the camera, the pyramid's size and levels, and its flags.
    void testOcclusionRecord() {
        MeshClusterOcclusionView view{Mat4::translation(1.0f, 2.0f, 3.0f), Mat4::scaling(4.0f, 5.0f, 6.0f), Vec3{7.0f, 8.0f, 9.0f}, true, false, nullptr, nullptr,
                                      MESH_CLUSTER_EARLY};
        const GpuMeshClusterOcclusion packed = packMeshClusterOcclusion(view, RenderSize{640, 360}, 10);
        assert(packed.previous[12] == 1.0f && packed.previous[13] == 2.0f && packed.previous[14] == 3.0f);
        assert(packed.current[0] == 4.0f && packed.current[5] == 5.0f && packed.current[10] == 6.0f);
        assert(packed.origin[0] == 7.0f && packed.origin[1] == 8.0f && packed.origin[2] == 9.0f);
        assert(packed.pyramid[0] == 640 && packed.pyramid[1] == 360 && packed.pyramid[2] == 10 && packed.pyramid[3] == MESH_CLUSTER_OCCLUSION_ON);
        view.history = true;
        assert(packMeshClusterOcclusion(view, RenderSize{640, 360}, 10).pyramid[3] == (MESH_CLUSTER_OCCLUSION_ON | MESH_CLUSTER_OCCLUSION_HISTORY));
        view.on = false;
        assert(packMeshClusterOcclusion(view, RenderSize{640, 360}, 10).pyramid[3] == MESH_CLUSTER_OCCLUSION_HISTORY);
    }

    // Compared as sets: the same pairs in another order agree; a pair one side lacks differs unless it is near an edge.
    void testComparison() {
        const std::vector<GpuMeshClusterItem> cpu{{1, 10}, {1, 11}, {2, 10}, {3, 12}};
        const std::vector<GpuMeshClusterItem> shuffled{{2, 10}, {1, 11}, {3, 12}, {1, 10}};
        const auto never = [](const GpuMeshClusterItem&) { return false; };
        const MeshClusterComparison same = compareMeshClusterLists(shuffled, cpu, never);
        assert(same.entries == 4 && same.matching == 4 && same.missing == 0 && same.extra == 0);
        assert(describeMeshClusterComparison(same, 9, 4).find("agrees") != std::string::npos);

        const std::vector<GpuMeshClusterItem> gpu{{1, 10}, {2, 10}, {3, 12}, {4, 13}};
        const MeshClusterComparison differ = compareMeshClusterLists(gpu, cpu, never);
        assert(differ.matching == 3 && differ.missing == 1 && differ.extra == 1 && differ.nearAnEdge == 0);
        assert(describeMeshClusterComparison(differ, 9, 4).find("DIFFERS") != std::string::npos);

        const MeshClusterComparison edges = compareMeshClusterLists(gpu, cpu, [](const GpuMeshClusterItem&) { return true; });
        assert(edges.nearAnEdge == 2 && describeMeshClusterComparison(edges, 9, 4).find("agrees") != std::string::npos);

        // With occlusion the GPU's lists may lack pairs (hidden), never add any.
        const std::vector<GpuMeshClusterItem> fewer{{1, 10}, {3, 12}};
        const MeshClusterComparison hidden = compareMeshClusterLists(fewer, cpu, never);
        const std::string occluded = describeMeshClusterComparison(hidden, 9, 2, true);
        assert(occluded.find("agrees") != std::string::npos && occluded.find("2 hidden by occlusion") != std::string::npos);
        assert(describeMeshClusterComparison(hidden, 9, 2, false).find("DIFFERS") != std::string::npos);
        assert(describeMeshClusterComparison(differ, 9, 4, true).find("DIFFERS") != std::string::npos);
    }

    // Each cluster-compare call asks for a comparison and shows the last one; it takes no arguments.
    void testCompareCommand() {
        CommandRegistry registry;
        int asked = 0;
        std::string last;
        addClusterCompareCommand(registry, [&]() {
            ++asked;
            return last;
        });

        assert(!registry.execute("cluster-compare now").ok && asked == 0);
        const CommandReply first = registry.execute("cluster-compare");
        assert(first.ok && asked == 1 && first.text.find("No comparison") != std::string::npos);
        last = "The GPU's list agrees with the CPU's.";
        const CommandReply second = registry.execute("cluster-compare");
        assert(second.ok && asked == 2 && second.text.find("agrees") != std::string::npos);
    }

    /*
    * A run's mesh shader workgroups reach each of its survivors exactly once (those past the count in the last row draw
    * nothing), within one row's width and the most rows any GPU takes; none for none.
    */
    void testTaskGroups() {
        for (const std::uint32_t count : {0u, 1u, 31u, MESH_CLUSTER_TASK_WIDTH - 1u, MESH_CLUSTER_TASK_WIDTH, MESH_CLUSTER_TASK_WIDTH + 1u,
                                          5u * MESH_CLUSTER_TASK_WIDTH + 3u, largestMeshClusterRoom}) {
            const std::array<std::uint32_t, 3> groups = meshClusterTaskGroups(count);
            assert(groups[0] <= MESH_CLUSTER_TASK_WIDTH && groups[2] == 1);
            std::uint64_t reached = 0;

            for (std::uint32_t y = 0; y < groups[1]; ++y) {
                for (std::uint32_t x = 0; x < groups[0]; ++x) {
                    const std::uint32_t index = meshClusterTaskIndex(x, y);
                    assert(index == static_cast<std::uint64_t>(y) * groups[0] + x || groups[1] == 1);
                    reached += index < count ? 1u : 0u;
                }
            }

            assert(reached == count);
            assert(static_cast<std::uint64_t>(groups[0]) * groups[1] < static_cast<std::uint64_t>(count) + MESH_CLUSTER_TASK_WIDTH);
        }
    }

    // Each address goes in as its low then high words, in the uniform block's order.
    void testBufferAddresses() {
        const MeshClusterBuffers buffers{0x1'0000'0002ull, 3, 0xFFFF'FFFF'FFFF'FFFFull, 0, 0x7'8000'0000ull, 9};
        std::uint32_t words[12] = {};
        packMeshClusterBuffers(buffers, words);
        const std::uint32_t expected[12] = {2, 1, 3, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0x80000000u, 7, 9, 0};
        assert(std::equal(std::begin(words), std::end(words), std::begin(expected)));
    }

    // F4's levels line shows the clusters kept of those tested and their triangles, only while they are culled.
    void testReadout() {
        RenderStats stats;
        assert(renderStatsLines(stats)[1].find("CLUSTERS") == std::string::npos);
        stats.meshClustersTested = 5000;
        stats.meshClustersKept = 2100;
        stats.meshClusterTriangles = 250000;
        assert(renderStatsLines(stats)[1].find("CLUSTERS 2100/5000 250K") != std::string::npos);
        assert(renderStatsLines(stats)[1].find("LATE") == std::string::npos);

        // With occlusion: the late list's share of those kept, and what neither list drew.
        stats.meshClusterOcclusion = true;
        stats.meshClustersLate = 40;
        stats.meshClustersHidden = 900;
        assert(renderStatsLines(stats)[1].find("CLUSTERS 2100/5000 250K  LATE 40  HIDDEN 900") != std::string::npos);
    }
}

/*
* Mesh clusters culled a view (render/gpu/mesh_cluster_culling.h): packing, the mirrored test hiding nothing seen, the
* passes' steps equal to a plain walk, the mesh shaders' workgroups and addresses, cluster-compare and its command, F4's line.
*/
void testMeshClusterCulling() {
    testPackingKeepsEveryTriangle();
    testPackingKeepsEachClustersBones();
    testACulledClusterHidesNothing();
    testADrawNotRigidKeepsEverything();
    testAPosedClusterHidesNothing();
    testThePoseOfAPair();
    testPackingKeepsTheCut();
    testTheCut();
    testThePassesEqualAPlainWalk();
    testTheDrawingOrderTakesThePlacedGroups();
    testPlacedPlantsCullAsTheirEntities();
    testThePlacedPairBound();
    testScreenBoxHoldsTheSphere();
    testFarLeafHiddenOnlyWhenUnseen();
    testOcclusionHidesNothingSeen();
    testTwoPassesMissNothing();
    testOcclusionRecord();
    testComparison();
    testCompareCommand();
    testTaskGroups();
    testBufferAddresses();
    testReadout();
}
