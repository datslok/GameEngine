#include "gpu_check.h"
#include "image_readback.h"
#include "gpu_test_functions.h"

#include "render/gpu/atmosphere.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/scene_layout.h"
#include "render/vulkan/vulkan_atmosphere.h"
#include "render/vulkan/vulkan_device.h"

#include <string>
#include <vector>

namespace {
    const std::string libraryShader = gpuTestShader("atmosphere_test");
    constexpr std::size_t atmosphereInputs = 12;

    const Atmosphere earth;

    // The atmosphere as every case begins: Earth's, the sun up and to the east, the moon up in the north-west, a camera
    // 2 m up.
    GpuAtmosphere packedEarth() {
        AtmosphereFrame frame;
        frame.viewRadius = earth.bottomRadius + 0.002f;
        frame.towardsSun = Vec3{0.6f, 0.8f, 0.0f};
        frame.sunLight = Vec3{3.0f, 2.9f, 2.7f};
        frame.discAngularRadius = 0.01f;
        frame.towardsMoon = Vec3{-0.3f, 0.5f, -0.81f}.normalized();
        frame.moonLight = Vec3{0.05f, 0.045f, 0.04f};
        frame.moonDiscAngularRadius = 0.02f;
        frame.moonDisc = Vec3{7.0f, 6.0f, 5.0f};
        return packAtmosphere(earth, frame);
    }

    void pushAtmosphere(std::vector<GpuVec4>& inputs) {
        const GpuAtmosphere packed = packedEarth();
        const float* values = &packed.rayleigh[0];

        for (std::size_t vector = 0; vector < atmosphereInputs; ++vector) {
            inputs.push_back(GpuVec4{values[vector * 4], values[vector * 4 + 1], values[vector * 4 + 2], values[vector * 4 + 3]});
        }
    }

    Vec3 sunIlluminance() {
        const GpuAtmosphere packed = packedEarth();
        return Vec3{packed.sunIlluminance[0], packed.sunIlluminance[1], packed.sunIlluminance[2]};
    }

    // The medium from the ground to the top and the phase functions all round.
    void checkMedium(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{101};
        std::vector<GpuVec4> inputs;
        const std::size_t perCase = atmosphereInputs + 2;

        for (int i = 0; i < 300; ++i) {
            pushAtmosphere(inputs);
            inputs.push_back(gpuVec4(random.between(-1.0f, 110.0f), random.between(-1.0f, 1.0f)));
            inputs.push_back(gpuVec4(0.0f));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_ATMOSPHERE_MEDIUM, inputs, static_cast<std::uint32_t>(perCase), 3);

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4& in = inputs[i * perCase + atmosphereInputs];
            const AtmosphereMedium medium = atmosphereMedium(earth, in[0]);
            const GpuVec4* out = &outputs[i * 3];
            report.check("atmosphereMedium rayleigh", transcendentalTolerance, medium.rayleighScattering, vec3From(out[0]));
            report.check("atmosphereMedium mie", transcendentalTolerance, medium.mieScattering, out[0][3]);
            report.check("atmosphereMedium scattering", transcendentalTolerance, medium.scattering, vec3From(out[1]));
            report.check("atmosphereMedium extinction", transcendentalTolerance, medium.extinction, vec3From(out[2]));
            report.check("rayleighPhase", arithmeticTolerance, rayleighPhase(in[1]), out[1][3]);
            report.check("henyeyGreensteinPhase", arithmeticTolerance, henyeyGreensteinPhase(earth.mieAnisotropy, in[1]), out[2][3]);
        }
    }

    /*
    * Rays from the ground to above the air, up, along and below the horizon, and table coordinates back to rays. A
    * march of 40 exponentials sums their rounding, and acos near the horizon is steep, so these get the transcendental
    * tolerance, with the angle a little more room where the table's cosine runs flat.
    */
    void checkTransmittance(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{102};
        std::vector<GpuVec4> inputs;
        const std::size_t perCase = atmosphereInputs + 2;

        for (int i = 0; i < 300; ++i) {
            pushAtmosphere(inputs);
            const float altitude = i % 3 == 0 ? random.between(0.001f, 0.1f) : random.between(0.001f, 105.0f);
            inputs.push_back(gpuVec4(earth.bottomRadius + altitude, random.between(-1.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.between(0.02f, 0.98f), random.between(0.02f, 0.98f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_ATMOSPHERE_TRANSMITTANCE, inputs, static_cast<std::uint32_t>(perCase), 3);
        const FloatTolerance angleTolerance{.absolute = 1e-4f, .relative = 1e-4f};

        // The light kept is exp(-depth): a relative error e in the summed depth is e times the depth in the result, and a
        // grazing ray's depth reaches about 3 (5% kept), so the transcendental allowance times a few.
        const FloatTolerance keptTolerance{.absolute = 1e-6f, .relative = 4e-4f};

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4& first = inputs[i * perCase + atmosphereInputs];
            const GpuVec4& second = inputs[i * perCase + atmosphereInputs + 1];
            const GpuVec4* out = &outputs[i * 3];
            const bool above = aboveHorizon(earth, first[0], first[1]);
            report.check("aboveHorizon", exactTolerance, above ? 1.0f : 0.0f, out[0][3]);

            if (above) {
                report.check("atmosphereTransmittance", keptTolerance, atmosphereTransmittance(earth, first[0], first[1]), vec3From(out[0]));
                const Vec2 uv = transmittanceLutUv(earth, first[0], first[1]);
                report.check("transmittanceLutUv", angleTolerance, Vec3{uv.x, uv.y, 0.0f}, Vec3{out[1][0], out[1][1], 0.0f});
            }

            const AtmosphereRay ray = transmittanceLutRay(earth, Vec2{second[0], second[1]});
            report.check("transmittanceLutRay", angleTolerance, Vec3{ray.radius, ray.cosZenith, 0.0f}, Vec3{out[1][2], out[1][3], 0.0f});
            report.check("atmosphereMarchLength", transcendentalTolerance, atmosphereMarchLength(earth, first[0], first[1]), out[2][0]);
        }
    }

    // Directions all round a camera on the ground or high up, through the sky view table's coordinates and back.
    void checkTables(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{103};
        std::vector<GpuVec4> inputs;
        const std::size_t perCase = atmosphereInputs + 2;

        for (int i = 0; i < 300; ++i) {
            pushAtmosphere(inputs);
            const float radius = earth.bottomRadius + (i % 2 == 0 ? random.between(0.001f, 0.01f) : random.between(0.01f, 50.0f));
            inputs.push_back(gpuVec4(random.direction(), radius));
            inputs.push_back(gpuVec4(random.direction()));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_ATMOSPHERE_TABLES, inputs, static_cast<std::uint32_t>(perCase), 5);
        const FloatTolerance angleTolerance{.absolute = 1e-4f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size() / 5; ++i) {
            const GpuVec4& first = inputs[i * perCase + atmosphereInputs];
            const GpuVec4& second = inputs[i * perCase + atmosphereInputs + 1];
            const GpuVec4* out = &outputs[i * 5];
            const float radius = first[3];

            const Vec2 scatteringUv = multipleScatteringLutUv(earth, radius, first[1]);
            const AtmosphereRay scattering = multipleScatteringLutRay(earth, scatteringUv);
            report.check("multipleScatteringLutUv", arithmeticTolerance, Vec3{scatteringUv.x, scatteringUv.y, 0.0f}, Vec3{out[0][0], out[0][1], 0.0f});
            report.check("multipleScatteringLutRay", arithmeticTolerance, Vec3{scattering.radius, scattering.cosZenith, 0.0f},
                         Vec3{out[0][2], out[0][3], 0.0f});

            const SkyViewDirection lookup = skyViewLookup(earth, radius, vec3From(first), vec3From(second));
            report.check("skyViewLookup", arithmeticTolerance, Vec3{lookup.cosViewZenith, lookup.cosLightView, 0.0f}, Vec3{out[1][0], out[1][1], 0.0f});
            report.check("skyViewLookup ground", exactTolerance, lookup.meetsGround ? 1.0f : 0.0f, out[1][2]);

            const Vec2 uv = skyViewLutUv(earth, radius, lookup);
            const SkyViewDirection back = skyViewLutDirection(earth, radius, uv);
            report.check("skyViewLutUv", angleTolerance, Vec3{uv.x, uv.y, 0.0f}, Vec3{out[2][0], out[2][1], 0.0f});
            report.check("skyViewLutDirection", angleTolerance, Vec3{back.cosViewZenith, back.cosLightView, 0.0f}, Vec3{out[2][2], out[2][3], 0.0f});
            report.check("skyViewTexelDirection", angleTolerance, skyViewTexelDirection(back), vec3From(out[3]));
            report.check("skyViewLutDirection ground", exactTolerance, back.meetsGround ? 1.0f : 0.0f, out[3][3]);
            report.check("skyViewSunDirection", arithmeticTolerance, skyViewSunDirection(second[1]), vec3From(out[4]));
        }
    }

    // One march step's source and update, a sphere's distance along random rays, the squared steps and the ambient's
    // directions.
    void checkMarch(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{104};
        std::vector<GpuVec4> inputs;
        const std::size_t perCase = atmosphereInputs + 6;

        for (int i = 0; i < 300; ++i) {
            pushAtmosphere(inputs);
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)}, random.between(0.01f, 5.0f)));
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)}, random.between(-1.0f, 1.0f)));
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, 0.1f), random.between(0.0f, 0.1f), random.between(0.0f, 0.1f)}, random.between(0.0f, 60.0f)));
            inputs.push_back(gpuVec4(earth.bottomRadius + random.between(0.001f, 3.0f), random.between(-1.0f, 1.0f),
                                     i % 2 == 0 ? earth.bottomRadius : earth.topRadius));
            inputs.push_back(gpuVec4(0.0f));
            const int steps = 1 + i % 40;
            inputs.push_back(gpuVec4(static_cast<float>(i % steps), static_cast<float>(steps), random.between(0.0f, 200.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_ATMOSPHERE_MARCH, inputs, static_cast<std::uint32_t>(perCase), 6);

        for (std::size_t i = 0; i < outputs.size() / 6; ++i) {
            const GpuVec4* in = &inputs[i * perCase + atmosphereInputs];
            const GpuVec4* out = &outputs[i * 6];
            const AtmosphereMedium medium = atmosphereMedium(earth, in[2][3]);
            const Vec3 source = skyStepSource(earth, medium, vec3From(in[1]), vec3From(in[2]), in[1][3], sunIlluminance());
            ScatteringMarch march{Vec3{0.1f, 0.2f, 0.3f}, vec3From(in[0]), Vec3{0.05f, 0.05f, 0.05f}};
            scatteringMarchStep(march, medium, source, in[0][3]);
            const MarchSample sample = marchSample(static_cast<int>(in[5][0]), static_cast<int>(in[5][1]), in[5][2]);

            report.check("skyStepSource", transcendentalTolerance, source, vec3From(out[0]));
            report.check("rayDistanceToSphere", arithmeticTolerance, rayDistanceToSphere(in[3][0], in[3][1], in[3][2]), out[0][3]);
            report.check("scatteringMarchStep luminance", transcendentalTolerance, march.luminance, vec3From(out[1]));
            report.check("scatteringMarchStep throughput", transcendentalTolerance, march.throughput, vec3From(out[2]));
            report.check("scatteringMarchStep scattered once", transcendentalTolerance, march.multipleScatteringAs1, vec3From(out[3]));
            report.check("marchSample", arithmeticTolerance, Vec3{sample.distance, sample.stepLength, 0.0f}, Vec3{out[1][3], out[2][3], 0.0f});
            report.check("skyAmbientDirection", arithmeticTolerance, skyAmbientDirection(static_cast<int>(in[5][0])), vec3From(out[4]));
            report.check("groundRadiance", arithmeticTolerance, groundRadiance(earth, vec3From(in[1]), in[1][3]), vec3From(out[5]));
        }
    }
}

namespace {
    /*
    * The sky's lights in the table's order, the moon's disc shaded over its whole disc and a little beyond, lit from all
    * round, and each sky's place in the table.
    */
    void checkLights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{106};
        std::vector<GpuVec4> inputs;
        const std::size_t perCase = atmosphereInputs + 4;

        for (int i = 0; i < 300; ++i) {
            pushAtmosphere(inputs);
            const Vec3 towardsBody = random.direction();
            const float radius = random.between(0.004f, 0.05f);
            const Vec3 offset = random.direction() * (radius * random.between(0.0f, 1.3f));
            const Vec3 direction = (towardsBody + offset - towardsBody * offset.dot(towardsBody)).normalized();
            inputs.push_back(gpuVec4(direction, std::sin(radius)));
            inputs.push_back(gpuVec4(towardsBody, static_cast<float>(i % skyViewLights)));
            inputs.push_back(gpuVec4(random.direction()));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_ATMOSPHERE_LIGHTS, inputs, static_cast<std::uint32_t>(perCase), 3);
        const GpuAtmosphere packed = packedEarth();
        const FloatTolerance shadingTolerance{.absolute = 1e-4f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4* in = &inputs[i * perCase + atmosphereInputs];
            const GpuVec4* out = &outputs[i * 3];
            const int light = static_cast<int>(in[1][3]);
            const Vec2 uv = skyViewAtlasUv(Vec2{in[3][0], in[3][1]}, light);
            const float* direction = light == 0 ? packed.sun : packed.moon;
            const float* illuminance = light == 0 ? packed.sunIlluminance : packed.moonIlluminance;

            report.check("lommelSeeliger", shadingTolerance, lommelSeeliger(vec3From(in[0]), vec3From(in[1]), in[0][3], vec3From(in[2])), out[0][0]);
            report.check("skyViewAtlasUv", arithmeticTolerance, Vec3{uv.x, uv.y, 0.0f}, Vec3{out[0][1], out[0][2], 0.0f});
            report.check("atmosphereLightDirection", exactTolerance, Vec3{direction[0], direction[1], direction[2]}, vec3From(out[1]));
            report.check("atmosphereLightIlluminance", exactTolerance, Vec3{illuminance[0], illuminance[1], illuminance[2]}, vec3From(out[2]));
        }
    }

    // One direction of the multiple scattering table's march, from the ground to the top, sun up and down, and the texel
    // from its sums. A march of 20 steps, each with a 40-step transmittance: their exponentials' rounding adds up.
    void checkMultipleScattering(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{105};
        std::vector<GpuVec4> inputs;
        const std::size_t perCase = atmosphereInputs + 2;

        for (int i = 0; i < 256; ++i) {
            pushAtmosphere(inputs);
            const float radius = earth.bottomRadius + (i % 2 == 0 ? random.between(0.001f, 2.0f) : random.between(2.0f, 99.0f));
            inputs.push_back(gpuVec4(radius, random.between(-0.5f, 1.0f), static_cast<float>(i % multipleScatteringDirections)));
            inputs.push_back(gpuVec4(random.between(0.0f, 10.0f), random.between(0.0f, 30.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_ATMOSPHERE_MULTIPLE_SCATTERING, inputs, static_cast<std::uint32_t>(perCase), 3);
        const FloatTolerance marchTolerance{.absolute = 1e-6f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4& first = inputs[i * perCase + atmosphereInputs];
            const GpuVec4& second = inputs[i * perCase + atmosphereInputs + 1];
            const GpuVec4* out = &outputs[i * 3];
            const ScatteringMarch march = multipleScatteringDirection(earth, first[0], first[1], static_cast<int>(first[2]));
            report.check("multipleScatteringDirection luminance", marchTolerance, march.luminance, vec3From(out[0]));
            report.check("multipleScatteringDirection scattered once", marchTolerance, march.multipleScatteringAs1, vec3From(out[1]));

            const Vec3 sums = multipleScatteringFromSums(Vec3{second[0], second[0] * 2.0f, second[0] * 3.0f},
                                                         Vec3{second[1], second[1] * 1.5f, second[1] * 2.0f});
            report.check("multipleScatteringFromSums", arithmeticTolerance, sums, vec3From(out[2]));
        }
    }
}

namespace {
    /*
    * The real passes (VulkanAtmosphere: the tables and the flat ambient), read back and set against the CPU, for Earth's
    * air with the sun low in the south-east (a camera 2 m up):
    * - the transmittance table's texels against the march for their rays;
    * - a few multiple scattering texels against multipleScatteringTexel;
    * - the sky view table's texels, the sun's sky and the moon's, against skyRadiance for their directions, its lookups
    *   the two tables as read back (filtered as the GPU's sampler does), so the pass is checked apart from the tables;
    * - the flat ambient against skyAmbient over the sky view table as read back, plus the frame's own.
    * Every table is half floats: about 1e-3 of each value, and the sampler's filter weights about as much again.
    */
    void checkPasses(const VulkanDevice& device, GpuCheckReport& report) {
        ImmediateCommands commands{device};
        FrameDescriptors descriptors{device.getDevice()};
        DescriptorLayouts layouts{device.getDevice()};

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        const OwnedHandle<VkSampler> sampler = device.createSampler(samplerInfo);
        VulkanAtmosphere atmosphere{device, layouts, sampler.get(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT};

        AtmosphereFrame frame;
        frame.viewRadius = earth.bottomRadius + 0.002f;
        frame.towardsSun = Vec3{0.5f, 0.35f, -0.6f}.normalized();
        frame.sunLight = Vec3{3.0f, 3.0f, 3.0f};
        frame.ambient = Vec3{0.01f, 0.02f, 0.03f};
        frame.towardsMoon = Vec3{-0.4f, 0.6f, 0.3f}.normalized();
        frame.moonLight = Vec3{0.05f, 0.045f, 0.04f};
        const GpuAtmosphere packed = packAtmosphere(earth, frame);
        UploadArray params{device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        params.write(&packed, sizeof(packed));

        commands.run([&](VkCommandBuffer recording) { atmosphere.update(recording, descriptors, earth, params.describe()); });

        const ReadImage transmittance = readHalfImage(device, commands, atmosphere.getTransmittanceTable(), 4);
        const ReadImage scattering = readHalfImage(device, commands, atmosphere.getMultipleScatteringTable(), 4);
        const ReadImage sky = readHalfImage(device, commands, atmosphere.getSkyViewTable(), 4);
        const std::vector<float> ambient = readFloatBuffer(device, commands, atmosphere.getSkyLight(), 4);
        const FloatTolerance halfTolerance{.absolute = 1e-5f, .relative = 2e-3f};

        for (int y = 0; y < transmittance.height; y += 3) {
            for (int x = 0; x < transmittance.width; x += 7) {
                const Vec2 uv{(static_cast<float>(x) + 0.5f) / static_cast<float>(transmittance.width),
                              (static_cast<float>(y) + 0.5f) / static_cast<float>(transmittance.height)};
                const AtmosphereRay ray = transmittanceLutRay(earth, uv);
                report.check("transmittance table", halfTolerance, atmosphereTransmittance(earth, ray.radius, ray.cosZenith),
                             vec3From(transmittance.texel(x, y)));
            }
        }

        for (const auto& [x, y] : {std::pair{0, 0}, std::pair{31, 0}, std::pair{16, 3}, std::pair{20, 15}, std::pair{12, 31}}) {
            const Vec2 uv{(static_cast<float>(x) + 0.5f) / static_cast<float>(multipleScatteringLutSize),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(multipleScatteringLutSize)};
            const AtmosphereRay ray = multipleScatteringLutRay(earth, uv);
            report.check("multiple scattering table", halfTolerance, multipleScatteringTexel(earth, ray.radius, ray.cosZenith),
                         vec3From(scattering.texel(x, y)));
        }

        // The tables as the sky view pass reads them.
        const AtmosphereLookups tables{
            [&](float radius, float cosZenith) {
                return aboveHorizon(earth, radius, cosZenith) ? vec3From(transmittance.bilinear(transmittanceLutUv(earth, radius, cosZenith)))
                                                              : Vec3{0.0f, 0.0f, 0.0f};
            },
            [&](float radius, float cosSunZenith) { return vec3From(scattering.bilinear(multipleScatteringLutUv(earth, radius, cosSunZenith))); }};
        const FloatTolerance filteredTolerance{.absolute = 1e-4f, .relative = 1e-2f};
        const Vec3 towardsLights[skyViewLights] = {frame.towardsSun, frame.towardsMoon};
        const Vec3 illuminances[skyViewLights] = {frame.sunLight * 3.14159265f, frame.moonLight * 3.14159265f};
        report.check("sky view table height", exactTolerance, static_cast<float>(skyViewLutHeight * skyViewLights), static_cast<float>(sky.height));

        for (int light = 0; light < skyViewLights; ++light) {
            const char* name = light == 0 ? "sky view table (sun)" : "sky view table (moon)";

            for (int y = 0; y < skyViewLutHeight; y += 9) {
                for (int x = 0; x < sky.width; x += 13) {
                    const Vec2 uv{(static_cast<float>(x) + 0.5f) / static_cast<float>(sky.width),
                                  (static_cast<float>(y) + 0.5f) / static_cast<float>(skyViewLutHeight)};
                    const SkyViewDirection view = skyViewLutDirection(earth, frame.viewRadius, uv);
                    const Vec3 direction = skyViewTexelDirection(view);
                    const Vec3 lightDirection = skyViewSunDirection(towardsLights[light].y);
                    report.check(name, filteredTolerance, skyRadiance(earth, frame.viewRadius, direction, lightDirection, illuminances[light], tables),
                                 vec3From(sky.texel(x, y + light * skyViewLutHeight)));
                }
            }
        }

        // Both skies, each looked up by its own light's bearing.
        const Vec3 expectedAmbient = frame.ambient + skyAmbient([&](const Vec3& direction) {
            Vec3 both{0.0f, 0.0f, 0.0f};

            for (int light = 0; light < skyViewLights; ++light) {
                const SkyViewDirection lookup = skyViewLookup(earth, frame.viewRadius, direction, towardsLights[light]);
                both = both + vec3From(sky.bilinear(skyViewAtlasUv(skyViewLutUv(earth, frame.viewRadius, lookup), light)));
            }

            return both;
        });
        report.check("flat ambient", filteredTolerance, expectedAmbient, Vec3{ambient[0], ambient[1], ambient[2]});
    }
}

void gpuTestAtmosphere(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("atmosphere.glsl");
    checkMedium(runner, report);
    checkTransmittance(runner, report);
    checkTables(runner, report);
    checkMarch(runner, report);
    checkMultipleScattering(runner, report);
    checkLights(runner, report);

    report.section("atmosphere passes (atmosphere_*.comp)");
    checkPasses(device, report);
}
