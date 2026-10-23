#include "cpu_trace.h"
#include "gpu_check.h"
#include "image_readback.h"
#include "gpu_test_functions.h"

#include "core/half_float.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/ddgi_sampling.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/atmosphere.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/probe_cascades.h"
#include "render/gpu/probe_relocation.h"
#include "render/gpu/ray_tracing.h"
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
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <span>
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

    // Every ray of a few counts, at every phase of the ray cycle in turn, each turned by a different frame's rotation.
    void checkRays(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;
        std::vector<ProbeRotationRows> rotations;

        for (int count : {minRaysPerProbe, 64, 100, maxRaysPerProbe}) {
            for (int ray = 0; ray < count; ++ray) {
                const ProbeRotationRows rows = probeRotationRows(randomProbeRotation(static_cast<std::uint64_t>(ray * 7 + count)));
                const int phase = (ray * 3 + count) % probeRayCycle;
                inputs.push_back(gpuVec4(static_cast<float>(ray), static_cast<float>(count), static_cast<float>(phase)));

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
            const int phase = static_cast<int>(inputs[i * 4][2]);
            report.check("sphericalFibonacci", trigonometryTolerance, sphericalFibonacci(ray, count), vec3From(outputs[i * 2]));
            report.check("probeRayDirection", trigonometryTolerance, probeRayDirection(ray, count, phase, rotations[i]), vec3From(outputs[i * 2 + 1]));
        }
    }

    // Probes within reach of a light or not, either side of the edge.
    void checkReach(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{65};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const Vec3 probe{random.between(-5.0f, 5.0f), random.between(-5.0f, 5.0f), random.between(-5.0f, 5.0f)};
            const Vec3 light = probe + random.direction() * random.between(0.0f, 6.0f);
            inputs.push_back(gpuVec4(probe, random.between(0.0f, 4.0f)));
            inputs.push_back(gpuVec4(light, random.between(0.0f, 2.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_REACH, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& probe = inputs[i * 2];
            const GpuVec4& light = inputs[i * 2 + 1];
            const bool near = probeNearLight(vec3From(probe), vec3From(light), probe[3], light[3]);

            // Exact unless the light is within rounding of the edge of its reach, where either answer is right.
            const float margin = (vec3From(probe) - vec3From(light)).length() - (probe[3] + light[3]);

            if (std::abs(margin) > 1e-4f) {
                report.check("probeNearLight", exactTolerance, near ? 1.0f : 0.0f, outputs[i][0]);
            }
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

    /*
    * The adaptive memory on texels that clearly keep their history (an estimate within a thousandth of the light, the
    * noise half the light) or clearly restart (an estimate ten times the light, little noise), so rounding cannot move a
    * case across the threshold; histories from none to past the cap.
    */
    void checkAdaptiveMemory(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{65};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const Vec3 light{random.between(0.01f, 20.0f), random.between(0.01f, 20.0f), random.between(0.01f, 20.0f)};
            const bool restart = i % 2 == 1;
            const Vec3 estimate = restart ? light * 10.0f : light * random.between(0.999f, 1.001f);
            const float noise = restart ? 0.001f * probeLuminance(light) : 0.5f * probeLuminance(light);
            const float history = i % 5 == 0 ? 0.0f : std::floor(random.between(1.0f, 3000.0f));
            inputs.push_back(gpuVec4(light.x, light.y, light.z, history));
            inputs.push_back(gpuVec4(light.x, light.y, light.z, noise));
            inputs.push_back(gpuVec4(estimate.x, estimate.y, estimate.z));
            inputs.push_back(gpuVec4(random.between(0.9f, 0.9999f), random.between(0.5f, 0.99f), random.between(2.0f, 8.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_ADAPTIVE, inputs, 4, 3);

        for (std::size_t i = 0; i < inputs.size() / 4; ++i) {
            const GpuVec4& texelIn = inputs[i * 4];
            const GpuVec4& changeIn = inputs[i * 4 + 1];
            const GpuVec4& shares = inputs[i * 4 + 3];
            const AdaptiveProbeTexel texel{vec3From(texelIn), texelIn[3], vec3From(changeIn), changeIn[3]};
            const Vec3 estimate = vec3From(inputs[i * 4 + 2]);
            const AdaptiveProbeTexel expected = adaptiveProbeBlend(texel, estimate, shares[0], shares[1], shares[2]);
            const GpuVec4& stored = outputs[i * 3];
            const GpuVec4& recent = outputs[i * 3 + 1];
            report.check("adaptiveProbeBlend stored", arithmeticTolerance, expected.stored, vec3From(stored));
            report.check("adaptiveProbeBlend history", arithmeticTolerance, expected.history, stored[3]);
            report.check("adaptiveProbeBlend recent", arithmeticTolerance, expected.recent, vec3From(recent));
            report.check("adaptiveProbeBlend noise", transcendentalTolerance, expected.noise, recent[3]);
            report.check("probeLuminance", arithmeticTolerance, probeLuminance(estimate), outputs[i * 3 + 2][0]);
        }
    }

    /*
    * The adaptive memory's update after scene events, for frame counts from none (negative) through the event's frame and
    * the event check's frames to past them, unchanged light and changes far outside the noise (so rounding cannot move
    * a case across the threshold), texels the event restarted (history marked negative) and not, the restart's growing
    * memory, and the sharing weight's fade. Where the adaptive memory itself runs,
    * the light stays well inside its noise (as in checkAdaptiveMemory), so rounding cannot move a case across the
    * threshold.
    */
    void checkRestarts(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{67};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 600; ++i) {
            const Vec3 light{random.between(0.01f, 20.0f), random.between(0.01f, 20.0f), random.between(0.01f, 20.0f)};
            const bool changed = i % 3 == 0;
            const Vec3 estimate = changed ? light * 10.0f : light * random.between(0.999f, 1.001f);
            const float frames = std::floor(random.between(-2.0f, 30.0f));
            const float history = i % 2 == 0 ? -std::floor(random.between(1.0f, 30.0f)) : std::floor(random.between(1.0f, 3000.0f));
            const float noise = changed ? 0.001f * probeLuminance(light) : 0.5f * probeLuminance(light);
            inputs.push_back(gpuVec4(light.x, light.y, light.z, history));
            inputs.push_back(gpuVec4(light.x, light.y, light.z, noise));
            inputs.push_back(gpuVec4(estimate.x, estimate.y, estimate.z));
            inputs.push_back(gpuVec4(random.between(0.9f, 0.9999f), random.between(0.5f, 0.95f), random.between(2.0f, 8.0f), frames));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 0.9f)));
            inputs.push_back(gpuVec4(random.between(2.0f, 6.0f), random.between(0.1f, 1.0f), std::floor(random.between(5.0f, 25.0f))));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_RESTART, inputs, 6, 3);

        for (std::size_t i = 0; i < inputs.size() / 6; ++i) {
            const GpuVec4& texelIn = inputs[i * 6];
            const GpuVec4& changeIn = inputs[i * 6 + 1];
            const GpuVec4& shares = inputs[i * 6 + 3];
            const GpuVec4& sharing = inputs[i * 6 + 4];
            const GpuVec4& event = inputs[i * 6 + 5];
            const int frames = static_cast<int>(shares[3]);
            const ProbeEventCheck check{event[0], event[1], event[2]};
            const AdaptiveProbeTexel expected = adaptiveProbeUpdate(AdaptiveProbeTexel{vec3From(texelIn), texelIn[3], vec3From(changeIn), changeIn[3]},
                                                                    vec3From(inputs[i * 6 + 2]), shares[0], shares[1], shares[2], frames, check);
            report.check("adaptiveProbeUpdate stored", arithmeticTolerance, expected.stored, vec3From(outputs[i * 3]));
            report.check("adaptiveProbeUpdate history", arithmeticTolerance, expected.history, outputs[i * 3][3]);
            report.check("adaptiveProbeUpdate recent", arithmeticTolerance, expected.recent, vec3From(outputs[i * 3 + 1]));
            report.check("adaptiveProbeUpdate noise", transcendentalTolerance, expected.noise, outputs[i * 3 + 1][3]);
            report.check("eventProbeSharingWeight", arithmeticTolerance, eventProbeSharingWeight(sharing[0], sharing[1], frames, shares[0]),
                         outputs[i * 3 + 2][0]);
            report.check("restartedProbeKeep", transcendentalTolerance, restartedProbeKeep(shares[0], sharing[2], event[1], shares[3]), outputs[i * 3 + 2][1]);
        }
    }

    /*
    * Sharing's functions: the shared value over random neighbours, some visible; the texel a random direction falls in
    * (directions kept off texel edges, where rounding could pick either); whether a probe sees its neighbour, away from
    * the threshold; and rays turned by a probe's own turn, then the frame's rotation.
    */
    void checkSharing(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{66};
        constexpr int inputsPerCase = 16;
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(0.0f, 1.0f)));

            for (int neighbour = 0; neighbour < DDGI_SHARE_NEIGHBOURS; ++neighbour) {
                inputs.push_back(gpuVec4(random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(0.0f, 20.0f),
                                         random.between(0.0f, 1.0f) < 0.6f ? 1.0f : 0.0f));
            }

            const int side = i % 2 == 0 ? probeIrradianceTexels : probeDistanceTexels;
            const TexelCoord texel{static_cast<int>(random.between(0.0f, static_cast<float>(side) - 0.01f)),
                                   static_cast<int>(random.between(0.0f, static_cast<float>(side) - 0.01f))};
            const Vec3 direction = probeTexelDirection(texel, side);
            inputs.push_back(gpuVec4(direction.x, direction.y, direction.z, static_cast<float>(side)));

            const float separation = random.between(0.5f, 2.0f);
            const float share = random.between(0.0f, 1.0f) < 0.5f ? random.between(0.0f, 0.85f) : random.between(0.95f, 2.0f);
            inputs.push_back(gpuVec4(share * separation, separation));

            const ProbeRotationRows rotation = probeRotationRows(randomProbeRotation(static_cast<std::uint64_t>(i)));
            const ProbeRotationRows turn = probeRayTurn(i);

            for (const ProbeRotationRows* rows : {&rotation, &turn}) {
                for (const Vec3& row : rows->rows) {
                    inputs.push_back(gpuVec4(row.x, row.y, row.z));
                }
            }

            const int count = i % 3 == 0 ? 256 : 64;
            inputs.push_back(gpuVec4(random.between(0.0f, static_cast<float>(count) - 0.01f), static_cast<float>(count),
                                     random.between(0.0f, static_cast<float>(probeRayCycle) - 0.01f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_SHARING, inputs, inputsPerCase, 3);

        for (std::size_t i = 0; i < inputs.size() / inputsPerCase; ++i) {
            const GpuVec4* in = &inputs[i * inputsPerCase];
            std::array<Vec3, 6> neighbours;
            std::array<bool, 6> visible{};

            for (std::size_t neighbour = 0; neighbour < neighbours.size(); ++neighbour) {
                neighbours[neighbour] = vec3From(in[1 + neighbour]);
                visible[neighbour] = in[1 + neighbour][3] > 0.5f;
            }

            report.check("sharedProbeValue", arithmeticTolerance, sharedProbeValue(vec3From(in[0]), neighbours, visible, in[0][3]),
                         vec3From(outputs[i * 3]));

            const TexelCoord texel = probeDirectionTexel(vec3From(in[7]), static_cast<int>(in[7][3]));
            const bool sees = probesSeeEachOther(in[8][0], in[8][1]);
            report.check("probeDirectionTexel", exactTolerance, Vec3{static_cast<float>(texel.x), static_cast<float>(texel.y), 0.0f},
                         Vec3{outputs[i * 3 + 1][0], outputs[i * 3 + 1][1], 0.0f});
            report.check("probesSeeEachOther", exactTolerance, sees ? 1.0f : 0.0f, outputs[i * 3 + 1][2]);

            const ProbeRotationRows rotation{{vec3From(in[9]), vec3From(in[10]), vec3From(in[11])}};
            const ProbeRotationRows turn{{vec3From(in[12]), vec3From(in[13]), vec3From(in[14])}};
            const Vec3 direction = probeRayDirection(static_cast<int>(in[15][0]), static_cast<int>(in[15][1]), static_cast<int>(in[15][2]), rotation, turn);
            report.check("probeRayDirection (turned)", transcendentalTolerance, direction, vec3From(outputs[i * 3 + 2]));
        }
    }

    // Every probe of an uneven grid, scrolled and not, at both probe sizes, with a random direction each.
    void checkGrid(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{64};
        const ProbeVolume plain{Vec3{-3.5f, 0.25f, 7.0f}, Vec3{1.0f, 0.75f, 1.5f}, ProbeCounts{5, 3, 4}};
        const ProbeVolume scrolled{plain.origin, plain.spacing, plain.counts, ProbeScroll{3, 1, 2}};
        std::vector<GpuVec4> inputs;

        for (const ProbeVolume& volume : {plain, scrolled}) {
            for (int side : {probeIrradianceTexels, probeDistanceTexels}) {
                for (int probe = 0; probe < probeCount(volume.counts); ++probe) {
                    inputs.push_back(gpuVec4(volume.origin));
                    inputs.push_back(gpuVec4(volume.spacing));
                    inputs.push_back(gpuVec4(static_cast<float>(volume.counts.x), static_cast<float>(volume.counts.y),
                                             static_cast<float>(volume.counts.z), static_cast<float>(probe)));
                    inputs.push_back(gpuVec4(random.direction(), static_cast<float>(side)));
                    inputs.push_back(gpuVec4(static_cast<float>(volume.scroll.x), static_cast<float>(volume.scroll.y),
                                             static_cast<float>(volume.scroll.z)));
                    const ProbeAtlasGrid atlas = probeAtlasGrid(probeCount(volume.counts));
                    inputs.push_back(gpuVec4(static_cast<float>(atlas.columns), static_cast<float>(atlas.rows), 0.0f));
                }
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_GRID, inputs, 6, 3);

        for (std::size_t i = 0; i < inputs.size() / 6; ++i) {
            const int probe = static_cast<int>(inputs[i * 6 + 2][3]);
            const int side = static_cast<int>(inputs[i * 6 + 3][3]);
            const GpuVec4& scroll = inputs[i * 6 + 4];
            const ProbeAtlasGrid atlas{static_cast<int>(inputs[i * 6 + 5][0]), static_cast<int>(inputs[i * 6 + 5][1])};
            const ProbeVolume volume{plain.origin, plain.spacing, plain.counts,
                                     ProbeScroll{static_cast<int>(scroll[0]), static_cast<int>(scroll[1]), static_cast<int>(scroll[2])}};
            const TexelCoord origin = probeTileOrigin(atlas, probe, side);
            const Vec2 uv = probeAtlasUv(atlas, probe, vec3From(inputs[i * 6 + 3]), side);
            const GpuVec4& out = outputs[i * 3 + 1];
            const ProbeGridCoord grid = probeGridCoord(volume, probe);
            const GpuVec4& placed = outputs[i * 3 + 2];

            report.check("probePosition", arithmeticTolerance, probePosition(volume, probe), vec3From(outputs[i * 3]));
            report.check("probeGridCoord", exactTolerance, Vec3{static_cast<float>(grid.x), static_cast<float>(grid.y), static_cast<float>(grid.z)},
                         vec3From(placed));
            report.check("probeSlot", exactTolerance, static_cast<float>(probeSlot(volume, grid)), placed[3]);
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
        std::vector<IndexedMesh> indexedMeshes; // the same meshes' triangles, for the CPU's rays

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
            indexedMeshes.push_back(buildIndexedMesh(mesh));
            meshes.push_back(std::make_unique<VulkanMesh>(device, commands, indexedMeshes.back()));
            rayTracing.addMesh(commands, *meshes.back());
            return MeshHandle{static_cast<std::uint32_t>(meshes.size() - 1)};
        }

        // The frame's set 1 for these draws (all one white matte material), lights and probes, as the renderer writes it
        // with the camera at the origin.
        VkDescriptorSet writeFrame(const std::vector<DrawItem>& draws, const FrameLighting& lighting, std::span<const ProbeCascade> cascades,
                                   const VulkanDdgi& ddgi, const GlobalIlluminationSettings& settings) {
            MaterialTable materials;
            std::vector<GpuInstance> instances;

            for (const DrawItem& draw : draws) {
                instances.push_back(packInstance(draw, materials.add(draw.material)));
            }

            const GpuLights lights = packLighting(lighting, Vec3{});
            const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 100.0f};
            GpuFrameData frameData = makeFrameData(lights, Mat4::identity(), Vec3{}, makeClusterGrid(camera, 64, 64), false, false);
            const ProbeHitLights hitLights = probeHitLights(lights, Vec3{}, cascades.back().volume);
            packProbeSampling(frameData, ProbeSampling{countCascadeProbes(cascades), static_cast<int>(cascades.size()), settings.selfShadowBias,
                                                       settings.bounceEnergy, settings.lightsPerHit, hitLights.points, hitLights.spots});

            buffers.frameData.write(&frameData, sizeof(frameData));
            buffers.skyLight.write(lights.ambient, sizeof(lights.ambient));
            const GpuAtmosphere noAtmosphere{};
            buffers.atmosphere.write(&noAtmosphere, sizeof(noAtmosphere));
            buffers.instances.write(instances);
            buffers.materials.write(materials.getEntries());
            buffers.directionalLights.write(lights.directional);
            buffers.pointLights.write(lights.points);
            buffers.spotLights.write(lights.spots);
            buffers.probeHitLights.write(hitLights.lights);
            fastLightsListed = hitLights.fast > 0;

            const SampledImage dummy{white->getView(), sampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            return writeSceneSet(descriptors, sceneLayout, buffers,
                                 SceneImages{dummy, dummy, dummy, ddgi.sampledIrradiance(), ddgi.sampledDistance(), ddgi.sampledFastIrradiance(), dummy, dummy, dummy},
                                 clusterMasks, SceneBufferViews{ddgi.describeProbeStates(0), buffers.skyLight.describe(), ddgi.describeProbeCascades(0)});
        }

        // Whether the last frame written lists a fast light for the probe rays' hits.
        bool fastLightsListed = false;
    };

    // What the update should store for one probe, worked out on the CPU from what each of its rays brings back.
    struct ExpectedProbe {
        std::vector<Vec3> irradiance;     // per inner texel
        std::vector<Vec3> fastIrradiance; // from fast lights, per inner texel
        std::vector<Vec2> distance;       // mean and mean square, per inner texel
    };

    struct RayResult {
        Vec3 light;
        float distance = 0.0f;
        Vec3 fastLight{0.0f, 0.0f, 0.0f};
    };

    ExpectedProbe expectedProbe(const std::vector<Vec3>& directions, const std::vector<RayResult>& results, float maxDistance) {
        ExpectedProbe expected;

        for (int texel = 0; texel < probeIrradianceTexels * probeIrradianceTexels; ++texel) {
            const Vec3 direction = probeTexelDirection(TexelCoord{texel % probeIrradianceTexels, texel / probeIrradianceTexels}, probeIrradianceTexels);
            Vec3 sum{0.0f, 0.0f, 0.0f};
            Vec3 fastSum{0.0f, 0.0f, 0.0f};
            float weights = 0.0f;

            for (std::size_t ray = 0; ray < directions.size(); ++ray) {
                const float weight = irradianceRayWeight(direction, directions[ray]);
                sum = sum + results[ray].light * weight;
                fastSum = fastSum + results[ray].fastLight * weight;
                weights += weight;
            }

            expected.irradiance.push_back(sum * (1.0f / weights));
            expected.fastIrradiance.push_back(fastSum * (1.0f / weights));
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

    // The three atlases after an update, and the shared irradiance (only written while probes share light).
    struct ReadAtlases {
        ReadImage irradiance;
        ReadImage fastIrradiance;
        ReadImage distance;
        ReadImage sharedIrradiance;
    };

    // The atlases' grid for a single volume's probes.
    ProbeAtlasGrid atlasOf(const ProbeCounts& counts) {
        return probeAtlasGrid(probeCount(counts));
    }

    // A probe's shared irradiance against what it should hold, inner texels and borders.
    void checkShared(const ReadAtlases& atlases, const ProbeAtlasGrid& atlas, int probe, const std::vector<Vec3>& expected, FloatTolerance tolerance,
                     const std::string& name, GpuCheckReport& report) {
        const TexelCoord origin = probeTileOrigin(atlas, probe, probeIrradianceTexels);

        for (int texel = 0; texel < probeIrradianceTexels * probeIrradianceTexels; ++texel) {
            const int x = origin.x + 1 + texel % probeIrradianceTexels;
            const int y = origin.y + 1 + texel / probeIrradianceTexels;
            report.check(name, tolerance, expected[static_cast<std::size_t>(texel)], vec3From(atlases.sharedIrradiance.texel(x, y)));
        }

        for (int number = 0; number < probeBorderTexelCount(probeIrradianceTexels); ++number) {
            const TexelCoord bordered = probeBorderTexel(number, probeIrradianceTexels);
            const TexelCoord source = probeBorderSource(bordered, probeIrradianceTexels);
            report.check(name + " borders", exactTolerance, vec3From(atlases.sharedIrradiance.texel(origin.x + source.x, origin.y + source.y)),
                         vec3From(atlases.sharedIrradiance.texel(origin.x + bordered.x, origin.y + bordered.y)));
        }
    }

    // A probe's own irradiance, inner texels in order, as read back.
    std::vector<Vec3> ownIrradiance(const ReadAtlases& atlases, const ProbeAtlasGrid& atlas, int probe) {
        const TexelCoord origin = probeTileOrigin(atlas, probe, probeIrradianceTexels);
        std::vector<Vec3> own;

        for (int texel = 0; texel < probeIrradianceTexels * probeIrradianceTexels; ++texel) {
            own.push_back(vec3From(atlases.irradiance.texel(origin.x + 1 + texel % probeIrradianceTexels, origin.y + 1 + texel / probeIrradianceTexels)));
        }

        return own;
    }

    /*
    * Compare one probe's squares in all three atlases with what the CPU expects, and check every border texel is an
    * exact copy of the inner texel it should copy.
    */
    void checkProbe(const ReadAtlases& atlases, const ProbeAtlasGrid& atlas, int probe, const ExpectedProbe& expected, const std::string& name,
                    GpuCheckReport& report) {
        const ReadImage& irradiance = atlases.irradiance;
        const ReadImage& fastIrradiance = atlases.fastIrradiance;
        const ReadImage& distance = atlases.distance;
        const TexelCoord irradianceOrigin = probeTileOrigin(atlas, probe, probeIrradianceTexels);

        for (int texel = 0; texel < probeIrradianceTexels * probeIrradianceTexels; ++texel) {
            const int x = irradianceOrigin.x + 1 + texel % probeIrradianceTexels;
            const int y = irradianceOrigin.y + 1 + texel / probeIrradianceTexels;
            report.check(name + ": irradiance", storedIrradianceTolerance, expected.irradiance[static_cast<std::size_t>(texel)],
                         vec3From(irradiance.texel(x, y)));
            report.check(name + ": fast irradiance", storedIrradianceTolerance, expected.fastIrradiance[static_cast<std::size_t>(texel)],
                         vec3From(fastIrradiance.texel(x, y)));
        }

        const TexelCoord distanceOrigin = probeTileOrigin(atlas, probe, probeDistanceTexels);

        for (int texel = 0; texel < probeDistanceTexels * probeDistanceTexels; ++texel) {
            const GpuVec4 stored = distance.texel(distanceOrigin.x + 1 + texel % probeDistanceTexels, distanceOrigin.y + 1 + texel / probeDistanceTexels);
            const Vec2 wanted = expected.distance[static_cast<std::size_t>(texel)];
            report.check(name + ": distance", storedDistanceTolerance, Vec3{wanted.x, wanted.y, 0.0f}, Vec3{stored[0], stored[1], 0.0f});
        }

        for (const auto& [atlas, side, origin] : {std::tuple{&irradiance, probeIrradianceTexels, irradianceOrigin},
                                                  std::tuple{&fastIrradiance, probeIrradianceTexels, irradianceOrigin},
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

    // The frame time of the tests' updates.
    constexpr float testFrameSeconds = 1.0f / 60.0f;

    // The first update's ray directions: a fresh VulkanDdgi traces frame 0's (phase 0 of the first cycle's rotation).
    std::vector<Vec3> firstFrameRays(int count) {
        const ProbeRayFrame first = probeRayFrame(0);
        const ProbeRotationRows rows = probeRotationRows(first.rotation);
        std::vector<Vec3> directions;

        for (int ray = 0; ray < count; ++ray) {
            directions.push_back(probeRayDirection(ray, count, first.phase, rows));
        }

        return directions;
    }

    // One fresh update of the volume over these draws and lights, all three atlases read back.
    ReadAtlases runProbes(ProbeTestScene& scene, const std::vector<DrawItem>& draws, const FrameLighting& lighting, const ProbeVolume& volume,
                          const GlobalIlluminationSettings& settings) {
        VulkanDdgi ddgi{scene.device, scene.layouts, scene.textures.getLayout(), scene.sceneLayout.getFrameLayout(), scene.rayTracing.getSetLayout(),
                        scene.sampler.get(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT};

        const std::vector<ProbeCascade> cascades{fixedProbeCascade(volume)};
        scene.descriptors.reset();
        scene.commands.run([&](VkCommandBuffer recording) { ddgi.prepare(recording, cascades, settings, draws, Vec3{}, 0); });
        const VkDescriptorSet frame = scene.writeFrame(draws, lighting, cascades, ddgi, settings);

        scene.commands.run([&](VkCommandBuffer recording) {
            scene.rayTracing.buildScene(recording, 0, scene.rayTracing.packInstances(draws));
            const ProbeSceneSets sets{scene.textures.getSet(), frame, scene.rayTracing.getSceneSet(0)};
            ddgi.update(recording, scene.descriptors, sets, settings, lighting.background, testFrameSeconds, scene.fastLightsListed, false,
                        lighting.probeEvents, 0);
        });

        return ReadAtlases{readHalfImage(scene.device, scene.commands, ddgi.getIrradianceAtlas(), 4), readHalfImage(scene.device, scene.commands, ddgi.getFastIrradianceAtlas(), 4),
                           readHalfImage(scene.device, scene.commands, ddgi.getDistanceAtlas(), 2), readHalfImage(scene.device, scene.commands, ddgi.getSharedIrradianceAtlas(), 4)};
    }

    // A lamp's light where a ray from the probe meets the floor below it (pointLightAtHit), as the packed lights hold it.
    Vec3 lampOnFloor(const GpuPointLight& lamp, const Vec3& hit) {
        const Vec3 offset = Vec3{lamp.positionRange[0], lamp.positionRange[1], lamp.positionRange[2]} - hit;
        return pointLightAtHit(offset, lamp.positionRange[3], Vec3{lamp.radiance[0], lamp.radiance[1], lamp.radiance[2]}, lamp.radiance[3],
                               Vec3{0.0f, 1.0f, 0.0f});
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
    *   the brighter lamp's light where it lands (pointLightAtHit), checked by a shadow ray that nothing blocks;
    * - the two lamps again, the first flagged fast and both kept per hit: the probe is within the fast lamp's reach, so
    *   it traces the fast count of rays, and each ray's light goes into the atlas of its lamp;
    * - the sunlit floor with a fast lamp listed for the hits but out of the probe's reach: the stride makes room for the
    *   fast count, but the probe traces the still count, and nothing reaches the fast atlas;
    * - sharing between probes: the furnace again, every probe's own and shared light still the sky; and two probes with
    *   a wall between them, neither taking the other's light.
    * The atlases start empty, so the light bounced from them (last frame's probes) adds nothing on a first update. Each
    * scene also holds a speck far below, since a top-level structure needs something in it.
    */
    void checkProbePasses(const VulkanDevice& device, GpuCheckReport& report) {
        ProbeTestScene scene{device};
        const MeshHandle cube = scene.addMesh(Mesh::cube());
        const MeshHandle floor = scene.addMesh(Mesh::plane(1000.0f));
        const DrawItem speck = drawOf(cube, Mat4::translation(0.0f, -10000.0f, 0.0f) * Mat4::scaling(0.001f, 0.001f, 0.001f));

        // The probes' plain rules, which these scenes check ray by ray: 64 rays in the frame's shared directions, every
        // frame, the fixed memory, every probe traced where the grid puts it (the ray budget, the adaptive memory, sharing,
        // relocation and probe states, on by default, are checked apart).
        GlobalIlluminationSettings settings;
        settings.probeRelocation = false;
        settings.probeStates = false;
        settings.raysPerProbe = 64;
        settings.probeUpdatesPerSecond = 0.0f;
        settings.adaptiveMemory = false;
        settings.probeSharing = 0.0f;
        settings.fastProbeSharing = 0.0f;
        const std::vector<Vec3> rays = firstFrameRays(settings.raysPerProbe);
        const std::vector<Vec3> fastRays = firstFrameRays(settings.fastRaysPerProbe);

        {
            const ProbeVolume volume{Vec3{-1.0f, 0.5f, 2.0f}, Vec3{1.0f, 1.0f, 1.5f}, ProbeCounts{3, 2, 2}};
            FrameLighting lighting;
            lighting.background = Vec3{0.6f, 0.75f, 1.0f};
            const ReadAtlases atlases = runProbes(scene, {speck}, lighting, volume, settings);

            const std::vector<RayResult> results(rays.size(), RayResult{lighting.background, static_cast<float>(DDGI_RAY_REACH)});
            const ExpectedProbe expected = expectedProbe(rays, results, probeMaxDistance(volume.spacing));

            for (int probe = 0; probe < probeCount(volume.counts); ++probe) {
                checkProbe(atlases, atlasOf(volume.counts), probe, expected, "furnace", report);
            }

            // Sharing on: each probe turns its rays its own way, and the furnace is still exactly the sky, shared or not.
            GlobalIlluminationSettings sharing = settings;
            sharing.probeSharing = 0.5f;
            const ReadAtlases shared = runProbes(scene, {speck}, lighting, volume, sharing);

            for (int probe = 0; probe < probeCount(volume.counts); ++probe) {
                checkProbe(shared, atlasOf(volume.counts), probe, expected, "furnace, sharing", report);
                checkShared(shared, atlasOf(volume.counts), probe, expected.irradiance, storedIrradianceTolerance, "furnace: shared irradiance", report);
            }
        }

        {
            // More rays than one chunk of the update's shared memory (three chunks, the last part-full): the furnace again.
            GlobalIlluminationSettings many = settings;
            many.raysPerProbe = 600;
            const ProbeVolume volume{Vec3{-1.0f, 0.5f, 2.0f}, Vec3{1.0f, 1.0f, 1.5f}, ProbeCounts{2, 1, 1}};
            FrameLighting lighting;
            lighting.background = Vec3{0.6f, 0.75f, 1.0f};
            const ReadAtlases atlases = runProbes(scene, {speck}, lighting, volume, many);

            const std::vector<Vec3> manyRays = firstFrameRays(many.raysPerProbe);
            const std::vector<RayResult> results(manyRays.size(), RayResult{lighting.background, static_cast<float>(DDGI_RAY_REACH)});
            const ExpectedProbe expected = expectedProbe(manyRays, results, probeMaxDistance(volume.spacing));

            for (int probe = 0; probe < probeCount(volume.counts); ++probe) {
                checkProbe(atlases, atlasOf(volume.counts), probe, expected, "furnace, 600 rays", report);
            }
        }

        {
            // A wall between two probes 1 m apart: one sealed in a box, the other outside under the sky. Each sees the
            // box's wall 0.6 m or less towards the other, so neither takes the other's light: shared is own, exactly.
            const ProbeVolume volume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{2, 1, 1}};
            const DrawItem box = drawOf(cube, Mat4::scaling(0.4f, 0.4f, 0.4f));
            FrameLighting lighting;
            lighting.background = Vec3{0.6f, 0.75f, 1.0f};
            GlobalIlluminationSettings sharing = settings;
            sharing.probeSharing = 1.0f;
            const ReadAtlases atlases = runProbes(scene, {speck, box}, lighting, volume, sharing);

            for (int probe = 0; probe < probeCount(volume.counts); ++probe) {
                checkShared(atlases, atlasOf(volume.counts), probe, ownIrradiance(atlases, atlasOf(volume.counts), probe), exactTolerance, "walled probes: shared is own",
                            report);
            }
        }

        {
            const Vec3 halfSize{0.7f, 0.5f, 0.9f};
            const ProbeVolume volume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{1, 1, 1}};
            const DrawItem box = drawOf(cube, Mat4::scaling(halfSize.x, halfSize.y, halfSize.z));
            const ReadAtlases atlases = runProbes(scene, {speck, box}, FrameLighting{}, volume, settings);

            std::vector<RayResult> results;

            for (const Vec3& ray : rays) {
                const float wall = std::min({halfSize.x / std::abs(ray.x), halfSize.y / std::abs(ray.y), halfSize.z / std::abs(ray.z)});
                results.push_back(RayResult{Vec3{0.0f, 0.0f, 0.0f}, -wall});
            }

            checkProbe(atlases, atlasOf(volume.counts), 0, expectedProbe(rays, results, probeMaxDistance(volume.spacing)), "sealed box", report);
        }

        const float height = 1.0f;
        const float sunRadiance = 2.0f;
        const ProbeVolume overFloor{Vec3{0.0f, height, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{1, 1, 1}};
        const DrawItem floorDraw = drawOf(floor, Mat4::identity());

        // What a ray brings back from the sunlit floor below, or the black sky above.
        const auto sunlitFloor = [&](const Vec3& ray) {
            return ray.y < 0.0f ? RayResult{Vec3{sunRadiance, sunRadiance, sunRadiance}, height / -ray.y}
                                : RayResult{Vec3{0.0f, 0.0f, 0.0f}, static_cast<float>(DDGI_RAY_REACH)};
        };

        FrameLighting sunlit;
        sunlit.directionalLights.push_back(DirectionalLight{Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, sunRadiance});

        {
            const ReadAtlases atlases = runProbes(scene, {speck, floorDraw}, sunlit, overFloor, settings);
            std::vector<RayResult> results;

            for (const Vec3& ray : rays) {
                results.push_back(sunlitFloor(ray));
            }

            checkProbe(atlases, atlasOf(overFloor.counts), 0, expectedProbe(rays, results, probeMaxDistance(overFloor.spacing)), "sunlit floor", report);
        }

        FrameLighting lampLit;

        for (const auto& [position, intensity] : {std::pair{Vec3{0.3f, 0.4f, 0.2f}, 3.0f}, std::pair{Vec3{-0.8f, 0.6f, -0.5f}, 2.0f}}) {
            PlacedPointLight lamp;
            lamp.position = position;
            lamp.light.intensity = intensity;
            lamp.light.range = 6.0f;
            lamp.light.sourceRadius = 0.2f;
            lampLit.pointLights.push_back(lamp);
        }

        {
            GlobalIlluminationSettings onePerHit = settings;
            onePerHit.lightsPerHit = 1;
            const ReadAtlases atlases = runProbes(scene, {speck, floorDraw}, lampLit, overFloor, onePerHit);

            const GpuLights packed = packLighting(lampLit, Vec3{});
            std::vector<RayResult> results;

            for (const Vec3& ray : rays) {
                if (ray.y >= 0.0f) {
                    results.push_back(RayResult{Vec3{0.0f, 0.0f, 0.0f}, static_cast<float>(DDGI_RAY_REACH)});
                    continue;
                }

                const float along = height / -ray.y;
                const Vec3 hit = overFloor.origin + ray * along;
                Vec3 brightest{0.0f, 0.0f, 0.0f};

                for (const GpuPointLight& light : packed.points) {
                    const Vec3 lit = lampOnFloor(light, hit);

                    if (hitLightStrength(lit) > hitLightStrength(brightest)) {
                        brightest = lit;
                    }
                }

                results.push_back(RayResult{brightest, along});
            }

            checkProbe(atlases, atlasOf(overFloor.counts), 0, expectedProbe(rays, results, probeMaxDistance(overFloor.spacing)), "lamp-lit floor", report);
        }

        {
            FrameLighting oneFast = lampLit;
            oneFast.pointLights[0].light.fast = true;
            GlobalIlluminationSettings bothPerHit = settings;
            bothPerHit.lightsPerHit = 2;
            const ReadAtlases atlases = runProbes(scene, {speck, floorDraw}, oneFast, overFloor, bothPerHit);

            const GpuLights packed = packLighting(oneFast, Vec3{});
            std::vector<RayResult> results;

            for (const Vec3& ray : fastRays) {
                if (ray.y >= 0.0f) {
                    results.push_back(RayResult{Vec3{0.0f, 0.0f, 0.0f}, static_cast<float>(DDGI_RAY_REACH)});
                    continue;
                }

                const float along = height / -ray.y;
                const Vec3 hit = overFloor.origin + ray * along;
                results.push_back(RayResult{lampOnFloor(packed.points[1], hit), along, lampOnFloor(packed.points[0], hit)});
            }

            checkProbe(atlases, atlasOf(overFloor.counts), 0, expectedProbe(fastRays, results, probeMaxDistance(overFloor.spacing)), "fast lamp", report);
        }

        {
            // Listed: its range touches the corner of the probe's box grown by the reach. Out of the probe's reach: that
            // corner is reach x sqrt(3) from the probe. Too far to light the floor the rays hit.
            const float reach = probeMaxDistance(overFloor.spacing);
            const float corner = reach + 0.02f;
            FrameLighting farFast = sunlit;
            PlacedPointLight lamp;
            lamp.position = overFloor.origin + Vec3{corner, corner, corner};
            lamp.light.range = 0.1f;
            lamp.light.fast = true;
            farFast.pointLights.push_back(lamp);

            assert(probeHitLights(packLighting(farFast, Vec3{}), Vec3{}, overFloor).fast == 1);
            assert(!probeNearLight(overFloor.origin, lamp.position, lamp.light.range, reach));

            const ReadAtlases atlases = runProbes(scene, {speck, floorDraw}, farFast, overFloor, settings);
            std::vector<RayResult> results;

            for (const Vec3& ray : rays) {
                results.push_back(sunlitFloor(ray));
            }

            checkProbe(atlases, atlasOf(overFloor.counts), 0, expectedProbe(rays, results, probeMaxDistance(overFloor.spacing)), "far fast lamp", report);
        }
    }
}

namespace {
    // A box of solid blocks: Mesh::cube spans -1..1, so it is scaled by half the box's size and moved to its middle.
    DrawItem blockDraw(MeshHandle cube, const Vec3& low, const Vec3& high, bool isStatic) {
        const Vec3 middle = (low + high) * 0.5f;
        const Vec3 half = (high - low) * 0.5f;
        DrawItem draw = drawOf(cube, Mat4::translation(middle.x, middle.y, middle.z) * Mat4::scaling(half.x, half.y, half.z));
        draw.isStatic = isStatic;
        return draw;
    }

    /*
    * The real relocation pass (ddgi_relocate.comp, through VulkanDdgi: recorded with a frame's update, read back by the
    * next frame's prepare) against relocateProbe on the CPU, its rays traced over the same triangles with the static
    * mask. A row of probes 1 m apart over a static floor:
    * - one with a moving block round it, which relocation must not see: it stays put;
    * - one 0.1 m inside a static wall: it leaves through the wall's nearer face;
    * - one 0.03 m from a static post: it moves away from it;
    * - one 0.5 m inside a static block, deeper than its offset limit reaches: off.
    * Then the frame's states: the probes near the floor vigilant, the buried one off.
    */
    void checkRelocationPass(const VulkanDevice& device, GpuCheckReport& report) {
        ProbeTestScene scene{device};
        const MeshHandle cube = scene.addMesh(Mesh::cube());
        const ProbeVolume volume{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{4, 1, 1}};

        const std::vector<DrawItem> draws{
            blockDraw(cube, Vec3{-1.0f, -1.5f, -5.0f}, Vec3{10.0f, -0.5f, 5.0f}, true), // floor
            blockDraw(cube, Vec3{-0.2f, -0.2f, -0.2f}, Vec3{0.2f, 0.2f, 0.2f}, false), // moving block round probe 0
            blockDraw(cube, Vec3{0.9f, -0.5f, -3.0f}, Vec3{1.2f, 3.0f, 3.0f}, true),   // wall round probe 1
            blockDraw(cube, Vec3{2.03f, -0.5f, -0.3f}, Vec3{2.3f, 2.0f, 0.3f}, true),  // post beside probe 2
            blockDraw(cube, Vec3{2.5f, -0.5f, -2.0f}, Vec3{5.0f, 2.0f, 2.0f}, true),   // block round probe 3
        };

        GlobalIlluminationSettings settings;
        VulkanDdgi ddgi{scene.device, scene.layouts, scene.textures.getLayout(), scene.sceneLayout.getFrameLayout(), scene.rayTracing.getSetLayout(),
                        scene.sampler.get(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT};

        // The first frame records the relocation; the second's prepare reads it back.
        const std::vector<ProbeCascade> cascades{fixedProbeCascade(volume)};

        for (int frame = 0; frame < 2; ++frame) {
            scene.descriptors.reset();
            scene.commands.run([&](VkCommandBuffer recording) { ddgi.prepare(recording, cascades, settings, draws, Vec3{}, 0); });
            const VkDescriptorSet frameSet = scene.writeFrame(draws, FrameLighting{}, cascades, ddgi, settings);

            scene.commands.run([&](VkCommandBuffer recording) {
                scene.rayTracing.buildScene(recording, 0, scene.rayTracing.packInstances(draws));
                const ProbeSceneSets sets{scene.textures.getSet(), frameSet, scene.rayTracing.getSceneSet(0)};
                ddgi.update(recording, scene.descriptors, sets, settings, Vec3{}, testFrameSeconds, false, false, {}, 0);
            });
        }

        std::vector<CpuTraceInstance> traced;

        for (const DrawItem& draw : draws) {
            traced.push_back(CpuTraceInstance{&scene.indexedMeshes[draw.mesh.index], draw.model, draw.castsShadows, draw.isStatic});
        }

        const ProbeRayTrace traceStatic = [&](const Vec3& origin, const Vec3& direction) {
            const std::optional<CpuTraceHit> hit = traceOnCpu(traced, Ray{origin, direction}, probeRayMiss, staticRayMask);
            return !hit ? probeRayMiss : hit->front ? hit->distance : -hit->distance;
        };

        const std::vector<GpuProbeRelocation>& relocations = ddgi.getRelocations();
        report.check("relocation records", exactTolerance, static_cast<float>(probeCount(volume.counts)), static_cast<float>(relocations.size()));

        if (relocations.size() != static_cast<std::size_t>(probeCount(volume.counts))) {
            return;
        }

        // Distances and the moves built from them differ by rounding only (the hardware's watertight test against the
        // CPU's Moller-Trumbore), about 1e-5 of the distance.
        const FloatTolerance placeTolerance{.absolute = 1e-4f, .relative = 1e-4f};

        for (int probe = 0; probe < probeCount(volume.counts); ++probe) {
            const GpuProbeRelocation expected = relocateProbe(probePosition(volume, probe), volume.spacing, probeRelocationIterations, traceStatic);
            const GpuProbeRelocation& actual = relocations[static_cast<std::size_t>(probe)];
            report.check("relocation offset", placeTolerance, Vec3{expected.offset[0], expected.offset[1], expected.offset[2]},
                         Vec3{actual.offset[0], actual.offset[1], actual.offset[2]});
            report.check("relocation off", exactTolerance, expected.off, actual.off);
            report.check("relocation surface beyond cells", placeTolerance, expected.surfaceBeyondCells, actual.surfaceBeyondCells);
        }

        // What each scene asks of the relocation, beyond matching the CPU.
        const auto offsetX = [&](int probe) { return relocations[static_cast<std::size_t>(probe)].offset[0]; };
        report.check("unmoved beside a mover", exactTolerance, 0.0f, offsetX(0));
        report.check("left the wall", exactTolerance, 1.0f, 1.0f + offsetX(1) < 0.9f ? 1.0f : 0.0f);
        report.check("moved off the post", exactTolerance, 1.0f, offsetX(2) < 0.0f ? 1.0f : 0.0f);
        report.check("buried and off", exactTolerance, 1.0f, relocations[3].off);

        const std::vector<GpuProbeState>& states = ddgi.getProbeStates();
        report.check("states: vigilant by the floor", exactTolerance, static_cast<float>(ProbeState::Vigilant), static_cast<float>(states[0].state));
        report.check("states: buried off", exactTolerance, static_cast<float>(ProbeState::Off), static_cast<float>(states[3].state));
    }

    /*
    * Cascades that follow the camera, in the furnace (an empty sky drawn at B, no flat ambient): two cascades of different sizes,
    * 15 x 13 x 15 probes 1 m apart and 17 x 13 x 17 2 m apart (wide enough to light the densest's whole window at full
    * weight, so the plane it brings in is seeded from it alone), for 8 frames round one point, then a metre on along x, which moves the densest a cell
    * and leaves the coarser where it was. Every probe still holds B. The plane the move brought in starts from a seed,
    * the coarser cascade's light where it stands (B, since that cascade lights it at full weight; the flat ambient, 0,
    * would show if it did not), as probeSeedFrames frames of history: its texels' history is marked restarted with one
    * frame more (an unseeded start would show 1). Every other probe keeps its history. The probes are all traced (states
    * off), so the plane starts on the frame it comes in.
    */
    void checkCascadeScroll(const VulkanDevice& device, GpuCheckReport& report) {
        ProbeTestScene scene{device};
        const MeshHandle cube = scene.addMesh(Mesh::cube());
        const std::vector<DrawItem> draws{drawOf(cube, Mat4::translation(0.0f, -10000.0f, 0.0f) * Mat4::scaling(0.001f, 0.001f, 0.001f))};

        GlobalIlluminationSettings settings;
        settings.probeStates = false;
        settings.probeRelocation = false;
        FrameLighting lighting;
        lighting.background = Vec3{0.6f, 0.75f, 1.0f};
        const ProbeCascadeLayout layout{ProbeCounts{15, 13, 15}, ProbeCounts{17, 13, 17}, 1.0f, 2};

        VulkanDdgi ddgi{scene.device, scene.layouts, scene.textures.getLayout(), scene.sceneLayout.getFrameLayout(), scene.rayTracing.getSetLayout(),
                        scene.sampler.get(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT};
        std::vector<ProbeCascade> before;
        std::vector<ProbeCascade> after;

        for (int frame = 0; frame < 9; ++frame) {
            const Vec3 centre = frame < 8 ? Vec3{-0.4f, 0.0f, 0.0f} : Vec3{0.6f, 0.0f, 0.0f};
            const std::vector<ProbeCascade> cascades = probeCascadesAround(centre, layout, settings.selfShadowBias);
            (frame < 8 ? before : after) = cascades;
            scene.descriptors.reset();
            scene.commands.run([&](VkCommandBuffer recording) { ddgi.prepare(recording, cascades, settings, draws, Vec3{}, 0); });
            const VkDescriptorSet frameSet = scene.writeFrame(draws, lighting, cascades, ddgi, settings);

            scene.commands.run([&](VkCommandBuffer recording) {
                scene.rayTracing.buildScene(recording, 0, scene.rayTracing.packInstances(draws));
                const ProbeSceneSets sets{scene.textures.getSet(), frameSet, scene.rayTracing.getSceneSet(0)};
                ddgi.update(recording, scene.descriptors, sets, settings, lighting.background, testFrameSeconds, false, false, {}, 0);
            });
        }

        const std::vector<int> densestBrought = scrolledProbes(before[0], after[0]);
        report.check("cascade scroll: a plane comes in", exactTolerance, 195.0f, static_cast<float>(densestBrought.size()));
        report.check("cascade scroll: the coarser stays", exactTolerance, 0.0f, static_cast<float>(scrolledProbes(before[1], after[1]).size()));

        const ReadImage atlas = readHalfImage(scene.device, scene.commands, ddgi.getIrradianceAtlas(), 4);
        const ProbeAtlasGrid grid = probeAtlasGrid(probeCascadeLayoutProbes(layout));
        const std::set<int> brought(densestBrought.begin(), densestBrought.end());
        const FloatTolerance lightTolerance{.absolute = 2e-3f, .relative = 2e-3f};

        for (int probe = 0; probe < probeCascadeLayoutProbes(layout); probe += 7) {
            const TexelCoord origin = probeTileOrigin(grid, probe, probeIrradianceTexels);
            const GpuVec4 texel = atlas.texel(origin.x + 4, origin.y + 4);
            report.check("cascade scroll: the furnace", lightTolerance, lighting.background, Vec3{texel[0], texel[1], texel[2]});

            const bool seeded = brought.count(probe) != 0;
            report.check(seeded ? "cascade scroll: seeded history" : "cascade scroll: kept history", exactTolerance, 1.0f,
                         seeded ? (texel[3] == -(probeSeedFrames + 1.0f) ? 1.0f : 0.0f) : (texel[3] > 1.0f ? 1.0f : 0.0f));
        }

        for (const int probe : densestBrought) {
            const TexelCoord origin = probeTileOrigin(grid, probe, probeIrradianceTexels);
            const GpuVec4 texel = atlas.texel(origin.x + 3, origin.y + 5);
            report.check("cascade scroll: seeded from the coarser cascade", lightTolerance, lighting.background, Vec3{texel[0], texel[1], texel[2]});
            report.check("cascade scroll: seeded history", exactTolerance, -(probeSeedFrames + 1.0f), texel[3]);
        }
    }

    /*
    * The same move over a static floor, with relocation and probe states on (the defaults): the plane the move brings in
    * waits off on the frame it comes in, its relocation asked for; on the next frame its records are back (this frame in
    * flight's, read once its fence has passed: no waiting for the GPU), the probes by the floor start vigilant, seeded,
    * and the rest sleep. The probes stand half a metre off whole metres (the lattice's offset), so the floor, a slab from
    * 1.2 to 0.8 m down, lies between two layers 0.3 m from each: those two see its top or its underside within their
    * cells, the rest lie too far from it.
    */
    void checkCascadeScrollWaitsForItsRecords(const VulkanDevice& device, GpuCheckReport& report) {
        ProbeTestScene scene{device};
        const MeshHandle cube = scene.addMesh(Mesh::cube());
        const std::vector<DrawItem> draws{blockDraw(cube, Vec3{-60.0f, -1.2f, -60.0f}, Vec3{60.0f, -0.8f, 60.0f}, true)};

        const GlobalIlluminationSettings settings;
        FrameLighting lighting;
        lighting.background = Vec3{0.6f, 0.75f, 1.0f};
        const ProbeCascadeLayout layout{ProbeCounts{15, 13, 15}, ProbeCounts{17, 13, 17}, 1.0f, 2};

        VulkanDdgi ddgi{scene.device, scene.layouts, scene.textures.getLayout(), scene.sceneLayout.getFrameLayout(), scene.rayTracing.getSetLayout(),
                        scene.sampler.get(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT};
        std::vector<ProbeCascade> before;
        std::vector<ProbeCascade> after;
        std::vector<GpuProbeState> statesOnArrival;

        for (int frame = 0; frame < 10; ++frame) {
            const Vec3 centre = frame < 8 ? Vec3{-0.4f, 0.0f, 0.0f} : Vec3{0.6f, 0.0f, 0.0f};
            const std::vector<ProbeCascade> cascades = probeCascadesAround(centre, layout, settings.selfShadowBias);
            (frame < 8 ? before : after) = cascades;
            scene.descriptors.reset();
            scene.commands.run([&](VkCommandBuffer recording) { ddgi.prepare(recording, cascades, settings, draws, Vec3{}, 0); });

            if (frame == 8) {
                statesOnArrival = ddgi.getProbeStates();
            }

            const VkDescriptorSet frameSet = scene.writeFrame(draws, lighting, cascades, ddgi, settings);

            scene.commands.run([&](VkCommandBuffer recording) {
                scene.rayTracing.buildScene(recording, 0, scene.rayTracing.packInstances(draws));
                const ProbeSceneSets sets{scene.textures.getSet(), frameSet, scene.rayTracing.getSceneSet(0)};
                ddgi.update(recording, scene.descriptors, sets, settings, lighting.background, testFrameSeconds, false, false, {}, 0);
            });
        }

        const std::vector<int> brought = scrolledProbes(before[0], after[0]);
        const std::vector<GpuProbeState>& states = ddgi.getProbeStates();
        const ReadImage atlas = readHalfImage(scene.device, scene.commands, ddgi.getIrradianceAtlas(), 4);
        const ProbeAtlasGrid grid = probeAtlasGrid(probeCascadeLayoutProbes(layout));
        int seeded = 0;

        for (const int probe : brought) {
            const std::size_t index = static_cast<std::size_t>(probe);
            report.check("cascade scroll: waits off for its records", exactTolerance, static_cast<float>(ProbeState::Off),
                         static_cast<float>(statesOnArrival[index].state));

            const ProbeGridCoord place = probeLatticeCoord(after[0], probe);
            const ProbeState expected = place.y == -1 || place.y == -2 ? ProbeState::Vigilant : ProbeState::Sleeping;
            report.check("cascade scroll: its state once its records are back", exactTolerance, static_cast<float>(expected),
                         static_cast<float>(states[index].state));

            if (expected == ProbeState::Vigilant) {
                const TexelCoord origin = probeTileOrigin(grid, probe, probeIrradianceTexels);
                report.check("cascade scroll: starts seeded", exactTolerance, -(probeSeedFrames + 1.0f), atlas.texel(origin.x + 4, origin.y + 4)[3]);
                ++seeded;
            }
        }

        report.check("cascade scroll: the rows by the floor start", exactTolerance, 30.0f, static_cast<float>(seeded));
    }
}

void gpuTestDdgi(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("ddgi.glsl");
    checkOctahedral(runner, report);
    checkTexels(runner, report);
    checkRays(runner, report);
    checkReach(runner, report);
    checkWeights(runner, report);
    checkBlending(runner, report);
    checkAdaptiveMemory(runner, report);
    checkRestarts(runner, report);
    checkSharing(runner, report);
    checkGrid(runner, report);

    report.section("light probe passes (ddgi_trace.comp, ddgi_update_*.comp)");

    if (!device.supportsRayQueries()) {
        std::cout << "Light probe passes skipped: this GPU has no hardware ray tracing\n";
        return;
    }

    checkProbePasses(device, report);

    report.section("light probe relocation (ddgi_relocate.comp)");
    checkRelocationPass(device, report);

    report.section("light probe cascades (scroll, ddgi_seed.comp)");
    checkCascadeScroll(device, report);
    checkCascadeScrollWaitsForItsRecords(device, report);
}
