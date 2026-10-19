#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/mat4.h"
#include "math/ray.h"
#include "physics/raycast.h"
#include "render/gpu/compute_dispatch.h"
#include "render/gpu/ray_tracing.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_mesh.h"
#include "render/vulkan/vulkan_ray_tracing.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
    const std::string libraryShader = gpuTestShader("ray_tracing_test");

    // The camera's rays and the corner weights, through the runner like every shader library.
    void checkLibrary(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{41};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 200; ++i) {
            inputs.push_back(gpuVec4(random.direction() * random.between(0.1f, 2.0f), random.between(-1.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.1f, 2.0f), random.between(-1.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.direction(), random.between(0.0f, 0.5f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 0.5f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_CAMERA_RAY, inputs, 4, 2);

        for (std::size_t i = 0; i < inputs.size() / 4; ++i) {
            const GpuVec4* in = &inputs[i * 4];
            const Vec3 expected = cameraRayDirection(vec3From(in[0]), vec3From(in[1]), vec3From(in[2]), in[0][3], in[1][3]);
            report.check("cameraRayDirection", arithmeticTolerance, expected, vec3From(outputs[i * 2]));
            report.check("hitCornerWeights", arithmeticTolerance, hitCornerWeights(in[2][3], in[3][0]), vec3From(outputs[i * 2 + 1]));
        }
    }

    // A mesh as both sides see it: uploaded for the GPU, and its triangles for the CPU's raycast.
    struct TestMesh {
        IndexedMesh indexed;
        std::unique_ptr<VulkanMesh> gpu;
    };

    // One instance of a test scene: which mesh, where, and whether it casts shadows (solid) or is only seen.
    struct TestInstance {
        std::uint32_t mesh = 0;
        Mat4 model = Mat4::identity();
        bool castsShadows = true;
    };

    // What a ray hit, as the CPU finds it with the physics raycast over every triangle of every instance it may see.
    struct ExpectedHit {
        float distance = 0.0f;
        std::uint32_t instance = 0;
        std::uint32_t triangle = 0;
        bool front = false;
        Vec3 normal; // the mesh's own normal, interpolated at the hit (not normalised)
        float cosine = 1.0f; // between the ray and the triangle's normal, unsigned: small for a grazing hit
    };

    Vec3 cornerPosition(const IndexedMesh& mesh, std::uint32_t corner, const Mat4& model) {
        const MeshVertex& vertex = mesh.vertices[mesh.indices[corner]];
        return transformPoint(model, vertex.position);
    }

    std::optional<ExpectedHit> traceOnCpu(const std::vector<TestMesh>& meshes, const std::vector<TestInstance>& instances, const Ray& ray,
                                          float maxDistance, std::uint32_t mask) {
        std::optional<ExpectedHit> nearest;

        for (std::uint32_t instance = 0; instance < instances.size(); ++instance) {
            const TestInstance& placed = instances[instance];
            DrawItem draw;
            draw.castsShadows = placed.castsShadows;

            if ((rayMaskFor(draw) & mask) == 0) {
                continue;
            }

            const IndexedMesh& mesh = meshes[placed.mesh].indexed;

            for (std::uint32_t triangle = 0; triangle * 3 < mesh.indices.size(); ++triangle) {
                const Vec3 a = cornerPosition(mesh, triangle * 3, placed.model);
                const Vec3 b = cornerPosition(mesh, triangle * 3 + 1, placed.model);
                const Vec3 c = cornerPosition(mesh, triangle * 3 + 2, placed.model);
                const float reach = nearest ? nearest->distance : maxDistance;
                const std::optional<RayHit> hit = raycastTriangle(ray, a, b, c, reach);

                if (!hit || (nearest && hit->distance >= nearest->distance)) {
                    continue;
                }

                // The hit's barycentric weights, from areas (an affine map keeps them), give the interpolated normal.
                const Vec3 normal = (b - a).cross(c - a);
                const float area = normal.lengthSquared();
                const float second = (hit->point - a).cross(c - a).dot(normal) / area;
                const float third = (b - a).cross(hit->point - a).dot(normal) / area;
                const Vec3 weights = hitCornerWeights(second, third);
                const MeshVertex& first = mesh.vertices[mesh.indices[triangle * 3]];
                const MeshVertex& secondCorner = mesh.vertices[mesh.indices[triangle * 3 + 1]];
                const MeshVertex& thirdCorner = mesh.vertices[mesh.indices[triangle * 3 + 2]];

                nearest = ExpectedHit{hit->distance, instance, triangle, hitsFrontFace(a, b, c, ray.direction, handedness(placed.model)),
                                      first.normal * weights.x + secondCorner.normal * weights.y + thirdCorner.normal * weights.z,
                                      std::abs(normal.normalized().dot(ray.direction))};
            }
        }

        return nearest;
    }

    /*
    * The test shader's inputs and outputs live in buffers it reaches by address, and its scene through the ray tracing
    * set (set 0 here), so its layout is that set plus push constants.
    */
    struct RayQueryPush {
        std::uint64_t rays = 0;
        std::uint64_t hits = 0;
        std::uint64_t meshOfInstance = 0;
        std::uint32_t count = 0;
        std::uint32_t unused = 0;
    };

    /*
    * Real acceleration structures, traced on the GPU and compared ray by ray with the CPU's physics raycast over the
    * same triangles: what is hit first (instance, triangle, distance), from which side, and the normal interpolated from
    * the mesh's own vertices, read through their buffer's address as the renderer reads them. The scene holds a turned
    * and stretched cube, a sphere, a mirrored cube, a sphere that casts no shadow (seen only by rays that want
    * everything) and a floor. Rays aim at points inside random triangles from in front of the scene, and some start
    * inside the first cube, so they leave through back faces.
    */
    void checkRayQueries(const VulkanDevice& device, GpuCheckReport& report) {
        ImmediateCommands commands{device};
        VulkanRayTracing rayTracing{device, 1};

        std::vector<TestMesh> meshes;

        for (const Mesh& source : {Mesh::cube(), Mesh::sphere(12, 6), Mesh::plane(6.0f)}) {
            TestMesh mesh;
            mesh.indexed = buildIndexedMesh(source);
            mesh.gpu = std::make_unique<VulkanMesh>(device, commands, mesh.indexed);
            rayTracing.addMesh(commands, *mesh.gpu);
            meshes.push_back(std::move(mesh));
        }

        const std::vector<TestInstance> instances{
            {0, Mat4::translation(0.0f, 0.0f, -5.0f) * Mat4::rotationY(0.3f) * Mat4::scaling(1.0f, 2.0f, 1.0f), true},
            {1, Mat4::translation(3.0f, 1.0f, -6.0f) * Mat4::scaling(1.5f, 1.5f, 1.5f), true},
            {0, Mat4::translation(-3.0f, 0.0f, -6.0f) * Mat4::rotationX(0.5f) * Mat4::scaling(-1.0f, 1.0f, 1.0f), true},
            {1, Mat4::translation(0.0f, 3.0f, -8.0f), false},
            {2, Mat4::translation(0.0f, -2.5f, -5.0f), true},
        };

        std::vector<DrawItem> draws;
        std::vector<std::uint32_t> meshOfInstance;

        for (const TestInstance& instance : instances) {
            DrawItem draw;
            draw.mesh = MeshHandle{instance.mesh};
            draw.model = instance.model;
            draw.castsShadows = instance.castsShadows;
            draws.push_back(draw);
            meshOfInstance.push_back(instance.mesh);
        }

        // Rays: (origin, reach) and (direction, mask).
        CaseRandom random{43};
        std::vector<GpuVec4> rays;
        std::vector<Ray> cpuRays;
        std::vector<std::uint32_t> masks;

        for (int i = 0; i < 2000; ++i) {
            const TestInstance& target = instances[static_cast<std::size_t>(i) % instances.size()];
            const IndexedMesh& mesh = meshes[target.mesh].indexed;
            const std::uint32_t triangle = static_cast<std::uint32_t>(random.between(0.0f, 0.9999f) * static_cast<float>(mesh.indices.size() / 3));
            const float second = random.between(0.15f, 0.7f);
            const float third = random.between(0.15f, 0.85f - second);
            const Vec3 weights = hitCornerWeights(second, third);
            const Vec3 aim = cornerPosition(mesh, triangle * 3, target.model) * weights.x + cornerPosition(mesh, triangle * 3 + 1, target.model) * weights.y +
                             cornerPosition(mesh, triangle * 3 + 2, target.model) * weights.z;

            const bool fromInside = i % 10 == 0;
            const Vec3 origin = fromInside ? transformPoint(instances[0].model, Vec3{0.1f, 0.2f, -0.1f})
                                           : Vec3{random.between(-8.0f, 8.0f), random.between(-1.0f, 6.0f), random.between(0.0f, 4.0f)};
            const Vec3 direction = fromInside ? random.direction() : (aim - origin).normalized();
            const std::uint32_t mask = i % 3 == 0 ? everythingRayMask : solidRayMask;

            rays.push_back(gpuVec4(origin, 100.0f));
            rays.push_back(gpuVec4(direction, static_cast<float>(mask)));
            cpuRays.push_back(Ray{origin, direction});
            masks.push_back(mask);
        }

        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VulkanBuffer rayBuffer{device.getAllocator(), rays.size() * sizeof(GpuVec4), usage, BufferMemory::Upload};
        VulkanBuffer meshBuffer{device.getAllocator(), meshOfInstance.size() * sizeof(std::uint32_t), usage, BufferMemory::Upload};
        VulkanBuffer hitBuffer{device.getAllocator(), rays.size() * sizeof(GpuVec4), usage, BufferMemory::Readback};
        std::memcpy(rayBuffer.getMapped(), rays.data(), rays.size() * sizeof(GpuVec4));
        std::memcpy(meshBuffer.getMapped(), meshOfInstance.data(), meshOfInstance.size() * sizeof(std::uint32_t));
        rayBuffer.flush();
        meshBuffer.flush();

        VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayQueryPush)};
        const VkDescriptorSetLayout setLayout = rayTracing.getSetLayout();
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        VkPipelineLayout createdLayout = VK_NULL_HANDLE;
        vulkanCheck(vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &createdLayout), "Ray query test layout creation failed");
        const OwnedHandle<VkPipelineLayout> pipelineLayout{device.getDevice(), createdLayout, vkDestroyPipelineLayout};

        ComputePipelineDescription description;
        description.shader = "tests/gpu/shaders/ray_query_test.comp.spv";
        description.sharedLayout = pipelineLayout.get();
        const ComputePipeline pipeline{device, description};

        const RayQueryPush push{rayBuffer.getDeviceAddress(), hitBuffer.getDeviceAddress(), meshBuffer.getDeviceAddress(),
                                static_cast<std::uint32_t>(cpuRays.size()), 0};

        commands.run([&](VkCommandBuffer recording) {
            rayTracing.buildScene(recording, 0, rayTracing.packInstances(draws));
            const VkDescriptorSet scene = rayTracing.getSceneSet(0);

            vkCmdBindPipeline(recording, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
            vkCmdBindDescriptorSets(recording, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.get(), 0, 1, &scene, 0, nullptr);
            vkCmdPushConstants(recording, pipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(recording, workgroupCount(push.count, GPU_TEST_GROUP_SIZE), 1, 1);

            VkMemoryBarrier2 toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &toHost;
            vkCmdPipelineBarrier2(recording, &dependency);
        });

        hitBuffer.invalidate();
        std::vector<GpuVec4> hits(rays.size());
        std::memcpy(hits.data(), hitBuffer.getMapped(), hits.size() * sizeof(GpuVec4));

        for (std::size_t i = 0; i < cpuRays.size(); ++i) {
            const std::optional<ExpectedHit> expected = traceOnCpu(meshes, instances, cpuRays[i], 100.0f, masks[i]);
            const GpuVec4& hit = hits[i * 2];
            const bool gpuHit = hit[0] >= 0.0f;

            report.check("hit or miss", exactTolerance, expected ? 1.0f : 0.0f, gpuHit ? 1.0f : 0.0f);

            if (!expected || !gpuHit) {
                continue;
            }

            /*
            * The two find the same plane crossing by different arithmetic (the hardware's own watertight test against
            * the CPU's Moller-Trumbore). Rounding of where the plane is, about 1e-5 of the distance, moves the hit along
            * the ray by that much over the cosine between ray and surface, so the allowance grows as hits graze (as the
            * specular test's does near sharp highlights), and so does the hit's place on the triangle the normal is
            * blended from.
            */
            const float grazing = 1.0f / std::max(expected->cosine, 0.01f);
            const FloatTolerance distanceTolerance{.absolute = 1e-5f, .relative = 1e-5f * grazing};
            const FloatTolerance normalTolerance{.absolute = 1e-4f * grazing, .relative = 1e-4f};

            report.check("distance", distanceTolerance, expected->distance, hit[0]);
            report.check("instance", exactTolerance, static_cast<float>(expected->instance), hit[1]);
            report.check("triangle", exactTolerance, static_cast<float>(expected->triangle), hit[2]);
            report.check("front face", exactTolerance, expected->front ? 1.0f : 0.0f, hit[3]);
            report.check("interpolated normal", normalTolerance, expected->normal, vec3From(hits[i * 2 + 1]));
        }
    }
}

void gpuTestRayTracing(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("ray_tracing.glsl");
    checkLibrary(runner, report);

    report.section("ray queries (hardware ray tracing)");

    if (!device.supportsRayQueries()) {
        std::cout << "Ray queries skipped: this GPU has no hardware ray tracing\n";
        return;
    }

    checkRayQueries(device, report);
}
