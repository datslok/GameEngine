#include "engine/debug_views.h"
#include "render/gpu/gpu_timing.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>

namespace {
    bool nearlyEqual(double actual, double expected) {
        return std::abs(actual - expected) < 1e-12;
    }

    // One stamp before the first pass and one after each, as the renderer writes them.
    using Stamps = std::array<std::uint64_t, gpuTimestampCount>;
}

void testGpuTiming() {
    // Every pass has its own short upper-case name (the readout's stroke font) and one stamp after it, plus the first.
    {
        std::set<std::string> names;
        for (std::size_t pass = 0; pass < gpuPassCount; ++pass) {
            const std::string name = gpuPassName(static_cast<GpuPass>(pass));
            assert(!name.empty());
            for (char letter : name) {
                assert(letter >= 'A' && letter <= 'Z');
            }
            names.insert(name);
        }
        assert(names.size() == gpuPassCount);
        assert(gpuTimestampCount == gpuPassCount + 1);
    }

    // The ray tracing scene is built first of the scene's passes, then the light probes traced through it, then the
    // clusters' light lists, before anything that could read any of them.
    {
        assert(static_cast<int>(GpuPass::DebugUi) + 1 == static_cast<int>(GpuPass::RayScene));
        assert(static_cast<int>(GpuPass::RayScene) + 1 == static_cast<int>(GpuPass::Atmosphere));
        assert(static_cast<int>(GpuPass::Atmosphere) + 1 == static_cast<int>(GpuPass::Probes));
        assert(std::string(gpuPassName(GpuPass::Atmosphere)) == "SKY");
        assert(static_cast<int>(GpuPass::Probes) + 1 == static_cast<int>(GpuPass::Clusters));
        assert(static_cast<int>(GpuPass::Clusters) + 1 == static_cast<int>(GpuPass::Shadows));
        assert(std::string(gpuPassName(GpuPass::Clusters)) == "CLUSTERS");
    }

    // The probes' debug balls are drawn over the main pass's picture, and the ray tracing debug view replaces it, so
    // both come right after it, before anything reads it.
    {
        assert(static_cast<int>(GpuPass::Main) + 1 == static_cast<int>(GpuPass::ProbeBalls));
        assert(static_cast<int>(GpuPass::ProbeBalls) + 1 == static_cast<int>(GpuPass::RayDebug));
        assert(static_cast<int>(GpuPass::RayDebug) < static_cast<int>(GpuPass::Bloom));
    }

    // Anti-aliasing (TAA, FSR or DLSS, so its timing is named AA, not TAA): the velocity pass draws over the finished
    // picture's depth, and the resolve reads the picture before anything that uses it (eye adaptation, bloom), after the
    // depth measurement, which reads the same depth. Screen-space reflections come between: they read the velocity
    // image and the depth, and change the picture the resolve smooths.
    {
        assert(static_cast<int>(GpuPass::RayDebug) + 1 == static_cast<int>(GpuPass::Motion));
        assert(static_cast<int>(GpuPass::Motion) + 1 == static_cast<int>(GpuPass::DepthRange));
        assert(static_cast<int>(GpuPass::DepthRange) + 1 == static_cast<int>(GpuPass::Reflections));
        assert(static_cast<int>(GpuPass::Reflections) + 1 == static_cast<int>(GpuPass::AntiAliasing));
        assert(std::string(gpuPassName(GpuPass::Reflections)) == "SSR");
        assert(static_cast<int>(GpuPass::AntiAliasing) + 1 == static_cast<int>(GpuPass::EyeAdaptation));
        assert(std::string(gpuPassName(GpuPass::Motion)) == "MOTION" && std::string(gpuPassName(GpuPass::AntiAliasing)) == "AA");
    }

    // The depth prepass is recorded after the shadows and right before the main pass, whose depth it fills.
    {
        assert(static_cast<int>(GpuPass::Shadows) < static_cast<int>(GpuPass::DepthPrepass));
        assert(static_cast<int>(GpuPass::DepthPrepass) + 1 == static_cast<int>(GpuPass::Main));
    }

    // Ticks between two stamps: a plain difference while the counter has not rolled over.
    {
        assert(timestampTicksBetween(100, 350, 64) == 250);
        assert(timestampTicksBetween(7, 7, 64) == 0);
    }

    // A counter with fewer valid bits rolls over at 2^bits, like an odometer: the difference is taken modulo that,
    // and bits above the valid ones are ignored.
    {
        assert(timestampTicksBetween(250, 4, 8) == 10);
        assert(timestampTicksBetween((1ull << 36) - 5, 20, 36) == 25);
        assert(timestampTicksBetween(0xFF00'0000'0000'0010ull, 0x0000'0000'0000'0030ull, 32) == 0x20);
        assert(timestampTicksBetween(std::uint64_t(-3), 2, 64) == 5);
    }

    // Each pass lasts from the stamp before it to the stamp after it, in seconds from the tick length, and the passes
    // add up to the total. A pass that drew nothing (two equal stamps) takes no time.
    {
        Stamps stamps{};
        const std::array<std::uint64_t, gpuPassCount> lengths{3, 11, 8, 4, 2, 40, 25, 900, 5, 0, 12, 6, 10, 30, 9, 120, 0, 15, 7};
        stamps[0] = 1'000'000;
        for (std::size_t pass = 0; pass < gpuPassCount; ++pass) {
            stamps[pass + 1] = stamps[pass] + lengths[pass];
        }

        const double nanosecondsPerTick = 2.5;
        const GpuTimings timings = gpuTimingsFromTicks(stamps, nanosecondsPerTick, 64);
        assert(timings.valid);

        double sum = 0.0;
        for (std::size_t pass = 0; pass < gpuPassCount; ++pass) {
            assert(nearlyEqual(timings.passSeconds[pass], static_cast<double>(lengths[pass]) * nanosecondsPerTick * 1e-9));
            sum += timings.passSeconds[pass];
        }
        assert(nearlyEqual(timings.totalSeconds, sum));
        assert(nearlyEqual(timings.totalSeconds, 1207 * nanosecondsPerTick * 1e-9));
        assert(timings.seconds(GpuPass::ToneMap) == 0.0);
        assert(nearlyEqual(timings.seconds(GpuPass::Main), 900 * nanosecondsPerTick * 1e-9));
    }

    // A frame across the counter's roll-over still gives every pass its true length.
    {
        Stamps stamps{};
        stamps[0] = (1ull << 40) - 50;
        for (std::size_t pass = 0; pass < gpuPassCount; ++pass) {
            stamps[pass + 1] = (stamps[pass] + 20) & ((1ull << 40) - 1);
        }

        const GpuTimings timings = gpuTimingsFromTicks(stamps, 1.0, 40);
        for (double seconds : timings.passSeconds) {
            assert(nearlyEqual(seconds, 20e-9));
        }
        assert(nearlyEqual(timings.totalSeconds, gpuPassCount * 20e-9));
    }

    // A queue without timestamps (zero valid bits) gives no timings rather than made-up ones.
    {
        const Stamps stamps{};
        const GpuTimings timings = gpuTimingsFromTicks(stamps, 1.0, 0);
        assert(!timings.valid);
        assert(timings.totalSeconds == 0.0);
    }

    // The wrong number of stamps is a mistake in the renderer.
    {
        const std::array<std::uint64_t, 3> tooFew{1, 2, 3};
        bool threw = false;
        try {
            gpuTimingsFromTicks(tooFew, 1.0, 64);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // The readout's GPU profiler: the total first, then one section per pass in pass order, named like the passes.
    {
        const Profiler profiler = makeGpuProfiler();
        assert(profiler.getSectionCount() == static_cast<int>(gpuPassCount) + 1);
        assert(profiler.getName(0) == "GPU");
        for (std::size_t pass = 0; pass < gpuPassCount; ++pass) {
            assert(profiler.getName(static_cast<int>(pass) + 1) == gpuPassName(static_cast<GpuPass>(pass)));
        }
    }

    // Each frame's timings are averaged over the profiler's window, so the readout holds still; frames without
    // timings (not measured yet, or no timestamp support) are left out instead of counting as zero.
    {
        Profiler profiler = makeGpuProfiler();
        profiler.update(0.0);

        GpuTimings fast;
        fast.valid = true;
        fast.passSeconds[static_cast<std::size_t>(GpuPass::Main)] = 0.001;
        fast.totalSeconds = 0.001;

        GpuTimings slow = fast;
        slow.passSeconds[static_cast<std::size_t>(GpuPass::Main)] = 0.003;
        slow.totalSeconds = 0.003;

        recordGpuTimings(profiler, fast);
        recordGpuTimings(profiler, slow);
        recordGpuTimings(profiler, GpuTimings{});
        profiler.update(defaultProfileWindowSeconds);

        const int mainSection = static_cast<int>(GpuPass::Main) + 1;
        assert(profiler.getTiming(0).calls == 2);
        assert(nearlyEqual(profiler.getTiming(0).averageSeconds, 0.002));
        assert(nearlyEqual(profiler.getTiming(mainSection).averageSeconds, 0.002));
        assert(nearlyEqual(profiler.getTiming(mainSection).worstSeconds, 0.003));
        assert(profiler.getTiming(static_cast<int>(GpuPass::Bloom) + 1).averageSeconds == 0.0);
    }
}
