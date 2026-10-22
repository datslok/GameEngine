#include "render/gpu/gpu_timing.h"

#include <stdexcept>

const char* gpuPassName(GpuPass pass) {
    switch (pass) {
    case GpuPass::DebugUi:
        return "UI";
    case GpuPass::RayScene:
        return "TLAS";
    case GpuPass::Atmosphere:
        return "SKY";
    case GpuPass::Probes:
        return "PROBES";
    case GpuPass::Clusters:
        return "CLUSTERS";
    case GpuPass::Shadows:
        return "SHADOW";
    case GpuPass::DepthPrepass:
        return "PREPASS";
    case GpuPass::Main:
        return "MAIN";
    case GpuPass::ProbeBalls:
        return "BALLS";
    case GpuPass::RayDebug:
        return "RAYS";
    case GpuPass::Motion:
        return "MOTION";
    case GpuPass::DepthRange:
        return "DEPTHS";
    case GpuPass::Taa:
        return "TAA";
    case GpuPass::EyeAdaptation:
        return "EYE";
    case GpuPass::Bloom:
        return "BLOOM";
    case GpuPass::ToneMap:
        return "TONE";
    case GpuPass::DebugLines:
        return "LINES";
    case GpuPass::Overlay:
        return "OVERLAY";
    case GpuPass::Count:
        break;
    }

    throw std::invalid_argument("gpuPassName: not a pass");
}

/*
* The GPU's counter is only validBits wide (often 64, sometimes 36 or 48): past its top it starts again at 0, like an
* odometer. Unsigned subtraction wraps modulo 2^64, so masking the difference to the valid bits gives the ticks modulo
* 2^validBits, which is right as long as less than one full turn passed (at 1 ns a tick, 36 bits is about a minute).
*/
std::uint64_t timestampTicksBetween(std::uint64_t earlier, std::uint64_t later, std::uint32_t validBits) {
    const std::uint64_t mask = validBits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << validBits) - 1;
    return (later - earlier) & mask;
}

/*
* Each pass lasts from the stamp before it to the stamp after it, so the passes add up exactly to the whole frame. The
* GPU overlaps work, and each stamp is written once everything before it has finished, so a pass's time is how much
* later the frame's work finished because of it.
*/
GpuTimings gpuTimingsFromTicks(std::span<const std::uint64_t> ticks, double nanosecondsPerTick, std::uint32_t validBits) {
    if (ticks.size() != gpuTimestampCount) {
        throw std::invalid_argument("gpuTimingsFromTicks: needs one stamp before the passes and one after each");
    }

    GpuTimings timings;

    if (validBits == 0) {
        return timings;
    }

    const double secondsPerTick = nanosecondsPerTick * 1e-9;

    for (std::size_t pass = 0; pass < gpuPassCount; ++pass) {
        const std::uint64_t passTicks = timestampTicksBetween(ticks[pass], ticks[pass + 1], validBits);
        timings.passSeconds[pass] = static_cast<double>(passTicks) * secondsPerTick;
        timings.totalSeconds += timings.passSeconds[pass];
    }

    timings.valid = true;
    return timings;
}
