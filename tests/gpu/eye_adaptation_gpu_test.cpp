#include "gpu_check.h"
#include "gpu_test_functions.h"
#include "image_readback.h"

#include "render/gpu/eye_adaptation.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_eye_adaptation.h"
#include "render/vulkan/vulkan_memory.h"

#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {
    const std::string libraryShader = gpuTestShader("eye_adaptation_test");

    // A luminance spread evenly in stops over the histogram's range and a few stops past each end.
    float randomLuminance(CaseRandom& random) {
        return std::exp2(random.between(eyeHistogramLowestEv - 3.0f, eyeHistogramHighestEv + 3.0f));
    }

    /*
    * Colours and luminances all over the range (and none at all), every bin, means and references on both sides of the
    * limits, steps into the dark and into the light, held from before the delay to past it. A bin is a whole number, but log2 rounds a little differently on
    * the GPU, so a luminance right at a bin's edge may land in the next one: a bin's width of slack.
    */
    void checkSteps(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{51};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 512; ++i) {
            const Vec3 colour{random.between(0.0f, 4.0f), random.between(0.0f, 4.0f), random.between(0.0f, 4.0f)};
            inputs.push_back(gpuVec4(colour, i % 50 == 0 ? 0.0f : randomLuminance(random)));
            inputs.push_back(GpuVec4{static_cast<float>(i % eyeHistogramBins), random.between(-18.0f, 10.0f), random.between(-4.0f, 1.0f),
                                     random.between(0.0f, 1.0f)});
            inputs.push_back(GpuVec4{random.between(-8.0f, 0.0f), random.between(0.0f, 8.0f), random.between(-6.0f, 6.0f), random.between(-6.0f, 6.0f)});
            inputs.push_back(GpuVec4{random.between(0.0f, 0.2f), random.between(0.0f, 5.0f), random.between(0.0f, 2.0f), random.between(0.0f, 1.5f)});
            inputs.push_back(GpuVec4{random.between(0.0f, 2.0f), 0.0f, 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_EYE_STEPS, inputs, 5, 2);
        const FloatTolerance binTolerance{.absolute = 1.0f, .relative = 0.0f};

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 5];
            const GpuVec4* out = &outputs[i * 2];
            ToneMappingSettings settings;
            settings.adaptationReference = std::exp2(in[1][2]);
            settings.adaptationStrength = in[1][3];
            settings.lowestExposureEv = in[2][0];
            settings.highestExposureEv = in[2][1];
            settings.darkAdaptationSeconds = in[3][1];
            settings.brightAdaptationSeconds = in[3][2];
            settings.darkAdaptationDelaySeconds = in[3][3];

            report.check("pixelLuminance", arithmeticTolerance, pixelLuminance(vec3From(in[0])), out[0][0]);
            report.check("histogramBin", binTolerance, static_cast<float>(histogramBin(in[0][3])), out[0][1]);
            report.check("histogramBinEv", arithmeticTolerance, histogramBinEv(static_cast<int>(in[1][0])), out[0][2]);
            report.check("targetExposureEv", transcendentalTolerance, targetExposureEv(in[1][1], settings), out[0][3]);
            report.check("adaptExposureEv", transcendentalTolerance, adaptExposureEv(in[2][2], in[2][3], in[3][0], settings), out[1][0]);
            const HeldExposure held = holdThenAdaptExposure(in[2][2], in[2][3], in[4][0], in[3][0], settings);
            report.check("holdThenAdaptExposure", transcendentalTolerance, Vec3{held.exposureEv, held.heldSeconds, 0.0f}, Vec3{out[1][1], out[1][2], 0.0f});
        }
    }

    // Histograms of every shape: one bin, a few, all, empty; the shares from narrow to the whole.
    void checkMean(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{52};
        std::vector<GpuVec4> inputs;
        std::vector<std::vector<std::uint32_t>> histograms;
        std::vector<GpuVec4> shares;
        const std::uint32_t perCase = eyeHistogramBins / 4 + 1;

        for (int i = 0; i < 200; ++i) {
            std::vector<std::uint32_t> counts(eyeHistogramBins, 0);
            const int filled = i % 10 == 0 ? 0 : (i % 3 == 0 ? eyeHistogramBins : 1 + i % 7);

            for (int fill = 0; fill < filled; ++fill) {
                counts[static_cast<std::size_t>(random.between(0.0f, static_cast<float>(eyeHistogramBins) - 0.01f))] +=
                    static_cast<std::uint32_t>(random.between(1.0f, 50000.0f));
            }

            for (int bin = 0; bin < eyeHistogramBins; bin += 4) {
                inputs.push_back(GpuVec4{static_cast<float>(counts[bin]), static_cast<float>(counts[bin + 1]), static_cast<float>(counts[bin + 2]),
                                         static_cast<float>(counts[bin + 3])});
            }

            const float low = random.between(0.0f, 0.8f);
            const GpuVec4 share{low, random.between(low, 1.0f), random.between(-5.0f, 5.0f), 0.0f};
            inputs.push_back(share);
            histograms.push_back(counts);
            shares.push_back(share);
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_EYE_MEAN, inputs, perCase, 1);
        const FloatTolerance meanTolerance{.absolute = 1e-3f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            report.check("histogramMeanEv", meanTolerance, histogramMeanEv(histograms[i], shares[i][0], shares[i][1], shares[i][2]), outputs[i][0]);
        }
    }

    // The bin counts as the GPU left them (whole numbers, read back as the floats' bits).
    std::vector<std::uint32_t> readHistogram(const VulkanDevice& device, ImmediateCommands& commands, const VulkanEyeAdaptation& adaptation) {
        const std::vector<float> raw = readFloatBuffer(device, commands, adaptation.getHistogram(), eyeHistogramBins);
        std::vector<std::uint32_t> counts(eyeHistogramBins);
        std::memcpy(counts.data(), raw.data(), sizeof(std::uint32_t) * counts.size());
        return counts;
    }

    /*
    * The real passes on an image of known colours (float texels, so the GPU reads exactly what the CPU has): the
    * histogram against the CPU's of the same pixels (every pixel counted once; a pixel at a bin's edge may land in the
    * neighbouring bin, so at most a few in a thousand differ), then the exposure against adaptToHistogram of the
    * histogram the GPU made: snapped on the first frame, eased on the next.
    */
    void checkPasses(const VulkanDevice& device, GpuCheckReport& report) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        const OwnedHandle<VkSampler> sampler = device.createSampler(samplerInfo);

        ImmediateCommands commands{device};
        FrameDescriptors descriptors{device.getDevice()};
        VulkanEyeAdaptation adaptation{device, sampler.get(), 1};

        // Not whole workgroups either way, so the groups cut off by the edges are covered.
        constexpr std::uint32_t width = 301;
        constexpr std::uint32_t height = 173;
        CaseRandom random{53};
        std::vector<float> texels;
        std::vector<std::uint32_t> expected(eyeHistogramBins, 0);

        for (std::uint32_t pixel = 0; pixel < width * height; ++pixel) {
            const float brightness = randomLuminance(random);
            const Vec3 colour = Vec3{random.between(0.2f, 1.0f), random.between(0.2f, 1.0f), random.between(0.2f, 1.0f)} * brightness;
            texels.insert(texels.end(), {colour.x, colour.y, colour.z, 1.0f});
            ++expected[static_cast<std::size_t>(histogramBin(pixelLuminance(colour)))];
        }

        const VkDeviceSize bytes = texels.size() * sizeof(float);
        VulkanBuffer staging{device.getAllocator(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
        std::memcpy(staging.getMapped(), texels.data(), bytes);
        staging.flush();

        ImageDescription description;
        description.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        description.width = width;
        description.height = height;
        description.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.name = "Eye adaptation test";
        VulkanImage image{device, description};

        ToneMappingSettings settings;
        const auto run = [&](float seconds, bool snap, bool upload) {
            descriptors.reset();
            commands.run([&](VkCommandBuffer recording) {
                if (upload) {
                    image.transition(recording, ImageState::TransferDestination, ImageContents::Discard);
                    VkBufferImageCopy copy{};
                    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    copy.imageSubresource.layerCount = 1;
                    copy.imageExtent = {width, height, 1};
                    vkCmdCopyBufferToImage(recording, staging.get(), image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                }

                adaptation.adapt(recording, descriptors, 0, image, settings, seconds, snap);
            });
        };

        run(0.02f, true, true);
        const std::vector<std::uint32_t> counts = readHistogram(device, commands, adaptation);
        std::uint32_t total = 0;
        std::uint32_t differing = 0;

        for (int bin = 0; bin < eyeHistogramBins; ++bin) {
            total += counts[static_cast<std::size_t>(bin)];
            differing += static_cast<std::uint32_t>(std::abs(static_cast<int>(counts[static_cast<std::size_t>(bin)]) -
                                                             static_cast<int>(expected[static_cast<std::size_t>(bin)])));
        }

        report.check("histogram: every pixel counted", exactTolerance, static_cast<float>(width * height), static_cast<float>(total));
        report.check("histogram: pixels in another bin", FloatTolerance{.absolute = 0.002f * width * height, .relative = 0.0f}, 0.0f,
                     static_cast<float>(differing));

        const std::optional<GpuEyeAdaptation> snapped = adaptation.collect(0);
        report.check("adaptation read back", exactTolerance, 1.0f, snapped ? 1.0f : 0.0f);

        if (!snapped) {
            return;
        }

        const GpuEyeAdaptation expectedSnap = adaptToHistogram(GpuEyeAdaptation{}, counts, settings, 0.02f, true);
        const FloatTolerance evTolerance{.absolute = 1e-3f, .relative = 1e-4f};
        report.check("snapped mean", evTolerance, expectedSnap.meanEv, snapped->meanEv);
        report.check("snapped exposure", evTolerance, expectedSnap.exposureEv, snapped->exposureEv);
        report.check("snapped measured", exactTolerance, 1.0f, snapped->measured);

        // A frame later, from a different start (darker settings), easing rather than jumping.
        settings.adaptationReference = 0.05f;
        run(0.1f, false, false);
        const std::optional<GpuEyeAdaptation> eased = adaptation.collect(0);

        if (eased) {
            const GpuEyeAdaptation expectedEase = adaptToHistogram(*snapped, counts, settings, 0.1f, false);
            report.check("eased target", evTolerance, expectedEase.targetEv, eased->targetEv);
            report.check("eased exposure", evTolerance, expectedEase.exposureEv, eased->exposureEv);
        }

        report.check("collected once", exactTolerance, 0.0f, adaptation.collect(0).has_value() ? 1.0f : 0.0f);
    }
}

void gpuTestEyeAdaptation(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("eye_adaptation.glsl");
    checkSteps(runner, report);
    checkMean(runner, report);

    report.section("eye adaptation passes (eye_histogram.comp, eye_adaptation.comp)");
    checkPasses(device, report);
}
