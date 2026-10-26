#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

/*
* The kinds of cloth pass (render/vulkan/vulkan_cloth.h), each timed on the GPU while the debug readout shows: a
* timestamp before the frame's first cloth pass and one after each, every gap credited to the kind of pass it follows,
* so a frame's thousand or so dispatches add up into a dozen numbers. Sweep is vertex block descent's points; Move is a
* level's line search moving the points; Copy is the copies of the points (kept for drawing between ticks, and read
* back).
*/
enum class ClothPass {
    Predict,
    Stretch,
    Bend,
    Sweep,
    Held,
    Gradient,
    CoarseStep,
    Spread,
    LineTerms,
    LineTotal,
    Move,
    Finish,
    Shape,
    Copy,
    Count,
};

inline constexpr std::size_t clothPassCount = static_cast<std::size_t>(ClothPass::Count);

// Short upper-case names for the readout (the stroke font has capitals only).
const char* clothPassName(ClothPass pass);

// How long each kind of cloth pass took on the GPU in one frame, all ticks and cloths together. Not valid for a frame
// not measured; overflowed for one with more passes than its stamps (render/vulkan/cloth_pass_stamps.h), not measured
// either, so the readout can say why.
struct ClothPassTimings {
    bool valid = false;
    bool overflowed = false;
    double totalSeconds = 0.0;
    std::array<double, clothPassCount> passSeconds{};

    double seconds(ClothPass pass) const {
        return passSeconds[static_cast<std::size_t>(pass)];
    }
};

/*
* A frame's stamps (one before the first pass, then one after each pass, kinds saying which) into seconds per kind,
* with the length of a tick and the counter's valid bits as for the renderer's own timings (render/gpu/gpu_timing.h).
* No stamps, or validBits 0, is not valid. Throws std::invalid_argument unless there is one stamp more than passes.
*/
ClothPassTimings clothPassTimingsFromTicks(std::span<const std::uint64_t> ticks, std::span<const ClothPass> kinds, double nanosecondsPerTick,
                                           std::uint32_t validBits);
