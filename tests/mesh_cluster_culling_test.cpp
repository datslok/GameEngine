#include "core/command_registry.h"
#include "engine/debug_views.h"
#include "engine/engine_commands.h"
#include "math/frustum.h"
#include "math/mat4.h"
#include "mesh_helpers.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/instance_shared.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "render/render_stats.h"
#include "scene/indexed_mesh.h"
#include "scene/level_of_detail.h"
#include "scene/mesh.h"
#include "scene/mesh_clusters.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace {
    // A view from an eye looking along yaw, slightly down, 0.9 radians tall; things sway within fadeEnd.
    GpuCameraCull testView(const Vec3& eye, float yaw, bool sway, float fadeEnd) {
        const Mat4 projection = toGpuDepthRange(Mat4::perspective(0.9f, 16.0f / 9.0f, 0.1f, 500.0f));
        const Mat4 view = Mat4::lookAt(eye, eye + Vec3{std::cos(yaw), -0.1f, std::sin(yaw)}, Vec3{0.0f, 1.0f, 0.0f});
        LevelView levels;
        levels.eye = eye;
        return packCameraCull(Frustum::fromClipMatrix(projection * view, ClipDepth::OneToZero), levels, true, sway, fadeEnd);
    }

    // A plain still instance placed by the matrix.
    GpuInstance placedInstance(const Mat4& model) {
        GpuInstance instance{};
        writeColumnMajor(model, instance.model);
        instance.sway[0] = -1.0f;
        return instance;
    }

    // A still single-sided draw's record round a sphere.
    GpuCullRecord stillRecord(const Vec3& centre, float radius) {
        GpuCullRecord record;
        record.bounds[0] = centre.x;
        record.bounds[1] = centre.y;
        record.bounds[2] = centre.z;
        record.bounds[3] = radius;
        record.flags = CULL_CASTS_SHADOWS;
        return record;
    }

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

    // Packed for the pools: every cluster's vertices and triangles as built, numbered from where they go.
    void testPackingKeepsEveryTriangle() {
        const IndexedMesh ball = buildIndexedMesh(Mesh::sphere(64, 32));
        const MeshClusters built = buildMeshClusters(ball);
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
    * Random views of rigid instances (turned, scaled unevenly, some mirrored, some double-sided): a cluster culled has no
    * triangle the view could see, each having all its corners outside one plane or (single-sided, not mirrored) its back to
    * the eye. And the test is of use: a good share culled.
    */
    void testACulledClusterHidesNothing() {
        std::mt19937 random(7);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const IndexedMesh meshes[2] = {buildIndexedMesh(Mesh::sphere(96, 48)), rippledSheet()};
        std::size_t culled = 0;
        std::size_t tested = 0;

        for (int round = 0; round < 60; ++round) {
            const IndexedMesh& mesh = meshes[round % 2];
            const PackedMeshClusters packed = packMeshClusters(buildMeshClusters(mesh), 0, 0, 0);
            const Vec3 eye{(unit(random) - 0.5f) * 100.0f, 2.0f + unit(random) * 10.0f, (unit(random) - 0.5f) * 100.0f};
            const float yaw = unit(random) * 2.0f * std::numbers::pi_v<float>;
            const GpuCameraCull view = testView(eye, yaw, false, 0.0f);
            const bool mirrored = round % 7 == 3;
            const bool doubleSided = round % 5 == 2;

            for (int copy = 0; copy < 8; ++copy) {
                const Vec3 place = eye + Vec3{(unit(random) - 0.5f) * 30.0f, (unit(random) - 0.5f) * 8.0f, (unit(random) - 0.5f) * 30.0f};
                const Mat4 model = Mat4::translation(place.x, place.y, place.z) * Mat4::rotationY(unit(random) * 6.0f) * Mat4::rotationX(unit(random) * 6.0f) *
                                   Mat4::scaling((mirrored ? -1.0f : 1.0f) * (0.5f + 3.0f * unit(random)), 0.5f + 3.0f * unit(random), 0.5f + 3.0f * unit(random));
                const GpuInstance instance = placedInstance(model);
                GpuCullRecord record = stillRecord(place, 10.0f);
                record.flags |= doubleSided ? CULL_DOUBLE_SIDED : 0u;
                const bool faces = !doubleSided && meshClusterDeterminant(instance) > 0.0f;

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

    // A draw not rigid keeps every cluster, even one far out of view: terrain, an impostor, ground-following, a liana,
    // thinned cards, swaying within the sway's reach; past the reach a swaying draw is rigid again.
    void testADrawNotRigidKeepsEverything() {
        const PackedMeshClusters packed = packMeshClusters(buildMeshClusters(buildIndexedMesh(Mesh::sphere(16, 8))), 0, 0, 0);
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
            const PackedMeshClusters packed = packMeshClusters(buildMeshClusters(mesh), 0, 0, 0);
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

    // F4's levels line shows the clusters kept of those tested and their triangles, only while they are culled.
    void testReadout() {
        RenderStats stats;
        assert(renderStatsLines(stats)[1].find("CLUSTERS") == std::string::npos);
        stats.meshClustersTested = 5000;
        stats.meshClustersKept = 2100;
        stats.meshClusterTriangles = 250000;
        assert(renderStatsLines(stats)[1].find("CLUSTERS 2100/5000 250K") != std::string::npos);
    }
}

/*
* Mesh clusters culled a view (render/gpu/mesh_cluster_culling.h): packing, the mirrored test hiding nothing seen, the
* passes' steps equal to a plain walk, cluster-compare and its command, F4's line.
*/
void testMeshClusterCulling() {
    testPackingKeepsEveryTriangle();
    testACulledClusterHidesNothing();
    testADrawNotRigidKeepsEverything();
    testThePassesEqualAPlainWalk();
    testComparison();
    testCompareCommand();
    testReadout();
}
