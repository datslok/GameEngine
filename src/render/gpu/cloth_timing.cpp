#include "render/gpu/cloth_timing.h"

#include "render/gpu/gpu_timing.h"

#include <stdexcept>

const char* clothPassName(ClothPass pass) {
    switch (pass) {
    case ClothPass::Predict:
        return "PREDICT";
    case ClothPass::Stretch:
        return "STRETCH";
    case ClothPass::Bend:
        return "BEND";
    case ClothPass::Sweep:
        return "SWEEP";
    case ClothPass::Held:
        return "HELD";
    case ClothPass::Gradient:
        return "GRAD";
    case ClothPass::CoarseStep:
        return "COARSE";
    case ClothPass::Spread:
        return "SPREAD";
    case ClothPass::LineTerms:
        return "TERMS";
    case ClothPass::LineTotal:
        return "TOTAL";
    case ClothPass::Move:
        return "MOVE";
    case ClothPass::Stitch:
        return "STITCH";
    case ClothPass::Finish:
        return "FINISH";
    case ClothPass::Shape:
        return "SHAPE";
    case ClothPass::Copy:
        return "COPY";
    case ClothPass::Search:
        return "SEARCH";
    case ClothPass::Self:
        return "SELF";
    case ClothPass::Count:
        break;
    }

    throw std::invalid_argument("clothPassName: not a pass");
}

/*
* Each stamp is written once everything before it has finished (the all-commands stage), and every cloth pass ends at a
* barrier, so each gap is how much later the cloth work finished because of that pass.
*/
ClothPassTimings clothPassTimingsFromTicks(std::span<const std::uint64_t> ticks, std::span<const ClothPass> kinds, double nanosecondsPerTick,
                                           std::uint32_t validBits) {
    ClothPassTimings timings;
    if (ticks.empty() || validBits == 0) {
        return timings;
    }
    if (ticks.size() != kinds.size() + 1) {
        throw std::invalid_argument("clothPassTimingsFromTicks: needs one stamp before the passes and one after each");
    }

    const double secondsPerTick = nanosecondsPerTick * 1e-9;
    for (std::size_t pass = 0; pass < kinds.size(); ++pass) {
        const double seconds = static_cast<double>(timestampTicksBetween(ticks[pass], ticks[pass + 1], validBits)) * secondsPerTick;
        timings.passSeconds[static_cast<std::size_t>(kinds[pass])] += seconds;
        timings.totalSeconds += seconds;
    }
    timings.valid = true;
    return timings;
}
