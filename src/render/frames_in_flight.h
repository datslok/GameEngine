#pragma once

#include <algorithm>
#include <cstdint>

/*
* How many frames the CPU may record ahead of the GPU. One: the CPU waits for the last frame to finish before
* recording the next, so they take turns and input is never more than a frame old. Two: the CPU records the next
* frame while the GPU draws this one, so a frame costs the larger of the two instead of their sum, for up to one frame
* more latency. More would only add latency, so two is the most. Everything the CPU writes for a frame (commands,
* uniforms, shadow tiles, debug lines, timestamps) has one copy per frame in flight.
*/
inline constexpr std::uint32_t maxFramesInFlight = 2;

// A requested count kept between 1 and maxFramesInFlight.
constexpr std::uint32_t validFramesInFlight(std::uint32_t requested) {
    return std::clamp(requested, std::uint32_t{1}, maxFramesInFlight);
}
