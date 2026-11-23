#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/frustum.h"
#include "math/mat4.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/instance_shared.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh.h"
#include "scene/mesh_clusters.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("mesh_cluster_culling_test");

    struct Case {
        GpuMeshCluster cluster;
        GpuInstance instance{};
        std::uint32_t entry = 0;
        GpuCullRecord record;
        GpuCameraCull view{};
        MeshClusterPose pose;
    };

    // Clusters of a sphere placed round random eyes: turned, scaled unevenly, some mirrored, some swaying, thinned,
    // double-sided or of another kind, some bound to bones (posed, with a sphere moved near the cluster's, or not), half
    // from every level under several pixel errors (some views orthographic), so every branch of meshClusterKept and the
    // cut is reached.
    std::vector<Case> cases() {
        const IndexedMesh ball = buildIndexedMesh(Mesh::sphere(48, 24));
        const PackedMeshClusters one = packMeshClusters(buildMeshClusters(ball, ClusterLevels::One), 0, 0, 0);
        const PackedMeshClusters all = packMeshClusters(buildMeshClusters(ball, ClusterLevels::All), 0, 0, 0);
        CaseRandom random{31};
        std::vector<Case> made;

        for (int index = 0; index < 1500; ++index) {
            Case c;
            const PackedMeshClusters& packed = index % 2 == 0 ? one : all;
            c.cluster = packed.clusters[static_cast<std::size_t>(random.between(0.0f, static_cast<float>(packed.clusters.size()) - 0.01f))];
            const Vec3 eye{random.between(-500.0f, 500.0f), random.between(1.0f, 40.0f), random.between(-500.0f, 500.0f)};
            const float yaw = random.between(0.0f, 6.28f);
            const Mat4 projection = toGpuDepthRange(Mat4::perspective(0.9f, 16.0f / 9.0f, 0.1f, 800.0f));
            const Mat4 look = Mat4::lookAt(eye, eye + Vec3{std::cos(yaw), -0.1f, std::sin(yaw)}, Vec3{0.0f, 1.0f, 0.0f});
            LevelView levels;
            levels.eye = eye;
            levels.pixelsPerMetre = 1200.0f;
            levels.pixelError = index % 3 == 0 ? 0.5f : index % 3 == 1 ? 1.0f : 4.0f;
            levels.orthographic = index % 11 == 3;
            c.view = packCameraCull(Frustum::fromClipMatrix(projection * look, ClipDepth::OneToZero), levels, false, index % 3 == 0, random.between(5.0f, 60.0f));

            const Vec3 place = eye + Vec3{random.between(-40.0f, 40.0f), random.between(-10.0f, 10.0f), random.between(-40.0f, 40.0f)};
            const float mirror = index % 9 == 4 ? -1.0f : 1.0f;
            const Mat4 model = Mat4::translation(place.x, place.y, place.z) * Mat4::rotationY(random.between(0.0f, 6.28f)) *
                               Mat4::rotationX(random.between(0.0f, 6.28f)) *
                               Mat4::scaling(mirror * random.between(0.3f, 4.0f), random.between(0.3f, 4.0f), random.between(0.3f, 4.0f));
            writeColumnMajor(model, c.instance.model);
            c.instance.material[2] = index % 37 == 5 ? 1u : 0u;
            c.instance.material[3] = index % 41 == 6 ? INSTANCE_KIND_IMPOSTOR : index % 43 == 7 ? INSTANCE_KIND_FOLLOWS_GROUND : INSTANCE_KIND_PLAIN;
            c.instance.hanging[3] = index % 47 == 8 ? 1u : 0u;
            c.entry = index % 23 == 9 ? DRAW_LIST_THINNED_BIT : 0u;
            c.record.bounds[0] = place.x;
            c.record.bounds[1] = place.y;
            c.record.bounds[2] = place.z;
            c.record.bounds[3] = 5.0f;
            c.record.flags = (index % 4 == 1 ? CULL_SWAYS : 0u) | (index % 7 == 2 ? CULL_DOUBLE_SIDED : 0u);
            c.pose.bound = index % 6 >= 3;
            c.pose.posed = index % 6 >= 4;

            if (c.pose.posed) {
                const std::array<float, 4> world = meshClusterWorldSphere(c.cluster, c.instance);
                c.pose.sphere = {world[0] + random.between(-3.0f, 3.0f), world[1] + random.between(-3.0f, 3.0f), world[2] + random.between(-3.0f, 3.0f),
                                 world[3] + random.between(0.0f, 2.0f)};
            }

            made.push_back(c);
        }

        return made;
    }

    std::vector<GpuVec4> inputsOf(const Case& c) {
        const float* m = c.instance.model;
        std::vector<GpuVec4> inputs{
            gpuVec4(c.cluster.sphere[0], c.cluster.sphere[1], c.cluster.sphere[2], c.cluster.sphere[3]),
            gpuVec4(c.cluster.cone[0], c.cluster.cone[1], c.cluster.cone[2], c.cluster.cone[3]),
            gpuVec4(c.cluster.axis[0], c.cluster.axis[1], c.cluster.axis[2]),
            gpuVec4(m[0], m[1], m[2], m[3]),
            gpuVec4(m[4], m[5], m[6], m[7]),
            gpuVec4(m[8], m[9], m[10], m[11]),
            gpuVec4(m[12], m[13], m[14], m[15]),
            gpuVec4(c.entry != 0u ? 1.0f : 0.0f, static_cast<float>(c.record.flags), static_cast<float>(c.view.flags), c.view.swayFadeEnd),
            gpuVec4(static_cast<float>(c.instance.material[2]), static_cast<float>(c.instance.material[3]), static_cast<float>(c.instance.hanging[3])),
            gpuVec4(c.record.bounds[0], c.record.bounds[1], c.record.bounds[2], c.record.bounds[3]),
            gpuVec4(c.view.levels.eye[0], c.view.levels.eye[1], c.view.levels.eye[2], c.view.levels.eye[3]),
        };

        for (const auto& plane : c.view.frustum.planes) {
            inputs.push_back(gpuVec4(plane[0], plane[1], plane[2], plane[3]));
        }

        inputs.push_back(gpuVec4(c.cluster.ownSphere[0], c.cluster.ownSphere[1], c.cluster.ownSphere[2], c.cluster.ownSphere[3]));
        inputs.push_back(gpuVec4(c.cluster.parentSphere[0], c.cluster.parentSphere[1], c.cluster.parentSphere[2], c.cluster.parentSphere[3]));
        inputs.push_back(gpuVec4(c.cluster.ownError, c.cluster.parentError, c.view.levels.pixelError, static_cast<float>(c.view.levels.flags)));
        inputs.push_back(gpuVec4(c.pose.bound ? 1.0f : 0.0f, c.pose.posed ? 1.0f : 0.0f));
        inputs.push_back(gpuVec4(c.pose.sphere[0], c.pose.sphere[1], c.pose.sphere[2], c.pose.sphere[3]));

        return inputs;
    }

    /*
    * The choices equal the mirror's, cases within rounding of a plane, the cone's edge or the sway's reach left out
    * (nearAMeshClusterEdge: the GPU's square roots and fused products may round either way there); the scale and
    * sphere within arithmetic rounding, and the errors' pixels within the square root's and division's (a parent never
    * made coarser is not measured: its pixels are past the largest float).
    */
    void checkKept(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<Case> all = cases();
        std::vector<GpuVec4> inputs;

        for (const Case& c : all) {
            const std::vector<GpuVec4> one = inputsOf(c);
            inputs.insert(inputs.end(), one.begin(), one.end());
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_MESH_CLUSTER_KEPT, inputs, 22, 3);

        for (std::size_t i = 0; i < all.size(); ++i) {
            const Case& c = all[i];
            const GpuVec4& choice = outputs[3 * i];
            const GpuVec4& sphere = outputs[3 * i + 1];
            const GpuVec4& cut = outputs[3 * i + 2];
            const std::array<float, 4> expected = meshClusterWorldSphere(c.cluster, c.instance);
            report.check("meshClusterLargestScale", arithmeticTolerance, meshClusterLargestScale(c.instance), choice[3]);
            report.check("meshClusterWorldSphere", arithmeticTolerance, Vec3{expected[0], expected[1], expected[2]}, Vec3{sphere[0], sphere[1], sphere[2]});
            report.check("meshClusterWorldSphere radius", arithmeticTolerance, expected[3], sphere[3]);
            report.check("meshClusterErrorPixels own", arithmeticTolerance, meshClusterErrorPixels(c.cluster.ownSphere, c.cluster.ownError, c.instance, c.view), cut[1]);

            if (c.cluster.parentError <= static_cast<float>(MESH_CLUSTER_NEVER_COARSER)) {
                report.check("meshClusterErrorPixels parent", arithmeticTolerance,
                             meshClusterErrorPixels(c.cluster.parentSphere, c.cluster.parentError, c.instance, c.view), cut[2]);
            }

            if (nearAMeshClusterEdge(c.cluster, c.instance, c.entry, c.record, c.view, c.pose)) {
                continue;
            }

            const float determinant = meshClusterDeterminant(c.instance);
            const Vec3 eye{c.view.levels.eye[0], c.view.levels.eye[1], c.view.levels.eye[2]};
            const bool away = determinant > 0.0f && meshClusterFacesAway(c.cluster, meshClusterEyeInMesh(c.instance, eye, determinant));
            report.check("meshClusterInCut", exactTolerance, meshClusterInCut(c.cluster, c.instance, c.view) ? 1.0f : 0.0f, cut[0]);
            const bool whole = meshClusterSeenWhole(c.instance, c.entry, c.record, c.view, c.pose);
            report.check("meshClusterKept", exactTolerance, meshClusterKept(c.cluster, c.instance, c.entry, c.record, c.view, c.pose) ? 1.0f : 0.0f, choice[0]);
            report.check("meshClusterSeenWhole", exactTolerance, whole ? 1.0f : 0.0f, choice[1]);

            // The facing's edge is measured only where it is used: draws not seen whole and not posed.
            if (!whole && !c.pose.posed) {
                report.check("meshClusterFacesAway", exactTolerance, away ? 1.0f : 0.0f, choice[2]);
            }
        }
    }
}

namespace {
    // Counts from none to the most a run holds, and workgroups across the rows: whole numbers below 2^24, exact as floats.
    void checkTasks(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{37};
        std::vector<GpuVec4> inputs;

        for (int index = 0; index < 400; ++index) {
            const float count = index < 4 ? static_cast<float>(index) : std::floor(random.between(0.0f, static_cast<float>(largestMeshClusterRoom)));
            inputs.push_back(gpuVec4(count, std::floor(random.between(0.0f, 32767.0f)), std::floor(random.between(0.0f, 127.0f))));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_MESH_CLUSTER_TASKS, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const std::array<std::uint32_t, 3> groups = meshClusterTaskGroups(static_cast<std::uint32_t>(inputs[i][0]));
            report.check("meshClusterTaskGroups", exactTolerance, Vec3{static_cast<float>(groups[0]), static_cast<float>(groups[1]), static_cast<float>(groups[2])},
                         Vec3{outputs[i][0], outputs[i][1], outputs[i][2]});
            const std::uint32_t index = meshClusterTaskIndex(static_cast<std::uint32_t>(inputs[i][1]), static_cast<std::uint32_t>(inputs[i][2]));
            report.check("meshClusterTaskIndex", exactTolerance, static_cast<float>(index), outputs[i][3]);
        }
    }
}

namespace {
    /*
    * Spheres round a camera at the origin turned every way, near and far, some cut by its plane or behind it, on screens
    * of several sizes: the box within the projective divide's rounding (pixels, so absolute slack for the cancellation
    * near the screen's left and top), the nearest depth and whether it is on screen; the pixels and level exactly, from
    * the GPU's own box (a box corner within rounding of a pixel edge may land on either side).
    */
    void checkScreenBox(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{41};
        const float sizes[3][2] = {{1920.0f, 1080.0f}, {191.0f, 107.0f}, {1.0f, 5.0f}};
        std::vector<GpuVec4> inputs;
        std::vector<Mat4> matrices;
        std::vector<std::array<float, 4>> spheres;

        for (int index = 0; index < 900; ++index) {
            const float yaw = random.between(0.0f, 6.28f);
            const Mat4 projection = toGpuDepthRange(Mat4::perspective(0.9f, 16.0f / 9.0f, 0.1f, 800.0f));
            const Mat4 viewProjection = projection * Mat4::lookAt(Vec3{}, Vec3{std::cos(yaw), random.between(-0.5f, 0.5f), std::sin(yaw)}, Vec3{0.0f, 1.0f, 0.0f});
            const std::array<float, 4> sphere{random.between(-60.0f, 60.0f), random.between(-20.0f, 20.0f), random.between(-60.0f, 60.0f), random.between(0.05f, 8.0f)};
            const float* size = sizes[index % 3];
            float columns[16];
            writeColumnMajor(viewProjection, columns);
            inputs.push_back(gpuVec4(sphere[0], sphere[1], sphere[2], sphere[3]));

            for (int column = 0; column < 4; ++column) {
                inputs.push_back(gpuVec4(columns[4 * column], columns[4 * column + 1], columns[4 * column + 2], columns[4 * column + 3]));
            }

            inputs.push_back(gpuVec4(size[0], size[1], 12.0f));
            matrices.push_back(viewProjection);
            spheres.push_back(sphere);
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_MESH_CLUSTER_SCREEN_BOX, inputs, 6, 4);
        const FloatTolerance pixelTolerance{.absolute = 1e-3f, .relative = 1e-5f};

        for (std::size_t i = 0; i < spheres.size(); ++i) {
            const GpuVec4& box = outputs[4 * i];
            const GpuVec4& depth = outputs[4 * i + 1];
            const GpuVec4& pixels = outputs[4 * i + 2];
            const float width = inputs[6 * i + 5][0];
            const float height = inputs[6 * i + 5][1];
            const MeshClusterScreenBox expected = meshClusterScreenBox(spheres[i], matrices[i], Vec2{width, height});

            // A corner within rounding of the camera's plane may go either way.
            if (std::fabs(depth[1] - (expected.onScreen ? 1.0f : 0.0f)) > 0.5f) {
                continue;
            }

            report.check("meshClusterScreenBox on screen", exactTolerance, expected.onScreen ? 1.0f : 0.0f, depth[1]);

            if (!expected.onScreen) {
                continue;
            }

            report.check("meshClusterScreenBox least", pixelTolerance, Vec3{expected.box[0], expected.box[1], 0.0f}, Vec3{box[0], box[1], 0.0f});
            report.check("meshClusterScreenBox most", pixelTolerance, Vec3{expected.box[2], expected.box[3], 0.0f}, Vec3{box[2], box[3], 0.0f});
            report.check("meshClusterScreenBox nearest depth", arithmeticTolerance, expected.nearestDepth, depth[0]);
            const std::array<int, 4> covered = meshClusterBoxPixels({box[0], box[1], box[2], box[3]}, static_cast<int>(width), static_cast<int>(height));
            report.check("meshClusterBoxPixels least", exactTolerance, Vec3{static_cast<float>(covered[0]), static_cast<float>(covered[1]), 0.0f},
                         Vec3{pixels[0], pixels[1], 0.0f});
            report.check("meshClusterBoxPixels most", exactTolerance, Vec3{static_cast<float>(covered[2]), static_cast<float>(covered[3]), 0.0f},
                         Vec3{pixels[2], pixels[3], 0.0f});
            report.check("meshClusterPyramidLevel", exactTolerance, static_cast<float>(meshClusterPyramidLevel(covered, 12)), outputs[4 * i + 3][0]);
        }
    }
}

void gpuTestMeshClusterCulling(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("mesh_cluster_culling.glsl");
    checkKept(runner, report);
    checkTasks(runner, report);
    checkScreenBox(runner, report);
}
