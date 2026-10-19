#pragma once

#include <cstdint>

/*
* A compute shader runs in workgroups of a size fixed in the shader (local_size_x), and a dispatch asks for a number of
* whole groups. To run once per item, ask for enough groups to cover every item: count / groupSize rounded up. The
* shader skips the spare invocations of the last group (index >= count). Throws std::invalid_argument for a groupSize of 0.
*/
std::uint32_t workgroupCount(std::uint32_t count, std::uint32_t groupSize);
