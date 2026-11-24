#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/virtual_shadows.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("virtual_shadows_test");

    /*
    * Distances across every level and exactly on each level's reach and a float step either side of it, for both
    * resolutions: (distance, first usable half-width, levels). The choice is by doubling, so CPU and GPU agree exactly.
    */
    void checkLevelChoice(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{41};
        std::vector<GpuVec4> inputs;

        for (int pagesPerSide : {32, 64}) {
            const float firstUsable = virtualShadowUsableHalfWidth(0.5f, pagesPerSide);

            for (int level = 0; level < 15; ++level) {
                const float reach = virtualShadowUsableHalfWidth(virtualShadowHalfWidth(0.5f, level), pagesPerSide);

                for (float distance : {std::nextafter(reach, 0.0f), reach, std::nextafter(reach, 1e9f)}) {
                    inputs.push_back(gpuVec4(distance, firstUsable, 15.0f));
                }
            }

            for (int i = 0; i < 200; ++i) {
                inputs.push_back(gpuVec4(std::exp(random.between(-3.0f, 10.0f)), firstUsable, static_cast<float>(1 + i % 16)));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_LEVEL, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            const int expected = virtualShadowLevelFor(in[0], in[1], static_cast<int>(in[2]));
            report.check("virtualShadowLevelFor", exactTolerance, static_cast<float>(expected), outputs[i][0]);
        }
    }

    // Texels across a level (and a little outside it, for the page alone), origin slots, levels and both resolutions:
    // (texel x, texel y, origin slot x, y), (pages a side, level).
    void checkPagesAndSlots(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{42};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const int pagesPerSide = i % 2 == 0 ? 32 : 64;
            const float side = static_cast<float>(pagesPerSide * virtualShadowPageTexels);
            inputs.push_back(gpuVec4(random.between(-200.0f, side + 200.0f), random.between(-200.0f, side + 200.0f),
                                     std::floor(random.between(0.0f, static_cast<float>(pagesPerSide) - 0.01f)),
                                     std::floor(random.between(0.0f, static_cast<float>(pagesPerSide) - 0.01f))));
            inputs.push_back(gpuVec4(static_cast<float>(pagesPerSide), std::floor(random.between(0.0f, 15.99f))));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_PAGE, inputs, 2, 2);

        for (std::size_t i = 0; i < inputs.size() / 2; ++i) {
            const GpuVec4& texel = inputs[2 * i];
            const int pagesPerSide = static_cast<int>(inputs[2 * i + 1][0]);
            const int level = static_cast<int>(inputs[2 * i + 1][1]);
            const VirtualShadowPage page = virtualShadowPageOf(texel[0], texel[1]);
            const VirtualShadowPage inLevel{std::clamp(page.x, 0, pagesPerSide - 1), std::clamp(page.y, 0, pagesPerSide - 1)};
            const VirtualShadowPage origin{static_cast<int>(texel[2]), static_cast<int>(texel[3])};
            const VirtualShadowPage slot = virtualShadowSlot(inLevel, origin, pagesPerSide);
            const GpuVec4& out = outputs[2 * i];

            report.check("virtualShadowPageOf", exactTolerance, static_cast<float>(page.x), out[0]);
            report.check("virtualShadowPageOf", exactTolerance, static_cast<float>(page.y), out[1]);
            report.check("virtualShadowSlot", exactTolerance, static_cast<float>(slot.x), out[2]);
            report.check("virtualShadowSlot", exactTolerance, static_cast<float>(slot.y), out[3]);
            report.check("virtualShadowTableIndex", exactTolerance, static_cast<float>(virtualShadowTableIndex(level, slot, pagesPerSide)),
                         outputs[2 * i + 1][0]);
        }
    }

    // Points up to a level's half-width from the camera at every level's scale, placements as the clipmap makes them
    // (the camera within half a page of the centre): (light-space point), (camera texel x, y, inverse texel), (depth
    // scale, offset).
    void checkLevelPoint(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{43};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const int level = i % 15;
            const float halfWidth = virtualShadowHalfWidth(0.5f, level);
            const float range = 2.0f * 16.0f * halfWidth + 200.0f;
            inputs.push_back(gpuVec4(random.between(-halfWidth, halfWidth), random.between(-halfWidth, halfWidth), random.between(-halfWidth, halfWidth)));
            inputs.push_back(gpuVec4(random.between(1984.0f, 2112.0f), random.between(1984.0f, 2112.0f), 1.0f / virtualShadowTexelSize(halfWidth, 32)));
            inputs.push_back(gpuVec4(1.0f / range, random.between(0.3f, 0.7f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_LEVEL_POINT, inputs, 3, 1);

        for (std::size_t i = 0; i < inputs.size() / 3; ++i) {
            const GpuVec4& point = inputs[3 * i];
            const GpuVec4& place = inputs[3 * i + 1];
            const GpuVec4& depth = inputs[3 * i + 2];
            VirtualShadowPlacement placement;
            placement.cameraTexelX = place[0];
            placement.cameraTexelY = place[1];
            placement.inverseTexel = place[2];
            placement.depthScale = depth[0];
            placement.depthOffset = depth[1];
            const Vec3 expected = virtualShadowLevelPoint(Vec3{point[0], point[1], point[2]}, placement);
            report.check("virtualShadowLevelPoint", arithmeticTolerance, expected, Vec3{outputs[i][0], outputs[i][1], outputs[i][2]});
        }
    }
}

namespace {
    // Texels across a level and outside it, reaches up to a page, both resolutions; slots back to pages from any origin:
    // (texel x, y, reach, origin x), (origin y, pages a side, slot x, y).
    void checkMarkedPages(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{44};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const float pagesPerSide = i % 2 == 0 ? 32.0f : 64.0f;
            const float side = pagesPerSide * static_cast<float>(virtualShadowPageTexels);
            inputs.push_back(gpuVec4(random.between(-100.0f, side + 100.0f), random.between(-100.0f, side + 100.0f),
                                     std::floor(random.between(0.0f, 128.0f)), std::floor(random.between(0.0f, pagesPerSide - 0.01f))));
            inputs.push_back(gpuVec4(std::floor(random.between(0.0f, pagesPerSide - 0.01f)), pagesPerSide,
                                     std::floor(random.between(0.0f, pagesPerSide - 0.01f)), std::floor(random.between(0.0f, pagesPerSide - 0.01f))));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_MARKED_PAGES, inputs, 2, 2);

        for (std::size_t i = 0; i < inputs.size() / 2; ++i) {
            const GpuVec4& a = inputs[2 * i];
            const GpuVec4& b = inputs[2 * i + 1];
            const int pagesPerSide = static_cast<int>(b[1]);
            const VirtualShadowPageSpan span = virtualShadowMarkedPages(a[0], a[1], a[2], pagesPerSide);
            const VirtualShadowPage slot{static_cast<int>(b[2]), static_cast<int>(b[3])};
            const VirtualShadowPage origin{static_cast<int>(a[3]), static_cast<int>(b[0])};
            const VirtualShadowPage page = virtualShadowPageOfSlot(slot, origin, pagesPerSide);
            const GpuVec4& marked = outputs[2 * i];
            report.check("virtualShadowMarkedPages", exactTolerance, static_cast<float>(span.first.x), marked[0]);
            report.check("virtualShadowMarkedPages", exactTolerance, static_cast<float>(span.first.y), marked[1]);
            report.check("virtualShadowMarkedPages", exactTolerance, static_cast<float>(span.last.x), marked[2]);
            report.check("virtualShadowMarkedPages", exactTolerance, static_cast<float>(span.last.y), marked[3]);
            report.check("virtualShadowPageOfSlot", exactTolerance, static_cast<float>(page.x), outputs[2 * i + 1][0]);
            report.check("virtualShadowPageOfSlot", exactTolerance, static_cast<float>(page.y), outputs[2 * i + 1][1]);
        }
    }

    // Entries with every flag mix, keys alike and apart, keys from first pages left of the origin: (the entry's high
    // and low halves, the stored and current keys' x), (their y, their generations).
    void checkEntries(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{45};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const float high = std::floor(random.between(0.0f, 16.0f)) * 4096.0f;
            const float x = std::floor(random.between(0.0f, 40.0f));
            const float y = std::floor(random.between(0.0f, 40.0f));
            const float generation = std::floor(random.between(1.0f, 4.0f));
            const bool same = i % 3 != 0;
            inputs.push_back(gpuVec4(high, std::floor(random.between(0.0f, 16383.0f)), x, same ? x : std::floor(random.between(0.0f, 40.0f))));
            inputs.push_back(gpuVec4(y, same ? y : std::floor(random.between(0.0f, 40.0f)), generation, i % 7 == 0 ? generation + 1.0f : generation));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_ENTRY, inputs, 2, 2);

        for (std::size_t i = 0; i < inputs.size() / 2; ++i) {
            const GpuVec4& a = inputs[2 * i];
            const GpuVec4& b = inputs[2 * i + 1];
            const std::uint32_t entry = (static_cast<std::uint32_t>(a[0]) << 16u) | static_cast<std::uint32_t>(a[1]);
            const VirtualShadowKey stored{static_cast<std::uint32_t>(a[2]), static_cast<std::uint32_t>(b[0]), static_cast<std::uint32_t>(b[2])};
            const VirtualShadowKey current{static_cast<std::uint32_t>(a[3]), static_cast<std::uint32_t>(b[1]), static_cast<std::uint32_t>(b[3])};
            const bool stale = virtualShadowEntryStale(entry, stored, current);
            const std::uint32_t packed = virtualShadowEntryWith(static_cast<std::uint32_t>(a[1]), entry);
            const GpuVec4& out = outputs[2 * i];
            report.check("virtualShadowEntryStale", exactTolerance, stale ? 1.0f : 0.0f, out[0]);
            report.check("virtualShadowEntryNeeds", exactTolerance, virtualShadowEntryNeeds(entry, stale) ? 1.0f : 0.0f, out[1]);
            report.check("virtualShadowPhysicalPage", exactTolerance, static_cast<float>(virtualShadowPhysicalPage(entry)), out[2]);
            report.check("virtualShadowEntryWith", exactTolerance, static_cast<float>(packed >> 16u), out[3]);

            // Keys come back through floats, so only small ones: the first page's x negative, as left of the origin.
            const VirtualShadowPage page{static_cast<int>(a[2]), static_cast<int>(a[3])};
            const VirtualShadowKey key = virtualShadowKeyOf(page, -static_cast<std::int32_t>(b[0]), static_cast<std::int32_t>(b[1]),
                                                            static_cast<std::uint32_t>(b[2]));
            report.check("virtualShadowKeyOf", exactTolerance, static_cast<float>(key.pageX), outputs[2 * i + 1][0]);
            report.check("virtualShadowKeyOf", exactTolerance, static_cast<float>(key.pageY), outputs[2 * i + 1][1]);
            report.check("virtualShadowKeyOf", exactTolerance, static_cast<float>(key.generation), outputs[2 * i + 1][2]);
        }
    }

    // Every k round the free list's top and the cached list's end: (k, free top, cached).
    void checkAllocationSource(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (int freeTop : {0, 1, 5}) {
            for (int cached : {0, 3}) {
                for (int k = 0; k < 10; ++k) {
                    inputs.push_back(gpuVec4(static_cast<float>(k), static_cast<float>(freeTop), static_cast<float>(cached)));
                }
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_SOURCE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const VirtualShadowSource source = virtualShadowAllocationSource(
                static_cast<std::uint32_t>(inputs[i][0]), static_cast<std::uint32_t>(inputs[i][1]), static_cast<std::uint32_t>(inputs[i][2]));
            report.check("virtualShadowAllocationSource", exactTolerance, static_cast<float>(source.from), outputs[i][0]);
            report.check("virtualShadowAllocationSource", exactTolerance, static_cast<float>(source.index), outputs[i][1]);
        }
    }

    // Every level's colour; texels by and away from page edges: (level, texel x, y, width).
    void checkPageView(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{46};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            inputs.push_back(gpuVec4(static_cast<float>(i % 16), random.between(-300.0f, 4400.0f), random.between(-300.0f, 4400.0f),
                                     random.between(0.5f, 6.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_VIRTUAL_SHADOW_PAGE_VIEW, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const GpuVec4& in = inputs[i];
            const Vec3 colour = virtualShadowLevelColour(static_cast<int>(in[0]));
            report.check("virtualShadowLevelColour", exactTolerance, colour, Vec3{outputs[i][0], outputs[i][1], outputs[i][2]});
            report.check("virtualShadowNearPageEdge", exactTolerance, virtualShadowNearPageEdge(in[1], in[2], in[3]) ? 1.0f : 0.0f, outputs[i][3]);
        }
    }
}

void gpuTestVirtualShadows(GpuFunctionRunner& runner, GpuCheckReport& report) {
    checkLevelChoice(runner, report);
    checkPagesAndSlots(runner, report);
    checkLevelPoint(runner, report);
    checkMarkedPages(runner, report);
    checkEntries(runner, report);
    checkAllocationSource(runner, report);
    checkPageView(runner, report);
}

