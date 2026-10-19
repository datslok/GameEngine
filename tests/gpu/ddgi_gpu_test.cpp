#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "core/half_float.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/ddgi_sampling.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/light_uniforms.h"
#include "render/vulkan/bindless_textures.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/scene_layout.h"
#include "render/vulkan/vulkan_ddgi.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_mesh.h"
#include "render/vulkan/vulkan_ray_tracing.h"
#include "render/vulkan/vulkan_texture.h"
#include "scene/camera.h"
#include "scene/indexed_mesh.h"
#include "scene/lighting.h"
#include "scene/mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
    const std::string libraryShader = gpuTestShader("ddgi_test");

    /*
    * The GPU's sin and cos need only be within 2^-11 (about 5e-4) of the truth (Vulkan's precision rules), far looser
    * than its arithmetic; the Fibonacci points and the rays turned from them carry that, a few times over.
    */
    constexpr FloatTolerance trigonometryTolerance{.absolute = 2e-3f, .relative = 1e-5f};

    Vec2 vec2From(const GpuVec4& value, int first = 0) {
        return Vec2{value[static_cast<std::size_t>(first)], value[static_cast<std::size_t>(first + 1)]};
    }

    void checkOctahedral(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{61};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.direction()));
            inputs.push_back(gpuVec4(random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_OCTAHEDRAL, inputs, 2, 2);

        for (std::size_t i = 0; i < inputs.size() / 2; ++i) {
            const Vec2 encoded = octahedralEncode(vec3From(inputs[i * 2]));
            const Vec2 gpuEncoded = vec2From(outputs[i * 2]);
            report.check("octahedralEncode", arithmeticTolerance, Vec3{encoded.x, encoded.y, 0.0f}, Vec3{gpuEncoded.x, gpuEncoded.y, 0.0f});
            report.check("octahedralDecode", arithmeticTolerance, octahedralDecode(vec2From(inputs[i * 2 + 1])), vec3From(outputs[i * 2 + 1]));
        }
    }

    // Every texel of both probe sizes, every bordered texel's source and every border texel's number.
    void checkTexels(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (int side : {probeIrradianceTexels, probeDistanceTexels}) {
            for (int y = 0; y < side + 2; ++y) {
                for (int x = 0; x < side + 2; ++x) {
                    const int number = (x + y * (side + 2)) % probeBorderTexelCount(side);
                    inputs.push_back(gpuVec4(static_cast<float>(x % side), static_cast<float>(y % side), static_cast<float>(side),
                                             static_cast<float>(number)));
                    inputs.push_back(gpuVec4(static_cast<float>(x), static_cast<float>(y)));
                }
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_TEXELS, inputs, 2, 2);

        for (std::size_t i = 0; i < inputs.size() / 2; ++i) {
            const GpuVec4& in = inputs[i * 2];
            const int side = static_cast<int>(in[2]);
            const TexelCoord texel{static_cast<int>(in[0]), static_cast<int>(in[1])};
            const TexelCoord bordered{static_cast<int>(inputs[i * 2 + 1][0]), static_cast<int>(inputs[i * 2 + 1][1])};
            const TexelCoord source = probeBorderSource(bordered, side);
            const TexelCoord numbered = probeBorderTexel(static_cast<int>(in[3]), side);
            const GpuVec4& out = outputs[i * 2 + 1];

            report.check("probeTexelDirection", arithmeticTolerance, probeTexelDirection(texel, side), vec3From(outputs[i * 2]));
            report.check("probeBorderTexelCount", exactTolerance, static_cast<float>(probeBorderTexelCount(side)), outputs[i * 2][3]);
            report.check("probeBorderSource", exactTolerance, Vec3{static_cast<float>(source.x), static_cast<float>(source.y), 0.0f},
                         Vec3{out[0], out[1], 0.0f});
            report.check("probeBorderTexel", exactTolerance, Vec3{static_cast<float>(numbered.x), static_cast<float>(numbered.y), 0.0f},
                         Vec3{out[2], out[3], 0.0f});
        }
    }

    // Every ray of a few counts, each turned by a different frame's rotation.
    void checkRays(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;
        std::vector<ProbeRotationRows> rotations;

        for (int count : {minRaysPerProbe, 64, 100, maxRaysPerProbe}) {
            for (int ray = 0; ray < count; ++ray) {
                const ProbeRotationRows rows = probeRotationRows(randomProbeRotation(static_cast<std::uint64_t>(ray * 7 + count)));
                inputs.push_back(gpuVec4(static_cast<float>(ray), static_cast<float>(count)));

                for (const Vec3& row : rows.rows) {
                    inputs.push_back(gpuVec4(row));
                }

                rotations.push_back(rows);
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_RAYS, inputs, 4, 2);

        for (std::size_t i = 0; i < rotations.size(); ++i) {
            const int ray = static_cast<int>(inputs[i * 4][0]);
            const int count = static_cast<int>(inputs[i * 4][1]);
            report.check("sphericalFibonacci", trigonometryTolerance, sphericalFibonacci(ray, count), vec3From(outputs[i * 2]));
            report.check("probeRayDirection", trigonometryTolerance, probeRayDirection(ray, count, rotations[i]), vec3From(outputs[i * 2 + 1]));
        }
    }

    // Texels and rays at every angle, and distances from back faces, front faces and misses either side of the cap.
    void checkWeights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{62};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const Vec3 texel = random.direction();
            // Half the rays near the texel, where the narrow distance lobe is not vanishingly small.
            const Vec3 ray = i % 2 == 0 ? random.direction() : (texel + random.direction() * 0.3f).normalized();
            const float maxDistance = random.between(0.5f, 5.0f);
            const float distance = i % 7 == 0 ? static_cast<float>(DDGI_RAY_REACH) : random.between(-8.0f, 8.0f);
            inputs.push_back(gpuVec4(texel));
            inputs.push_back(gpuVec4(ray));
            inputs.push_back(gpuVec4(distance, maxDistance));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_WEIGHTS, inputs, 3, 1);

        for (std::size_t i = 0; i < inputs.size() / 3; ++i) {
            const Vec3 texel = vec3From(inputs[i * 3]);
            const Vec3 ray = vec3From(inputs[i * 3 + 1]);
            const GpuVec4& out = outputs[i];
            report.check("irradianceRayWeight", arithmeticTolerance, irradianceRayWeight(texel, ray), out[0]);
            report.check("distanceRayWeight", transcendentalTolerance, distanceRayWeight(texel, ray), out[1]);
            report.check("distanceForUpdate", arithmeticTolerance, distanceForUpdate(inputs[i * 3 + 2][0], inputs[i * 3 + 2][1]), out[2]);
        }
    }

    void checkBlending(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{63};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(0.0f, 40.0f), random.between(0.0f, 3.0f), random.between(0.0f, 0.01f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 40.0f), random.between(0.0f, 3.0f), random.between(0.0f, 0.01f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_BLENDING, inputs, 3, 2);

        for (std::size_t i = 0; i < inputs.size() / 3; ++i) {
            const Vec3 old = vec3From(inputs[i * 3]);
            const Vec3 fresh = vec3From(inputs[i * 3 + 1]);
            const float share = inputs[i * 3 + 2][0];
            const Vec2 blended2 = blendProbeValue(Vec2{old.x, old.y}, Vec2{fresh.x, fresh.y}, share);
            report.check("blendProbeValue", arithmeticTolerance, blendProbeValue(old, fresh, share), vec3From(outputs[i * 2]));
            report.check("blendProbeValue (2)", arithmeticTolerance, Vec3{blended2.x, blended2.y, 0.0f},
                         Vec3{outputs[i * 2 + 1][0], outputs[i * 2 + 1][1], 0.0f});
        }
    }

    // Every probe of an uneven grid, at both probe sizes, with a random direction each.
    void checkGrid(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{64};
        const ProbeVolume volume{Vec3{-3.5f, 0.25f, 7.0f}, Vec3{1.0f, 0.75f, 1.5f}, ProbeCounts{5, 3, 4}};
        std::vector<GpuVec4> inputs;

        for (int side : {probeIrradianceTexels, probeDistanceTexels}) {
            for (int probe = 0; probe < probeCount(volume.counts); ++probe) {
                inputs.push_back(gpuVec4(volume.origin));
                inputs.push_back(gpuVec4(volume.spacing));
                inputs.push_back(gpuVec4(static_cast<float>(volume.counts.x), static_cast<float>(volume.counts.y),
                                         static_cast<float>(volume.counts.z), static_cast<float>(probe)));
                inputs.push_back(gpuVec4(random.direction(), static_cast<float>(side)));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_GRID, inputs, 4, 2);

        for (std::size_t i = 0; i < inputs.size() / 4; ++i) {
            const int probe = static_cast<int>(inputs[i * 4 + 2][3]);
            const int side = static_cast<int>(inputs[i * 4 + 3][3]);
            const TexelCoord origin = probeTileOrigin(volume.counts, probe, side);
            const Vec2 uv = probeAtlasUv(volume.counts, probe, vec3From(inputs[i * 4 + 3]), side);
            const GpuVec4& out = outputs[i * 2 + 1];

            report.check("probePosition", arithmeticTolerance, probePosition(volume, probe), vec3From(outputs[i * 2]));
            report.check("probeTileOrigin", exactTolerance, Vec3{static_cast<float>(origin.x), static_cast<float>(origin.y), 0.0f},
                         Vec3{out[0], out[1], 0.0f});
            report.check("probeAtlasUv", arithmeticTolerance, Vec3{uv.x, uv.y, 0.0f}, Vec3{out[2], out[3], 0.0f});
        }
    }

    // A scene the probe passes can trace: meshes with their acceleration structures, and the frame's sets as the
    // renderer writes them (every texture, white at slot 0; instances, materials and lights; dummy images for the
    // shadow atlas and BRDF table, which the trace never reads).
    struct ProbeTestScene {
        const VulkanDevice& device;
        ImmediateCommands commands;
        FrameDescriptors descriptors;
        DescriptorLayouts layouts;
        OwnedHandle<VkSampler> sampler;
        BindlessTextures textures;
        SceneLayout sceneLayout;
        SceneBuffers buffers;
        VulkanRayTracing rayTracing;
        std::unique_ptr<VulkanTexture> white;
        VulkanBuffer clusterMasks;
        std::vector<std::unique_ptr<VulkanMesh>> meshes;

        explicit ProbeTestScene(const VulkanDevice& device):
            device(device),
            commands(device),
            descriptors(device.getDevice()),
            layouts(device.getDevice()),
            sampler(device.createSampler(clampedLinearSampler())),
            textures(device, 16),
            sceneLayout(device.getDevice(), textures.getLayout()),
            buffers(device.getAllocator()),
            rayTracing(device, 1),
            clusterMasks(device.getAllocator(), 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu)
        {
            const std::array<std::uint8_t, 4> whiteTexel{255, 255, 255, 255};
            white = std::make_unique<VulkanTexture>(device, commands, 1, 1, whiteTexel, surfaceTexture(TextureEncoding::Srgb));
            textures.write(0, white->getView(), sampler.get());
        }

        static VkSamplerCreateInfo clampedLinearSampler() {
            VkSamplerCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            info.magFilter = VK_FILTER_LINEAR;
            info.minFilter = VK_FILTER_LINEAR;
            info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            return info;
        }

        MeshHandle addMesh(const Mesh& mesh) {
            meshes.push_back(std::make_unique<VulkanMesh>(device, commands, buildIndexedMesh(mesh)));
            rayTracing.addMesh(commands, *meshes.back());
            return MeshHandle{static_cast<std::uint32_t>(meshes.size() - 1)};
        }

        // The frame's set 1 for these draws (all one white matte material), lights and probes, as the renderer writes it
        // with the camera at the origin.
        VkDescriptorSet writeFrame(const std::vector<DrawItem>& draws, const FrameLighting& lighting, const ProbeVolume& volume,
                                   const VulkanDdgi& ddgi, const GlobalIlluminationSettings& settings) {
            MaterialTable materials;
            std::vector<GpuInstance> instances;

            for (const DrawItem& draw : draws) {
                instances.push_back(packInstance(draw, materials.add(draw.material)));
            }

            const GpuLights lights = packLighting(lighting, Vec3{});
            const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 100.0f};
            GpuFrameData frameData = makeFrameData(lights, Mat4::identity(), Vec3{}, makeClusterGrid(camera, 64, 64), false, false);
            const ProbeHitLights hitLights = probeHitLights(lights, Vec3{}, volume);
            packProbeSampling(frameData, ProbeSampling{volume, Vec3{}, settings.selfShadowBias, settings.bounceEnergy, settings.lightsPerHit,
                                                       hitLights.points, hitLights.spots});

            buffers.frameData.write(&frameData, sizeof(frameData));
            buffers.instances.write(instances);
            buffers.materials.write(materials.getEntries());
            buffers.directionalLights.write(lights.directional);
            buffers.pointLights.write(lights.points);
            buffers.spotLights.write(lights.spots);
            buffers.probeHitLights.write(hitLights.lights);

            const SampledImage dummy{white->getView(), sampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            return writeSceneSet(descriptors, sceneLayout, buffers, SceneImages{dummy, dummy, dummy, ddgi.sampledIrradiance(), ddgi.sampledDistance()},
                                 clusterMasks);
        }
    };

    // An atlas read back as floats, four channels per texel whatever its format.
    struct ReadAtlas {
        int width = 0;
        int height = 0;
        int channels = 0;
        std::vector<float> values;

        GpuVec4 texel(int x, int y) const {
            GpuVec4 value{0.0f, 0.0f, 0.0f, 0.0f};
            for (int channel = 0; channel < channels; ++channel) {
                value[static_cast<std::size_t>(channel)] = values[static_cast<std::size_t>((y * width + x) * channels + channel)];
            }
            return value;
        }
    };

    ReadAtlas readAtlas(ProbeTestScene& scene, VulkanImage& atlas, int channels) {
        const ImageDescription& description = atlas.getDescription();
        ReadAtlas read{static_cast<int>(description.width), static_cast<int>(description.height), channels, {}};
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(read.width) * read.height * channels * sizeof(std::uint16_t);
        VulkanBuffer readback{scene.device.getAllocator(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback};

        scene.commands.run([&](VkCommandBuffer recording) {
            atlas.transition(recording, ImageState::TransferSource);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {description.width, description.height, 1};
            vkCmdCopyImageToBuffer(recording, atlas.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.get(), 1, &copy);
            bufferBarrier(recording, readback.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                          {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
        });

        readback.invalidate();
        std::vector<std::uint16_t> halves(static_cast<std::size_t>(bytes / sizeof(std::uint16_t)));
        std::memcpy(halves.data(), readback.getMapped(), bytes);

        for (std::uint16_t half : halves) {
            read.values.push_back(halfToFloat(half));
        }

        return read;
    }

    // What the update should store for one probe, worked out on the CPU from what each of its rays brings back.
    struct ExpectedProbe {
        std::vector<Vec3> irradiance; // decoded, per inner texel
        std::vector<Vec2> distance;   // mean and mean square, per inner texel
    };

    struct RayResult {
        Vec3 light;
        float distance = 0.0f;
    };

    ExpectedProbe expectedProbe(const std::vector<Vec3>& directions, const std::vector<RayResult>& results, float maxDistance) {
        ExpectedProbe expected;

        for (int texel = 0; texel < probeIrradianceTexels * probeIrradianceTexels; ++texel) {
            const Vec3 direction = probeTexelDirection(TexelCoord{texel % probeIrradianceTexels, texel / probeIrradianceTexels}, probeIrradianceTexels);
            Vec3 sum{0.0f, 0.0f, 0.0f};
            float weights = 0.0f;

            for (std::size_t ray = 0; ray < directions.size(); ++ray) {
                const float weight = irradianceRayWeight(direction, directions[ray]);
                sum = sum + results[ray].light * weight;
                weights += weight;
            }

            expected.irradiance.push_back(sum * (1.0f / weights));
        }

        for (int texel = 0; texel < probeDistanceTexels * probeDistanceTexels; ++texel) {
            const Vec3 direction = probeTexelDirection(TexelCoord{texel % probeDistanceTexels, texel / probeDistanceTexels}, probeDistanceTexels);
            Vec2 sum{0.0f, 0.0f};
            float weights = 0.0f;

            for (std::size_t ray = 0; ray < directions.size(); ++ray) {
                const float weight = distanceRayWeight(direction, directions[ray]);
                const float distance = distanceForUpdate(results[ray].distance, maxDistance);
                sum = sum + Vec2{distance, distance * distance} * weight;
                weights += weight;
            }

            expected.distance.push_back(sum * (1.0f / weights));
        }

        return expected;
    }

    /*
    * Half floats keep 11 significant bits, and Vulkan lets a GPU round toward zero when it stores one, so a stored value
    * may sit a whole step (up to 2^-10 of it) below the true one, not just half. The rays' hits differ from the CPU's
    * by the hardware's rounding.
    */
    constexpr FloatTolerance storedIrradianceTolerance{.absolute = 1e-4f, .relative = 2e-3f};
    constexpr FloatTolerance storedDistanceTolerance{.absolute = 1e-3f, .relative = 2e-3f};

    /*
    * Compare one probe's squares in both atlases with what the CPU expects, and check every border texel is an exact
    * copy of the inner texel it should copy.
    */
    void checkProbe(const ReadAtlas& irradiance, const ReadAtlas& distance, const ProbeCounts& counts, int probe, const ExpectedProbe& expected,
                    const std::string& name, GpuCheckReport& report) {
        const TexelCoord irradianceOrigin = probeTileOrigin(counts, probe, probeIrradianceTexels);

        for (int texel = 0; texel < probeIrradianceTexels * probeIrradianceTexels; ++texel) {
            const GpuVec4 stored = irradiance.texel(irradianceOrigin.x + 1 + texel % probeIrradianceTexels, irradianceOrigin.y + 1 + texel / probeIrradianceTexels);
            report.check(name + ": irradiance", storedIrradianceTolerance, expected.irradiance[static_cast<std::size_t>(texel)],
                         vec3From(stored));
        }

        const TexelCoord distanceOrigin = probeTileOrigin(counts, probe, probeDistanceTexels);

        for (int texel = 0; texel < probeDistanceTexels * probeDistanceTexels; ++texel) {
            const GpuVec4 stored = distance.texel(distanceOrigin.x + 1 + texel % probeDistanceTexels, distanceOrigin.y + 1 + texel / probeDistanceTexels);
            const Vec2 wanted = expected.distance[static_cast<std::size_t>(texel)];
            report.check(name + ": distance", storedDistanceTolerance, Vec3{wanted.x, wanted.y, 0.0f}, Vec3{stored[0], stored[1], 0.0f});
        }

        for (const auto& [atlas, side, origin] : {std::tuple{&irradiance, probeIrradianceTexels, irradianceOrigin},
                                                  std::tuple{&distance, probeDistanceTexels, distanceOrigin}}) {
            for (int number = 0; number < probeBorderTexelCount(side); ++number) {
                const TexelCoord bordered = probeBorderTexel(number, side);
                const TexelCoord source = probeBorderSource(bordered, side);
                const GpuVec4 copy = atlas->texel(origin.x + bordered.x, origin.y + bordered.y);
                const GpuVec4 original = atlas->texel(origin.x + source.x, origin.y + source.y);
                report.check(name + ": borders", exactTolerance, vec3From(original), vec3From(copy));
            }
        }
    }

    // The first update's ray directions: a fresh VulkanDdgi turns its rays by frame 0's rotation.
    std::vector<Vec3> firstFrameRays(int count) {
        const ProbeRotationRows rows = probeRotationRows(randomProbeRotation(0));
        std::vector<Vec3> directions;

        for (int ray = 0; ray < count; ++ray) {
            directions.push_back(probeRayDirection(ray, count, rows));
        }

        return directions;
    }

    // One fresh update of the volume over these draws and lights, both atlases read back.
    std::pair<ReadAtlas, ReadAtlas> runProbes(ProbeTestScene& scene, const std::vector<DrawItem>& draws, const FrameLighting& lighting,
                                              const ProbeVolume& volume, const GlobalIlluminationSettings& settings) {
        VulkanDdgi ddgi{scene.device, scene.layouts, scene.textures.getLayout(), scene.sceneLayout.getFrameLayout(), scene.rayTracing.getSetLayout(),
                        scene.sampler.get(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT};

        scene.descriptors.reset();
        scene.commands.run([&](VkCommandBuffer recording) { ddgi.prepare(recording, volume); });
        const VkDescriptorSet frame = scene.writeFrame(draws, lighting, volume, ddgi, settings);

        scene.commands.run([&](VkCommandBuffer recording) {
            scene.rayTracing.buildScene(recording, 0, scene.rayTracing.packInstances(draws));
            const ProbeSceneSets sets{scene.textures.getSet(), frame, scene.rayTracing.getSceneSet(0)};
            ddgi.update(recording, scene.descriptors, sets, volume, settings, lighting.background, 1.0f / 60.0f);
        });

        return {readAtlas(scene, ddgi.getIrradianceAtlas(), 4), readAtlas(scene, ddgi.getDistanceAtlas(), 2)};
    }

    DrawItem drawOf(MeshHandle mesh, const Mat4& model) {
        DrawItem draw;
        draw.mesh = mesh;
        draw.model = model;
        draw.material.colour = Pixel{255, 255, 255};
        draw.material.roughness = 1.0f;
        return draw;
    }

    /*
    * The real probe passes (VulkanDdgi: ddgi_trace.comp, then both updates), on scenes whose answer the CPU works out
    * ray by ray with the same directions:
    * - the furnace: probes under an empty sky drawn at B store B in every texel, and the longest distance;
    * - a probe sealed in a box sees only back faces: no light, and a fifth of each wall's distance;
    * - a probe over a white floor lit straight down by a sun of radiance R: rays that go down bring back R, rays that
    *   go up the sky (black), and their distances the floor's;
    * - the same floor lit by two lamps a little above it instead, one kept per hit: each ray that goes down brings back
    *   the brighter lamp's light where it lands (pointLightAtHit), checked by a shadow ray that nothing blocks.
    * The atlases start empty, so the light bounced from them (last frame's probes) adds nothing on a first update. Each
    * scene also holds a speck far below, since a top-level structure needs something in it.
    */
    void checkProbePasses(const VulkanDevice& device, GpuCheckReport& report) {
        ProbeTestScene scene{device};
        const MeshHandle cube = scene.addMesh(Mesh::cube());
        const MeshHandle floor = scene.addMesh(Mesh::plane(1000.0f));
        const DrawItem speck = drawOf(cube, Mat4::translation(0.0f, -10000.0f, 0.0f) * Mat4::scaling(0.001f, 0.001f, 0.001f));

        GlobalIlluminationSettings settings;
        const std::vector<Vec3> rays = firstFrameRays(settings.raysPerProbe);

        {
            const ProbeVolume volume{Vec3{-1.0f, 0.5f, 2.0f}, Vec3{1.0f, 1.0f, 1.5f}, ProbeCounts{3, 2, 2}};
            FrameLighting lighting;
            lighting.background = Vec3{0.6f, 0.75f, 1.0f};
            const auto [irradiance, distance] = runProbes(scene, {speck}, lighting, volume, settings);

            const std::vector<RayResult> results(rays.size(), RayResult{lighting.background, static_cast<float>(DDGI_RAY_REACH)});
            const ExpectedProbe expected = expectedProbe(rays, results, probeMaxDistance(volume.spacing));

            for (int probe = 0; probe < probeCount(volume.counts); ++probe) {
                checkProbe(irradiance, distance, volume.counts, probe, expected, "furnace", report);
            }
        }

        {
            const Vec3 halfSize{0.7f, 0.5f, 0.9f};
            const ProbeVolume volume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{1, 1, 1}};
            const DrawItem box = drawOf(cube, Mat4::scaling(halfSize.x, halfSize.y, halfSize.z));
            const auto [irradiance, distance] = runProbes(scene, {speck, box}, FrameLighting{}, volume, settings);

            std::vector<RayResult> results;

            for (const Vec3& ray : rays) {
                const float wall = std::min({halfSize.x / std::abs(ray.x), halfSize.y / std::abs(ray.y), halfSize.z / std::abs(ray.z)});
                results.push_back(RayResult{Vec3{0.0f, 0.0f, 0.0f}, -wall});
            }

            checkProbe(irradiance, distance, volume.counts, 0, expectedProbe(rays, results, probeMaxDistance(volume.spacing)), "sealed box", report);
        }

        {
            const float height = 1.0f;
            const float sunRadiance = 2.0f;
            const ProbeVolume volume{Vec3{0.0f, height, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{1, 1, 1}};
            FrameLighting lighting;
            lighting.directionalLights.push_back(DirectionalLight{Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, sunRadiance});
            const auto [irradiance, distance] = runProbes(scene, {speck, drawOf(floor, Mat4::identity())}, lighting, volume, settings);

            std::vector<RayResult> results;

            for (const Vec3& ray : rays) {
                results.push_back(ray.y < 0.0f ? RayResult{Vec3{sunRadiance, sunRadiance, sunRadiance}, height / -ray.y}
                                               : RayResult{Vec3{0.0f, 0.0f, 0.0f}, static_cast<float>(DDGI_RAY_REACH)});
            }

            checkProbe(irradiance, distance, volume.counts, 0, expectedProbe(rays, results, probeMaxDistance(volume.spacing)), "sunlit floor", report);
        }

        {
            const float height = 1.0f;
            const ProbeVolume volume{Vec3{0.0f, height, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{1, 1, 1}};
            FrameLighting lighting;

            for (const auto& [position, intensity] : {std::pair{Vec3{0.3f, 0.4f, 0.2f}, 3.0f}, std::pair{Vec3{-0.8f, 0.6f, -0.5f}, 2.0f}}) {
                PlacedPointLight lamp;
                lamp.position = position;
                lamp.light.intensity = intensity;
                lamp.light.range = 6.0f;
                lamp.light.sourceRadius = 0.2f;
                lighting.pointLights.push_back(lamp);
            }

            GlobalIlluminationSettings onePerHit = settings;
            onePerHit.lightsPerHit = 1;
            const auto [irradiance, distance] = runProbes(scene, {speck, drawOf(floor, Mat4::identity())}, lighting, volume, onePerHit);

            const GpuLights packed = packLighting(lighting, Vec3{});
            const Vec3 up{0.0f, 1.0f, 0.0f};
            std::vector<RayResult> results;

            for (const Vec3& ray : rays) {
                if (ray.y >= 0.0f) {
                    results.push_back(RayResult{Vec3{0.0f, 0.0f, 0.0f}, static_cast<float>(DDGI_RAY_REACH)});
                    continue;
                }

                const float along = height / -ray.y;
                const Vec3 hit = volume.origin + ray * along;
                Vec3 brightest{0.0f, 0.0f, 0.0f};

                for (const GpuPointLight& light : packed.points) {
                    const Vec3 offset = Vec3{light.positionRange[0], light.positionRange[1], light.positionRange[2]} - hit;
                    const Vec3 lit = pointLightAtHit(offset, light.positionRange[3], vec3From(GpuVec4{light.radiance[0], light.radiance[1],
                                                                                                      light.radiance[2], 0.0f}),
                                                     light.radiance[3], up);
                    if (hitLightStrength(lit) > hitLightStrength(brightest)) {
                        brightest = lit;
                    }
                }

                results.push_back(RayResult{brightest, along});
            }

            checkProbe(irradiance, distance, volume.counts, 0, expectedProbe(rays, results, probeMaxDistance(volume.spacing)), "lamp-lit floor", report);
        }
    }
}

void gpuTestDdgi(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("ddgi.glsl");
    checkOctahedral(runner, report);
    checkTexels(runner, report);
    checkRays(runner, report);
    checkWeights(runner, report);
    checkBlending(runner, report);
    checkGrid(runner, report);

    report.section("light probe passes (ddgi_trace.comp, ddgi_update_*.comp)");

    if (!device.supportsRayQueries()) {
        std::cout << "Light probe passes skipped: this GPU has no hardware ray tracing\n";
        return;
    }

    checkProbePasses(device, report);
}
